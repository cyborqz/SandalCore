// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_items_session.cpp: the player's items on a connection, worldd plan B2
//
//  The session is played with a fake client (the harness of test_worldd_movement): the join hands the client its storages and its gear, and what
//  the client believes after every request is rebuilt from the server's replies alone (the Mirror below: the PlayerController's storage events and
//  payloads, the pawn's HeldItems and AppearanceIDs), and compared with the session's inventory.
//
//  the join                      the client's storages, HeldItems and AppearanceIDs are the character's
//  wearing and taking off        ServerItemUsed (with ServerActivateItem, as the retail client sends both): the answer is the retail one, two
//                                events on the PlayerController and the pawn's HeldItems (all 23 items, the array key 23 on) and AppearanceIDs
//                                in the same packet, and no stats; taking the chest off changes the look, putting it on restores it
//  the bag                       ServerRequestStorageEvent: a move is Remove, Add; a swap Remove, Remove, Add, Add; a split, a stack, a destroy and
//                                the other storages are refused
//  refusals                      nothing changes, nothing but the storages' payloads is sent, and only a few times; unknown containers, garbage
//                                and hostile numbers never fail the session
//  budgets                       a burst of requests is cut at the budget, the rest counted
//  saving                        the inventory is handed to the sink every 60 s when it changed, and at the end of the session if it changed at
//                                all; a character that was never touched saves nothing; a real CharacterSaver gives the character service the
//                                whole inventory (and retries Unavailable); a second login starts from the old session's inventory
//  ServerTriggerAbility          a trigger that names an item now decodes (it used to be read from its first two parameters)
//  the real content.db           with AOC_CONTENT_DB set, the join, the chest off and on, and the two-hander on the file contentc built
//  item use and levels (plan D1) the join's 16th chain is the XPTracker (Adventuring XP and debt); using a level scroll consumes it, raises the
//                                XP total and the level, sends the level-up (the XPTracker items, BroadcastPlayLevelUpEffects, the pawn's Level, the
//                                TempHealth mark, the stats of the new level with the granted passives' mods, their CurrentEffects items, the tree's
//                                rank list and ClientUpdateSkillOnActionbar per grant) and saves the progress; a class scroll changes the class and
//                                the stats; a gear or consumables scroll fills the bag; the refusals (a scroll for one's level, an action of another
//                                type, a stale instance, an empty slot, a used-up stack, a full bag, spam) change nothing; a relog carries it all
//  professions (Gathering stage 2) on the real content.db: the join lists the 22 professions in the retail order with their certification levels (the
//                                retail join's maps are the golden) and the ExperienceFastMap only of those with XP; a GM artisan scroll gives profession XP,
//                                promotes and gives its tool, sends the XPTracker items and BroadcastPlayLevelUpEffects (the profession's leveling type is the
//                                category), saves all 22 rows and survives a relog; a scroll that changes nothing is refused and stays; the T3 tool equips
//  stats (plan B3)               the join's StatsComponent arrays are the engine's stats; an equip sends, in the same packet as the events, only the stats
//                                that changed, as deltas of their arrays, and the client's arrays are the session's stats again; Health and Mana above
//                                a new maximum are clamped, below it they stay; nothing is sent when no stat moves; the deltas wait for a full window
//                                with the rest of the update; a stat of the ability runtime goes through it (MaxStamina clamps Stamina)
// ============================================================================
#include "capture_leveling.h"
#include "capture_packets.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "bootstrap/captured_replies.h"
#include "bootstrap/captured_setup.h"
#include "bootstrap/character_setup.h"
#include "items/inventory.h"
#include "net/endpoint.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/leveling.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/skill_rpc.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "session/position_saver.h"
#include "session/talents.h"
#include "stats/character_stats.h"
#include "world_stats.h"
#include <bit>
#include <cmath>
#include <limits>
#include <tuple>
// [CoA ~ C-Y-B]

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace sg = aoc::worldd::stats;
namespace fs = std::filesystem;
namespace cf = content_fixture;
namespace fi = content_fixture::item;
using emit::BunchWriter;
using svc::ItemContainer;
using std::chrono::milliseconds;
using std::chrono::seconds;

static std::ostringstream g_log;
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
static const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
static const Clock::time_point t0{seconds(5000)};
static const Address kA{0x7F000001u, 50510};
static const Address kB{0x7F000001u, 50511};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;
static constexpr uint32_t kAny = 0xFFFFFFFFu;
static const ItemContainer Bag = ItemContainer::Backpack;
static const ItemContainer Worn = ItemContainer::Equipment;

static uint8_t g_counter = 1;
static void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_counter++;
}

template <class Pred>
static bool wait_until(Pred pred, milliseconds limit = milliseconds(3000)) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

static const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

// ---- the character --------------------------------------------------------------------------------------------------------------

static std::string guid_n(uint32_t n) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xA0000000u + n, 0x1000u + n, 0x2000u + n, n);
    return buf;
}
// == C.Y.B | C O A ==

static svc::InventoryItem make_item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_n(n);
    it.quantity = quantity;
    it.quality = 1;
    it.durability = 11;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

// the guids of the Bard's kit: 101 helmet, 102 chest, 105 legs, 106 feet, 115 scepter, 116 focus, 117 book, 123 bag, 126 pickaxe ...
static std::vector<svc::InventoryItem> bard_items() {
    const std::pair<int, int64_t> kit[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook},
                                            {23, fi::kBag}, {26, fi::kPickaxe}, {29, fi::kAxe}, {33, fi::kSickle}, {35, fi::kFishingRod}, {38, fi::kBow}};
    std::vector<svc::InventoryItem> out;
    for (const auto& [slot, record] : kit) out.push_back(make_item(Worn, slot, record, 100 + slot));
    // and the bag: a two-hander, a potion, a ring and a heavy chest piece
    out.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    out.push_back(make_item(Bag, 1, fi::kPotion, 201, 5));
    out.push_back(make_item(Bag, 2, fi::kRingA, 202));
    out.push_back(make_item(Bag, 3, fi::kHeavyChest, 203));
    return out;
}

namespace gm = content_fixture::gm;

/// The Bard's kit, the four bag items and the GM starter set of the fixture in bag slots 4-11 (guids 210-217).
static std::vector<svc::InventoryItem> bard_items_with_scrolls() {
    std::vector<svc::InventoryItem> out = bard_items();
    const int64_t scrolls[] = {gm::kLevel10, gm::kLevel25, gm::kClassTank, gm::kClassBard, gm::kGear01, gm::kConsumables, gm::kOdd, gm::kKeep};
    for (int i = 0; i < 8; ++i) out.push_back(make_item(Bag, 4 + i, scrolls[i], 210 + static_cast<uint32_t>(i)));
    return out;
}

static svc::Character cyb_record() {
    svc::Character c;
    c.id = kCyb;
    c.account_id = 3;
    c.name = "cyb";
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    c.items = bard_items();
    return c;
}

static bs::LoginIdentity identity() { return {kMachine, kMachine}; }
static double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

// ---- the world ------------------------------------------------------------------------------------------------------------------

struct RecordingSink : PositionSink {
    struct Saved {
        std::string id;
        std::vector<svc::InventoryItem> items;
    };
    std::vector<svc::Position> positions;
    std::vector<Saved> inventories;
    std::vector<std::pair<std::string, svc::Progress>> progress;   // plan D1
    void save(const std::string&, const svc::Position& p) override { positions.push_back(p); }
    void save_inventory(const std::string& id, std::vector<svc::InventoryItem> items) override { inventories.push_back({id, std::move(items)}); }
    void save_progress(const std::string& id, svc::Progress p) override { progress.emplace_back(id, std::move(p)); }
};

struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;

    /// The small content.db of content_db_fixture.h, or, with `real` and AOC_CONTENT_DB set, the real file.
    explicit Fixture(bool real = false, const char* real_path = nullptr) {
        dir = cf::make_temp_dir("worldd_items_session_");
        fs::path db = dir / "content.db";
        const char* real_db = std::getenv("AOC_CONTENT_DB");
        if (real && real_path && *real_path) db = real_path;
        else if (real && real_db && *real_db) db = real_db;
        else if (!cf::make_db(db)) std::exit(2);
        std::string error;
        content = content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
// [C O A - C.Y.B]

// ---- the client's view, rebuilt from what the server sent ---------------------------------------------------------------------------

struct Mirror {
    bs::SessionGuids guids;
    const schema::ClassSchema* info_class = nullptr;     // the pawn's BaseCharacterInfo
    const schema::ClassSchema* appearance_class = nullptr;
    std::map<std::pair<int, int>, repl::ItemInstance> storage;   // (0 bag / 1 equipment / 2 quest, slot) -> item
    std::vector<std::pair<int64_t, int>> held_updates;    // each HeldItems update: the array key and the number of items it carried
    std::map<int32_t, std::pair<int, int64_t>> held;      // ReplicationID -> (key, record)
    int held_key = 0;
    std::vector<std::optional<repl::AppearanceId>> appearance;
    size_t appearance_updates = 0;
    std::vector<std::string> pc_calls;                    // the PlayerController's RPCs, in order
    std::vector<repl::ItemStorageEvent> events;           // every storage event received, in order
    size_t payloads = 0, stat_blocks = 0, unreadable = 0;
    // plan B3: the client's StatsComponent arrays, by field name and ReplicationID, and each update that changed them
    const schema::ClassSchema* stats_class = nullptr;
    std::map<std::string, std::map<int32_t, repl::StatInt32Inst>> stat_items;
    struct StatUpdate {
        size_t chain = 0;                                  // the index of the chain in Harness::got
        std::map<std::string, std::set<int64_t>> changed;  // by array: the stats that changed
        size_t deleted = 0;
    };
    std::vector<StatUpdate> stat_updates;
    std::vector<size_t> held_chains;               // the index of the chain of each HeldItems update
    // plan D1: the client's XPTracker (the five maps, rebuilt from the deltas), the pawn's Level, the level-up broadcasts and the effect items
    repl::LevelingState tracker;
    size_t tracker_updates = 0;
    int    level = 1;
    std::vector<int> level_ups;                    // BroadcastPlayLevelUpEffects' InLevel, in order
    std::vector<int64_t> level_up_categories;      // and its Category (the leveling type), in the same order
    size_t effects_added = 0, effects_removed = 0;
    size_t absorbed = 0;
    // plan D1, stage 2: the client's AbilityComponent lists, its CurrentEffects items, its action bar and what it was told of skills, and the order of it all
    const schema::ClassSchema* ability_class = nullptr;
    std::vector<int64_t> known, records;
    struct EffectSeen {
        int64_t guid = 0;
        float   duration = 0;
    };
    std::map<int32_t, EffectSeen> effect_items;
    std::vector<repl::SkillOnActionbar> skill_updates;
    std::vector<repl::ActionbarPayload> bar_payloads;
    std::map<std::pair<int, int>, repl::ActionbarUseData> bar;
    /// "A" an update of KnownAbilities or the records, "E+" and "E-" an effect item added or removed, "S" stat items changed, "L" an XPTracker update,
    /// "U" a ClientUpdateSkillOnActionbar, "P" an action bar payload: one entry each, in the order of the blocks the server sent.
    std::vector<std::string> sequence;
    std::function<bool(int64_t)> stat_extended;   // the client's stat table: which stats carry base and equipment (the bundle's kStatBuckets)

    static int storage_of(const bs::SessionGuids& g, const emit::FIntrepidNetworkGUID& o) {
        for (int i = 0; i < 3; ++i) {
            if (o == g.resource_storage[static_cast<size_t>(i)]) return 3 + i;   // Gathering stage 3: Resource0 to Resource2
        }
        return o == g.backpack ? 0 : o == g.equipment ? 1 : o == g.quest_storage ? 2 : -1;
    }

    void absorb(const fake::Delivered& d, uint32_t pc_ch, uint32_t pawn_ch, uint32_t channel) {
        const size_t chain = absorbed++;
        if (d.open || d.close) return;   // an open chain is SerializeNewActor and the first state: no storages, no gear
        if (d.content.bit_pos() == 8) return;   // a test's one-byte filler bunch (clog_channel): nothing of the server's is that short
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) {
            if (d.content.bit_pos() > 0) ++unreadable;
            return;
        }
        for (const repl::BlockView& v : *views) {
            const size_t end = v.payload_pos + v.payload_bits;
            size_t p = v.payload_pos;
            if (channel == pc_ch && !v.header.is_actor && v.header.object == guids.xp_tracker) {
                tracker_block(d, v);
                continue;
            }
            if (channel == pc_ch && v.header.is_actor) {
                if (v.header.has_rep_layout) continue;
                const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
                const auto calls = repl::read_rpc_fields(pc, d.content.data(), end, p);
                if (!calls) continue;
                for (const repl::RpcCall& c : *calls) {
                    if (!c.decoded) continue;
                    pc_calls.push_back(c.rpc->name);
                    if (std::string(c.rpc->name) == "ClientUpdateSkillOnActionbar") {
                        if (const auto u = repl::parse_update_skill_on_actionbar(c)) {
                            skill_updates.push_back(*u);
                            sequence.push_back("U");
                        } else {
                            ++unreadable;
                        }
                    }
                    if (std::string(c.rpc->name) == "ClientReceiveActionbarSlotPayload") {
                        if (const auto payload = repl::parse_actionbar_payload(c)) {
                            bar_payloads.push_back(*payload);
                            for (const repl::ActionbarSlot& slot : payload->slots) {
                                if (slot.use.use_type == repl::ActionbarUseType::None) bar.erase({slot.bank_index, slot.slot_index});
                                else bar[{slot.bank_index, slot.slot_index}] = slot.use;
                            }
                            sequence.push_back("P");
                        } else {
                            ++unreadable;
                        }
                    }
                    if (std::string(c.rpc->name) == "ClientReceiveStorageEvent") {
                        const auto ev = repl::parse_storage_event(c);
                        if (!ev || ev->targets.size() != 1 || ev->targets[0].slots.size() != 1) {
                            ++unreadable;
                            continue;
                        }
                        events.push_back(*ev);
                        const int s = storage_of(guids, ev->targets[0].object);
                        const std::pair<int, int> key{s, ev->targets[0].slots[0].slot};
                        if (ev->type == static_cast<uint8_t>(repl::StorageEventType::Add)) storage[key] = ev->item;
                        else storage.erase(key);
                    } else if (std::string(c.rpc->name) == "ClientReceiveStoragePayload") {
                        absorb_payload(repl::parse_storage_payload(c));
                    }
                }
            } else if (channel == pawn_ch) {
                if (v.header.is_actor) {
                    if (v.header.has_rep_layout) continue;
                    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
                    const auto calls = repl::read_rpc_fields(pawn, d.content.data(), end, p);
                    if (calls) {
                        for (const repl::RpcCall& c : *calls) {
                            if (c.decoded && std::string(c.rpc->name) == "ClientReceiveStoragePayload") absorb_payload(repl::parse_storage_payload(c));
                            if (c.decoded && std::string(c.rpc->name) == "BroadcastPlayLevelUpEffects") {
                                const schema::RepCommand* cmd = repl::rpc_command(*c.rpc, "InLevel");
                                const auto it = cmd ? c.params.values.find(cmd->handle) : c.params.values.end();
                                const int64_t* lv = it != c.params.values.end() ? it->second.get_if<int64_t>() : nullptr;
                                level_ups.push_back(lv ? static_cast<int>(*lv) : -1);
                                const schema::RepCommand* catcmd = repl::rpc_command(*c.rpc, "Category");
                                const auto cit = catcmd ? c.params.values.find(catcmd->handle) : c.params.values.end();
                                const int64_t* cat = cit != c.params.values.end() ? cit->second.get_if<int64_t>() : nullptr;
                                level_up_categories.push_back(cat ? *cat : -1);
                            }
                        }
                    }
                    continue;
                }
                if (v.header.object == guids.ability && ability_class) ability_block(d, v);
                if (v.header.object == guids.stats) {
                    ++stat_blocks;
                    if (stats_class) stats_block(d, v, chain);
                }
                if (v.header.object == guids.base_character_info && info_class) {
                    const size_t before = held_updates.size();
                    held_block(d, v, info_class);
                    if (held_updates.size() > before) held_chains.push_back(chain);
                }
                if (v.header.object == guids.appearance && appearance_class) appearance_block(d, v);
            }
        }
    }

    void absorb_payload(const std::optional<repl::StoragePayload>& payload) {
        if (!payload) {
            ++unreadable;
            return;
        }
        ++payloads;
        const int s = storage_of(guids, payload->target);
        if (s < 0) return;   // a resource storage
        for (auto it = storage.begin(); it != storage.end();) it = it->first.first == s ? storage.erase(it) : std::next(it);
        for (const repl::StorageSlot& slot : payload->storage) {
            if (slot.item.has_item) storage[{s, slot.slot}] = slot.item;
        }
    }
// (Cyb + C O A)

    /// Plan D1: the XPTracker's maps, applied to the client's copy.
    void tracker_block(const fake::Delivered& d, const repl::BlockView& v) {
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        const schema::ClassSchema& cls = tracker.schema();
        if (v.header.has_rep_layout && !repl::read_rep_block(cls, d.content.data(), end, p)) {
            ++unreadable;
            return;
        }
        if (p >= end) return;
        const auto fields = repl::read_fast_array_fields(cls, d.content.data(), end, p);
        if (!fields) {
            ++unreadable;
            return;
        }
        for (const repl::FastArrayField& f : *fields) {
            if (!tracker.apply(f)) ++unreadable;
        }
        ++tracker_updates;
        sequence.push_back("L");
    }

    /// The StatsComponent's fast arrays of a block: the items the client now has, and what this update changed.
    void stats_block(const fake::Delivered& d, const repl::BlockView& v, size_t chain) {
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        repl::CodecOptions opts;
        opts.stat_int32_extended = stat_extended;
        if (v.header.has_rep_layout && !repl::read_rep_block(*stats_class, d.content.data(), end, p, opts)) {
            ++unreadable;
            return;
        }
        if (p >= end) return;
        const auto fields = repl::read_fast_array_fields(*stats_class, d.content.data(), end, p, opts);
        if (!fields) {
            ++unreadable;
            return;
        }
        StatUpdate u;
        u.chain = chain;
        for (const repl::FastArrayField& f : *fields) {
            const std::string name = stats_class->field(f.field)->name;
            if (name == "CurrentEffects") {
                effects_added += f.delta.changed.size();
                effects_removed += f.delta.deleted.size();
                for (const int32_t id : f.delta.deleted) {
                    effect_items.erase(id);
                    sequence.push_back("E-");
                }
                for (const auto& c : f.delta.changed) {
                    EffectSeen e;
                    if (const auto g = c.values.values.find(2); g != c.values.values.end() && g->second.get_if<int64_t>()) e.guid = *g->second.get_if<int64_t>();
                    if (const auto dur = c.values.values.find(4); dur != c.values.values.end() && dur->second.get_if<float>()) e.duration = *dur->second.get_if<float>();
                    effect_items[c.id] = e;
                    sequence.push_back("E+");
                }
            }
            if (name.rfind("StatRepInt32", 0) != 0) continue;
            auto& items = stat_items[name];
            for (const int32_t id : f.delta.deleted) {
                items.erase(id);
                ++u.deleted;
            }
            for (const auto& c : f.delta.changed) {
                const auto it = c.values.values.find(1);
                const auto* s = it != c.values.values.end() ? it->second.get_if<repl::StatInt32Inst>() : nullptr;
                if (!s) {
                    ++unreadable;
                    continue;
                }
                items[c.id] = *s;
                u.changed[name].insert(s->record);
            }
        }
        if (!u.changed.empty() || u.deleted) {
            stat_updates.push_back(std::move(u));
            sequence.push_back("S");
        }
    }
// -- C.Y.B & CoA --

    /// The AbilityComponent's KnownAbilities and ReplicatedRecordsToLoad: an update carries the new count and the elements from the first changed one
    /// (the client resizes in place and keeps the elements an update does not name).
    void ability_block(const fake::Delivered& d, const repl::BlockView& v) {
        if (!v.header.has_rep_layout) return;
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        const auto st = repl::read_rep_block(*ability_class, d.content.data(), end, p);
        if (!st) {
            ++unreadable;
            return;
        }
        bool changed = false;
        const std::pair<const char*, std::vector<int64_t>*> lists[] = {{"KnownAbilities", &known}, {"ReplicatedRecordsToLoad", &records}};
        for (const auto& [name, list] : lists) {
            const repl::Value* val = st->get(*ability_class, name);
            const repl::ArrayValue* a = val ? val->get_if<repl::ArrayValue>() : nullptr;
            if (!a) continue;
            list->resize(a->elements.size());
            for (size_t i = 0; i < a->elements.size(); ++i) {
                const auto it = a->elements[i].values.find(1);
                if (it != a->elements[i].values.end() && it->second.get_if<int64_t>()) (*list)[i] = *it->second.get_if<int64_t>();
            }
            changed = true;
        }
        if (changed) sequence.push_back("A");
    }

    void held_block(const fake::Delivered& d, const repl::BlockView& v, const schema::ClassSchema* cls) {
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        if (v.header.has_rep_layout) {
            const auto st = repl::read_rep_block(*cls, d.content.data(), end, p);
            if (!st) {
                ++unreadable;
                return;
            }
            if (const repl::Value* lv = st->get(*cls, "Level")) {
                if (const int64_t* n = lv->get_if<int64_t>()) level = static_cast<int>(*n);   // plan D1: UPlayerInfo.Level
            }
        }
        if (p >= end) return;
        const auto fields = repl::read_fast_array_fields(*cls, d.content.data(), end, p);
        if (!fields) {
            ++unreadable;
            return;
        }
        for (const repl::FastArrayField& f : *fields) {
            if (std::string(cls->field(f.field)->name) != "HeldItems") continue;
            for (const int32_t id : f.delta.deleted) held.erase(id);
            for (const auto& c : f.delta.changed) {
                const auto key = c.values.values.find(1);
                const auto rec = c.values.values.find(2);
                held[c.id] = {key != c.values.values.end() && key->second.get_if<uint64_t>() ? static_cast<int>(*key->second.get_if<uint64_t>()) : 0,
                              rec != c.values.values.end() && rec->second.get_if<int64_t>() ? *rec->second.get_if<int64_t>() : 0};
            }
            held_key = f.delta.array_key;
            held_updates.emplace_back(f.delta.array_key, static_cast<int>(f.delta.changed.size()));
        }
    }

    void appearance_block(const fake::Delivered& d, const repl::BlockView& v) {
        if (!v.header.has_rep_layout) return;
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        const auto st = repl::read_rep_block(*appearance_class, d.content.data(), end, p);
        if (!st) {
            ++unreadable;
            return;
        }
        const repl::Value* v2 = st->get(*appearance_class, "AppearanceIDs");
        const repl::ArrayValue* a = v2 ? v2->get_if<repl::ArrayValue>() : nullptr;
        if (!a) return;
        ++appearance_updates;
        appearance.resize(a->elements.size());   // the client resizes in place and keeps the elements an update does not name
        for (size_t i = 0; i < a->elements.size(); ++i) {
            const auto it = a->elements[i].values.find(1);
            if (it != a->elements[i].values.end() && it->second.get_if<repl::AppearanceId>()) appearance[i] = *it->second.get_if<repl::AppearanceId>();
        }
    }

    /// The held records by key, 0 for a key with nothing.
    std::map<int, int64_t> held_by_key() const {
        std::map<int, int64_t> out;
        for (const auto& [id, kv] : held) out[kv.first] = kv.second;
        return out;
    }
};

// ---- the harness -----------------------------------------------------------------------------------------------------------------

struct Harness {
    Fixture fx;
    RecordingSink sink;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::vector<size_t> packet_of;      // the server packet each delivered chain came in
    size_t packets = 0;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0;
    Mirror mirror;
    size_t mirrored = 0;                // chains of `got` the mirror has absorbed
// C-Y-B with C-O-A

    explicit Harness(svc::Character record = cyb_record(), bool with_sink = true, bool real_db = false, const char* real_path = nullptr) : fx(real_db, real_path) {
        if (with_sink) fx.world->set_position_sink(&sink);
        s = PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
        mirror.guids = s->bootstrap().setup().guids;
        const repl::Subobject* info = s->bootstrap().pawn().find(mirror.guids.base_character_info);
        const repl::Subobject* app = s->bootstrap().pawn().find(mirror.guids.appearance);
        mirror.info_class = info ? info->obj.schema : nullptr;
        const repl::Subobject* stats = s->bootstrap().pawn().find(mirror.guids.stats);
        mirror.stats_class = stats ? stats->obj.schema : nullptr;
        const repl::Subobject* ability = s->bootstrap().pawn().find(mirror.guids.ability);
        mirror.ability_class = ability ? ability->obj.schema : nullptr;
        {
            const sg::StatBundle* bundle = world_stats::of(*fx.content).bundle;
            mirror.stat_extended = [bundle](int64_t record) {
                const auto it = bundle->by_guid.find(record);
                return it != bundle->by_guid.end() && (bundle->stats[it->second].flags & sg::kStatBuckets) != 0;
            };
        }
        mirror.appearance_class = app ? app->obj.schema : nullptr;
        settle();
    }
    void flush() {
        for (auto& pkt : conn.flush(t)) {
            const auto r = client.receive(pkt);
            ++packets;
            for (const auto& d : r.delivered) {
                got.push_back(d);
                packet_of.push_back(packets);
            }
        }
        for (; mirrored < got.size(); ++mirrored) mirror.absorb(got[mirrored], pc_ch, pawn_ch, got[mirrored].channel);
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        const std::vector<InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const InBunch& b : in) s->on_bunch(conn, b, t);
        s->end_of_datagram(conn);
    }
    void step(milliseconds d = milliseconds(20)) {
        t += d;
        s->tick(conn, t);
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    /// A request is sent, the server's answers collected until things are quiet.
    void request(const BunchWriter& pawn_or_pc, uint32_t channel, bool reliable = true) {
        client_sends({client.actor(channel, pawn_or_pc, reliable)});
        settle();
    }
    void request_both(const BunchWriter& a, const BunchWriter& b) {   // ServerItemUsed and ServerActivateItem in one datagram, as the retail client does
        client_sends({client.actor(pawn_ch, a, true), client.actor(pawn_ch, b, true)});
        settle();
    }
    size_t chains_since(size_t from) const { return got.size() - from; }
};

// ---- the client's requests -----------------------------------------------------------------------------------------------------------

static BunchWriter actor_rpc(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) {
    BunchWriter out;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

static BunchWriter item_use(const char* rpc_name, int storage, int slot, const std::string& guid_hex, uint32_t requested = kAny) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc(rpc_name);
    repl::RpcParams p;
    const auto g = items::guid_of(guid_hex);
    const repl::GuidValue gv = g ? *g : repl::GuidValue{};
    repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(static_cast<uint64_t>(storage)));
    repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(requested)));
    return actor_rpc(pawn, rpc_name, p);
}
static BunchWriter item_used(int storage, int slot, const std::string& guid_hex, uint32_t requested = kAny) { return item_use("ServerItemUsed", storage, slot, guid_hex, requested); }
static BunchWriter item_activate(int storage, int slot, const std::string& guid_hex) { return item_use("ServerActivateItem", storage, slot, guid_hex); }
// C:Y:B : C O A

static BunchWriter storage_request(const repl::ItemStorageEvent& ev) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestStorageEvent");
    const auto params = repl::storage_event_params(*rpc, ev);
    return params ? actor_rpc(pc, "ServerRequestStorageEvent", *params) : BunchWriter();
}

/// The ServerRequestStorageEvent the client sends for a move (type 2) or swap (type 4) inside the bag.
static BunchWriter bag_move(Harness& h, repl::StorageEventType type, int from, int count, const std::string& guid, int to, const std::string& to_guid = {}, int to_count = 1) {
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    repl::ItemStorageEvent ev;
    ev.type = static_cast<uint8_t>(type);
    const auto gv = items::guid_of(guid);
    ev.item.has_item = true;
    ev.item.record_id = fi::kPotion;
    ev.item.quantity = static_cast<uint32_t>(count);
    ev.item.quality = 1;
    if (gv) ev.item.guid = *gv;
    ev.item.creation_time = items::ticks_of_ms(1'771'299'474'000ll);
    ev.move_source.object = g.backpack;
    ev.move_source.slots.push_back({from, count, gv ? *gv : repl::GuidValue{}});
    repl::StorageEventContainer target;
    target.object = g.backpack;
    const auto tg = items::guid_of(to_guid);
    target.slots.push_back({to, to_count, tg ? *tg : repl::GuidValue{}});
    ev.targets.push_back(target);
    ev.instigator = repl::GuidValue{0xbbec5c86u, 0xeb101fe5u, 0xf38c87f0u, 0xfa22e7d0u};
    return storage_request(ev);
}

// ---- reading what the server sent -----------------------------------------------------------------------------------------------------

/// The PlayerController's calls (by name) among the chains `got[from..]`, in order.
static std::vector<std::string> pc_calls_since(const Harness& h, size_t from_chain) {
    std::vector<std::string> out;
    for (size_t i = from_chain; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch) continue;
        const auto blocks = repl::read_content_blocks(h.got[i].content.data(), h.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            size_t p = b.payload_pos;
            const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), h.got[i].content.data(), b.payload_pos + b.payload_bits, p);
            if (calls) for (const auto& c : *calls) out.push_back(c.rpc->name);
        }
    }
    return out;
}

/// What the mirror holds, as an inventory of the items it was told of (bag, equipment).
static bool mirror_matches(const Harness& h) {
    const auto items = h.s->inventory().items();
    size_t n = 0;
    for (const svc::InventoryItem& it : items) {
        if (it.container == ItemContainer::Quest) continue;
        ++n;
        const auto f = h.mirror.storage.find({static_cast<int>(it.container), it.slot});
        if (f == h.mirror.storage.end() || !(f->second == items::to_instance(it))) return false;
    }
    return n == h.mirror.storage.size();
}

static bool gear_matches(const Harness& h) {
    const auto want = items::held_records(h.s->inventory());
    const auto held = h.mirror.held_by_key();
    for (size_t i = 0; i < want.size(); ++i) {
        const auto f = held.find(items::kHeldSlots[i]);
        if (f == held.end() || f->second != want[i]) return false;
    }
    const auto app = items::appearance_ids(h.s->inventory());
    if (app.size() != h.mirror.appearance.size()) return false;
    for (size_t i = 0; i < app.size(); ++i)
        if (!h.mirror.appearance[i] || !(*h.mirror.appearance[i] == app[i])) return false;
    return true;
}

// ============================================================================================================================
//  tests
// ============================================================================================================================

static void test_join() {
    std::printf("the join: the client has the character's storages and gear\n");
    Harness h;
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
    CHECK(h.s->inventory().size() == 17 && h.s->inventory().count(Worn) == 13 && h.s->inventory().count(Bag) == 4, "the session plays 17 items: 13 worn, 4 in the bag");
    CHECK(mirror_matches(h), "the client's storages are the inventory: every item as the instance the server sent, nothing more");
    CHECK(gear_matches(h), "HeldItems and AppearanceIDs show what is worn: the Scepter, Focus and Book, the five tools; the helmet, chest, legs and feet");
    CHECK(h.mirror.held_updates.size() >= 1 && h.mirror.held.size() == 23, "HeldItems arrived with its 23 items");
    CHECK(h.mirror.events.size() == 17 && h.mirror.payloads >= 3, "17 ClientReceiveStorageEvent (an Add for each item) and the storages' payloads");
    bool adds = true;
    for (const auto& e : h.mirror.events) adds = adds && e.type == static_cast<uint8_t>(repl::StorageEventType::Add) && !(e.instigator == repl::GuidValue{});
    CHECK(adds, "all Adds, the character as the instigator");
    CHECK(!h.s->inventory_changed() && !h.s->inventory_dirty() && h.s->item_requests_applied() == 0, "nothing has changed yet");
}
// C.O.A and C_Y_B

static void test_unequip_and_equip_the_chest() {
    std::printf("taking the chest piece off and putting it on again\n");
    Harness h;
    const std::vector<repl::AppearanceId> look = items::appearance_ids(h.s->inventory());
    const size_t from = h.got.size();
    const size_t events_before = h.mirror.events.size();
    const size_t held_before = h.mirror.held_updates.size();
    const int key_before = h.mirror.held_key;
    const size_t stat_updates_before = h.mirror.stat_updates.size();
    // what the retail client sends: both RPCs, in one datagram, for the equipment slot 2
    h.request_both(item_used(4, 2, guid_n(102)), item_activate(4, 2, guid_n(102)));
    CHECK(h.mirror.events.size() == events_before + 2, "two ClientReceiveStorageEvent (ServerActivateItem is not answered a second time)");
    if (h.mirror.events.size() >= events_before + 2) {
        const auto& add = h.mirror.events[events_before];
        const auto& remove = h.mirror.events[events_before + 1];
        const auto& g = h.s->bootstrap().setup().guids;
        CHECK(add.type == 0 && add.targets[0].object == g.backpack && add.targets[0].slots[0].slot == 4 && add.item.record_id == static_cast<uint64_t>(fi::kChest) &&
                  !(add.instigator == repl::GuidValue{}),
              "an Add to the lowest free bag slot (4), the character as the instigator");
        CHECK(remove.type == 1 && remove.targets[0].object == g.equipment && remove.targets[0].slots[0].slot == 2 && remove.instigator == repl::GuidValue{} &&
                  items::guid_hex(remove.item.guid) == guid_n(102),
              "then a Remove from Chest, with no instigator: as the retail server's");
    }
    CHECK(h.mirror.held_updates.size() == held_before + 1 && h.mirror.held_updates.back().second == 23 && h.mirror.held_key == key_before + 23,
          "one HeldItems update with all 23 items and the array key 23 higher");
    CHECK(h.mirror.appearance_updates >= 1 && h.mirror.appearance.size() == 3, "AppearanceIDs now lists three pieces: the chest is gone from the look");
    CHECK(h.mirror.stat_updates.size() == stat_updates_before + 1, "the stats the chest piece changes are sent: one update of the StatsComponent (plan B3)");
    // the events and the pawn's update (gear, look and stats) left in the same server packet
    size_t pc_packet = 0, pawn_packet = 0;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel == h.pc_ch && h.got[i].reliable) pc_packet = h.packet_of[i];
        if (h.got[i].channel == h.pawn_ch && h.got[i].reliable) pawn_packet = h.packet_of[i];
    }
    // (with the stats' deltas the pawn's chain is 4,045 bits and the events' 3,930: together they no longer fit one 1,028-byte packet, so the
    // update follows the events in the next packet of the same flush; retail's chest piece changes 46 stats, which no single packet holds either)
    CHECK(pc_packet != 0 && pawn_packet >= pc_packet && pawn_packet <= pc_packet + 1, "the storage events and the pawn's update leave in the same flush: one packet or two in a row");
    CHECK(!h.mirror.stat_updates.empty() && !h.mirror.held_chains.empty() && h.mirror.stat_updates.back().chain == h.mirror.held_chains.back(),
          "...and the stats' deltas are in the one chain that carries the HeldItems and AppearanceIDs"); 
    CHECK(mirror_matches(h) && gear_matches(h), "the client's view is the inventory's, storages and gear");
    CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->item_record_id == fi::kChest && !h.s->inventory().at(Worn, 2), "the chest is in the bag");
    CHECK(h.s->inventory_dirty() && h.s->inventory_changed() && h.s->item_requests_applied() == 1, "the inventory is marked changed");

    // and on again: from bag slot 4 into Chest; the look is the one it started with
    h.request_both(item_used(3, 4, guid_n(102)), item_activate(3, 4, guid_n(102)));
    CHECK(h.s->inventory().at(Worn, 2) && !h.s->inventory().at(Bag, 4), "the chest is worn again, in Chest");
    CHECK(mirror_matches(h) && gear_matches(h), "the client's view is the inventory's again");
    bool same_look = h.mirror.appearance.size() == look.size();
    for (size_t i = 0; same_look && i < look.size(); ++i) same_look = h.mirror.appearance[i] && *h.mirror.appearance[i] == look[i];
    CHECK(same_look && h.mirror.held_key == key_before + 46, "AppearanceIDs are the original four again, and the HeldItems key moved 23 more");
}

static void test_two_hander_and_slots() {
    std::printf("a two-hander over a main hand and an off hand; rings\n");
    Harness h;
    const size_t events_before = h.mirror.events.size();
    const int key_before = h.mirror.held_key;
    // the Bard wears Scepter (Primary), Focus (Secondary) and Book (Ranged); the Sword2H in bag slot 0 takes Primary and Secondary
    h.request(item_used(3, 0, guid_n(200)), h.pawn_ch);
    CHECK(h.mirror.events.size() == events_before + 6, "six events: the new item out of the bag, and the Scepter and the Focus each added to the bag and taken off");
    CHECK(h.s->inventory().at(Worn, 15) && h.s->inventory().at(Worn, 15)->item_record_id == fi::kSword2H && !h.s->inventory().at(Worn, 16) && h.s->inventory().at(Worn, 17),
          "the sword is in Primary, Secondary is empty, the Book stays");
    CHECK(h.mirror.held_key == key_before + 69, "the HeldItems key moved by 69: three changes of the Equipment storage");
    CHECK(mirror_matches(h) && gear_matches(h), "the client's view is the inventory's");
    CHECK(h.mirror.held_by_key()[15] == fi::kSword2H && h.mirror.held_by_key()[16] == 0, "HeldItems: the sword in Primary, nothing in Secondary");
    // a ring, then the same ring (a second item) into the other hand
    h.request(item_used(3, 2, guid_n(202)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 10) && h.s->inventory().at(Worn, 10)->instance_guid == guid_n(202), "a ring goes to RingRight");
    CHECK(mirror_matches(h) && gear_matches(h), "and the client agrees; a ring changes no AppearanceIDs");
}

static void test_moves_and_swaps() {
    std::printf("moving and swapping inside the bag\n");
    Harness h;
    const size_t events_before = h.mirror.events.size();
    const size_t held_updates = h.mirror.held_updates.size();
    // the potion stack of 5 in slot 1 to slot 32
    h.request(bag_move(h, repl::StorageEventType::Move, 1, 5, guid_n(201), 32), h.pc_ch);
    CHECK(h.mirror.events.size() == events_before + 2 && h.mirror.events[events_before].type == 1 && h.mirror.events[events_before + 1].type == 0 &&
              h.mirror.events[events_before + 1].targets[0].slots[0].slot == 32 && h.mirror.events[events_before + 1].targets[0].slots[0].count == 5,
          "a move is a Remove and an Add, the stack's 5 kept");
    bool both_instigated = !(h.mirror.events[events_before].instigator == repl::GuidValue{}) && !(h.mirror.events[events_before + 1].instigator == repl::GuidValue{});
    CHECK(both_instigated, "both events carry the character as instigator, as the retail moves'");
    CHECK(h.s->inventory().at(Bag, 32) && !h.s->inventory().at(Bag, 1) && mirror_matches(h), "the item is in slot 32 and the client agrees");
    CHECK(h.mirror.held_updates.size() == held_updates, "the pawn's gear is not touched by a move in the bag");
    // swap slot 32 (the potions) with slot 3 (the heavy chest piece)
    const size_t before_swap = h.mirror.events.size();
    h.request(bag_move(h, repl::StorageEventType::Swap, 32, 5, guid_n(201), 3, guid_n(203), 1), h.pc_ch);
    CHECK(h.mirror.events.size() == before_swap + 4, "a swap is four events");
    const auto types = [&] { std::vector<int> v; for (size_t i = before_swap; i < h.mirror.events.size(); ++i) v.push_back(h.mirror.events[i].type); return v; }();
    CHECK(types == std::vector<int>({1, 1, 0, 0}), "Remove, Remove, Add, Add");
    CHECK(h.s->inventory().at(Bag, 3) && h.s->inventory().at(Bag, 3)->instance_guid == guid_n(201) && h.s->inventory().at(Bag, 32)->instance_guid == guid_n(203) && mirror_matches(h),
          "the two changed places and the client agrees");
}


// ---- general requests (a move or swap with every field in the test's hands) -----------------------------------------------------------

struct MoveSpec {
    uint8_t type = static_cast<uint8_t>(repl::StorageEventType::Move);
    emit::FIntrepidNetworkGUID from_object, to_object;
    int from_slot = 1, from_count = 5, to_slot = 32, to_count = 1;
    std::string source_guid = guid_n(201);   // what the source slot of the event says
    std::string event_guid = guid_n(201);    // the item of the event
    std::string target_guid;                 // what the client says is in the target slot
    bool has_source = true;
    size_t targets = 1;
};

/// A valid move of the potion stack in bag slot 1 to the empty slot 32.
static MoveSpec potion_move(const Harness& h) {
    MoveSpec m;
    m.from_object = h.s->bootstrap().setup().guids.backpack;
    m.to_object = m.from_object;
    return m;
}

static BunchWriter storage_move(const MoveSpec& m) {
    repl::ItemStorageEvent ev;
    ev.type = m.type;
    const auto eg = items::guid_of(m.event_guid);
    ev.item.has_item = true;
    ev.item.record_id = static_cast<uint64_t>(fi::kPotion);
    ev.item.quantity = static_cast<uint32_t>(std::max(1, m.from_count));
    ev.item.quality = 1;
    if (eg) ev.item.guid = *eg;
    ev.item.creation_time = items::ticks_of_ms(1'771'299'474'000ll);
    ev.move_source.object = m.from_object;
    const auto sg = items::guid_of(m.source_guid);
    if (m.has_source) ev.move_source.slots.push_back({m.from_slot, m.from_count, sg ? *sg : repl::GuidValue{}});
    const auto tg = items::guid_of(m.target_guid);
    for (size_t i = 0; i < m.targets; ++i) {
        repl::StorageEventContainer t;
        t.object = m.to_object;
        t.slots.push_back({m.to_slot + static_cast<int>(i), m.to_count, tg ? *tg : repl::GuidValue{}});
        ev.targets.push_back(std::move(t));
    }
    ev.instigator = repl::GuidValue{0xbbec5c86u, 0xeb101fe5u, 0xf38c87f0u, 0xfa22e7d0u};
    return storage_request(ev);
}

/// A request is sent in its own moment (the budget is full again) and must be refused: nothing in the inventory changes, no event is sent,
/// the refusal is counted.  Returns whether it was.
static bool refused_cleanly(Harness& h, const items::Inventory& before, const char* what, const BunchWriter& bunch, uint32_t channel) {
    h.step(milliseconds(150));
    const size_t refused = h.s->item_requests_refused(), applied = h.s->item_requests_applied(), events = h.mirror.events.size();
    h.request(bunch, channel);
    const bool ok = bunch.bit_pos() > 0 && h.s->item_requests_refused() == refused + 1 && h.s->item_requests_applied() == applied &&
                    h.mirror.events.size() == events && h.s->inventory() == before && !h.s->inventory_dirty() && !h.s->inventory_changed();
    if (!ok) std::printf("    not refused cleanly: %s\n", what);
    return ok;
}

static svc::Character cyb_with_a_full_bag() {
    svc::Character c = cyb_record();
    std::vector<svc::InventoryItem> items;
    for (const auto& it : c.items)
        if (it.container != Bag) items.push_back(it);
    items.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    for (int slot = 1; slot < 70; ++slot) items.push_back(make_item(Bag, slot, fi::kPotion, 300 + slot, 1 + slot % 7));
    c.items = items;
    return c;
}
// built with C O A and C Y B

static void test_refusals() {
    std::printf("requests that are refused change nothing\n");
    Harness h;
    const items::Inventory before = h.s->inventory();
    const size_t payloads_before = h.mirror.payloads, held_before = h.mirror.held_updates.size(), stats_before = h.mirror.stat_blocks;
    int bad = 0, n = 0;
    const auto use = [&](const char* what, int storage, int slot, const std::string& guid, uint32_t requested = kAny) {
        ++n;
        bad += refused_cleanly(h, before, what, item_used(storage, slot, guid, requested), h.pawn_ch) ? 0 : 1;
    };
    const auto move = [&](const char* what, const MoveSpec& m) {
        ++n;
        bad += refused_cleanly(h, before, what, storage_move(m), h.pc_ch) ? 0 : 1;
    };
    use("an empty bag slot", 3, 50, guid_n(900));
    use("a bag slot with another instance's guid", 3, 0, guid_n(999));
    use("the zero guid", 3, 0, "00000000000000000000000000000000");
    use("a potion (not wearable)", 3, 1, guid_n(201));
    use("the quest storage", 18, 0, guid_n(900));
    use("storage 0", 0, 0, guid_n(200));
    use("storage 255", 255, 0, guid_n(200));
    use("the vanity storage", 5, 0, guid_n(200));
    use("bag slot -1", 3, -1, guid_n(200));
    use("bag slot 70", 3, 70, guid_n(200));
    use("bag slot INT32_MAX", 3, INT32_MAX, guid_n(200));
    use("bag slot INT32_MIN", 3, INT32_MIN, guid_n(200));
    use("equipment slot 83", 4, 83, guid_n(102));
    use("equipment slot 2 with a bag item's guid", 4, 2, guid_n(200));
    use("an empty equipment slot", 4, 3, guid_n(102));
    use("a slot the item cannot be worn in (the two-hander into Chest)", 3, 0, guid_n(200), 2);
    use("requested slot 0", 3, 0, guid_n(200), 0);
    use("requested slot INT32_MAX", 3, 0, guid_n(200), 0x7FFFFFFFu);
    {
        MoveSpec m = potion_move(h);
        m.to_slot = 2;   // the ring
        move("a Move onto an occupied slot", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 0;
        move("an Add", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 1;
        move("a Remove", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 3;
        move("a Fill", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 4;   // a swap onto an empty slot
        move("a Swap onto an empty slot", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_count = 2;
        move("a partial stack (a split)", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_count = 9;
        move("more than the stack holds", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.source_guid = m.event_guid = guid_n(998);
        move("an instance that is not in the source slot", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.event_guid = guid_n(200);
        move("an event whose item is not the source slot's", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_slot = 60;
        move("an empty source slot", m);
    }
    for (const int slot : {-1, 70, 71, INT32_MAX, INT32_MIN}) {
        MoveSpec m = potion_move(h);
        m.to_slot = slot;
        move("a target slot outside the bag", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.to_object = h.s->bootstrap().setup().guids.equipment;
        move("a move into the Equipment storage", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_object = h.s->bootstrap().setup().guids.equipment;
        m.from_slot = 2;
        m.source_guid = m.event_guid = guid_n(102);
        m.from_count = 1;
        move("a move out of the Equipment storage", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_object = m.to_object = h.s->bootstrap().setup().guids.quest_storage;
        move("a move in the quest storage", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.to_object = emit::FIntrepidNetworkGUID{777777, 60, 5};
        move("a container that is no storage of the character", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_object = emit::FIntrepidNetworkGUID{};
        move("a source container that is the null object", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.has_source = false;
        move("a move with no source slot", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.targets = 2;
        move("a move with two targets", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.targets = 0;
        move("a move with no target", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 4;
        m.to_slot = 3;
        m.target_guid = guid_n(998);   // the client's idea of the target slot's item is wrong
        m.to_count = 1;
        move("a Swap with a wrong guid for the target", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 4;
        m.to_slot = 3;
        m.target_guid = guid_n(203);
        m.to_count = 4;
        move("a Swap with a wrong count for the target", m);
    }
    CHECK(bad == 0, (std::to_string(n) + " requests of every kind: each is refused, counted, sends no storage event and leaves the inventory and the saving flags as they were").c_str());
    CHECK(h.mirror.held_updates.size() == held_before && h.mirror.stat_blocks == stats_before, "the pawn's gear and stats are not touched by refusals");
    CHECK(h.mirror.payloads > payloads_before, "the client's storages were sent again (the correction)");
    CHECK(mirror_matches(h) && gear_matches(h) && h.mirror.unreadable == 0 && !h.s->failed(), "and the client's view is still the inventory's; the session is up");
    h.s->finish("test");
    CHECK(h.sink.inventories.empty(), "a session whose requests were all refused saves no inventory");
}
// C | O | A : Cyb

static void test_refusals_that_need_a_state() {
    std::printf("requests that depend on the inventory\n");
    {
        // a full bag: taking a piece off has nowhere to go, and a two-hander displaces two items for the one place it frees
        Harness h(cyb_with_a_full_bag());
        const items::Inventory before = h.s->inventory();
        CHECK(h.s->inventory().count(Bag) == 70 && h.s->inventory().first_free(Bag) == -1, "(the bag is full)");
        int bad = 0;
        bad += refused_cleanly(h, before, "unequip into a full bag", item_used(4, 2, guid_n(102)), h.pawn_ch) ? 0 : 1;
        bad += refused_cleanly(h, before, "a two-hander displacing two items", item_used(3, 0, guid_n(200)), h.pawn_ch) ? 0 : 1;
        CHECK(bad == 0 && mirror_matches(h) && gear_matches(h), "a full bag: both are refused, nothing changes, the client is told where everything is");
    }
    {
        // an item the catalog does not know
        svc::Character c = cyb_record();
        c.items.push_back(make_item(Bag, 10, 123456789ll, 400));
        Harness h(c);
        const items::Inventory before = h.s->inventory();
        CHECK(refused_cleanly(h, before, "an item of an unknown record", item_used(3, 10, guid_n(400)), h.pawn_ch), "an item whose record content.db does not know is not worn");
    }
}

static void test_budgets() {
    std::printf("the request budgets\n");
    {
        // twelve refused requests in one datagram: eight are looked at, four dropped; four are corrected
        Harness h;
        const size_t payloads = h.mirror.payloads;
        std::vector<fake::Bunch> bunches;
        for (int i = 0; i < 12; ++i) bunches.push_back(h.client.actor(h.pawn_ch, item_used(3, 50, guid_n(900 + i)), true));
        h.client_sends(std::move(bunches));
        h.settle();
        CHECK(h.s->item_requests_refused() == 8 && h.s->requests_dropped() == 4, "12 requests in an instant: 8 refused, 4 dropped for the budget and counted");
        CHECK(h.mirror.payloads == payloads + 12, "and the client was corrected four times (a Backpack, an Equipment and, since Gathering stage 3, a Resource0 payload each): the resync budget");
        // the correction budget fills again: one per two seconds
        h.step(milliseconds(2100));
        h.request(item_used(3, 50, guid_n(900)), h.pawn_ch);
        CHECK(h.mirror.payloads == payloads + 15 && h.s->item_requests_refused() == 9, "two seconds later a refusal is corrected again, once");
        h.request(item_used(3, 50, guid_n(901)), h.pawn_ch);
        CHECK(h.mirror.payloads == payloads + 15 && h.s->item_requests_refused() == 10, "(and not twice in a row)");
        CHECK(mirror_matches(h) && gear_matches(h), "the client ends up with the inventory");
    }
    {
        // the same request twelve times: the first is applied, the others find the item gone, past eight they are dropped
        Harness h;
        std::vector<fake::Bunch> bunches;
        for (int i = 0; i < 12; ++i) bunches.push_back(h.client.actor(h.pawn_ch, item_used(4, 2, guid_n(102)), true));
        h.client_sends(std::move(bunches));
        h.settle();
        CHECK(h.s->item_requests_applied() == 1 && h.s->item_requests_refused() == 7 && h.s->requests_dropped() == 4,
              "the chest taken off twelve times at once: applied once, refused seven times (it is not there), four dropped");
        CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->item_record_id == fi::kChest && !h.s->inventory().at(Worn, 2) && h.s->inventory().count(Bag) == 5,
              "the inventory holds the chest in the bag once");
        CHECK(mirror_matches(h) && gear_matches(h) && h.mirror.unreadable == 0, "and the client agrees");
        // a second later the budget is back
        h.step(seconds(1));
        h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);
        CHECK(h.s->item_requests_applied() == 2 && h.s->inventory().at(Worn, 2) && mirror_matches(h) && gear_matches(h), "a second later the chest goes on again");
    }
}

static void test_activate_alone() {
    std::printf("ServerActivateItem\n");
    Harness h;
    const items::Inventory before = h.s->inventory();
    const size_t events = h.mirror.events.size();
    h.request(item_activate(4, 2, guid_n(102)), h.pawn_ch);
    h.request(item_activate(3, 0, guid_n(200)), h.pawn_ch);
    CHECK(h.mirror.events.size() == events && h.s->inventory() == before && h.s->item_requests_applied() == 0 && h.s->item_requests_refused() == 0 && !h.s->failed(),
          "on its own it changes nothing, sends nothing and is not counted: ServerItemUsed carries the request");
    // the pair, in the order the retail client sends it, applied once; the order swapped, the same
    h.request_both(item_activate(4, 2, guid_n(102)), item_used(4, 2, guid_n(102)));
    CHECK(h.s->item_requests_applied() == 1 && h.mirror.events.size() == events + 2 && h.s->inventory().at(Bag, 4), "the pair, activate first, is one unequip: two events");
}

static void test_wire_forms() {
    std::printf("what is on the wire\n");
    Harness h;
    // the request builders write what the retail client wrote: ServerItemUsed's 201 bits for the chest, from a capture's guid
    const BunchWriter used = item_used(4, 2, "b7a32f5ffa5b7f8e82bef4bc" "4ca1d7a9");   // (not a guid of this character: only the size matters)
    CHECK(used.bit_pos() > 0, "ServerItemUsed writes");
    const BunchWriter sr = bag_move(h, repl::StorageEventType::Move, 1, 5, guid_n(201), 32);
    CHECK(sr.bit_pos() > 0, "ServerRequestStorageEvent writes");
    // an event read back from the client's own bunch is the one that was written
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const auto parsed = repl::read_client_bunch(pc, [](const emit::FIntrepidNetworkGUID&) -> const schema::ClassSchema* { return nullptr; }, sr.data(), sr.bit_pos());
    const bool found = parsed && parsed->rpcs.size() == 1 && parsed->rpcs[0].call.decoded && parsed->rpcs[0].call.rpc && std::string(parsed->rpcs[0].call.rpc->name) == "ServerRequestStorageEvent";
    CHECK(found, "the client's bunch reads as one ServerRequestStorageEvent, decoded to its last bit");
    if (found) {
        const auto ev = repl::parse_storage_event(parsed->rpcs[0].call);
        CHECK(ev && ev->type == 2 && ev->move_source.slots.size() == 1 && ev->move_source.slots[0].slot == 1 && ev->move_source.slots[0].count == 5 && ev->targets.size() == 1 &&
                  ev->targets[0].slots[0].slot == 32,
              "a Move from slot 1 (5 items) to slot 32");
    }
}

// ---- hostile requests ------------------------------------------------------------------------------------------------------------

static BunchWriter with_bits(const BunchWriter& src, size_t bits) {
    BunchWriter w;
    if (bits > 0) w.write_bit_range(src.data(), 0, std::min(bits, src.bit_pos()));
    return w;
}

static BunchWriter flipped_bits(const BunchWriter& src, const std::vector<size_t>& at) {
    std::vector<uint8_t> bytes(src.data(), src.data() + src.byte_size());
    for (const size_t b : at)
        if (b / 8 < bytes.size()) bytes[b / 8] ^= static_cast<uint8_t>(1u << (b % 8));
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, src.bit_pos());
    return w;
}
// built with C.Y.B and C O A

static bool inventory_is_sane(const items::Inventory& inv, const std::set<std::string>& guids) {
    std::string error;
    const auto again = items::Inventory::from_items(inv.items(), error);
    if (!again || !(*again == inv)) return false;
    std::set<std::string> now;
    for (const auto& it : inv.items()) now.insert(it.instance_guid);
    return now == guids && inv.size() == guids.size();
}

static void test_hostile() {
    std::printf("hostile requests\n");
    Harness h;
    std::mt19937_64 rng(0xB2B2B2B2ull);
    std::set<std::string> guids;
    for (const auto& it : h.s->inventory().items()) guids.insert(it.instance_guid);
    const auto pick = [&](uint64_t n) { return static_cast<size_t>(rng() % n); };
    const int64_t extremes[] = {0, 1, -1, 2, 3, 69, 70, 71, 82, 83, 99, 100, 255, 256, 65535, INT32_MAX, INT32_MIN, INT32_MAX - 1};
    const auto number = [&]() -> int {
        if (pick(3) == 0) return static_cast<int>(extremes[pick(sizeof extremes / sizeof extremes[0])]);
        return static_cast<int>(pick(90));
    };
    const auto some_guid = [&](const std::vector<svc::InventoryItem>& items) -> std::string {
        if (!items.empty() && pick(3) != 0) return items[pick(items.size())].instance_guid;
        return guid_n(static_cast<uint32_t>(900 + pick(40)));
    };
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    size_t applied_by_use = 0, applied_by_move = 0;
    const size_t kRequests = 2500;
    bool sane = true;
    for (size_t i = 0; i < kRequests; ++i) {
        h.step(milliseconds(120));   // inside the budget: ten a second
        const auto items = h.s->inventory().items();
        const size_t applied = h.s->item_requests_applied();
        switch (pick(5)) {
            case 0:
            case 1: {   // ServerItemUsed: a real item (the right slot and guid, most times) or numbers
                const svc::InventoryItem* it = items.empty() ? nullptr : &items[pick(items.size())];
                const bool real = it && pick(4) != 0;
                const int storage = real ? (it->container == Bag ? 3 : it->container == Worn ? 4 : 18) : static_cast<int>(pick(4) == 0 ? number() & 0xFF : (pick(2) ? 3 : 4));
                const int slot = real ? it->slot : number();
                const std::string guid = real && pick(5) != 0 ? it->instance_guid : some_guid(items);
                const uint32_t requested = pick(3) == 0 ? static_cast<uint32_t>(number()) : kAny;
                h.client_sends({h.client.actor(h.pawn_ch, item_used(storage, slot, guid, requested), true)});
                if (h.s->item_requests_applied() > applied) ++applied_by_use;
                break;
            }
            case 2:
            case 3: {   // ServerRequestStorageEvent between the bag's slots, valid or not
                MoveSpec m;
                m.from_object = m.to_object = g.backpack;
                if (pick(10) == 0) m.to_object = pick(2) ? g.equipment : g.quest_storage;
                if (pick(12) == 0) m.from_object = g.equipment;
                m.type = pick(3) == 0 ? static_cast<uint8_t>(pick(8)) : static_cast<uint8_t>(pick(2) ? 2 : 4);
                const svc::InventoryItem* it = items.empty() ? nullptr : &items[pick(items.size())];
                const bool real = it && pick(4) != 0;
                m.from_slot = real ? it->slot : number();
                m.from_count = real && pick(5) != 0 ? it->quantity : number();
                m.source_guid = m.event_guid = real && pick(6) != 0 ? it->instance_guid : some_guid(items);
                if (pick(10) == 0) m.event_guid = some_guid(items);
                m.to_slot = pick(2) ? static_cast<int>(pick(40)) : number();
                const svc::InventoryItem* other = items.empty() ? nullptr : &items[pick(items.size())];
                if (other && pick(2)) {
                    m.to_slot = other->slot;
                    m.target_guid = pick(4) ? other->instance_guid : some_guid(items);
                    m.to_count = pick(4) ? other->quantity : number();
                }
                m.has_source = pick(20) != 0;
                m.targets = pick(25) == 0 ? pick(3) : 1;
                h.client_sends({h.client.actor(h.pc_ch, storage_move(m), true)});
                if (h.s->item_requests_applied() > applied) ++applied_by_move;
                break;
            }
            default: {   // damaged requests: a valid one cut short or with bits flipped, and noise
                BunchWriter base;
                switch (pick(3)) {
                    case 0: base = item_used(pick(2) ? 3 : 4, static_cast<int>(pick(4)), some_guid(items)); break;
                    case 1: base = item_activate(pick(2) ? 3 : 4, static_cast<int>(pick(4)), some_guid(items)); break;
                    default: base = bag_move(h, repl::StorageEventType::Move, 1, 5, guid_n(201), static_cast<int>(pick(40))); break;
                }
                const uint32_t channel = base.bit_pos() > 0 && pick(2) ? h.pawn_ch : h.pc_ch;
                BunchWriter bad;
                switch (pick(3)) {
                    case 0: bad = with_bits(base, pick(base.bit_pos() + 1)); break;
                    case 1: {
                        std::vector<size_t> at;
                        for (size_t k = 0, n = 1 + pick(3); k < n; ++k) at.push_back(pick(std::max<size_t>(1, base.bit_pos())));
                        bad = flipped_bits(base, at);
                        break;
                    }
                    default: {
                        for (size_t k = 0, n = pick(300); k < n; ++k) bad.write_bit(pick(2) != 0);
                        break;
                    }
                }
                h.client_sends({h.client.actor(channel, bad, pick(2) != 0)});
                break;
            }
        }
        if (i % 50 == 49) {
            h.settle();
            sane = sane && inventory_is_sane(h.s->inventory(), guids) && !h.s->failed();
        }
    }
    h.settle();
    sane = sane && inventory_is_sane(h.s->inventory(), guids);
    std::printf("    %zu requests: %zu applied (%zu ServerItemUsed, %zu storage events), %zu refused, %zu dropped for the budget\n", kRequests, h.s->item_requests_applied(),
                applied_by_use, applied_by_move, h.s->item_requests_refused(), h.s->requests_dropped());
    CHECK(!h.s->failed() && sane, "2500 requests, valid, wrong and damaged: the session is up and the inventory is still a valid one with the same 17 items, each in one slot");
    CHECK(h.s->item_requests_applied() > 50 && applied_by_use > 10 && applied_by_move > 10, "(the run did apply requests of both kinds)");
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
    // the client was told of every change by events: no resend of the storages is needed to agree (a resync may have come, which agrees too)
    CHECK(mirror_matches(h), "the client's storages equal the inventory");
    CHECK(gear_matches(h), "and its HeldItems and AppearanceIDs equal what is worn");
}

// ---- saving ---------------------------------------------------------------------------------------------------------------------
// C.O.A + C Y B

static bool same_items(std::vector<svc::InventoryItem> a, std::vector<svc::InventoryItem> b) {
    const auto less = [](const svc::InventoryItem& x, const svc::InventoryItem& y) {
        return std::make_pair(static_cast<int>(x.container), x.slot) < std::make_pair(static_cast<int>(y.container), y.slot);
    };
    std::sort(a.begin(), a.end(), less);
    std::sort(b.begin(), b.end(), less);
    return a == b;
}

static void test_saving_in_the_session() {
    std::printf("saving the inventory, in the session\n");
    {
        Harness h;
        h.s->finish("test");
        CHECK(h.sink.inventories.empty(), "a session that never changed an item saves no inventory at its end");
    }
    {
        Harness h;
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        h.s->finish("the client quit");
        CHECK(h.sink.inventories.size() == 1 && h.sink.inventories[0].id == kCyb && h.s->inventories_handed_over() == 1, "the end of a session that changed an item hands the inventory over, with the character's id");
        if (!h.sink.inventories.empty()) {
            CHECK(same_items(h.sink.inventories[0].items, h.s->inventory().items()) && h.sink.inventories[0].items.size() == 17, "it is the whole inventory as the session has it: 17 items");
            const auto& items = h.sink.inventories[0].items;
            bool chest_in_bag = false;
            for (const auto& it : items) chest_in_bag = chest_in_bag || (it.item_record_id == fi::kChest && it.container == Bag && it.slot == 4 && it.instance_guid == guid_n(102));
            CHECK(chest_in_bag, "the chest is in bag slot 4 (the same instance: its guid is kept)");
        }
        h.s->finish("again");
        CHECK(h.sink.inventories.size() == 1, "finish is once");
        CHECK(h.sink.positions.empty(), "(no position was saved: the character did not move)");
    }
    {
        // a change and its undoing: the inventory is as it was, but the end still hands it over (the saver may have lost a periodic save)
        Harness h;
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        h.step(milliseconds(150));
        h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);
        CHECK(h.s->inventory() == items::Inventory::from_items(cyb_record().items, h.error).value(), "(the chest is worn again: the inventory is the one it started as)");
        h.s->finish("end");
        CHECK(h.sink.inventories.size() == 1, "it is still handed over: the character service may hold the intermediate state of a lost periodic save");
    }
    {
        // the 60 s tick: it saves a changed inventory, and not the same one twice
        Harness h;
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        CHECK(h.sink.inventories.empty(), "(a change does not save at once: the saving is the interval's)");
        h.step(seconds(30));
        CHECK(h.sink.inventories.empty(), "30 s on: nothing is saved yet");
        h.step(seconds(31));
        CHECK(h.sink.inventories.size() == 1 && !h.s->inventory_dirty() && h.s->inventory_changed(), "after 60 s the changed inventory is handed over");
        h.step(seconds(61));
        CHECK(h.sink.inventories.size() == 1, "60 s later, with nothing changed since: it is not saved again");
        h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);
        h.step(seconds(61));
        CHECK(h.sink.inventories.size() == 2 && same_items(h.sink.inventories[1].items, h.s->inventory().items()) && h.s->inventory().at(Worn, 2),
              "a change after that is handed over at the next interval, as the session has it now");
        h.s->finish("end");
        CHECK(h.sink.inventories.size() == 3, "and the end hands it over once more: it changed this session");
        h.step(seconds(61));
        CHECK(h.sink.inventories.size() == 3, "after finish the session saves no more");
    }
    {
        // no sink: requests are served, nothing is handed over, nothing breaks
        Harness h(cyb_record(), /*with_sink=*/false);
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        h.step(seconds(61));
        h.s->finish("end");
        CHECK(h.s->inventories_handed_over() == 0 && h.s->item_requests_applied() == 1 && !h.s->failed() && mirror_matches(h), "without a PositionSink nothing is handed over");
    }
    {
        // moves are saved as positions and items as inventories: the two do not depend on each other
        Harness h;
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        h.s->finish("end");
        CHECK(h.sink.positions.empty() && h.sink.inventories.size() == 1, "an inventory is saved without a position, and (movement tests) a position without an inventory");
    }
}

// ---- the saver ---------------------------------------------------------------------------------------------------------------------

struct FakeCharacters : svc::CharacterApi {
    struct Inv {
        std::string id;
        std::vector<svc::InventoryItem> items;
    };
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<std::string, svc::Position>> positions;   // the position calls that were answered Ok
    std::vector<Inv> inventories;                                   // the inventory calls that were answered Ok
    size_t position_calls = 0, inventory_calls = 0;
    std::deque<svc::CallStatus> position_script, inventory_script;
    bool block = false;
    milliseconds max_block{2000};
    int running = 0;

    std::vector<std::pair<std::string, svc::Progress>> progress;    // plan D1: the progress calls answered Ok
    svc::CharacterResult get_character(const std::string&) override { return {}; }
    svc::CallStatus save_progress(const std::string& id, const svc::Progress& p) override {
        std::lock_guard<std::mutex> lock(mu);
        progress.emplace_back(id, p);
        cv.notify_all();
        return svc::CallStatus::Ok;
    }
    size_t progress_count() { std::lock_guard<std::mutex> l(mu); return progress.size(); }
    svc::CallStatus save_position(const std::string& id, const svc::Position& p) override {
        std::unique_lock<std::mutex> lock(mu);
        ++position_calls;
        svc::CallStatus status = svc::CallStatus::Ok;
        if (!position_script.empty()) {
            status = position_script.front();
            position_script.pop_front();
        }
        if (status == svc::CallStatus::Ok) positions.emplace_back(id, p);
        cv.notify_all();
        return status;
    }
    svc::CallStatus save_inventory(const std::string& id, const std::vector<svc::InventoryItem>& items) override {
        std::unique_lock<std::mutex> lock(mu);
        ++inventory_calls;
        ++running;
        cv.notify_all();
        cv.wait_for(lock, max_block, [this] { return !block; });
        --running;
        svc::CallStatus status = svc::CallStatus::Ok;
        if (!inventory_script.empty()) {
            status = inventory_script.front();
            inventory_script.pop_front();
        }
        if (status == svc::CallStatus::Ok) inventories.push_back({id, items});
        cv.notify_all();
        return status;
    }
    void hold() { std::lock_guard<std::mutex> l(mu); block = true; }
    void release() {
        { std::lock_guard<std::mutex> l(mu); block = false; }
        cv.notify_all();
    }
    bool in_flight() { std::lock_guard<std::mutex> l(mu); return running > 0; }
    size_t inventory_count() { std::lock_guard<std::mutex> l(mu); return inventories.size(); }
    size_t position_count() { std::lock_guard<std::mutex> l(mu); return positions.size(); }
    size_t inventory_call_count() { std::lock_guard<std::mutex> l(mu); return inventory_calls; }
    size_t position_call_count() { std::lock_guard<std::mutex> l(mu); return position_calls; }
    std::vector<Inv> saved_inventories() { std::lock_guard<std::mutex> l(mu); return inventories; }
};

static svc::Position pos_of(double x) { return {x, 0, 0, 0}; }
// CoA - C-Y-B

static CharacterSaver::Config quick(milliseconds destructor_wait = milliseconds(2000)) {
    CharacterSaver::Config c;
    c.retry_delay = milliseconds(5);
    c.destructor_wait = destructor_wait;
    return c;
}

static std::vector<svc::InventoryItem> first_items(size_t n) {
    auto all = bard_items();
    all.resize(std::min(n, all.size()));
    return all;
}

static void test_saver_inventories() {
    std::printf("CharacterSaver: inventories\n");
    {
        FakeCharacters api;
        CharacterSaver saver(api, quick());
        saver.save_inventory(kCyb, bard_items());
        CHECK(saver.flush(seconds(3)), "an inventory is saved; flush says everything is");
        const auto s = api.saved_inventories();
        CHECK(s.size() == 1 && s[0].id == kCyb && same_items(s[0].items, bard_items()) && saver.inventories_saved() == 1 && saver.saved() == 0 && saver.failed() == 0 && saver.queued() == 0,
              "the character service got the id and all 17 items as given; the position count is not touched");
    }
    {
        // latest wins: the service is slow, the character changes more
        FakeCharacters api;
        api.hold();
        CharacterSaver saver(api, quick());
        saver.save_inventory("zz", first_items(1));
        CHECK(wait_until([&] { return api.in_flight(); }), "(the worker is inside the first call)");
        saver.save("aa", pos_of(1));
        saver.save_inventory("aa", first_items(1));
        saver.save_inventory("aa", first_items(2));
        saver.save_inventory("aa", first_items(3));
        CHECK(saver.queued() == 1 && saver.replaced() == 2, "while it is busy a character has one entry, with one position and one inventory in it: the later inventories replaced the waiting one (two replaced)");
        api.release();
        CHECK(saver.flush(seconds(3)), "(released)");
        const auto s = api.saved_inventories();
        CHECK(s.size() == 2 && s[0].id == "zz" && s[1].id == "aa" && s[1].items.size() == 3 && api.position_count() == 1,
              "the service saw zz's, then aa's latest inventory (3 items, not 1 or 2) and aa's position: nothing older after something newer");
    }
    {
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save_inventory("aa", first_items(4));
        CHECK(saver.flush(seconds(3)) && api.inventory_call_count() == 3 && saver.inventories_saved() == 1 && saver.failed() == 0, "an Unavailable service is tried again: two refusals and Ok is one saved inventory, not a failure");
    }
    {
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save_inventory("aa", first_items(4));
        saver.flush(seconds(3));
        CHECK(api.inventory_call_count() == 3 && saver.inventories_saved() == 0 && saver.failed() == 1 && saver.queued() == 0, "three Unavailable answers and the inventory is given up: counted as failed");
    }
    {
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Refused};
        CharacterSaver saver(api, quick());
        saver.save_inventory("aa", first_items(4));
        saver.flush(seconds(3));
        api.inventory_script = {svc::CallStatus::NotFound};
        saver.save_inventory("bb", first_items(4));
        saver.flush(seconds(3));
        CHECK(api.inventory_call_count() == 2 && saver.failed() == 2 && saver.inventories_saved() == 0, "a refusal (items the service's rules do not take) and NotFound are final: one call each, no retry");
    }
    {
        // a position that is saved and an inventory that is not yet: each part on its own count, the saved one not sent again
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save("aa", pos_of(5));
        saver.save_inventory("aa", first_items(5));
        CHECK(saver.flush(seconds(3)) && api.position_call_count() == 1 && api.inventory_call_count() == 2 && saver.saved() == 1 && saver.inventories_saved() == 1 && saver.failed() == 0,
              "a position and an inventory for one character: the inventory is retried, the position is not sent again");
    }
    {
        // a newer inventory that arrives while an Unavailable one waits to be tried again is the one that is sent
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver::Config cfg = quick();
        cfg.retry_delay = milliseconds(300);
        CharacterSaver saver(api, cfg);
        saver.save_inventory("aa", first_items(2));
        wait_until([&] { return api.inventory_call_count() == 1; });
        saver.save_inventory("aa", first_items(6));
        CHECK(saver.flush(seconds(3)), "(after the pause everything is saved)");
        const auto s = api.saved_inventories();
        CHECK(s.size() == 1 && s[0].items.size() == 6 && api.inventory_call_count() == 2, "the retry sends the newer inventory (6 items), not the old one: two calls, one saved");
    }
    {
        // the base class: a service that does not know SaveInventory answers Failed, and the saver gives up on it
        struct OldService : svc::CharacterApi {
            size_t positions = 0;
            svc::CharacterResult get_character(const std::string&) override { return {}; }
            svc::CallStatus save_position(const std::string&, const svc::Position&) override { ++positions; return svc::CallStatus::Ok; }
        } api;
        CharacterSaver saver(api, quick());
        saver.save("aa", pos_of(1));
        saver.save_inventory("aa", first_items(2));
        saver.flush(seconds(3));
        CHECK(api.positions == 1 && saver.saved() == 1 && saver.failed() == 1 && saver.inventories_saved() == 0, "a service without SaveInventory: the position is saved, the inventory is a counted failure (no retry)");
    }
}
// ~ Cyb - C O A ~

// ---- through the endpoint, with a real CharacterSaver on a fake character service -----------------------------------------------------

struct FakeLogins : LoginQueue {
    std::vector<std::pair<uint64_t, LoginRequest>> submitted;
    void submit(uint64_t id, LoginRequest r) override { submitted.emplace_back(id, std::move(r)); }
};

struct Rig {
    CookieSigner signer{counting_random};
    FakeLogins logins;
    std::vector<std::pair<Address, std::vector<uint8_t>>> sent;
    Endpoint ep;

    explicit Rig(EndpointConfig cfg)
        : ep(std::move(cfg), signer, logins, [this](const Address& a, const std::vector<uint8_t>& p) { sent.emplace_back(a, p); }, counting_random, t0) {}

    std::vector<std::vector<uint8_t>> take(const Address& a) {
        std::vector<std::vector<uint8_t>> out;
        std::vector<std::pair<Address, std::vector<uint8_t>>> rest;
        for (auto& [to, p] : sent) {
            if (to == a) out.push_back(p);
            else rest.emplace_back(to, p);
        }
        sent.swap(rest);
        return out;
    }
};

struct Joined {
    std::optional<conn::HandshakePacket> challenge;
    std::optional<fake::Client> client;
    uint64_t session = 0;
    std::vector<fake::Delivered> got;
};

static Joined handshake(Rig& rig, const Address& addr, Clock::time_point now) {
    Joined j;
    rig.take(addr);
    const auto initial = capture::bytes(capture::kInitial);
    rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
    auto out = rig.take(addr);
    if (out.size() != 1) return j;
    j.challenge = conn::parse_handshake(out[0].data(), out[0].size());
    conn::HandshakePacket resp = *j.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    const auto bytes = conn::build_handshake(resp);
    rig.ep.on_datagram(addr, bytes.data(), bytes.size(), now);
    out = rig.take(addr);
    if (out.size() != 1) return j;
    const auto seqs = conn::initial_sequences(j.challenge->cookie);
    j.client.emplace(conn::outer_header_of(*j.challenge), j.challenge->ext->custom_field(), seqs.server, seqs.client);
    j.session = rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
    return j;
}

static void drain(Rig& rig, Joined& j, const Address& addr) {
    for (const auto& p : rig.take(addr)) {
        const auto r = j.client->receive(p);
        for (const auto& d : r.delivered) j.got.push_back(d);
    }
}

static void exchange(Rig& rig, Joined& j, const Address& addr, std::vector<fake::Bunch> bunches, Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}
// C | O | A captures, Cyb code

static void log_in(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(j.session, r);
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login(kMachine, kMachine)}, now + milliseconds(2));
}

static void join_world(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    log_in(rig, j, addr, rec, now);
    exchange(rig, j, addr, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)}, now + milliseconds(40));
}

struct Endpoints {
    Fixture fx;
    FakeCharacters api;
    std::unique_ptr<CharacterSaver> saver;
    EndpointConfig cfg;
    std::unique_ptr<Rig> rig;
    Endpoints() {
        saver = std::make_unique<CharacterSaver>(api, quick());
        fx.world->set_position_sink(saver.get());
        cfg.players = fx.world.get();
        rig = std::make_unique<Rig>(cfg);
    }
    ~Endpoints() {
        rig.reset();
        saver.reset();
    }
    std::vector<FakeCharacters::Inv> saves() {
        saver->flush(seconds(3));
        return api.saved_inventories();
    }
    /// The client of `j` (at `addr`) sends ServerItemUsed (and ServerActivateItem) and the server's answers are drained.
    void use(Joined& j, const Address& addr, int storage, int slot, const std::string& guid, Clock::time_point now) {
        const PlayerSession* p = rig->ep.player_at(addr);
        const uint32_t ch = p ? p->channel_of(bs::Role::Pawn) : 0;
        exchange(*rig, j, addr, {j.client->actor(ch, item_used(storage, slot, guid), true), j.client->actor(ch, item_activate(storage, slot, guid), true)}, now);
    }
};

static bool has_chest_in_bag(const std::vector<svc::InventoryItem>& items, int slot = 4) {
    for (const auto& it : items)
        if (it.item_record_id == fi::kChest && it.container == Bag && it.slot == slot) return true;
    return false;
}
static bool has_chest_worn(const std::vector<svc::InventoryItem>& items) {
    for (const auto& it : items)
        if (it.item_record_id == fi::kChest && it.container == Worn && it.slot == 2) return true;
    return false;
}

static void test_endpoint_saves() {
    std::printf("saving the inventory, through the endpoint\n");
    {
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, guid_n(102), t0 + milliseconds(100));
        const PlayerSession* p = rig.ep.player_at(kA);
        CHECK(p && p->item_requests_applied() == 1 && p->inventory().at(Bag, 4) && e.api.inventory_count() == 0,
              "(a player in the world takes the chest off, in a datagram that holds ServerItemUsed and ServerActivateItem: one unequip, nothing saved yet)");
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && s[0].id == kCyb && s[0].items.size() == 17 && has_chest_in_bag(s[0].items) && e.saver->inventories_saved() == 1,
              "the client quits: the character service gets the whole inventory, once: 17 items, the chest in the bag");
    }
    {
        // a character that never changed an item is not written
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        CHECK(rig.ep.session_count() == 0 && e.saves().empty() && e.api.inventory_call_count() == 0, "a client that joins and quits without touching an item saves no inventory");
    }
    {
        // the client vanishes: the session times out and the inventory is saved
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, guid_n(102), t0 + milliseconds(100));
        rig.ep.tick(t0 + seconds(30));
        CHECK(rig.ep.session_count() == 1 && e.saves().empty(), "(30 s of silence: still there, nothing saved)");
        rig.ep.tick(t0 + seconds(70));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && has_chest_in_bag(s[0].items), "the client times out: its inventory is saved");
        g_log.str("");
    }
    {
        // a second login of the character kicks the first session: its items are saved when it closes, and the second session starts from them
        Endpoints e;
        Rig& rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        e.use(a, kA, 4, 2, guid_n(102), t0 + milliseconds(100));
        Joined b = handshake(rig, kB, t0 + milliseconds(200));
        join_world(rig, b, kB, cyb_record(), t0 + milliseconds(210));   // the record as GetCharacter had it: the kit, the chest worn
        const PlayerSession* pb = rig.ep.player_at(kB);
        CHECK(pb && pb->inventory().at(Bag, 4) && pb->inventory().at(Bag, 4)->item_record_id == fi::kChest && !pb->inventory().at(Worn, 2),
              "the new session starts from the old one's items (the chest in the bag), not from the record the login read");
        CHECK(e.saves().size() == 1 && has_chest_in_bag(e.saves()[0].items), "and the old session's inventory was handed to the saver when the new one took over");
        rig.ep.tick(t0 + seconds(3));   // A's linger ends: it is removed
        CHECK(rig.ep.session_count() == 1 && e.saves().size() == 1, "A's removal saves nothing more, and B saves nothing yet");
        // B puts the chest on and quits: its inventory lands after A's
        const PlayerSession* p2 = rig.ep.player_at(kB);
        (void)p2;
        e.use(b, kB, 3, 4, guid_n(102), t0 + seconds(4));
        exchange(rig, b, kB, {b.client->close_channel()}, t0 + seconds(5));
        rig.ep.tick(t0 + seconds(6));
        const auto s = e.saves();
        CHECK(s.size() == 2 && has_chest_in_bag(s[0].items) && has_chest_worn(s[1].items), "B's own inventory lands after A's: the chest in the bag, then worn");
    }
    {
        // a second login takes over an inventory that differs from the record's, and the old session's final save is lost (the service is down):
        // the new session is marked changed, so when it ends, though the player touched nothing, the carried inventory is saved again
        Endpoints e;
        Rig& rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        e.use(a, kA, 4, 2, guid_n(102), t0 + milliseconds(100));
        e.api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable};
        Joined b = handshake(rig, kB, t0 + milliseconds(200));
        join_world(rig, b, kB, cyb_record(), t0 + milliseconds(210));   // the record the login read: the chest still worn
        e.saver->flush(seconds(3));
        CHECK(e.api.inventory_call_count() == 3 && e.api.inventory_count() == 0 && e.saver->failed() == 1,
              "(A's final save: three Unavailable answers, given up on; the service holds nothing of the chest in the bag)");
        const PlayerSession* pb = rig.ep.player_at(kB);
        CHECK(pb && pb->inventory().at(Bag, 4) && pb->inventory_changed() && !pb->inventory_dirty() && pb->inventories_handed_over() == 0,
              "B started from items that differ from the record's: marked changed this session (and nothing handed over yet)");
        rig.ep.tick(t0 + seconds(3));   // A's linger ends
        exchange(rig, b, kB, {b.client->close_channel()}, t0 + seconds(5));
        rig.ep.tick(t0 + seconds(6));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && has_chest_in_bag(s[0].items) && s[0].items.size() == 17,
              "B quits having touched nothing: the carried inventory is saved again, and the service has it (17 items, the chest in the bag)");
    }
    {
        // when the old session's items are the record's, nothing is carried that needs a save: a session that touches nothing saves nothing
        Endpoints e;
        Rig& rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        Joined b = handshake(rig, kB, t0 + milliseconds(200));
        join_world(rig, b, kB, cyb_record(), t0 + milliseconds(210));
        const PlayerSession* pb = rig.ep.player_at(kB);
        CHECK(pb && !pb->inventory_changed() && !pb->inventory_dirty(), "a second login over an unchanged inventory is not marked changed");
        rig.ep.tick(t0 + seconds(3));
        exchange(rig, b, kB, {b.client->close_channel()}, t0 + seconds(5));
        rig.ep.tick(t0 + seconds(6));
        CHECK(rig.ep.session_count() == 0 && e.saves().empty() && e.api.inventory_call_count() == 0, "and neither session saves an inventory");
    }
    {
        // worldd stops: finish_players hands over what the open sessions have, and the later removal does not save again
        Endpoints e;
        Rig& rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        e.use(a, kA, 4, 2, guid_n(102), t0 + milliseconds(100));
        rig.ep.kick_all(login_text::kShuttingDown, t0 + milliseconds(200));
        rig.ep.finish_players("worldd is stopping");
        const auto s = e.saves();
        CHECK(s.size() == 1 && has_chest_in_bag(s[0].items), "worldd stops: finish_players saves the open session's inventory");
        rig.ep.tick(t0 + seconds(5));
        CHECK(rig.ep.session_count() == 0 && e.saves().size() == 1, "and the session's removal afterwards saves nothing again");
    }
    {
        // the 60 s tick through the endpoint, the client alive
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, guid_n(102), t0 + milliseconds(100));
        size_t saves_at_30 = 0;
        for (int sec = 1; sec <= 65; ++sec) {
            exchange(rig, j, kA, {}, t0 + seconds(sec));
            if (sec == 30) saves_at_30 = e.api.inventory_count() + e.saver->queued();
        }
        e.saver->flush(seconds(3));
        const auto s = e.api.saved_inventories();
        CHECK(saves_at_30 == 0 && s.size() == 1 && has_chest_in_bag(s[0].items), "an inventory that changed is saved once at about 60 s, not before, and not again while nothing changes");
    }
    {
        // the 60 s save is lost (three Unavailable answers); the player quits: the end of the session hands the inventory over again
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, guid_n(102), t0 + milliseconds(100));
        e.api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable};
        for (int sec = 1; sec <= 65; ++sec) exchange(rig, j, kA, {}, t0 + seconds(sec));
        e.saver->flush(seconds(3));
        CHECK(e.api.inventory_call_count() == 3 && e.api.inventory_count() == 0 && e.saver->failed() == 1, "(the 60 s save: three Unavailable answers, given up on; the service holds nothing)");
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + seconds(66));
        rig.ep.tick(t0 + seconds(67));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && has_chest_in_bag(s[0].items), "the player quits standing still: the end of the session hands the inventory over again, and the service has it");
    }
    {
        // the service refuses the inventory (its rules): logged, given up, the session goes on
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, guid_n(102), t0 + milliseconds(100));
        e.api.inventory_script = {svc::CallStatus::Refused};
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        e.saver->flush(seconds(3));
        spdlog::default_logger()->flush();
        CHECK(e.api.inventory_call_count() == 1 && e.saver->failed() == 1 && g_log.str().find("could not be saved") != std::string::npos, "a refused inventory is one call, counted and logged: no retry");
        g_log.str("");
    }
}

// ---- ServerTriggerAbility that names an item -----------------------------------------------------------------------------------------

static repl::CodecOptions client_options() {
    repl::CodecOptions o;
    o.inline_objects = true;
    return o;
}

static BunchWriter trigger_with_item(const emit::FIntrepidNetworkGUID& ability, int64_t guid, int32_t id, int slot, const repl::ItemInstance& item, uint8_t location) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    BunchWriter out;
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(item));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(location));
    BunchWriter field;
    bool failed = false;
    if (!ok || repl::write_rpc_field(cls, *rpc, p, field, client_options(), &failed) == 0 || failed) return out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = ability;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}
// [CoA ~ C:Y:B]

static void test_item_trigger() {
    std::printf("ServerTriggerAbility that names an item\n");
    constexpr int64_t kQuickWit = 6064630736949673984ll;
    Harness h;
    const emit::FIntrepidNetworkGUID ability = h.s->bootstrap().setup().guids.ability;
    const svc::InventoryItem* scepter = h.s->inventory().at(Worn, 15);
    CHECK(scepter != nullptr, "(the Bard wears the Scepter)");
    if (!scepter) return;
    const BunchWriter bunch = trigger_with_item(ability, kQuickWit, 3, 15, items::to_instance(*scepter), 4);
    CHECK(bunch.bit_pos() > 0, "(the trigger with the Scepter as its item writes)");
    const auto resolver = [&](const emit::FIntrepidNetworkGUID& g) -> const schema::ClassSchema* { return g == ability ? &class_named("UAoCAbilityComponent") : nullptr; };
    const auto parsed = repl::read_client_bunch(class_named("PlayerPawn_C"), resolver, bunch.data(), bunch.bit_pos(), client_options());
    const bool found = parsed && parsed->rpcs.size() == 1 && parsed->rpcs[0].call.decoded;
    CHECK(found, "it reads whole: the item is decoded, the call is not half-read any more");
    if (found) {
        const auto t = repl::parse_trigger_ability(parsed->rpcs[0].call);
        CHECK(t && t->ability_guid == kQuickWit && t->client_instance_id == 3 && t->item_slot == 15 && t->item_location == 4, "with the ability, the client's id, the item's slot 15 and its location 4");
    }
    h.request(bunch, h.pawn_ch);
    CHECK(h.s->ability_triggers() == 1 && h.s->ability_ends_sent() == 1 && !h.s->failed(), "the session handles it as any trigger: an ability worldd does not run is answered with ClientEndAbility");
    CHECK(h.s->item_requests_applied() == 0 && h.s->item_requests_refused() == 0, "(and it is no item request)");
}

// ---- the real content.db ---------------------------------------------------------------------------------------------------------------

static void test_real_content_db() {
    const char* real = std::getenv("AOC_CONTENT_DB");
    if (!real || !*real) {
        std::printf("the real content.db: skipped (AOC_CONTENT_DB is not set)\n");
        return;
    }
    std::printf("the real content.db (contentc's item and item_equip_slot rows)\n");
    // the Bard's real kit, and the real two-handed sword in the bag: the same session, on the data the game has
    svc::Character c = cyb_record();
    std::vector<svc::InventoryItem> items;
    for (const auto& it : c.items)
        if (it.container == Worn) items.push_back(it);
    items.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    c.items = items;
    Harness h(c, true, /*real_db=*/true);
    CHECK(h.mirror.unreadable == 0 && mirror_matches(h) && gear_matches(h) && h.mirror.held.size() == 23 && h.mirror.appearance.size() == 4,
          "the join: storages, HeldItems and the four AppearanceIDs are the kit's");
    const size_t events = h.mirror.events.size();
    h.request_both(item_used(4, 2, guid_n(102)), item_activate(4, 2, guid_n(102)));
    CHECK(h.mirror.events.size() == events + 2 && h.s->inventory().at(Bag, 1) && h.s->inventory().at(Bag, 1)->item_record_id == fi::kChest && h.mirror.appearance.size() == 3 &&
              mirror_matches(h) && gear_matches(h),
          "the chest comes off (the starter armor's bCanEquip is 0 in the design data and is not a rule): an Add to bag slot 1 and a Remove, the look loses a piece");
    h.step(milliseconds(150));
    h.request_both(item_used(3, 1, guid_n(102)), item_activate(3, 1, guid_n(102)));
    CHECK(h.s->inventory().at(Worn, 2) && !h.s->inventory().at(Bag, 1) && h.mirror.appearance.size() == 4 && mirror_matches(h) && gear_matches(h), "and goes on again, in Chest");
    h.step(milliseconds(150));
    const size_t before = h.mirror.events.size();
    h.request(item_used(3, 0, guid_n(200)), h.pawn_ch);
    CHECK(h.mirror.events.size() == before + 6 && h.s->inventory().at(Worn, 15) && h.s->inventory().at(Worn, 15)->item_record_id == fi::kSword2H && !h.s->inventory().at(Worn, 16) &&
              mirror_matches(h) && gear_matches(h),
          "the real two-hander takes Primary and Secondary: six events, the Scepter and the Focus in the bag");
    CHECK(h.mirror.unreadable == 0 && !h.s->failed(), "everything the server sent reads");
}

// ---- helpers of the review's probes and of the window test ------------------------------------------------------------

static svc::Character record_with(std::vector<svc::InventoryItem> items) {
    svc::Character c = cyb_record();
    c.items = std::move(items);
    return c;
}

/// The Bard's 13 kit items all in the bag (nothing worn), slots 0..12 in the kit's order.
static std::vector<svc::InventoryItem> bard_all_in_bag() {
    std::vector<svc::InventoryItem> out;
    int slot = 0;
    for (auto it : bard_items()) {
        if (it.container != Worn) continue;
        it.container = Bag;
        it.slot = slot++;
        out.push_back(it);
    }
    return out;
}

static std::string describe_gear(const Harness& h) {
    std::string s = "held:";
    for (const auto& [k, r] : h.mirror.held_by_key())
        if (r) s += " " + std::to_string(k);
    s += " | appearance " + std::to_string(h.mirror.appearance.size()) + " | held_key " + std::to_string(h.mirror.held_key) + " updates " + std::to_string(h.mirror.held_updates.size());
    return s;
}

// ---- an update that waits for the window ---------------------------------------------------------------------------------------------

/// Raw reliable bunches the client gets and ignores: they take up the window of a channel (Connection::send queues them without opening anything).
static void clog_channel(Harness& h, uint32_t channel, size_t bunches) {
    conn::BunchHeader header;
    header.channel = channel;
    header.reliable = true;
    header.name_index = Connection::kActorChannelName;
    BunchWriter filler;
    filler.write_bits(0xA5, 8);
    for (size_t i = 0; i < bunches; ++i) h.conn.send(header, filler);
}
// [C-O-A + Cyb]

/// The pawn's HeldItems update is built (apply_equipment moves the array key) and the connection cannot take it yet: the reply is built again once the
/// window has room, from the state before it, so the key moves once, as the retail server's answer did (23 for a chest piece), not twice.
static void test_held_items_update_waits_for_the_window() {
    std::printf("the HeldItems update waits for the pawn channel's window\n");
    {
        Harness h;   // the chest worn
        const int key0 = h.mirror.held_key;
        CHECK(key0 > 0 && h.mirror.held.size() == 23, "(the join gave the client HeldItems at its initial key)");
        const size_t room = Connection::kMaxUnackedReliable - h.conn.unacked_reliable(h.pawn_ch);
        clog_channel(h, h.pawn_ch, room);
        CHECK(h.conn.unacked_reliable(h.pawn_ch) == Connection::kMaxUnackedReliable, "(the pawn channel's window is full)");
        h.client_sends({h.client.actor(h.pawn_ch, item_used(4, 2, guid_n(102)), true)});
        CHECK(h.s->item_requests_applied() == 1 && h.s->replies_pending() == 1 && h.mirror.held_key == key0,
              "the chest comes off: the events went out on the PlayerController's channel, the pawn's update waits for the window");
        h.settle();
        CHECK(h.s->replies_pending() == 0 && !h.s->failed(), "once the client acknowledges, the update goes out");
        CHECK(h.mirror.held_key == key0 + 23, (std::string("the HeldItems key moved once, by 23: ") + std::to_string(h.mirror.held_key - key0)).c_str());
        CHECK(mirror_matches(h) && gear_matches(h) && h.mirror.unreadable == 0, "and the client's storages and gear are the inventory's (the chest in the bag, not shown)");
        size_t held_updates_after = 0;
        for (const auto& u : h.mirror.held_updates) held_updates_after += u.first > key0 ? 1 : 0;
        CHECK(held_updates_after == 1, "exactly one HeldItems update reached the client");
    }
    {
        // nothing worn at the join: the first update inserts the 23 entries instead of changing them; the retry must insert again (the state restored),
        // so the client ends with the key an unobstructed equip gives
        const auto equip_chest = [&](bool clog, int& key, size_t& updates, bool& fine) {
            Harness h(record_with(bard_all_in_bag()));
            if (!h.mirror.held.empty() || !h.mirror.held_updates.empty()) fine = false;   // a join with nothing worn sends no HeldItems
            if (clog) clog_channel(h, h.pawn_ch, Connection::kMaxUnackedReliable - h.conn.unacked_reliable(h.pawn_ch));
            svc::InventoryItem chest;
            for (const auto& it : h.s->inventory().items())
                if (it.item_record_id == fi::kChest) chest = it;
            h.client_sends({h.client.actor(h.pawn_ch, item_used(3, chest.slot, chest.instance_guid), true)});
            if (clog && h.s->replies_pending() != 1) fine = false;   // the pawn's update waits
            h.settle();
            key = h.mirror.held_key;
            updates = h.mirror.held_updates.size();
            fine = fine && h.s->replies_pending() == 0 && h.s->item_requests_applied() == 1 && h.mirror.held.size() == 23 && h.mirror.unreadable == 0 && mirror_matches(h) && gear_matches(h);
        };
        int free_key = 0, waited_key = 0;
        size_t free_updates = 0, waited_updates = 0;
        bool free_ok = true, waited_ok = true;
        equip_chest(false, free_key, free_updates, free_ok);
        equip_chest(true, waited_key, waited_updates, waited_ok);
        CHECK(free_ok && waited_ok, "the chest goes on after a join with nothing worn, with and without a full window: the client has HeldItems (23 entries) and the same storages and gear");
        CHECK(free_updates == 1 && waited_updates == 1 && waited_key == free_key && free_key > 0,
              (std::string("one update, and the same array key as without the wait: ") + std::to_string(waited_key) + " against " + std::to_string(free_key)).c_str());
    }
}

// ---- the review's probes (the reviewer's hostile and long scenarios, kept as tests) -----------------------------------------------------

static void test_review_naked_join_then_equip() {
    std::printf("review: join with nothing worn, then equip every item (HeldItems was never sent at the join)\n");
    Harness h(record_with(bard_all_in_bag()));
    CHECK(h.mirror.unreadable == 0, "the join reads");
    CHECK(mirror_matches(h), "the join: the client's storages are the inventory");
    // equip each of the 13 in turn, by the guid in its bag slot
    const auto start = h.s->inventory().items();
    int n = 0;
    bool all_match = true;
    for (const auto& it : start) {
        if (it.container != Bag) continue;
        const svc::InventoryItem* now = h.s->inventory().at(Bag, it.slot);
        if (!now || now->instance_guid != it.instance_guid) continue;
        h.request(item_used(3, it.slot, it.instance_guid), h.pawn_ch);
        ++n;
        if (!(mirror_matches(h) && gear_matches(h))) {
            all_match = false;
            std::printf("    equip %d (record %lld slot %d): client view differs; %s; unreadable=%zu\n", n, static_cast<long long>(it.item_record_id), it.slot,
                        describe_gear(h).c_str(), h.mirror.unreadable);
            break;
        }
    }
    CHECK(n == 13 && h.s->inventory().count(Worn) == 13, "all 13 were worn");
    CHECK(all_match, "after every equip from a naked join the client's storages and gear equal the inventory");
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
}

static void test_review_unequip_everything_and_back() {
    std::printf("review: take everything off, put it all on again, twice\n");
    Harness h;
    bool ok = true;
    for (int round = 0; round < 2 && ok; ++round) {
        const auto worn = [&] {
            std::vector<svc::InventoryItem> v;
            for (const auto& it : h.s->inventory().items())
                if (it.container == Worn) v.push_back(it);
            return v;
        }();
        for (const auto& it : worn) {
            h.request(item_used(4, it.slot, it.instance_guid), h.pawn_ch);
            if (!(mirror_matches(h) && gear_matches(h))) {
                ok = false;
                std::printf("    unequip of slot %d: client differs\n", it.slot);
                break;
            }
        }
        if (!ok) break;
        // all on again, from the bag (the items the rules let be worn; the rest stay in the bag)
        const auto bag = h.s->inventory().items();
        for (const auto& it : bag) {
            if (it.container != Bag) continue;
            const svc::InventoryItem* now = h.s->inventory().at(Bag, it.slot);
            if (!now || now->instance_guid != it.instance_guid) continue;
            h.request(item_used(3, it.slot, it.instance_guid), h.pawn_ch);
            if (!(mirror_matches(h) && gear_matches(h))) {
                ok = false;
                std::printf("    equip of slot %d: client differs\n", it.slot);
                break;
            }
        }
    }
    CHECK(ok, "the client's view equals the inventory through two rounds of everything off and on");
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
}
// built with C Y B and CoA

static void test_review_full_bag() {
    std::printf("review: a full bag: unequip refused, a two-hander over two items refused, nothing changes\n");
    // 13 worn (the kit) and the bag filled with 70 items: a two-hander in slot 0, potions (not wearable) in the rest
    std::vector<svc::InventoryItem> items;
    for (auto it : bard_items())
        if (it.container == Worn) items.push_back(it);
    for (int s = 0; s < 70; ++s) items.push_back(make_item(Bag, s, s == 0 ? fi::kSword2H : fi::kPotion, 1000 + s));
    Harness h(record_with(items));
    CHECK(mirror_matches(h), "the join");
    const auto before = h.s->inventory().items();
    h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);   // unequip the chest: no room
    CHECK(h.s->inventory().items() == before && h.s->inventory().at(Worn, 2) && mirror_matches(h), "unequip with a full bag refused, nothing changed, the client agrees");
    h.request(item_used(3, 0, guid_n(1000)), h.pawn_ch);   // the Sword2H over the Scepter and the Focus: two items to the bag, one place freed
    CHECK(h.s->inventory().items() == before && h.s->inventory().at(Bag, 0) && h.s->inventory().at(Worn, 15)->item_record_id == fi::kScepter && mirror_matches(h),
          "a two-hander that needs two bag slots with one free is refused, nothing changed");
    CHECK(h.mirror.unreadable == 0 && !h.s->failed(), "everything the server sent reads");
}

static void test_review_empty_character() {
    std::printf("review: a character with no items: every request is refused and the session lives\n");
    Harness h(record_with({}));
    h.request(item_used(3, 0, guid_n(1)), h.pawn_ch);
    h.request(item_used(4, 2, guid_n(1)), h.pawn_ch);
    h.request(bag_move(h, repl::StorageEventType::Move, 0, 1, guid_n(1), 5), h.pc_ch);
    CHECK(!h.s->failed() && h.s->item_requests_applied() == 0 && h.s->inventory().size() == 0, "refused, nothing applied");
    h.s->finish("probe");
    CHECK(h.sink.inventories.empty(), "an empty inventory is never handed to the saver (it would be refused)");
}

static void test_review_saves_after_equip() {
    std::printf("review: the periodic save and the end-of-session save carry the whole set\n");
    Harness h;
    h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
    h.step(seconds(61));
    CHECK(h.sink.inventories.size() == 1 && h.sink.inventories[0].items.size() == 17, "one save of 17 items at the 60 s tick");
    h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);   // chest back on
    h.s->finish("probe");
    CHECK(h.sink.inventories.size() == 2 && h.sink.inventories[1].items.size() == 17 && h.sink.inventories[1].items != h.sink.inventories[0].items, "the final save is the latest");
    h.s->finish("again");
    CHECK(h.sink.inventories.size() == 2, "finish is idempotent");
    const size_t n = h.sink.inventories.size();
    h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
    h.step(seconds(61));
    CHECK(h.sink.inventories.size() == n, "a request after finish is not saved (a new session carries what it has)");
}

// ---- stats (plan B3) -------------------------------------------------------------------------------------------------------------------------

static const char* kArrays[] = {"StatRepInt32Owner", "StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy"};
static const char* array_of(uint8_t replication) { return replication == 1 ? kArrays[0] : replication == 5 ? kArrays[1] : kArrays[2]; }
static uint32_t bits_of(float f) { return std::bit_cast<uint32_t>(f); }

/// The client's StatsComponent arrays are the session's stats: an item for each stat that replicates to the owner, with its float bits (and
/// base and equipment for the stats that carry them), and nothing else.
/// `unlaid`: after a class change, the stats the join's arrays have no item for (the new class's own resource) are not expected on the client, and the
/// old class's resource keeps its item there.
static bool stats_match(const Harness& h, std::string* why = nullptr, bool unlaid = false) {
    const sg::CharacterStats* cs = h.s->character_stats();
    if (!cs) return false;
    size_t expected = 0;
    for (const sg::StatValue& v : cs->values()) {
        if (v.float_array() || (v.replication != 1 && v.replication != 5 && v.replication != 7)) continue;
        ++expected;
        const auto arr = h.mirror.stat_items.find(array_of(v.replication));
        bool found = false;
        if (arr != h.mirror.stat_items.end()) {
            for (const auto& [id, item] : arr->second) {
                if (item.record != v.guid) continue;
                found = item.value == bits_of(v.value) && (!v.buckets() || (item.extended && item.base == bits_of(v.base) && item.equipment == bits_of(v.equip)));
                if (!found && why) *why = "stat " + std::to_string(v.guid) + " is " + std::to_string(std::bit_cast<float>(item.value)) + " on the client, " + std::to_string(v.value) + " in the session";
            }
        }
        if (!found && unlaid && !h.s->bootstrap().has_stat_item(v.guid)) {
            --expected;
            continue;
        }
        if (!found) {
            if (why && why->empty()) *why = "stat " + std::to_string(v.guid) + " is missing or different";
            return false;
        }
    }
    size_t have = 0;
    for (const auto& [name, items] : h.mirror.stat_items) have += items.size();
    if (unlaid) return have >= expected;   // (the old class's own resource keeps its item on the client: nothing removes it)
    if (have != expected && why) *why = std::to_string(have) + " items on the client, " + std::to_string(expected) + " stats in the session";
    return have == expected;
}

static size_t changed_total(const Mirror::StatUpdate& u) {
    size_t n = 0;
    for (const auto& [name, set] : u.changed) n += set.size();
    return n;
}

/// The stats an engine run says change when `items` (the worn items before) become `after`: the guids by array, for a character of the session's class and level.
static std::map<std::string, std::set<int64_t>> engine_diff(const Harness& h, const std::vector<sg::WornItem>& before, const std::vector<sg::WornItem>& after) {
    const bs::StatContext& ctx = world_stats::of(*h.fx.content);
    sg::CharacterStats cs(*ctx.bundle, *ctx.gear, h.s->character_stats()->class_enum(), h.s->character_stats()->level(), ctx.rounding);
    std::string err;
    std::vector<sg::StatValue> changed;
    std::map<std::string, std::set<int64_t>> out;
    if (!cs.start(before, err) || !cs.update(after, changed, err)) return out;
    for (const sg::StatValue& v : changed) {
        if (!v.float_array() && (v.replication == 1 || v.replication == 5 || v.replication == 7)) out[array_of(v.replication)].insert(v.guid);
    }
    return out;
}
// [C_Y_B - CoA]

static std::vector<sg::WornItem> without_slot(std::vector<sg::WornItem> worn, int slot) {
    worn.erase(std::remove_if(worn.begin(), worn.end(), [&](const sg::WornItem& w) { return w.slot == slot; }), worn.end());
    return worn;
}

static void test_stats_at_the_join() {
    std::printf("stats: the join gives the client the engine's stats\n");
    Harness h;
    const sg::CharacterStats* cs = h.s->character_stats();
    CHECK(cs && cs->started() && cs->class_enum() == 2 && cs->level() == 1 && cs->values().size() == 466, "the session has the Bard's 466 stats at level 1");
    std::string why;
    CHECK(stats_match(h, &why), "the client's arrays are those stats, every value's float bits");
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK(h.mirror.stat_items[kArrays[0]].size() == 335 && h.mirror.stat_items[kArrays[1]].size() == 96 && h.mirror.stat_items[kArrays[2]].size() == 11,
          "335 Owner, 96 OwnerProxy and 11 EveryoneProxy items");
    const auto value = [&](int64_t guid) { return cs && cs->find(guid) ? cs->find(guid)->value : -1.f; };
    constexpr int64_t kHealth = 108986352686712ll, kMaxHealth = 108986356618873ll;
    CHECK(value(kHealth) == value(kMaxHealth) && value(kMaxHealth) > 0, "Health starts at its maximum");
    CHECK(h.mirror.stat_updates.empty() || h.mirror.stat_updates.size() <= 2, "(the join is the arrays' first state: no later update has changed a stat)");
    CHECK(h.s->stat_updates() == 0 && h.s->stat_updates_failed() == 0, "no gear change yet, no failure");
}

static void test_stats_follow_the_chest() {
    std::printf("stats: the chest piece off and on\n");
    Harness h;
    const std::vector<sg::WornItem> before = bs::worn_items_of(h.s->inventory());
    const sg::CharacterStats* cs = h.s->character_stats();
    const std::vector<sg::StatValue> at_join = cs->values();
    const size_t updates = h.mirror.stat_updates.size();
    h.request_both(item_used(4, 2, guid_n(102)), item_activate(4, 2, guid_n(102)));
    CHECK(h.mirror.stat_updates.size() == updates + 1 && h.s->stat_updates() == 1, "taking it off sends one stat update");
    std::string why;
    CHECK(stats_match(h, &why), "the client's arrays are the session's stats again");
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    if (h.mirror.stat_updates.size() == updates + 1) {
        const Mirror::StatUpdate& u = h.mirror.stat_updates.back();
        const auto want = engine_diff(h, before, without_slot(before, 2));
        CHECK(!want.empty() && u.changed == want && u.deleted == 0, "exactly the stats the engine says change, by array, none removed");
        std::printf("    the starter chest changes %zu Owner, %zu OwnerProxy and %zu EveryoneProxy stats (retail's chest piece: 30, 14 and 2)\n",
                    u.changed.count(kArrays[0]) ? u.changed.at(kArrays[0]).size() : 0, u.changed.count(kArrays[1]) ? u.changed.at(kArrays[1]).size() : 0,
                    u.changed.count(kArrays[2]) ? u.changed.at(kArrays[2]).size() : 0);
        CHECK(changed_total(u) == 2 && u.changed.count(kArrays[0]) && u.changed.count(kArrays[1]),
              "the starter chest has one stat, so one Owner stat and the OwnerProxy stat derived from it change, and no other (retail's chest pieces have many)");
        const std::vector<int64_t> mitigation = {109183607068294ll};   // PhysicalMitigationRating: the chest's own stat
        bool has = u.changed.count(kArrays[0]) && u.changed.at(kArrays[0]).count(mitigation[0]);
        CHECK(has, "the chest's own stat (the physical mitigation rating) is one of them");
        const sg::StatValue* now = cs->find(mitigation[0]);
        const sg::StatValue* was = nullptr;
        for (const auto& v : at_join) was = v.guid == mitigation[0] ? &v : was;
        CHECK(now && was && was->value - now->value == 20.0f, "it is 20 lower: the chest's value");
    }
    // and on again: every stat is what it was at the join
    const size_t updates2 = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 4, guid_n(102)), item_activate(3, 4, guid_n(102)));
    CHECK(h.mirror.stat_updates.size() == updates2 + 1 && stats_match(h), "putting it on sends the stats back, and the client agrees");
    bool restored = cs->values().size() == at_join.size();
    for (size_t i = 0; restored && i < at_join.size(); ++i) restored = at_join[i].guid == cs->values()[i].guid && bits_of(at_join[i].value) == bits_of(cs->values()[i].value);
    CHECK(restored, "every stat has the value it had at the join, to the bit");
    CHECK(h.s->stat_updates() == 2 && h.s->stat_updates_failed() == 0 && h.mirror.unreadable == 0, "two stat updates, none failed, everything read");
}

static std::vector<svc::InventoryItem> bard_wearing_heavy_chest() {
    std::vector<svc::InventoryItem> items = bard_items();
    for (svc::InventoryItem& it : items) {
        if (it.container == Worn && it.slot == 2) it.item_record_id = fi::kHeavyChest;   // the heavy chest in Chest instead of the starter's
    }
    return items;
}

static int64_t guid_named(const Harness& h, const char* name) {
    const sg::StatBundle& b = *world_stats::of(*h.fx.content).bundle;
    for (size_t i = 0; i < b.stats.size(); ++i) {
        if (b.stat_name(i) == name) return b.stats[i].guid;
    }
    return 0;
}

static void test_health_and_mana_clamp() {
    std::printf("stats: Health and Mana above a new maximum are clamped, below it they stay\n");
    Harness h(record_with(bard_wearing_heavy_chest()));
    const sg::CharacterStats* cs = h.s->character_stats();
    CHECK(stats_match(h), "(joined wearing the heavy chest: the client has the stats)");
    for (const char* name : {"Health", "Mana"}) {
        const int64_t cur = guid_named(h, name), max = guid_named(h, (std::string("Max") + name).c_str());
        CHECK(cur != 0 && max != 0 && cs->find(cur) && cs->find(max), (std::string(name) + " and Max" + name + " are stats of the Bard").c_str());
        CHECK(cs->find(cur)->value == cs->find(max)->value && cs->find(max)->value > 100, (std::string(name) + " starts at its maximum, which the heavy chest raised past 100").c_str());
    }
    const int64_t kHealth = guid_named(h, "Health"), kMaxHealth = guid_named(h, "MaxHealth"), kMana = guid_named(h, "Mana"), kMaxMana = guid_named(h, "MaxMana");
    const float max_health_with = cs->find(kMaxHealth)->value, max_mana_with = cs->find(kMaxMana)->value;
    // off: the maxima fall, so the current values are clamped to them
    h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
    const float health_without = cs->find(kHealth)->value, mana_without = cs->find(kMana)->value;
    CHECK(cs->find(kMaxHealth)->value < max_health_with && health_without == cs->find(kMaxHealth)->value, "taking it off: MaxHealth falls and Health is clamped to the new maximum");
    CHECK(cs->find(kMaxMana)->value < max_mana_with && mana_without == cs->find(kMaxMana)->value, "...and so are MaxMana and Mana");
    CHECK(stats_match(h), "the client's current values and maxima are the session's");
    const Mirror::StatUpdate& u = h.mirror.stat_updates.back();
    const std::set<int64_t>& everyone = u.changed.at(kArrays[2]);
    CHECK(everyone.count(kHealth) && everyone.count(kMaxHealth) && everyone.count(kMana) && everyone.count(kMaxMana), "Health, MaxHealth, Mana and MaxMana are all deltas of the EveryoneProxy array");
    // on again: the maxima rise, the current values stay
    h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);
    CHECK(cs->find(kMaxHealth)->value == max_health_with && cs->find(kHealth)->value == health_without && health_without < max_health_with,
          "putting it on: MaxHealth is back up and Health stays where it is (no healing by gear)");
    CHECK(cs->find(kMaxMana)->value == max_mana_with && cs->find(kMana)->value == mana_without, "...and Mana too");
    CHECK(stats_match(h), "...and the client agrees");
    const std::set<int64_t>& later = h.mirror.stat_updates.back().changed.at(kArrays[2]);
    CHECK(later.count(kMaxHealth) && later.count(kMaxMana) && !later.count(kHealth) && !later.count(kMana), "the maxima are deltas, the current values are not");
}
// ~ CoA & C:Y:B ~

static void test_nothing_changes_nothing_sent() {
    std::printf("stats: a change of gear that moves no stat sends none\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fi::kCostume, 204));   // a costume: no stat block, no tag
    Harness h(record_with(items));
    CHECK(stats_match(h), "(the join)");
    const size_t updates = h.mirror.stat_updates.size(), held = h.mirror.held_updates.size();
    h.request(item_used(3, 4, guid_n(204)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 71) && h.mirror.held_updates.size() == held + 1, "the costume is worn: HeldItems and AppearanceIDs are updated");
    CHECK(h.mirror.stat_updates.size() == updates && h.s->stat_updates() == 0 && h.s->stat_updates_failed() == 0, "and no stat update is sent: no stat moved");
    CHECK(stats_match(h) && gear_matches(h), "the client's stats and gear are the session's");
    // a bag move changes no gear at all
    h.request(bag_move(h, repl::StorageEventType::Move, 1, 5, guid_n(201), 33), h.pc_ch);
    CHECK(h.mirror.stat_updates.size() == updates && h.s->stat_updates() == 0, "a move inside the bag does not touch the stats either");
}

static void test_stats_wait_for_the_window() {
    std::printf("stats: the deltas go out with the update when the pawn channel has room\n");
    Harness h;
    const size_t updates = h.mirror.stat_updates.size();
    const size_t room = Connection::kMaxUnackedReliable - h.conn.unacked_reliable(h.pawn_ch);
    clog_channel(h, h.pawn_ch, room);
    CHECK(h.conn.unacked_reliable(h.pawn_ch) == Connection::kMaxUnackedReliable, "(the pawn channel's window is full)");
    h.client_sends({h.client.actor(h.pawn_ch, item_used(4, 2, guid_n(102)), true)});
    CHECK(h.s->item_requests_applied() == 1 && h.s->stat_updates() == 1 && h.s->replies_pending() == 1 && h.mirror.stat_updates.size() == updates,
          "the chest comes off: the session's stats are changed at once, and the client has not heard yet: the update waits");
    CHECK(!stats_match(h), "(the client's arrays are still the old ones)");
    h.settle();
    CHECK(h.s->replies_pending() == 0 && !h.s->failed() && h.mirror.stat_updates.size() == updates + 1, "once the client acknowledges, one update goes out");
    CHECK(stats_match(h) && mirror_matches(h) && gear_matches(h) && h.mirror.unreadable == 0, "and the client's stats, storages and gear are the session's");
    // a second equip while the first still waits: the stats are one update, with both changes
    Harness g;
    clog_channel(g, g.pawn_ch, Connection::kMaxUnackedReliable - g.conn.unacked_reliable(g.pawn_ch));
    const size_t u0 = g.mirror.stat_updates.size();
    g.client_sends({g.client.actor(g.pawn_ch, item_used(4, 2, guid_n(102)), true)});
    g.client_sends({g.client.actor(g.pawn_ch, item_used(3, 3, guid_n(203)), true)});   // the heavy chest from bag slot 3 into Chest
    g.settle();
    CHECK(g.s->stat_updates() == 2 && g.mirror.stat_updates.size() <= u0 + 2 && stats_match(g) && gear_matches(g) && mirror_matches(g),
          "two equips behind a full window: the client ends with the session's stats, gear and storages");
}

/// The client's value of a stat (NaN when it has none).
static float client_stat(const Harness& h, int64_t guid) {
    for (const auto& [name, items] : h.mirror.stat_items) {
        for (const auto& [id, item] : items) {
            if (item.record == guid) return std::bit_cast<float>(item.value);
        }
    }
    return std::numeric_limits<float>::quiet_NaN();
}

static void test_stats_through_the_runtime() {
    std::printf("stats: MaxStamina goes through the ability runtime (a Rest has started it) and clamps its Stamina\n");
    constexpr int64_t kStamina = 6064629575339934476ll;
    std::vector<svc::InventoryItem> items = bard_items();
    for (svc::InventoryItem& it : items) {
        if (it.container == Bag && it.item_record_id == fi::kRingA) {   // ring A (MaxStamina +50), worn from the join
            it.container = Worn;
            it.slot = 10;
        }
    }
    Harness h(record_with(items));
    CHECK(client_stat(h, kStamina) == 150.0f && client_stat(h, content_fixture::kMaxStamina) == 150.0f && stats_match(h), "joined wearing ring A: Stamina and MaxStamina are 150 on the client");
    // a Rest starts the runtime and costs nothing
    const emit::FIntrepidNetworkGUID ability = h.s->bootstrap().setup().guids.ability;
    const svc::InventoryItem* scepter = h.s->inventory().at(Worn, 15);
    CHECK(h.s->abilities() && h.s->abilities()->stamina() == 150 && scepter != nullptr, "(the runtime exists from the first tick and started from the pawn values: Stamina 150)");
    if (!scepter) return;
    h.request(trigger_with_item(ability, content::kGuidRest, 5, 15, items::to_instance(*scepter), 4), h.pawn_ch);
    CHECK(h.s->abilities() && h.s->abilities()->stamina() == 150 && h.s->abilities()->max_stamina() == 150 && h.s->abilities()->resting(),
          "the runtime holds 150 and 150 and the Rest is on");
    // the ring comes off: MaxStamina 100, and the runtime clamps Stamina to it
    h.request(item_used(4, 10, guid_n(202)), h.pawn_ch);
    CHECK(h.s->item_requests_applied() == 1 && h.s->abilities()->max_stamina() == 100 && h.s->abilities()->stamina() == 100,
          "the ring off: the runtime's MaxStamina is 100 and its Stamina is clamped to 100");
    CHECK(client_stat(h, content_fixture::kMaxStamina) == 100.0f && client_stat(h, kStamina) == 100.0f && stats_match(h) && h.s->stat_updates() == 1 && !h.s->abilities_failed(),
          "the client has both at 100, the session's stats agree, one stat update, the abilities still on");
    // spend 50 on a Dodge, then put the ring back: the maximum rises, the Stamina is the runtime's and stays
    h.request(trigger_with_item(ability, content::kGuidDodge, 6, 15, items::to_instance(*scepter), 4), h.pawn_ch);
    const float after_dodge = client_stat(h, kStamina);
    CHECK(after_dodge <= 50.0f && h.s->abilities()->stamina() <= 50, "(a Dodge took 50 Stamina)");
    h.request(item_used(3, 2, guid_n(202)), h.pawn_ch);
    CHECK(h.s->abilities()->max_stamina() == 150 && client_stat(h, content_fixture::kMaxStamina) == 150.0f && h.s->abilities()->stamina() <= 50 &&
              client_stat(h, kStamina) == static_cast<float>(h.s->abilities()->stamina()) && h.mirror.unreadable == 0,
          "the ring back on: MaxStamina is 150 again and Stamina is not raised by it: the client's is the runtime's");
    CHECK(h.s->stat_updates_failed() == 0 && !h.s->failed(), "no stat update failed");
}
// [C | O | A x Cyb]

/// The four fishing stats that replicate base and equipment (the Owner array's items carry all three numbers); the rod gives one of them, Power.
static const char* const kRodStats[] = {"Artisan_Gathering_Fishing_Sport_Keep_Lure", "Artisan_Gathering_Fishing_Sport_Hook_Rate",
                                        "Artisan_Gathering_Fishing_Sport_Rare_Fish_Chance", "Artisan_Gathering_Fishing_Sport_Power"};

static void test_stats_follow_the_rod() {
    std::printf("stats: the fishing rod (a stat that replicates base and equipment) off and on\n");
    Harness h;
    const sg::CharacterStats* cs = h.s->character_stats();
    for (const char* name : kRodStats) {
        const sg::StatValue* v = cs->find(guid_named(h, name));
        CHECK(v && v->buckets() && v->replication == 1, (std::string(name) + ": an Owner stat that replicates base and equipment").c_str());
    }
    const int64_t power = guid_named(h, "Artisan_Gathering_Fishing_Sport_Power");
    const float base = cs->find(power)->base;
    CHECK(cs->find(power)->equip == 100.0f && cs->find(power)->value == base + 100.0f && stats_match(h), "at the join the rod's 100 is in Power's equipment, and the client's item has base, equipment and value");
    const size_t updates = h.mirror.stat_updates.size();
    h.request_both(item_used(4, 35, guid_n(135)), item_activate(4, 35, guid_n(135)));
    CHECK(h.mirror.stat_updates.size() == updates + 1, "the rod off: one stat update");
    std::string why;
    CHECK(stats_match(h, &why), "the client's arrays (value, base, equipment) are the session's after the rod is off");
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    if (h.mirror.stat_updates.size() == updates + 1) {
        const Mirror::StatUpdate& u = h.mirror.stat_updates.back();
        const auto it = u.changed.find(kArrays[0]);
        CHECK(it != u.changed.end() && it->second.count(power) == 1 && u.deleted == 0, "Power is a delta of the Owner array");
        const auto lure = it != u.changed.end() ? it->second.count(guid_named(h, kRodStats[0])) : 0;
        CHECK(lure == 0, "and a bucket stat the rod does not touch is not");
    }
    CHECK(cs->find(power)->equip == 0 && cs->find(power)->value == base && client_stat(h, power) == base, "Power is its base again, on the client too");
    h.request_both(item_used(3, 4, guid_n(135)), item_activate(3, 4, guid_n(135)));
    CHECK(stats_match(h, &why), "and after it is back on");
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK(cs->find(power)->equip == 100.0f && cs->find(power)->base == base && client_stat(h, power) == base + 100.0f, "Power has the rod's 100 in its equipment again, the base untouched");
}

/// A stat whose replication is 0 (no array of the pawn holds it: the real rods give Size_Small, Type_Fish01 ...) moves with gear next to the stats
/// that replicate. The session keeps it in its stats and sends nothing for it: it must not be looked up in the pawn's arrays (that fails, and the
/// whole update with it).
static void test_stats_of_replication_zero_are_not_sent() {
    std::printf("stats: a rod that moves a stat no array holds (Size_Small) next to Power\n");
    std::vector<svc::InventoryItem> items = bard_items();
    for (svc::InventoryItem& it : items) {
        if (it.container == Worn && it.slot == 35) it.item_record_id = fi::kSizeRod;
    }
    Harness h(record_with(items));
    const sg::CharacterStats* cs = h.s->character_stats();
    const int64_t power = content_fixture::kFishingPower, size = content_fixture::kSizeSmall;
    CHECK(cs->find(size) && cs->find(size)->replication == 0 && cs->find(size)->value == 7.0f && cs->find(power)->equip == 100.0f && stats_match(h),
          "joined wearing the size rod: Size_Small is 7 (replication 0), Power has its 100, and the client has what replicates");
    CHECK(std::isnan(client_stat(h, size)), "(the client's arrays hold no item for Size_Small)");
    const float base = cs->find(power)->base;
    const size_t updates = h.mirror.stat_updates.size();
    h.request_both(item_used(4, 35, guid_n(135)), item_activate(4, 35, guid_n(135)));
    CHECK(h.s->inventory().at(Worn, 35) == nullptr, "the rod is off");
    CHECK(h.s->stat_updates_failed() == 0 && h.s->stat_updates() == 1 && h.mirror.stat_updates.size() == updates + 1,
          "the rod off: no failed update, one update (Power moved)");
    std::string why;
    CHECK(cs->find(size)->value == 0.0f && cs->find(power)->equip == 0.0f && cs->find(power)->value == base && stats_match(h, &why),
          "Size_Small is 0 in the session, Power is its base, and the client has the session's stats");
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    if (h.mirror.stat_updates.size() == updates + 1) {
        const Mirror::StatUpdate& u = h.mirror.stat_updates.back();
        const auto it = u.changed.find(kArrays[0]);
        CHECK(it != u.changed.end() && it->second.count(power) == 1 && it->second.count(size) == 0 && u.deleted == 0, "the update carries Power and no item for Size_Small");
    }
    CHECK(std::isnan(client_stat(h, size)), "and still no item for Size_Small on the client");
    h.request_both(item_used(3, 4, guid_n(135)), item_activate(3, 4, guid_n(135)));
    CHECK(h.s->stat_updates_failed() == 0 && h.s->stat_updates() == 2 && cs->find(size)->value == 7.0f && cs->find(power)->equip == 100.0f && stats_match(h, &why),
          "the rod back on: no failure, Size_Small is 7 again, Power has its 100, the client agrees");
    CHECK(!h.s->failed() && !h.s->abilities_failed() && h.mirror.unreadable == 0, "the session carries on");
}

/// Gear that moves only stats of replication 0: nothing is sent, nothing fails, and the session's stats are the new ones all the same (they are the
/// base of the next change).
static void test_gear_that_moves_only_unreplicated_stats() {
    std::printf("stats: ring B (it moves only Size_Small, a stat no array holds) on and off\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fi::kRingB, 212));
    Harness h(record_with(items));
    const sg::CharacterStats* cs = h.s->character_stats();
    const int64_t size = content_fixture::kSizeSmall;
    CHECK(cs->find(size) && cs->find(size)->value == 0.0f && stats_match(h), "(joined: Size_Small is 0)");
    const size_t updates = h.mirror.stat_updates.size();
    h.request(item_used(3, 4, guid_n(212)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 10) != nullptr, "ring B is worn (RingRight)");
    CHECK(h.s->stat_updates_failed() == 0 && h.s->stat_updates() == 0 && h.mirror.stat_updates.size() == updates, "no failed update, no stat update sent");
    CHECK(cs->find(size)->value == 7.0f && cs->find(size)->equip == 7.0f && stats_match(h), "Size_Small is 7 in the session all the same (the next diff starts from it)");
    h.request(item_used(4, 10, guid_n(212)), h.pawn_ch);
    CHECK(h.s->stat_updates_failed() == 0 && h.s->stat_updates() == 0 && h.mirror.stat_updates.size() == updates && cs->find(size)->value == 0.0f && stats_match(h),
          "ring B off: nothing failed, nothing sent, Size_Small is 0 again");
    CHECK(!h.s->failed() && h.mirror.unreadable == 0, "the session carries on");
}
// (CoA : C-Y-B)

/// Two earrings give Hook_Rate +150 and +200, a stat clamped to 100: wearing the second changes the equipment contribution (150 -> 350) and not the
/// value (100 either way). The client must hear of it all the same.
static void test_stats_equipment_changes_with_the_value_clamped() {
    std::printf("stats: a change of the equipment contribution alone (the value is clamped) is sent\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Worn, 14, fi::kEarLeftFirst, 210));   // Hook_Rate +150: value 100, equipment 150
    items.push_back(make_item(Bag, 4, fi::kEarRightFirst, 211));    // Hook_Rate +200
    Harness h(record_with(items));
    const sg::CharacterStats* cs = h.s->character_stats();
    const int64_t hook = content_fixture::kHookRate;
    CHECK(cs->find(hook) && cs->find(hook)->value == 100.0f && cs->find(hook)->equip == 150.0f && stats_match(h),
          "joined wearing the first earring: Hook_Rate is 100 (clamped), its equipment 150, and the client has them");
    const float base_before = cs->find(hook)->base;
    const size_t updates = h.mirror.stat_updates.size();
    h.request(item_used(3, 4, guid_n(211)), h.pawn_ch);
    CHECK(cs->find(hook)->value == 100.0f && cs->find(hook)->equip == 350.0f && cs->find(hook)->base == base_before, "the second earring: still 100, the equipment 350, the base untouched");
    CHECK(h.mirror.stat_updates.size() == updates + 1 && h.s->stat_updates() == 1, "one stat update goes out, though no value moved");
    std::string why;
    CHECK(stats_match(h, &why), "and the client's equipment is 350, as the session's");
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK(client_stat(h, hook) == 100.0f, "(the client's value is still 100)");
    // and back: the equipment falls to 200, the value stays
    h.request(item_used(4, 14, guid_n(210)), h.pawn_ch);
    CHECK(cs->find(hook)->value == 100.0f && cs->find(hook)->equip == 200.0f && stats_match(h, &why), "the first earring off: equipment 200, value 100, the client agrees");
}

/// Stamina is the ability runtime's: after two Dodges it is low, and a MaxStamina item coming off, which would make the engine clamp its own Stamina,
/// must not set the pawn's Stamina to the engine's value.
static void test_stamina_low_after_dodges_is_not_refilled_by_gear() {
    std::printf("stats: a ring of MaxStamina off while the runtime's Stamina is low (two Dodges)\n");
    constexpr int64_t kStamina = 6064629575339934476ll;
    std::vector<svc::InventoryItem> items = bard_items();
    for (svc::InventoryItem& it : items) {
        if (it.container == Bag && it.item_record_id == fi::kRingA) {   // ring A (MaxStamina +50), worn from the join
            it.container = Worn;
            it.slot = 10;
        }
    }
    Harness h(record_with(items));
    const emit::FIntrepidNetworkGUID ability = h.s->bootstrap().setup().guids.ability;
    const svc::InventoryItem* scepter = h.s->inventory().at(Worn, 15);
    CHECK(h.s->abilities() && scepter && h.s->abilities()->stamina() == 150, "(joined wearing ring A: Stamina 150, the runtime is up)");
    if (!scepter) return;
    h.request(trigger_with_item(ability, content::kGuidDodge, 6, 15, items::to_instance(*scepter), 4), h.pawn_ch);
    CHECK(h.s->abilities()->stamina() <= 100, "(the first Dodge took 50)");
    for (int i = 0; i < 40; ++i) h.step();   // 0.8 s: past the Dodge's 0.65 s cooldown
    h.request(trigger_with_item(ability, content::kGuidDodge, 7, 15, items::to_instance(*scepter), 4), h.pawn_ch);
    const double low = h.s->abilities()->stamina();
    CHECK(low <= 55, "(the second Dodge landed too: Stamina is about 50)");
    CHECK(client_stat(h, kStamina) == static_cast<float>(low), "(the client has the runtime's Stamina)");
    const size_t updates = h.s->stat_updates();
    h.request(item_used(4, 10, guid_n(202)), h.pawn_ch);
    CHECK(h.s->stat_updates() == updates + 1 && h.s->abilities()->max_stamina() == 100, "the ring off: MaxStamina is 100 in the runtime");
    CHECK(h.s->abilities()->stamina() <= low, "the runtime's Stamina is not raised (the engine's own would be 100)");
    CHECK(client_stat(h, kStamina) <= static_cast<float>(low) && client_stat(h, kStamina) == static_cast<float>(h.s->abilities()->stamina()),
          "and neither is the client's: it is the runtime's");
    CHECK(client_stat(h, content_fixture::kMaxStamina) == 100.0f && !h.s->abilities_failed() && h.s->stat_updates_failed() == 0, "MaxStamina is 100 on the client; nothing failed");
}

/// A stat program that faults for the worn gear (the shirt's Constitution +1e39 overflows a float32) gives stats that are not the character's:
/// the join refuses them (test_worldd_character_setup) and a gear change does not send them.
static void test_stats_of_a_faulted_run_are_not_sent() {
    std::printf("stats: a change of gear whose stat run faults sends nothing and leaves the stats as the client has them\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fi::kShirt, 205));
    Harness h(record_with(items));
    const sg::CharacterStats* cs = h.s->character_stats();
    const std::vector<sg::StatValue> at_join = cs->values();
    const size_t updates = h.mirror.stat_updates.size();
    CHECK(stats_match(h) && cs->faults() == 0, "(joined: the client has the stats, no fault)");
    h.request(item_used(3, 4, guid_n(205)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 19) != nullptr, "the shirt is worn (the item rules take it)");
    CHECK(h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0 && h.mirror.stat_updates.size() == updates, "its stat run faulted: counted as a failed update, nothing is sent");
    bool same = cs->values().size() == at_join.size();
    for (size_t i = 0; same && i < at_join.size(); ++i) same = bits_of(at_join[i].value) == bits_of(cs->values()[i].value);
    CHECK(same && cs->faults() == 0 && stats_match(h), "the session's stats are still the client's: the faulted values were not kept");
    CHECK(!h.s->failed() && h.mirror.unreadable == 0, "the session carries on");
    // taking the shirt off: the run is clean again and there is nothing to change
    h.request(item_used(4, 19, guid_n(205)), h.pawn_ch);
    CHECK(h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0 && stats_match(h), "the shirt off: no failure, no change");
}

/// The same when the clamp hides the overflow from the VALUE: the cursed earring gives Hook_Rate +1e39, a stat held to 100, so the value stays 100 and only
/// the equipment's sum (which the client is sent too) is beyond a float32. The run's fault must come from it, and nothing that is not a finite float
/// may reach the client's arrays or the session's stats (a stored infinity would make the character's next login refused).
static void test_equipment_overflow_behind_a_clamp_is_not_sent() {
    std::printf("stats: an equipment sum no float32 holds, behind a clamp (Hook_Rate), sends nothing\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fi::kCursedEarring, 206));
    Harness h(record_with(items));
    const sg::CharacterStats* cs = h.s->character_stats();
    const int64_t hook = content_fixture::kHookRate;
    const std::vector<sg::StatValue> at_join = cs->values();
    const size_t updates = h.mirror.stat_updates.size();
    CHECK(stats_match(h) && cs->faults() == 0 && cs->find(hook)->equip == 0.0f, "(joined: the client has the stats, no fault)");
    h.request(item_used(3, 4, guid_n(206)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 13) != nullptr, "the earring is worn (the item rules take it)");
    CHECK(h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0 && h.mirror.stat_updates.size() == updates,
          "its stat run faulted (the equipment's sum, though Hook_Rate's value is a clamped 100): a failed update, nothing is sent");
    bool same = cs->values().size() == at_join.size();
    for (size_t i = 0; same && i < at_join.size(); ++i) same = bits_of(at_join[i].value) == bits_of(cs->values()[i].value) && bits_of(at_join[i].equip) == bits_of(cs->values()[i].equip);
    CHECK(same && cs->faults() == 0 && stats_match(h), "the session's stats are still the client's");
    bool finite = true;
    for (const sg::StatValue& v : cs->values()) finite = finite && std::isfinite(v.value) && std::isfinite(v.base) && std::isfinite(v.equip);
    for (const auto& [name, arr] : h.mirror.stat_items) {
        for (const auto& [id, item] : arr) {
            finite = finite && std::isfinite(std::bit_cast<float>(item.value));
            if (item.extended) finite = finite && std::isfinite(std::bit_cast<float>(item.base)) && std::isfinite(std::bit_cast<float>(item.equipment));
        }
    }
    CHECK(finite, "and nothing in them or in the client's arrays is an infinity or a NaN");
    CHECK(!h.s->failed() && h.mirror.unreadable == 0, "the session carries on");
}
// Cyb - C | O | A

/// Takes the item of a stat out of the pawn's Owner array: the pawn then refuses every change of that stat.
static bool drop_owner_stat_item(const Harness& h, int64_t record) {
    repl::Subobject* stats = const_cast<repl::Subobject*>(h.s->bootstrap().pawn().find(h.mirror.guids.stats));
    repl::FastArrayState* owner = stats ? stats->obj.arrays.get(*stats->obj.schema, "StatRepInt32Owner") : nullptr;
    int32_t id = 0;
    if (owner) {
        for (const repl::FastArrayItem& it : owner->items()) {
            const auto found = it.values.values.find(1);
            const auto* t = found != it.values.values.end() ? found->second.get_if<repl::StatInt32Inst>() : nullptr;
            if (t && t->record == record) id = it.id;
        }
    }
    return owner && id != 0 && owner->remove(id);
}

/// The pawn refuses a stat change (here: its Owner array has lost the item of a stat): the session's stats stay what the client has, so that the next
/// change of gear is a diff against what the client knows, and the ability runtime holds nothing the client was not told.
static void test_stats_refused_by_the_pawn_stay_the_clients() {
    std::printf("stats: a change the pawn's arrays cannot take is not committed to the session\n");
    constexpr int64_t kMitigation = 109183607068294ll;
    {
        Harness h;
        const sg::CharacterStats* cs = h.s->character_stats();
        const float before = cs->find(kMitigation)->value;
        const std::vector<sg::StatValue> at_join = cs->values();
        CHECK(drop_owner_stat_item(h, kMitigation), "(the pawn's Owner array lost the mitigation item)");
        const size_t updates = h.mirror.stat_updates.size();
        h.request_both(item_used(4, 2, guid_n(102)), item_activate(4, 2, guid_n(102)));
        CHECK(h.s->inventory().at(Worn, 2) == nullptr, "the chest is off (the item rules do not care)");
        CHECK(h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0, "the pawn refused the stats: a failed update");
        bool sent = false;   // (the pawn's array was cut by this test: the client is told of that deletion, and of no stat change)
        for (size_t i = updates; i < h.mirror.stat_updates.size(); ++i) sent = sent || !h.mirror.stat_updates[i].changed.empty();
        CHECK(!sent, "...and no stat value is sent");
        bool same = cs->values().size() == at_join.size();
        for (size_t i = 0; same && i < at_join.size(); ++i) same = bits_of(at_join[i].value) == bits_of(cs->values()[i].value);
        CHECK(same && cs->find(kMitigation)->value == before, "the session's stats are as the client has them (the chest's 20 is still in): not committed");
        CHECK(!h.s->failed(), "the session carries on");
    }
    // the same for a stat the ability runtime holds: ring A (MaxStamina +50) comes off and the pawn refuses; the runtime keeps 150
    {
        std::vector<svc::InventoryItem> items = bard_items();
        for (svc::InventoryItem& it : items) {
            if (it.container == Bag && it.item_record_id == fi::kRingA) {
                it.container = Worn;
                it.slot = 10;
            }
        }
        Harness h(record_with(items));
        const sg::CharacterStats* cs = h.s->character_stats();
        CHECK(h.s->abilities() && h.s->abilities()->max_stamina() == 150 && drop_owner_stat_item(h, content_fixture::kMaxStamina), "(joined with ring A; the pawn lost MaxStamina's item)");
        h.request(item_used(4, 10, guid_n(202)), h.pawn_ch);
        CHECK(h.s->inventory().at(Worn, 10) == nullptr && h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0, "the ring is off, the stat update failed");
        CHECK(h.s->abilities()->max_stamina() == 150 && h.s->abilities()->stamina() == 150, "the runtime holds MaxStamina 150 and Stamina 150 still: it takes back what the client was not told");
        CHECK(cs->find(content_fixture::kMaxStamina)->value == 150.0f, "and so do the session's stats");
        CHECK(!h.s->failed() && !h.s->abilities_failed(), "the session and its abilities carry on");
    }
}

// ============================================================================================================================
//  plan D1: item use and levels
// ============================================================================================================================

static repl::DesignDataId adventuring_id() { return repl::DesignDataId::named(content::kGuidAdventuring, content::kTypeLevelingTypeDef, "Adventuring"); }
/// A skill tree's key in the XPTracker's SkillTreePurchasesFastMap: its Guid and TypeId with the hardcoded name 0 (the retail server's replies key every tree so).
static repl::DesignDataId tree_id(int64_t guid) {
    repl::DesignDataId d;
    d.guid = guid;
    d.type_id = content::kTypeSkillTree;
    return d;
}
static repl::DesignDataId universal_id() { return tree_id(content::kGuidUniversalTree); }

static svc::Character record_with_scrolls() { return record_with(bard_items_with_scrolls()); }

/// The PlayerController's RPC names among the chains got[from..] and the pawn's level-up broadcasts since `ups`.
static size_t count_calls(const std::vector<std::string>& calls, const char* name) {
    size_t n = 0;
    for (const std::string& c : calls) n += c == name ? 1 : 0;
    return n;
}

static void test_d1_join_has_the_tracker() {
    std::printf("plan D1: the join carries the XPTracker\n");
    Harness h(record_with_scrolls());
    CHECK(h.mirror.unreadable == 0 && h.mirror.tracker_updates == 1, "the join's XPTracker chain reads: one tracker update");
    const auto xp = h.mirror.tracker.experience(adventuring_id());
    const auto debt = h.mirror.tracker.experience_debt(adventuring_id());
    CHECK(xp && *xp == 0 && debt && *debt == 0 && !h.mirror.tracker.purchased_ranks(universal_id()), "a new character: Adventuring 0 XP, debt 0, no Universal rank");
    CHECK(h.s->progress() == (leveling::Progress{1, 0, 0}) && h.s->class_id() == 2 && h.s->universal_ranks().empty(), "the session plays level 1, 0 XP, a Bard");
    CHECK(!h.got.empty() && h.got.back().channel == h.pc_ch && h.got.size() == 18, "the XPTracker is the join's last chain, on the PlayerController's channel (18 with this record's two Add chains)");
    CHECK(h.sink.progress.empty() && !h.s->progress_changed(), "nothing is saved at the join");
}

static void test_d1_level_scroll() {
    std::printf("plan D1: the level-10 scroll, then the level-25 scroll\n");
    Harness h(record_with_scrolls());
    const sg::CharacterStats* cs = h.s->character_stats();
    const float max_stamina_before = cs->find(content::kGuidMaxStamina)->value;
    const size_t chains = h.got.size();
    const size_t stat_updates = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
    CHECK(h.s->uses_applied() == 1 && h.s->uses_refused() == 0 && h.mirror.unreadable == 0, "the scroll is used: one use applied, everything the server sent reads");
    CHECK(h.s->progress() == (leveling::Progress{10, 418410, 0}) && h.s->levels_gained() == 9, "level 10, 418,410 XP (curve(10) + 10), no debt");
    CHECK(h.s->inventory().at(Bag, 4) == nullptr && mirror_matches(h), "the scroll is gone from bag slot 4, and the client's storages agree");
    const auto xp = h.mirror.tracker.experience(adventuring_id());
    const auto ranks = h.mirror.tracker.purchased_ranks(universal_id());
    CHECK(xp && *xp == 418410 && ranks && ranks->size() == 2 && (*ranks)[0].guid == gm::kSprint1Rank && (*ranks)[1].guid == gm::kStamBoost1Rank &&
              (*ranks)[0].type_id == content::kTypeSkillRank,
          "the client's XPTracker: Adventuring 418,410 and the Universal list with Sprint Training I and Endurance Training I");
    CHECK(h.mirror.level == 10 && h.mirror.level_ups == (std::vector<int>{10}), "the pawn's Level is 10 and BroadcastPlayLevelUpEffects(Adventuring, 10) was sent");
    CHECK(h.s->universal_ranks() == (std::vector<int64_t>{gm::kSprint1Rank, gm::kStamBoost1Rank}), "the session holds the two ranks");
    const auto calls = pc_calls_since(h, chains);
    CHECK(count_calls(calls, "ClientUpdateSkillOnActionbar") == 5 && count_calls(calls, "ClientReceiveStorageEvent") == 1,
          "five ClientUpdateSkillOnActionbar calls (Sprint Training I and Endurance Training I of the Universal tree, Flourish, Resonant Weapon and Fortissimo of the Bard's) and the one storage event (the Remove of the scroll)");
    CHECK(h.mirror.events.size() == 25 + 1 && h.mirror.events.back().type == static_cast<uint8_t>(repl::StorageEventType::Remove), "the event is a Remove");
    std::string why;
    CHECK(stats_match(h, &why) && h.mirror.stat_updates.size() == stat_updates + 1, "the stats of level 10 went out in one update and the client's arrays are the session's");
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    const float max_stamina_after = cs->find(content::kGuidMaxStamina)->value;
    CHECK(max_stamina_after > max_stamina_before && cs->level() == 10 && cs->passives().size() == 1 && cs->passives()[0].mods.size() == 1 && cs->passives()[0].mods[0].value == 10,
          "MaxStamina rose (the level, and Endurance Training I's +10 is among the engine's inputs, as an effect)");
    CHECK(h.mirror.effects_added == 3 && h.mirror.effects_removed == 0, "three CurrentEffects items: the granted passives (Sprint Training's 1 s marker, Endurance Training, Resonant Weapon)");
    size_t infinite = 0, markers = 0;
    for (const auto& [id, e] : h.mirror.effect_items) (e.duration > 1e30f ? infinite : markers) += e.duration == 1.0f || e.duration > 1e30f ? 1 : 0;
    CHECK(infinite == 2 && markers == 1, "two of them infinite (FLT_MAX: the record has no duration), one a 1 s marker (Sprint Training's effect lasts 1 s)");
    CHECK(h.sink.progress.size() == 1 && h.sink.progress[0].second.level == 10 && h.sink.progress[0].second.class_id == 2 &&
              h.sink.progress[0].second.experience.size() == 1 && h.sink.progress[0].second.experience[0].xp == 418410 &&
              h.sink.progress[0].second.experience[0].leveling_type_guid == content::kGuidAdventuring,
          "the progress is handed to the sink at once: level 10, Bard, Adventuring 418,410");
// C.Y.B.CoA

    // the level-25 scroll: 15 levels more, three grants (15, 20, 25)
    const size_t chains2 = h.got.size();
    h.request_both(item_used(3, 5, guid_n(211)), item_activate(3, 5, guid_n(211)));
    CHECK(h.s->progress() == (leveling::Progress{25, 418410 + 14295400 - 418400 + 10, 0}), "level 25, the total raised by curve(25) - curve(10) + 10");
    const auto ranks2 = h.mirror.tracker.purchased_ranks(universal_id());
    CHECK(ranks2 && ranks2->size() == 4 && h.mirror.level == 25 && h.mirror.level_ups == (std::vector<int>{10, 25}), "four Universal ranks (Guard Training I at 15 and Sprint Training II at 20 are the new ones), Level 25, a second broadcast");
    CHECK(count_calls(pc_calls_since(h, chains2), "ClientUpdateSkillOnActionbar") == 2 && stats_match(h) && h.sink.progress.size() == 2, "two more skill calls, the stats agree, saved again");
    // a level-10 scroll at level 25 would be refused by its predicate (the bag has none left: the hostile test covers it with a fresh record)
    h.s->finish("end");
    CHECK(h.sink.progress.size() == 3 && h.sink.inventories.size() == 1, "the end hands the progress over once more, and the inventory");
}

static void test_d1_class_scroll() {
    std::printf("plan D1: the class scrolls\n");
    Harness h(record_with_scrolls());
    const sg::CharacterStats* cs = h.s->character_stats();
    const float strength_bard = cs->find(109183598614138ll)->value;
    // the Bard scroll for a Bard: refused, nothing consumed
    h.request_both(item_used(3, 7, guid_n(213)), item_activate(3, 7, guid_n(213)));
    CHECK(h.s->uses_refused() == 1 && h.s->uses_applied() == 0 && h.s->inventory().at(Bag, 7) && h.s->class_id() == 2 && mirror_matches(h),
          "the Bard scroll for a Bard is refused, the scroll stays");
    // the Tank scroll
    const size_t stat_updates = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 6, guid_n(212)), item_activate(3, 6, guid_n(212)));
    CHECK(h.s->uses_applied() == 1 && h.s->class_id() == 7 && cs->class_enum() == 7 && h.s->inventory().at(Bag, 6) == nullptr, "the Tank scroll: the class is Tank (7), the scroll consumed");
    CHECK(h.mirror.stat_updates.size() == stat_updates + 1 && stats_match(h, nullptr, true) && cs->find(109183598614138ll)->value != strength_bard, "the stats are a Tank's now (Strength differs), and the client has them");
    CHECK(h.s->bootstrap().setup().class_record_guid == 108640201692798ll, "CombatInfo.PrimaryClassId is the Tank's record");
    CHECK(h.s->progress() == (leveling::Progress{1, 0, 0}) && h.mirror.level_ups.empty() && h.mirror.level == 1, "no XP, no level-up");
    CHECK(h.sink.progress.size() == 1 && h.sink.progress[0].second.class_id == 7 && h.sink.progress[0].second.level == 1, "the progress is saved with class 7");
    // and back to Bard with the Bard scroll
    h.request_both(item_used(3, 7, guid_n(213)), item_activate(3, 7, guid_n(213)));
    CHECK(h.s->class_id() == 2 && cs->find(109183598614138ll)->value == strength_bard && stats_match(h, nullptr, true), "the Bard scroll now works: a Bard again, the stats back (the Tank's own resource keeps its item on the client)");
}

static void test_d1_gear_and_consumables() {
    std::printf("plan D1: the gear and consumables scrolls\n");
    Harness h(record_with_scrolls());
    const size_t chains = h.got.size();
    const size_t stat_updates = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 8, guid_n(214)), item_activate(3, 8, guid_n(214)));
    const svc::InventoryItem* chest = h.s->inventory().at(Bag, 8);
    const svc::InventoryItem* legs = h.s->inventory().at(Bag, 12);
    CHECK(h.s->uses_applied() == 1 && chest && chest->item_record_id != gm::kGear01 && h.s->inventory().count(Bag) == 13, "the gear scroll is consumed: 12 bag items became 13");
    CHECK(chest && chest->item_record_id == fi::kChest && chest->durability == 11 && legs && legs->item_record_id == fi::kLegs,
          "the chest took the scroll's slot (the lowest free) and the legs the next free slot, with the record's durability");
    CHECK(mirror_matches(h) && count_calls(pc_calls_since(h, chains), "ClientReceiveStorageEvent") == 3, "a Remove and two Adds; the client's bag agrees");
    CHECK(chest->instance_guid != legs->instance_guid && chest->instance_guid.size() == 32, "fresh instances with their own guids");
    CHECK(h.s->progress() == (leveling::Progress{1, 0, 0}) && h.sink.progress.empty() && h.mirror.stat_updates.size() == stat_updates, "no XP, no save of progress, no stats");
    h.request_both(item_used(3, 9, guid_n(215)), item_activate(3, 9, guid_n(215)));
    const svc::InventoryItem* potions = h.s->inventory().at(Bag, 9);
    CHECK(h.s->uses_applied() == 2 && potions && potions->item_record_id == fi::kPotion && potions->quantity == 20 && mirror_matches(h), "the consumables scroll: a stack of 20 potions where it was");
    // a scroll whose archetype does not consume it
    h.request_both(item_used(3, 11, guid_n(217)), item_activate(3, 11, guid_n(217)));
    const svc::InventoryItem* keep = h.s->inventory().at(Bag, 11);
    CHECK(h.s->uses_applied() == 3 && keep && keep->item_record_id == gm::kKeep && keep->instance_guid == guid_n(217) && h.s->inventory().count(Bag) == 14, "the keep scroll stays, its potions arrive");
    h.s->finish("end");
    CHECK(h.sink.inventories.size() == 1 && h.sink.inventories[0].items.size() == h.s->inventory().size(), "the end saves the inventory with the new items");
}
// (C Y B + CoA)

static void test_d1_refusals() {
    std::printf("plan D1: refused uses change nothing\n");
    Harness h(record_with_scrolls());
    const items::Inventory before = h.s->inventory();
    const auto progress = h.s->progress();
    size_t refused = 0;
    const auto refusal = [&](const char* what, const BunchWriter& req) {
        const size_t chains = h.got.size();
        h.request_both(req, req);
        ++refused;
        CHECK(h.s->uses_refused() + h.s->item_requests_refused() >= refused && h.s->uses_applied() == 0 && h.s->inventory() == before && h.s->progress() == progress &&
                  h.sink.progress.empty() && h.mirror.level_ups.empty(),
              what);
        (void)chains;
    };
    refusal("an action of another type (Callout): refused", item_used(3, 10, guid_n(216)));
    refusal("a stale instance (another guid in the slot): refused", item_used(3, 4, guid_n(999)));
    refusal("an empty slot: refused", item_used(3, 20, guid_n(210)));
    refusal("a slot past the bag: refused", item_used(3, 70, guid_n(210)));
    CHECK(h.mirror.tracker_updates == 1, "no XPTracker update went out");
    // a scroll for a level the character has: a level-10 record using the level-10 scroll
    {
        svc::Character r = record_with_scrolls();
        r.level = 12;
        Harness at12(r);
        at12.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
        CHECK(at12.s->uses_refused() == 1 && at12.s->inventory().at(Bag, 4) && at12.s->progress().level == 12, "the level-10 scroll at level 12 is refused and stays");
        CHECK(g_log.str().find("below level 10") != std::string::npos, "the log says why");
    }
    // a used-up stack: the consumed scroll's slot and guid again
    {
        Harness g(record_with_scrolls());
        g.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
        const auto p = g.s->progress();
        g.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
        CHECK(g.s->uses_applied() == 1 && g.s->progress() == p && g.s->item_requests_refused() == 1, "the scroll used up: the slot is empty, the second use is refused");
    }
    // a full bag: the gear scroll needs two free slots and the scroll's own
    {
        std::vector<svc::InventoryItem> items = bard_items_with_scrolls();
        for (int slot = 12; slot < 70; ++slot) items.push_back(make_item(Bag, slot, fi::kPotion, 300 + static_cast<uint32_t>(slot)));
        Harness full(record_with(items));
        full.request_both(item_used(3, 8, guid_n(214)), item_activate(3, 8, guid_n(214)));
        CHECK(full.s->uses_refused() == 1 && full.s->inventory().at(Bag, 8) && full.s->inventory().at(Bag, 8)->item_record_id == gm::kGear01, "a full bag: the gear scroll is refused and stays");
        full.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
        CHECK(full.s->uses_applied() == 1 && full.s->progress().level == 10, "a level scroll needs no room: used");
    }
    // spam: a burst of uses within the item budget, the rest dropped and counted
    {
        Harness spam(record_with_scrolls());
        std::vector<fake::Bunch> bunches;
        for (int i = 0; i < 12; ++i) bunches.push_back(spam.client.actor(spam.pawn_ch, item_used(3, 10, guid_n(216)), true));
        spam.client_sends(std::move(bunches));
        spam.settle();
        CHECK(spam.s->requests_dropped() >= 4 && spam.s->uses_refused() <= PlayerSession::kItemBurst && !spam.s->failed() && spam.s->inventory() == before,
              "12 uses at once: at most the burst are judged, the rest dropped, nothing changes");
    }
}

static void test_d1_relog() {
    std::printf("plan D1: a record with a level, XP and a class carries them to the join\n");
    svc::Character r = record_with_scrolls();
    r.level = 10;
    r.experience.push_back({content::kGuidAdventuring, 500000, 7});
    Harness h(r);
    CHECK(h.s->progress() == (leveling::Progress{10, 500000, 7}) && h.s->universal_ranks().size() == 2, "level 10 with its XP row: 500,000 XP, debt 7, two ranks granted");
    const auto xp = h.mirror.tracker.experience(adventuring_id());
    const auto debt = h.mirror.tracker.experience_debt(adventuring_id());
    const auto ranks = h.mirror.tracker.purchased_ranks(universal_id());
    CHECK(xp && *xp == 500000 && debt && *debt == 7 && ranks && ranks->size() == 2, "the join's XPTracker says so, the Universal list included");
    // (the mirror reads updates, not the pawn's open: the Level of the join is read from the pawn's state, which is what the open carried)
    const repl::Subobject* base_info = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.base_character_info);
    const repl::Value* level_value = base_info ? base_info->obj.state.get(*base_info->obj.schema, "Level") : nullptr;
    const int64_t* pawn_level = level_value ? level_value->get_if<int64_t>() : nullptr;
    CHECK(pawn_level && *pawn_level == 10 && h.s->character_stats()->level() == 10 && h.s->character_stats()->passives().size() == 1, "the pawn's Level is 10 and the stats were computed with Endurance Training I");
    // a record whose XP trails its level: the threshold
    svc::Character t = record_with_scrolls();
    t.level = 5;
    t.experience.push_back({content::kGuidAdventuring, 100, 0});
    Harness h2(t);
    CHECK(h2.s->progress() == (leveling::Progress{5, 29900, 0}) && h2.s->universal_ranks() == (std::vector<int64_t>{gm::kSprint1Rank}), "a record with 100 XP at level 5 joins at level 5's threshold, Sprint Training I granted");
    // a Tank record
    svc::Character tk = record_with_scrolls();
    tk.class_id = 7;
    tk.class_name = "Tank";
    tk.archetype_id = 17754;
    Harness h3(tk);
    CHECK(h3.s->class_id() == 7 && h3.s->character_stats()->class_enum() == 7, "a Tank record plays as a Tank");
}
// C | Y | B captures, CoA code

static void test_d1_saver_progress() {
    std::printf("plan D1: the progress reaches the character service through the saver\n");
    FakeCharacters api;
    {
        CharacterSaver saver(api, quick());
        Fixture fx;
        fx.world->set_position_sink(&saver);
        Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
        std::string error;
        auto s = PlayerSession::join(conn, *fx.world, record_with_scrolls(), identity(), 0, error);
        CHECK(s != nullptr, "(joined)");
        if (!s) return;
        svc::Progress p;
        p.level = 10;
        p.class_id = 2;
        p.experience.push_back({content::kGuidAdventuring, 418410, 0});
        saver.save_progress(kCyb, p);
        CHECK(wait_until([&] { return api.progress_count() == 1; }) && api.progress[0].first == kCyb && api.progress[0].second == p, "a progress handed to the saver reaches SaveProgress whole");
        CHECK(saver.progress_saved() == 1, "counted as saved");
    }
}

// ============================================================================================================================
//  Gathering stage 2, part "Professions and GM items": the 22 professions, the GM artisan scrolls and the tools, on the real content.db
// ============================================================================================================================

/// The real content.db: AOC_CONTENT_DB, else the working tree's dist/content/content.db; null when there is none (the tests then skip).
static const char* professions_db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef PROFESSIONS_REAL_DB
    if (fs::exists(PROFESSIONS_REAL_DB)) return PROFESSIONS_REAL_DB;
#endif
    return nullptr;
}

namespace g2 {
inline constexpr int64_t kMining25 = 6064632989080814323ll, kMining9 = 6064634615541072001ll, kAlchemy9 = 6064634615657267369ll, kAll25 = 6064632988513402885ll;
inline constexpr int64_t kT3Pickaxe = 6064629376467927053ll;   // Gear_Artisan_Mining_Tool_Pickaxe_T3_0: the Mining Level 25 scroll's tool
inline constexpr int64_t kMining = 108752769996425ll, kAlchemy = 108752757020295ll;
inline constexpr int64_t kLtMining = 109460719621753ll, kLtAlchemy = 109460747540106ll;
inline constexpr int64_t kNovice = 108906339653240ll, kApprentice = 108906340767353ll, kJourneyman = 108906342012538ll;
}  // namespace g2

static repl::DesignDataId leveling_id(int64_t leveling_type) {
    repl::DesignDataId d;
    d.guid = leveling_type;
    d.type_id = content::kTypeLevelingTypeDef;
    return d;
}

/// The Bard's worn kit and the GM artisan scrolls of the real file in bag slots 4 ... (guids 300 ...).
static svc::Character record_with_artisan_scrolls(const std::vector<int64_t>& scrolls) {
    svc::Character c = cyb_record();
    std::vector<svc::InventoryItem> items;
    for (const auto& it : c.items)
        if (it.container == Worn) items.push_back(it);
    for (size_t i = 0; i < scrolls.size(); ++i) items.push_back(make_item(Bag, 4 + static_cast<int>(i), scrolls[i], 300 + static_cast<uint32_t>(i)));
    c.items = std::move(items);
    return c;
}

static std::string profession_name(const professions::Book& book, int64_t leveling_type) {
    for (const professions::Profession& p : book.all())
        if (p.leveling_type == leveling_type) return p.name;
    return "?";
}

static void test_g2_join() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Gathering stage 2: the join's professions: skipped (no real content.db)\n");
        return;
    }
    std::printf("Gathering stage 2: the join lists the professions\n");
    // a new character: 22 entries, all Novice, in the order of the retail join; no profession XP
    {
        Harness h(record_with_artisan_scrolls({}), true, true, path);
        const professions::Book* book = h.fx.world->professions();
        CHECK(book && book->size() == 22 && h.s->professions().valid(), "the world and the session have the 22 professions");
        const auto certs = h.mirror.tracker.certifications();
        bool order = certs.size() == std::size(capture_leveling::kJoinCertifications), novice = true;
        for (size_t i = 0; order && i < certs.size(); ++i) {
            order = certs[i].key.guid == capture_leveling::kJoinCertifications[i].key && certs[i].key.type_id == content::kTypeLevelingTypeDef;
            novice = novice && certs[i].value == g2::kNovice;
        }
        CHECK(order && novice && certs.size() == 22, "the client's LevelingCertificationsFastMap has the 22 professions' leveling types in the retail join's order, each Novice");
        const auto xp = h.mirror.tracker.experience();
        CHECK(xp.size() == 1 && xp[0].key.guid == content::kGuidAdventuring, "the ExperienceFastMap has Adventuring only: a profession with no XP is not listed");
        bool all_novice = true;
        for (const professions::Profession& p : book->all()) all_novice = all_novice && h.s->professions().state(p.guid) == professions::State{};
        CHECK(all_novice, "every profession is Novice, level 1, no XP");
        CHECK(h.sink.progress.empty() && !h.s->progress_changed(), "nothing is saved at the join");
        CHECK(h.mirror.unreadable == 0 && h.mirror.tracker_updates == 1, "the join's XPTracker is still one chain");
    }
    // a character in the state of the retail join (1198 pkt 1510): its certification levels and its professions' XP
    {
        Harness probe(record_with_artisan_scrolls({}), true, true, path);
        const professions::Book& book = *probe.fx.world->professions();
        svc::Character c = record_with_artisan_scrolls({});
        for (const professions::Profession& p : book.all()) {
            int64_t cert = g2::kNovice, xp = 0;
            for (const auto& e : capture_leveling::kJoinCertifications)
                if (e.key == p.leveling_type) cert = e.value;
            for (const auto& e : capture_leveling::kJoinExperience)
                if (e.key == p.leveling_type) xp = e.value;
            const int tier = book.tier_of_guid(cert);
            c.professions.push_back({p.guid, tier, book.level_of(p, xp, tier), xp});
        }
        Harness h(c, true, true, path);
        const auto certs = h.mirror.tracker.certifications();
        bool same = certs.size() == std::size(capture_leveling::kJoinCertifications);
        for (size_t i = 0; same && i < certs.size(); ++i)
            same = certs[i].key.guid == capture_leveling::kJoinCertifications[i].key && certs[i].value == capture_leveling::kJoinCertifications[i].value;
        CHECK(same, "the join's LevelingCertificationsFastMap equals the retail join's: the same 22 keys in the same order, the same certification levels");
        std::map<int64_t, int64_t> want, got;
        for (const auto& e : capture_leveling::kJoinExperience)
            if (profession_name(book, e.key) != "?") want[e.key] = e.value;
        for (const auto& e : h.mirror.tracker.experience())
            if (profession_name(book, e.key.guid) != "?") got[e.key.guid] = e.value;
        CHECK(want.size() == 19 && got == want, "the ExperienceFastMap lists the same 19 professions with the same XP (the 3 with none are not listed)");
        const professions::Professions& p = h.s->professions();
        CHECK(p.level(108752769996425ll) == 19 && p.tier(108752769996425ll) == 1 && p.level(108752769996426ll) == 21 && p.tier(108752769996426ll) == 2 && p.level(108752770061964ll) == 18,
              "Mining is level 19 Apprentice, Lumberjacking 21 Journeyman, Fishing 18 Apprentice (the curve and the tiers fit the retail state)");
    }
}

static void test_g2_scrolls() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Gathering stage 2: the scrolls: skipped (no real content.db)\n");
        return;
    }
    std::printf("Gathering stage 2: the GM artisan scrolls, the XPTracker, the level-ups, the saves, the tool and a relog\n");
    Harness h(record_with_artisan_scrolls({g2::kMining25, g2::kMining9, g2::kAlchemy9, g2::kAll25}), true, true, path);
    const professions::Book& book = *h.fx.world->professions();
    CHECK(h.mirror.unreadable == 0 && h.s->professions().valid(), "(joined)");

    // Alchemy Level 9: level 9, still Novice
    size_t chains = h.got.size();
    const size_t stat_updates0 = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 6, guid_n(302)), item_activate(3, 6, guid_n(302)));
    CHECK(h.s->uses_applied() == 1 && h.s->uses_refused() == 0 && h.mirror.unreadable == 0 && !h.s->failed(), "the Alchemy Level 9 scroll is used");
    CHECK(h.s->professions().state(g2::kAlchemy) == (professions::State{0, 9, 32401}), "Alchemy is Novice level 9 with 32,401 XP (the XP from the start of level 1 to level 9, plus 1)");
    CHECK(h.s->inventory().at(Bag, 6) == nullptr && mirror_matches(h), "the scroll is consumed");
    const auto alchemy_xp = h.mirror.tracker.experience(leveling_id(g2::kLtAlchemy));
    const auto alchemy_cert = h.mirror.tracker.certification(leveling_id(g2::kLtAlchemy));
    CHECK(alchemy_xp && *alchemy_xp == 32401 && alchemy_cert && *alchemy_cert == g2::kNovice, "the client's XPTracker: Alchemy 32,401 XP, Novice");
    const auto adv = h.mirror.tracker.experience(adventuring_id());
    CHECK(adv && *adv == 0 && h.s->progress() == (leveling::Progress{1, 0, 0}) && h.mirror.level == 1, "Adventuring is untouched, the pawn's Level is still 1");
    CHECK(h.mirror.level_ups == (std::vector<int>{9}) && h.mirror.level_up_categories == (std::vector<int64_t>{g2::kLtAlchemy}), "BroadcastPlayLevelUpEffects(Alchemy's leveling type, 9) was sent");
    CHECK(count_calls(pc_calls_since(h, chains), "ClientUpdateSkillOnActionbar") == 0 && h.mirror.stat_updates.size() == stat_updates0, "no skill, no stat changed");
    CHECK(h.sink.progress.size() == 1 && h.sink.progress[0].second.professions.size() == 22, "the progress is saved with all 22 profession rows");
    {
        const auto& rows = h.sink.progress[0].second.professions;
        const auto it = std::find_if(rows.begin(), rows.end(), [](const svc::ProfessionState& r) { return r.profession_guid == g2::kAlchemy; });
        CHECK(it != rows.end() && it->certification_tier == 0 && it->level == 9 && it->xp == 32401 && rows[0].profession_guid < rows[21].profession_guid,
              "Alchemy's row is Novice, level 9, 32,401 XP; the rows are in guid order");
    }
// -- CoA x C.Y.B --

    // Mining Level 25: XP, Promote, XP, Promote, XP, and the T3 pickaxe in the bag
    chains = h.got.size();
    h.request_both(item_used(3, 4, guid_n(300)), item_activate(3, 4, guid_n(300)));
    CHECK(h.s->uses_applied() == 2 && h.s->professions().state(g2::kMining) == (professions::State{2, 25, 266400}), "the Mining Level 25 scroll: Journeyman, level 25, 266,400 XP");
    const auto mining_cert = h.mirror.tracker.certification(leveling_id(g2::kLtMining));
    const auto mining_xp = h.mirror.tracker.experience(leveling_id(g2::kLtMining));
    CHECK(mining_cert && *mining_cert == g2::kJourneyman && mining_xp && *mining_xp == 266400, "the client's XPTracker: Mining Journeyman, 266,400 XP");
    CHECK(h.mirror.level_up_categories == (std::vector<int64_t>{g2::kLtAlchemy, g2::kLtMining}) && h.mirror.level_ups == (std::vector<int>{9, 25}), "a second broadcast: Mining's leveling type, level 25");
    int pickaxe_slot = -1;
    std::string pickaxe_guid;
    for (const svc::InventoryItem& it : h.s->inventory().items()) {
        if (it.container == Bag && it.item_record_id == g2::kT3Pickaxe) {
            pickaxe_slot = it.slot;
            pickaxe_guid = it.instance_guid;
        }
    }
    CHECK(pickaxe_slot >= 0 && h.s->inventory().at(Bag, 4) == nullptr && mirror_matches(h), "the T3 pickaxe arrived in the bag, the scroll is gone, and the client's bag agrees");
    CHECK(count_calls(pc_calls_since(h, chains), "ClientReceiveStorageEvent") == 2, "two storage events: the scroll's Remove and the pickaxe's Add");
    CHECK(h.s->character_stats()->profession_levels().count("Mining") == 1 && h.s->character_stats()->profession_levels().at("Mining") == 25 &&
              h.s->character_stats()->profession_levels().at("Alchemy") == 9,
          "the stat engine answers GetPlayerProfessionLevel with the real levels");
    CHECK(h.sink.progress.size() == 2, "saved again");

    // the tool equips into its slot (the kit's pickaxe goes back to the bag)
    if (pickaxe_slot >= 0) {
        h.step(milliseconds(150));
        h.request_both(item_used(3, pickaxe_slot, pickaxe_guid), item_activate(3, pickaxe_slot, pickaxe_guid));
        CHECK(h.s->inventory().at(Worn, 26) && h.s->inventory().at(Worn, 26)->item_record_id == g2::kT3Pickaxe && mirror_matches(h),
              "the T3 pickaxe is worn in the Pickaxe slot 26, the client's equipment agrees");
    }

    // Mining Level 9 for a level 25 miner: refused, the scroll stays
    const size_t refused = h.s->uses_refused(), applied = h.s->uses_applied();
    h.request_both(item_used(3, 5, guid_n(301)), item_activate(3, 5, guid_n(301)));
    CHECK(h.s->uses_refused() == refused + 1 && h.s->uses_applied() == applied && h.s->inventory().at(Bag, 5) && h.s->inventory().at(Bag, 5)->item_record_id == g2::kMining9,
          "a Level 9 scroll for a level 25 miner is refused and stays");

    // _All: level 25 in every profession (modelled), Journeyman, a broadcast for each profession that gained a level
    const size_t broadcasts = h.mirror.level_ups.size();
    h.request_both(item_used(3, 7, guid_n(303)), item_activate(3, 7, guid_n(303)));
    bool every = true;
    for (const professions::Profession& p : book.all()) {
        const professions::State s = h.s->professions().state(p.guid);
        every = every && s.level == 25 && s.tier == 2;
    }
    CHECK(every && h.s->professions().state(g2::kMining) == (professions::State{2, 25, 266400}), "_All: all 22 professions are level 25 and Journeyman (Mining, already there, is left as it was)");
    const auto certs = h.mirror.tracker.certifications();
    bool journeymen = certs.size() == 22;
    for (const auto& c : certs) journeymen = journeymen && c.value == g2::kJourneyman;
    CHECK(journeymen && h.mirror.tracker.experience().size() == 23, "the client's maps: 22 Journeyman certifications, 22 professions with XP (and Adventuring)");
    CHECK(h.mirror.level_ups.size() == broadcasts + 21, "21 level-up broadcasts (every profession but Mining gained levels)");
    CHECK(h.mirror.unreadable == 0 && !h.s->failed() && h.s->inventory().at(Bag, 7) == nullptr, "everything the server sent reads; the scroll is consumed");

    // the end hands the progress over once more, and a relog from the saved rows plays the same professions
    h.s->finish("end");
    CHECK(h.sink.progress.size() >= 3, "(saved)");
    svc::Character relog = record_with_artisan_scrolls({});
    relog.professions = h.sink.progress.back().second.professions;
    relog.experience = h.sink.progress.back().second.experience;
    Harness again(relog, true, true, path);
    bool same = again.s->professions().valid();
    for (const professions::Profession& p : book.all()) same = same && again.s->professions().state(p.guid) == h.s->professions().state(p.guid);
    CHECK(same, "a relog from the saved rows plays the same 22 professions");
    const auto recerts = again.mirror.tracker.certifications();
    bool rejourney = recerts.size() == 22;
    for (const auto& c : recerts) rejourney = rejourney && c.value == g2::kJourneyman;
    const auto re_xp = again.mirror.tracker.experience(leveling_id(g2::kLtMining));
    CHECK(rejourney && re_xp && *re_xp == 266400 && again.mirror.tracker.experience().size() == 23, "and the join lists them: Journeyman everywhere, Mining 266,400 XP");
}

static void test_g2_saver_round_trip() {
    const char* path = professions_db_path();
    if (!path) {
        std::printf("Gathering stage 2: the saver round trip: skipped (no real content.db)\n");
        return;
    }
    std::printf("Gathering stage 2: the professions reach the character service through the saver and come back\n");
    // the session hands the progress to its sink; a real CharacterSaver hands it to the (fake) character service, whole
    Harness h(record_with_artisan_scrolls({g2::kMining25}), true, true, path);
    h.request_both(item_used(3, 4, guid_n(300)), item_activate(3, 4, guid_n(300)));
    CHECK(h.s->professions().state(g2::kMining) == (professions::State{2, 25, 266400}) && !h.sink.progress.empty(), "the scroll is used and the progress is handed over");
    FakeCharacters api;
    svc::Progress saved;
    {
        CharacterSaver saver(api, quick());
        saver.save_progress(kCyb, h.sink.progress.back().second);
        CHECK(wait_until([&] { return api.progress_count() == 1; }), "the saver gives it to SaveProgress");
        CHECK(saver.progress_saved() == 1, "counted as saved");
    }
    {
        std::lock_guard<std::mutex> l(api.mu);
        if (!api.progress.empty()) saved = api.progress.back().second;
    }
    CHECK(saved.professions.size() == 22 && saved == h.sink.progress.back().second, "the character service got all 22 profession rows, the progress whole");
    svc::Character relog = record_with_artisan_scrolls({});
    relog.professions = saved.professions;
    Harness again(relog, true, true, path);
    CHECK(again.s->professions().state(g2::kMining) == (professions::State{2, 25, 266400}), "a relog from the character service's rows: Mining Journeyman, level 25, 266,400 XP");
    const auto cert = again.mirror.tracker.certification(leveling_id(g2::kLtMining));
    CHECK(cert && *cert == g2::kJourneyman, "the join's XPTracker says so");
}

// ============================================================================================================================
//  plan D1, stage 2: talents, the action bar, KnownAbilities per class, the class change, the weapon swap and regeneration
// ============================================================================================================================
// CoA.C:Y:B

namespace tl = aoc::worldd::talents;

static const tl::Catalog& catalog_of(const Harness& h) { return *h.fx.world->talents(); }

static int64_t tree_guid(const Harness& h, const char* name) {
    const tl::Tree* t = catalog_of(h).tree_named(name);
    return t ? t->guid : 0;
}

/// The first rank of the node `node` of the tree `tree`.
static int64_t rank_named(const Harness& h, const char* tree, const char* node) {
    if (const tl::Tree* t = catalog_of(h).tree_named(tree)) {
        for (const tl::Node& n : t->nodes) {
            if (n.name == node && !n.ranks.empty()) return n.ranks.front().guid;
        }
    }
    return 0;
}

/// The first node of a tree that has no gate and a rank `want` accepts.
static const tl::Node* node_where(const Harness& h, const char* tree, const std::function<bool(const tl::Node&)>& want) {
    if (const tl::Tree* t = catalog_of(h).tree_named(tree)) {
        for (const tl::Node& n : t->nodes) {
            const bool gated = std::any_of(n.requires_skills.begin(), n.requires_skills.end(), [](int64_t r) { return r != 0; }) || !n.requires_nodes.empty();   // (one that names nothing is met)
            if (!n.auto_granted && !n.ranks.empty() && !gated && n.point_requirement == 0 && n.level_requirement <= 3 && want(n)) return &n;
        }
    }
    return nullptr;
}

static const tl::Node* node_named(const Harness& h, const char* tree, const char* node) {
    if (const tl::Tree* t = catalog_of(h).tree_named(tree)) {
        for (const tl::Node& n : t->nodes) {
            if (n.name == node) return &n;
        }
    }
    return nullptr;
}

/// The ability of the (first) rank `rank` in the Bard tree.
static int64_t menacing_ability_of(const Harness& h, int64_t rank) {
    const auto ref = catalog_of(h).find(tree_guid(h, "Bard"), rank);
    return ref.ok() ? ref.rank->ability : 0;
}

struct Buy {
    int64_t rank, tree;
    bool    purchase;
};
static repl::SkillBuild build_of(std::initializer_list<Buy> entries) {
    repl::SkillBuild b;
    for (const Buy& e : entries) {
        repl::SkillRankChange c;
        c.rank.guid = e.rank;
        c.rank.type_id = content::kTypeSkillRank;
        c.tree.guid = e.tree;
        c.tree.type_id = content::kTypeSkillTree;
        c.tree.name = repl::NameValue::plain("Prototype_Assassin_SkillTable");   // what a build's class tree id carries: never read
        c.leveling_type.guid = content::kGuidAdventuring;
        c.leveling_type.type_id = content::kTypeLevelingTypeDef;
        c.purchase = e.purchase;
        b.changes.push_back(c);
    }
    return b;
}

static BunchWriter purchase_rpc(const repl::SkillBuild& b) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerPurchaseSkillBuild");
    const auto params = repl::purchase_skill_build_params(*rpc, b);
    return params ? actor_rpc(pawn, "ServerPurchaseSkillBuild", *params) : BunchWriter();
}

static BunchWriter set_slot_rpc(const repl::ActionbarSlot& slot) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerSetActionbarSlotUseData");
    const auto params = repl::set_actionbar_slot_params(*rpc, slot);
    return params ? actor_rpc(pc, "ServerSetActionbarSlotUseData", *params) : BunchWriter();
}

static BunchWriter ranged_rpc(bool ranged) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerSetUseRangedWeapon");
    const auto params = repl::set_use_ranged_weapon_params(*rpc, ranged);
    return params ? actor_rpc(pawn, "ServerSetUseRangedWeapon", *params) : BunchWriter();
}

static BunchWriter client_load_rpc() { return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerRequestClientLoad", repl::RpcParams{}); }

static void advance(Harness& h, int ms) {
    for (int i = 0; i < ms / 20; ++i) h.step(milliseconds(20));
}

static std::vector<std::string> sequence_since(const Harness& h, size_t from) { return std::vector<std::string>(h.mirror.sequence.begin() + static_cast<std::ptrdiff_t>(from), h.mirror.sequence.end()); }

/// The Bard's Instrument talent (a slot of three: Flute, Fiddle, Drum; it costs no point) that the instrument songs need.
static constexpr int64_t kBardTree = 6064629885900499588ll, kInstrumentFlute = 6064630871707615237ll, kInstrumentFiddle = 6064630871711875083ll;

/// A level-`level` Bard that has picked the Flute (the Music_* songs of the tests below need an Instrument picked, as in the game).
static svc::Character bard_at(int level) {
    svc::Character c = record_with_scrolls();
    c.level = level;
    c.skill_ranks.push_back({kBardTree, {kInstrumentFlute}});
    return c;
}

static int count_tag(const std::vector<std::string>& seq, const char* tag) {
    return static_cast<int>(std::count(seq.begin(), seq.end(), std::string(tag)));
}

/// The client's list of a tree's ranks (the XPTracker the mirror rebuilt).
static std::vector<int64_t> client_list(const Harness& h, int64_t tree) {
    std::vector<int64_t> out;
    if (const auto ids = h.mirror.tracker.purchased_ranks(tree_id(tree))) {
        for (const repl::DesignDataId& d : *ids) out.push_back(d.guid);
    }
    return out;
}

/// What the client holds of the action bar, as a bar (the slots it was sent).
static bool client_bar_is(const Harness& h, const actionbar::Bar& bar) {
    const auto slots = bar.slots();
    if (slots.size() != h.mirror.bar.size()) return false;
    for (const repl::ActionbarSlot& s : slots) {
        const auto it = h.mirror.bar.find({s.bank_index, s.slot_index});
        if (it == h.mirror.bar.end() || !(it->second == s.use)) return false;
    }
    return true;
}
// C:Y:B : CoA

/// A payload's slots by bank then slot (the captured payload is not in that order: retail's arrays are not sorted).
static std::vector<repl::ActionbarSlot> sorted_slots(std::vector<repl::ActionbarSlot> slots) {
    std::sort(slots.begin(), slots.end(), [](const repl::ActionbarSlot& a, const repl::ActionbarSlot& b) {
        return a.bank_index != b.bank_index ? a.bank_index < b.bank_index : a.slot_index < b.slot_index;
    });
    return slots;
}

/// A payload's slots as a set: by bank then slot, each once (the weapon bank is listed three times, as retail lists it).
static std::vector<repl::ActionbarSlot> distinct_slots(std::vector<repl::ActionbarSlot> slots) {
    slots = sorted_slots(std::move(slots));
    slots.erase(std::unique(slots.begin(), slots.end(), [](const repl::ActionbarSlot& a, const repl::ActionbarSlot& b) {
                    return a.bank_index == b.bank_index && a.slot_index == b.slot_index && a.use == b.use;
                }),
                slots.end());
    return slots;
}

static std::optional<repl::ActionbarPayload> captured_default_payload() {
    const auto bytes = bs::captured::bytes_of(bs::captured::kActionBarBlock);
    const auto views = repl::read_content_blocks(bytes.data(), bs::captured::kActionBarBlock.bits, 0);
    if (!views || views->empty()) return std::nullopt;
    const repl::BlockView& v = (*views)[0];
    size_t p = v.payload_pos;
    const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), bytes.data(), v.payload_pos + v.payload_bits, p);
    if (!calls || calls->empty()) return std::nullopt;
    return repl::parse_actionbar_payload((*calls)[0]);
}

static void test_d1s2_join() {
    std::printf("plan D1, stage 2: the join lists the character's trees, abilities and bar\n");
    Harness h(record_with_scrolls());
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
    const std::vector<int64_t> captured = bs::captured_bard_known_abilities();
    const auto records = bs::captured_bard_records_to_load();
    CHECK(h.s->known_abilities() == captured && h.mirror.known == captured, "a level-1 Bard's KnownAbilities are the captured Bard's, in the session and on the client");
    // the records: the captured 58 and, after the Book's combo, the combo of the two-hander this record carries in its bag (kit weapons only in the capture)
    std::vector<int64_t> expected_records;
    for (size_t i = 0; i < records.size(); ++i) {
        expected_records.push_back(records[i].guid);
        if (i == 12) expected_records.push_back(6064630480735371270ll);   // Gear_Weapon_Sword_2H_Starter's combo, carried in the bag
    }
    if (h.s->record_abilities() != expected_records || h.mirror.records != expected_records) {
        std::printf("    session %zu, client %zu, expected %zu\n", h.s->record_abilities().size(), h.mirror.records.size(), expected_records.size());
        for (size_t i = 0; i < expected_records.size(); ++i) {
            const int64_t a = i < h.s->record_abilities().size() ? h.s->record_abilities()[i] : 0, b = i < h.mirror.records.size() ? h.mirror.records[i] : 0;
            if (a != expected_records[i] || b != expected_records[i]) {
                std::printf("    first difference at %zu: session %lld, client %lld, expected %lld\n", i, static_cast<long long>(a), static_cast<long long>(b),
                            static_cast<long long>(expected_records[i]));
                break;
            }
        }
    }
    CHECK(h.s->record_abilities() == expected_records && h.mirror.records == expected_records && records.size() == 58,
          "...and the records the captured 58 with the bag's two-hander's combo after the Book's (as the retail character's carried weapons: after the combo, before the tail)");
    const int64_t bard = tree_guid(h, "Bard");
    CHECK(client_list(h, bard) == std::vector<int64_t>{rank_named(h, "Bard", "Bard_Rhetoric")}, "the XPTracker lists the Bard tree with its one auto-granted rank (Rhetoric)");
    size_t professions = 0;
    for (const char* name : {"Mining", "Herbalism", "Lumberjacking", "Hunting", "Fishing"}) professions += client_list(h, tree_guid(h, name)).size();
    CHECK(professions == 9 && client_list(h, content::kGuidUniversalTree).empty(), "...and the gathering trees' nine ranks (1, 1, 1, 3 and 3), no Universal rank at level 1");
    CHECK(h.s->talents() && h.s->talents()->purchased().empty(), "nothing is bought");
    // the bar: the default is the captured bar
    const auto cap = captured_default_payload();
    CHECK(cap && !cap->auto_granted && cap->slots.size() == 5, "(the captured default bar decodes: five slots)");
    if (cap && h.s->action_bar().slots() != sorted_slots(cap->slots)) {
        const auto describe = [](const std::vector<repl::ActionbarSlot>& slots, const char* who) {
            for (const repl::ActionbarSlot& x : slots) {
                std::printf("    %s: bank %d slot %d type %u instigator %u ability %lld rank %lld pet %lld item %lld combo %u\n", who, x.bank_index, x.slot_index,
                            static_cast<unsigned>(x.use.use_type), static_cast<unsigned>(x.use.instigator), static_cast<long long>(x.use.ability_id),
                            static_cast<long long>(x.use.skill_rank_id), static_cast<long long>(x.use.pet_command_id), static_cast<long long>(x.use.item_record_id),
                            static_cast<unsigned>(x.use.combo_button));
            }
        };
        describe(h.s->action_bar().slots(), "session ");
        describe(cap->slots, "captured");
    }
    CHECK(cap && h.s->action_bar().slots() == sorted_slots(cap->slots), "the session's default bar for a level-1 Bard is the captured one (as a set of slots): QuickWit on bank 0 slot 0, the weapon bank's Guard, Sprint, swap and Dodge");
    // ServerRequestClientLoad sends it once, as one payload
    const size_t payloads = h.mirror.bar_payloads.size();
    const size_t from = h.mirror.sequence.size();
    h.request(client_load_rpc(), h.pc_ch);
    CHECK(h.mirror.bar_payloads.size() == payloads + 1 && count_tag(sequence_since(h, from), "P") == 1 && client_bar_is(h, h.s->action_bar()), "ServerRequestClientLoad: the bar goes out once, as one payload, and the client has exactly it");
    CHECK(cap && distinct_slots(h.mirror.bar_payloads.back().slots) == sorted_slots(cap->slots), "...and it is the captured payload, slot for slot (retail lists bank 4's slot 4 before slot 3; the set is the same)");
    CHECK(h.sink.progress.empty() && !h.s->progress_changed(), "nothing is saved at the join");
}
// CoA + C Y B

static void test_d1s2_purchase() {
    std::printf("plan D1, stage 2: a purchase is answered as the retail server answers\n");
    Harness h(bard_at(3));   // two Archetype, two Stamina and two Weapon points; Rhetoric and Flourish granted
    const int64_t bard = tree_guid(h, "Bard");
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing"), shielding = rank_named(h, "Bard", "Bard_Dance_Shielding");
    const tl::Node* menacing_node = node_named(h, "Bard", "Bard_Music_Menacing");
    CHECK(menacing_node->name == "Bard_Music_Menacing" && h.s->known_abilities().size() == 58, "(a level-3 Bard knows 58 abilities: Flourish joined the captured 57)");
    CHECK(client_list(h, bard).size() == 3 && h.s->action_bar().at(0, 1) && h.s->action_bar().at(0, 1)->use_type == repl::ActionbarUseType::Skill, "(Rhetoric, Flourish and the Flute are listed, and the two skills are on the bar)");
    const size_t known_before = h.mirror.known.size();

    // a rank with an ability: A, L, U (retail's shape 1)
    size_t from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 1 && h.mirror.unreadable == 0, "the purchase is applied and everything the server sent reads");
    CHECK(sequence_since(h, from) == (std::vector<std::string>{"A", "L", "U"}), "the reply is the abilities' update, the tree's list and the skill update, in that order");
    CHECK(h.mirror.known.size() == known_before + 1 && h.mirror.known.back() == menacing_node->ranks[0].ability && h.mirror.known == h.s->known_abilities() &&
              h.mirror.records.size() == h.s->record_abilities().size() && h.mirror.records.back() == menacing_node->ranks[0].ability,
          "KnownAbilities and the records grow by the rank's ability, appended at the end (the client's lists are the session's)");
    CHECK(client_list(h, bard) == (std::vector<int64_t>{rank_named(h, "Bard", "Bard_Rhetoric"), catalog_of(h).auto_ranks(*catalog_of(h).tree(bard), 3)[1].rank->guid, kInstrumentFlute, menacing}),
          "the tree's whole list: the two granted ranks, the Flute the character picked, then the bought one");
    CHECK(h.mirror.skill_updates.size() == 1 && h.mirror.skill_updates[0].skill_id == menacing_node->skill && h.mirror.skill_updates[0].skill_tree_id == bard &&
              h.mirror.skill_updates[0].rank_id == 0 && !h.mirror.skill_updates[0].is_refunding && h.mirror.skill_updates[0].use_hint,
          "ClientUpdateSkillOnActionbar: the Skill, the tree, the default rank id, not a refund, with the hint");
    CHECK(h.s->action_bar().at(0, 2) && h.s->action_bar().at(0, 2)->skill_rank_id == menacing && h.s->action_bar().at(0, 2)->ability_id == menacing_node->ranks[0].ability,
          "the skill is on the first empty slot of the class pages (a design choice, not retail's)");
    CHECK(h.sink.progress.size() == 1 && h.sink.progress[0].second.replace_skill_ranks && h.sink.progress[0].second.skill_ranks.size() == 1 &&
              h.sink.progress[0].second.skill_ranks[0].tree_guid == bard && h.sink.progress[0].second.skill_ranks[0].rank_guids == (std::vector<int64_t>{kInstrumentFlute, menacing}) &&
              h.sink.progress[0].second.replace_action_bar && h.sink.progress[0].second.action_bar.size() == 7,
          "the ranks and the bar are handed to the sink at once (the weapon bank's four slots, QuickWit, Flourish and Menacing)");

    // a second rank, which needs a point spent in the tree first
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{shielding, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 2 && sequence_since(h, from) == (std::vector<std::string>{"A", "L", "U"}) && h.s->talents()->points_left(*catalog_of(h).tree(bard), catalog_of(h).point_type(tl::Point::Archetype)) == 0,
          "Dance of Shielding (a point spent in the tree first): bought with the second point");

    // a refund of an ability rank: A, L and no U
    from = h.mirror.sequence.size();
    const size_t updates = h.mirror.skill_updates.size();
    h.request(purchase_rpc(build_of({{shielding, bard, false}})), h.pawn_ch);
    CHECK(sequence_since(h, from) == (std::vector<std::string>{"A", "L"}) && h.mirror.skill_updates.size() == updates, "a refund: the abilities' update and the list, no skill update");
    CHECK(h.mirror.known == h.s->known_abilities() && h.mirror.known.back() == menacing_node->ranks[0].ability && !h.s->action_bar().has_skill(shielding) &&
              h.s->talents()->points_left(*catalog_of(h).tree(bard), catalog_of(h).point_type(tl::Point::Archetype)) == 1,
          "the ability left the lists (a removal at the tail: the count shrinks), its slot is free, the point is back");

    // a swap: the refund and a purchase in one call
    from = h.mirror.sequence.size();
    const int64_t leech = rank_named(h, "Bard", "Bard_Music_Leech");
    h.request(purchase_rpc(build_of({{leech, bard, true}, {menacing, bard, false}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 4 && count_tag(sequence_since(h, from), "A") == 1 && count_tag(sequence_since(h, from), "L") == 1 && count_tag(sequence_since(h, from), "U") == 1,
          "a swap (refund Menacing, buy Leech) is one reply: one abilities update, one list, one skill update");
    CHECK(h.mirror.known == h.s->known_abilities() && std::find(h.mirror.known.begin(), h.mirror.known.end(), menacing_node->ranks[0].ability) == h.mirror.known.end() &&
              client_list(h, bard).back() == leech,
          "the client's lists have Leech's ability and not Menacing's; its tree list ends with Leech");

    // a respec: every bought rank in one call
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{leech, bard, false}})), h.pawn_ch);
    CHECK(h.s->talents()->purchased(bard) == std::vector<int64_t>{kInstrumentFlute} && client_list(h, bard).size() == 3 && count_tag(sequence_since(h, from), "U") == 0 &&
              h.s->known_abilities().size() == 58 && h.mirror.known == h.s->known_abilities(),
          "a respec leaves the two granted ranks and the Flute (a free pick, not in the call) listed, and the abilities of the level only: no skill update");
    h.s->finish("end");
    CHECK(!h.sink.progress.empty() && h.sink.progress.back().second.replace_skill_ranks && h.sink.progress.back().second.skill_ranks.size() == 1 &&
              h.sink.progress.back().second.skill_ranks[0].rank_guids == std::vector<int64_t>{kInstrumentFlute},
          "the end hands the ranks over once more: the Flute alone");
}

static void test_d1s2_passives() {
    std::printf("plan D1, stage 2: a passive's effect item, a marker's end\n");
    Harness h(bard_at(3));
    const int64_t universal = content::kGuidUniversalTree;
    // a Universal rank: a 1 s marker; a weapon rank of the line in hand (the Scepter): 0.1 s or infinite as its record says
    const tl::Node* un = node_where(h, "Universal", [](const tl::Node& n) {
        return n.ranks.front().effect != 0 && !n.ranks.front().effect_infinite && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity == 2;
    });
    const tl::Node* wp = node_where(h, "Weapon_1H_Scepter", [](const tl::Node& n) {
        return n.ranks.front().effect != 0 && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity <= 2;
    });
    CHECK(un && wp, un ? "(the fixture has a Scepter weapon rank to buy)" : "(the fixture has a Universal marker rank to buy)");
    if (!un || !wp) return;
    size_t from = h.mirror.sequence.size();
    const size_t items_before = h.mirror.effect_items.size();
    h.request(purchase_rpc(build_of({{un->ranks.front().guid, universal, true}})), h.pawn_ch);
    const auto seq = sequence_since(h, from);
    CHECK(count_tag(seq, "E+") == 1 && count_tag(seq, "L") == 1 && h.mirror.effect_items.size() == items_before + 1 && seq.back() == (un->auto_hotbar ? "U" : "L"),
          "a Universal rank: its effect item, the list and (the node has the flag) the skill update");
    const auto item = h.mirror.effect_items.rbegin()->second;
    CHECK(item.guid == un->ranks.front().effect && std::fabs(item.duration - static_cast<float>(un->ranks.front().effect_duration)) < 1e-6f && item.duration == 1.0f,
          "the item is the rank's effect with the record's duration: a 1 s marker");
    CHECK(h.s->effect_markers_pending() == 1, "the session has the marker's end to send");
    advance(h, 1100);
    CHECK(h.mirror.effects_removed >= 1 && h.mirror.effect_items.size() == items_before && h.s->effect_markers_pending() == 0, "a second later the server removes the marker's item itself");
// -- C | O | A . C Y B --

    from = h.mirror.sequence.size();
    const size_t before_w = h.mirror.effect_items.size();
    h.request(purchase_rpc(build_of({{wp->ranks.front().guid, tree_guid(h, "Weapon_1H_Scepter"), true}})), h.pawn_ch);
    CHECK(count_tag(sequence_since(h, from), "E+") == 1 && h.mirror.effect_items.size() == before_w + 1, "a rank of the weapon line in hand (the Scepter): its effect item");
    if (wp->ranks.front().effect_infinite) {
        const auto w = h.mirror.effect_items.rbegin()->second;
        CHECK(w.duration > 1e30f && h.s->effect_markers_pending() == 0, "an infinite effect: FLT_MAX and no end");
    } else {
        advance(h, 400);
        CHECK(h.mirror.effect_items.size() == before_w, "a marker: its item is gone 0.1 s later");
    }
    // a weapon rank of a line that is not in hand: no item (retail sent none)
    const tl::Node* other = node_where(h, "Weapon_2H_Axe", [](const tl::Node& n) { return n.ranks.front().effect != 0 && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity <= 2; });
    if (other) {
        from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{other->ranks.front().guid, tree_guid(h, "Weapon_2H_Axe"), true}})), h.pawn_ch);
        const auto s2 = sequence_since(h, from);
        CHECK(count_tag(s2, "E+") == 0 && count_tag(s2, "L") == 1 && h.s->talent_requests_applied() == 3, "a rank of an axe tree while a scepter is in hand: the list, no effect item");
    }
    // a refund whose item is gone sends no removal; a refund of a passive whose item is on the pawn removes it
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{un->ranks.front().guid, universal, false}})), h.pawn_ch);
    CHECK(count_tag(sequence_since(h, from), "E-") == 0 && count_tag(sequence_since(h, from), "L") == 1, "a refund of a marker that has ended: the list alone (L, as retail's weapon and Universal refunds)");
}

static void test_d1s2_refusals() {
    std::printf("plan D1, stage 2: refused purchases change nothing\n");
    const auto state = [](const Harness& h) {
        return std::make_tuple(h.s->known_abilities(), h.s->record_abilities(), h.s->talents()->purchased(), h.s->action_bar().slots().size(), h.s->progress());
    };
    {
        Harness h(bard_at(1));
        const int64_t bard = tree_guid(h, "Bard");
        const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
        const auto before = state(h);
        const auto list = client_list(h, bard);
        size_t from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 1 && h.s->talent_requests_applied() == 0 && state(h) == before, "no points at level 1: refused, nothing changes");
        CHECK(sequence_since(h, from) == std::vector<std::string>{"L"} && client_list(h, bard) == list, "the client is corrected: the tree's unchanged list is sent again, nothing else");
        CHECK(h.sink.progress.empty() && !h.s->failed(), "nothing is saved, the session carries on");
// CoA and C | Y | B

        // the other refusals, one request each (the resync budget lets four corrections through at once)
        const int64_t rhetoric = rank_named(h, "Bard", "Bard_Rhetoric");
        const int64_t shielding = rank_named(h, "Bard", "Bard_Dance_Shielding");
        struct Case {
            const char* what;
            repl::SkillBuild build;
        };
        std::vector<Case> cases;
        cases.push_back({"a refund of an auto-granted rank", build_of({{rhetoric, bard, false}})});
        cases.push_back({"a rank twice in one request", build_of({{menacing, bard, true}, {menacing, bard, true}})});
        cases.push_back({"an unknown rank", build_of({{424242, bard, true}})});
        cases.push_back({"an unknown tree", build_of({{menacing, 424242, true}})});
        cases.push_back({"a refund of a rank that is not bought", build_of({{shielding, bard, false}})});
        cases.push_back({"a profession tree", build_of({{rank_named(h, "Mining", "Mining_SurveyPylon"), tree_guid(h, "Mining"), true}})});
        cases.push_back({"another class's tree", build_of({{rank_named(h, "Tank", "Tank_Grit"), tree_guid(h, "Tank"), false}})});
        cases.push_back({"the auto-granted rank bought", build_of({{rhetoric, bard, true}})});
        size_t refused = 1;
        for (const Case& c : cases) {
            h.step(milliseconds(2100));   // the resync budget refills
            h.request(purchase_rpc(c.build), h.pawn_ch);
            ++refused;
            const std::string msg = std::string(c.what) + ": refused, nothing changes";
            CHECK(h.s->talent_requests_refused() == refused && state(h) == before && h.s->talent_requests_applied() == 0, msg.c_str());
        }
    }
    {
        Harness h(bard_at(25));   // points are no issue: the gates are
        const int64_t bard = tree_guid(h, "Bard");
        const int64_t shielding = rank_named(h, "Bard", "Bard_Dance_Shielding"), conflict = rank_named(h, "Bard", "Bard_Story_Conflict");
        const auto before = state(h);
        h.request(purchase_rpc(build_of({{shielding, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 1 && state(h) == before, "a missing prerequisite (a point spent in the tree) is refused");
        h.step(milliseconds(2100));   // the resync budget refills
        h.request(purchase_rpc(build_of({{conflict, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 2 && state(h) == before, "a story needs ten points spent in the tree: refused");
        h.step(milliseconds(2100));
        h.request(purchase_rpc(build_of({{424242, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 3 && state(h) == before, "an unknown Guid: refused");
        h.step(milliseconds(2100));
        h.request(purchase_rpc(build_of({{rank_named(h, "Bard", "Bard_Rhetoric"), bard, false}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 4 && state(h) == before, "a refund of an auto-granted rank: refused");
        h.step(milliseconds(2100));
        const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
        h.request(purchase_rpc(build_of({{menacing, bard, true}, {menacing, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 5 && state(h) == before, "a rank twice in one request: refused whole");
        h.step(milliseconds(2100));
        // (the cap on a request's size is the plan's: test_worldd_talents refuses 257 entries; a request that long is partial bunches the fake client does not send)
        h.request(purchase_rpc(build_of({{menacing, bard, false}, {shielding, bard, false}, {conflict, bard, false}, {menacing, bard, false}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 6 && state(h) == before && !h.s->failed(), "a refund of ranks that are not bought, one of them twice: refused");
        h.step(milliseconds(2100));
        h.request(purchase_rpc(build_of({{rank_named(h, "Mining", "Mining_SurveyPylon"), tree_guid(h, "Mining"), true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 7 && state(h) == before, "a profession tree: refused");
        h.step(milliseconds(2100));
        h.request(purchase_rpc(build_of({{rank_named(h, "Tank", "Tank_Grit"), tree_guid(h, "Tank"), true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 8 && state(h) == before, "another class's tree: refused");
        h.step(milliseconds(2100));
        // an empty request
        h.request(purchase_rpc(build_of({})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 9 && state(h) == before, "an empty request: refused");
        // dependents: bought in order, refunded in the wrong one
        h.step(milliseconds(2100));
        h.request(purchase_rpc(build_of({{menacing, bard, true}, {shielding, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_applied() == 1 && h.s->talents()->purchased(bard).size() == 3, "(Menacing and Shielding bought in one request, beside the Flute: the second counts the first's point)");
    }
    {
        // spam within the budget: twelve requests in one datagram
        Harness h(bard_at(25));
        const int64_t bard = tree_guid(h, "Bard");
        const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
        std::vector<fake::Bunch> burst;
        for (int i = 0; i < 12; ++i) burst.push_back(h.client.actor(h.pawn_ch, purchase_rpc(build_of({{menacing, bard, i % 2 == 0}})), true));
        h.client_sends(std::move(burst));
        h.settle();
        CHECK(h.s->requests_dropped() >= 4 && h.s->talent_requests_applied() + h.s->talent_requests_refused() <= PlayerSession::kTalentBurst && !h.s->failed(),
              "twelve purchases at once: at most the burst are judged, the rest dropped and counted");
        CHECK(h.s->talents()->purchased(bard).size() <= 1, "(and the state is one the rules allow)");
    }
}

static void test_d1s2_bar() {
    std::printf("plan D1, stage 2: the action bar is stored, saved and sent\n");
    Harness h(bard_at(1));
    const auto slot = [](int bank, int s, uint8_t type, int64_t ability = 0, int64_t rank = 0) {
        repl::ActionbarSlot x;
        x.bank_index = bank;
        x.slot_index = s;
        x.use.use_type = type;
        x.use.ability_id = ability;
        x.use.skill_rank_id = rank;
        return x;
    };
    const size_t sent = h.got.size();
    h.request(set_slot_rpc(slot(0, 3, repl::ActionbarUseType::Ability, content::kGuidSprint)), h.pc_ch);
    CHECK(h.s->bar_sets_applied() == 1 && h.s->action_bar().at(0, 3) && h.s->action_bar().at(0, 3)->ability_id == content::kGuidSprint, "ServerSetActionbarSlotUseData: the slot is stored");
    CHECK(pc_calls_since(h, sent).empty() && h.sink.progress.size() == 1 && h.sink.progress[0].second.replace_action_bar && h.sink.progress[0].second.action_bar.size() == 6,
          "no reply to it; the bar (six slots now) is handed to the sink");
    // a clear of the slot QuickWit is on
    h.request(set_slot_rpc(slot(0, 0, repl::ActionbarUseType::None)), h.pc_ch);
    CHECK(!h.s->action_bar().at(0, 0) && h.s->action_bar().size() == 5, "a UseType of None clears a slot");
    // out of range
    const size_t refused = h.s->bar_sets_refused();
    for (const repl::ActionbarSlot& bad : {slot(20, 0, 1, 5), slot(0, 16, 1, 5), slot(-1, 0, 1, 5), slot(0, -3, 1, 5), slot(30000, 30000, 1, 5)}) {
        h.step(milliseconds(120));
        h.request(set_slot_rpc(bad), h.pc_ch);
    }
    CHECK(h.s->bar_sets_refused() == refused + 5 && h.s->action_bar().size() == 5 && !h.s->failed(), "a bank or slot out of range (five of them): refused and counted, the bar is as it was");
    // an enumeration the client does not have is refused, too
    repl::ActionbarSlot odd = slot(1, 1, 1, 5);
    odd.use.instigator = 7;
    h.step(milliseconds(120));
    h.request(set_slot_rpc(odd), h.pc_ch);
    CHECK(h.s->bar_sets_refused() == refused + 6 && !h.s->action_bar().at(1, 1), "an instigator the client does not have: refused");
    // a skill the client places itself is on the bar once
    const int64_t flourish_rank = rank_named(h, "Bard", "Bard_Rhetoric");
    h.step(milliseconds(120));
    h.request(set_slot_rpc(slot(1, 4, repl::ActionbarUseType::Skill, 6064630736949673984ll, flourish_rank)), h.pc_ch);
    CHECK(h.s->action_bar().at(1, 4) && !h.s->action_bar().at(0, 0), "a skill the client sets elsewhere is on the new slot");
    // spam: a burst of 30 in one datagram
    std::vector<fake::Bunch> burst;
    for (int i = 0; i < 30; ++i) burst.push_back(h.client.actor(h.pc_ch, set_slot_rpc(slot(2, i % 12, 1, 5)), true));
    h.client_sends(std::move(burst));
    h.settle();
    CHECK(h.s->requests_dropped() >= 14 && !h.s->failed(), "thirty slot sets at once: the burst is judged, the rest dropped and counted");
    // the load: the stored bar, once
    const size_t payloads = h.mirror.bar_payloads.size();
    h.step(milliseconds(2100));
    h.request(client_load_rpc(), h.pc_ch);
    CHECK(h.mirror.bar_payloads.size() == payloads + 1 && client_bar_is(h, h.s->action_bar()) && h.mirror.bar_payloads.back().slots == h.s->action_bar().wire_slots() &&
              distinct_slots(h.mirror.bar_payloads.back().slots) == h.s->action_bar().slots(),
          "ServerRequestClientLoad sends the stored bar, whole, once (the weapon bank as retail lists it, three times)");
    h.s->finish("end");
    CHECK(h.sink.progress.back().second.action_bar.size() == h.s->action_bar().size(), "the end hands the bar over once more");
}
// [C:Y:B ~ C-O-A]

static void test_d1s2_join_bar_lists_the_weapon_bank_three_times() {
    std::printf("plan D1, bar animation probe 2: the join's bar payload lists the weapon bank as retail does\n");
    Harness h(record_with_scrolls());
    h.request(client_load_rpc(), h.pc_ch);
    const std::vector<repl::ActionbarSlot>& slots = h.mirror.bar_payloads.back().slots;
    std::vector<int> bank4;
    size_t others = 0;
    for (const repl::ActionbarSlot& x : slots) {
        if (x.bank_index == repl::ActionbarBank::Weapon) bank4.push_back(x.slot_index);
        else ++others;
    }
    CHECK(bank4 == (std::vector<int>{1, 2, 4, 3, 1, 2, 4, 3, 1, 2, 4, 3}), "bank 4's slots 1, 2, 4, 3 are listed three times, in capture 873 line 131's order");
    CHECK(others == h.s->action_bar().size() - 4 && slots.size() == others + 12,
          "...and every other slot once (a level-1 Bard: QuickWit, so 13 slots in all; retail's 24 of capture 873 also hold slots of a higher-level bar and banks 2 and 18)");
    CHECK(client_bar_is(h, h.s->action_bar()) && h.s->action_bar().size() == 5, "the client's bar is the stored one (five slots): the copies are the same slots");
}

static void test_d1s2_purchase_reply_waits_84_ms() {
    std::printf("plan D1, bar animation probe 1: a purchase is answered 84 ms after the request, in one tick\n");
    Harness h(bard_at(3));
    const int64_t bard = tree_guid(h, "Bard");
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
    CHECK(h.s->purchase_reply_delay() == milliseconds(84) && PlayerSession::kPurchaseReplyDelay == milliseconds(84), "the default delay is retail's median, 84 ms");
    const size_t from = h.mirror.sequence.size();
    const size_t chains = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, purchase_rpc(build_of({{menacing, bard, true}})), true)});
    h.flush();
    CHECK(h.s->talent_requests_applied() == 1 && h.got.size() == chains && h.s->replies_pending() == 3,
          "the request is applied in its own tick and nothing is sent: the pawn's update, the XPTracker and the skill update wait");
    h.step(milliseconds(10));
    CHECK(h.got.size() == chains && h.s->replies_pending() == 3, "10 ms after the request: still nothing on the wire (neither on the PlayerController's channel nor the pawn's)");
    h.step(milliseconds(73));
    CHECK(h.got.size() == chains && h.s->replies_pending() == 3, "83 ms after it: still nothing");
    h.step(milliseconds(1));
    CHECK(sequence_since(h, from) == (std::vector<std::string>{"A", "L", "U"}) && h.s->replies_pending() == 0,
          "84 ms after it, in one tick: the pawn's update, then the XPTracker, then the skill update");
    bool one_packet = h.got.size() == chains + 3;
    for (size_t i = chains; one_packet && i < h.got.size(); ++i) one_packet = h.packet_of[i] == h.packet_of[chains];
    CHECK(one_packet, "...and the three chains go in one packet (retail: 116 of 123 replies)");

    // two requests inside the delay keep their order, and a refund after a purchase is answered after it
    const int64_t leech = rank_named(h, "Bard", "Bard_Music_Leech");
    const size_t from2 = h.mirror.sequence.size();
    h.client_sends({h.client.actor(h.pawn_ch, purchase_rpc(build_of({{leech, bard, true}})), true)});
    h.step(milliseconds(20));
    h.client_sends({h.client.actor(h.pawn_ch, purchase_rpc(build_of({{leech, bard, false}})), true)});
    h.settle();
    CHECK(h.s->talent_requests_applied() == 3 && count_tag(sequence_since(h, from2), "U") == 1 && h.mirror.known == h.s->known_abilities() &&
              !h.s->action_bar().has_skill(leech),
          "a purchase and its refund 20 ms apart: both answered after their delay, in order, and the client ends with the session's lists");

    // a delay of 0 answers in the tick of the request
    Harness z(bard_at(3));
    z.s->set_purchase_reply_delay(milliseconds(0));
    const size_t zfrom = z.mirror.sequence.size();
    z.client_sends({z.client.actor(z.pawn_ch, purchase_rpc(build_of({{menacing, tree_guid(z, "Bard"), true}})), true)});
    z.flush();
    CHECK(sequence_since(z, zfrom) == (std::vector<std::string>{"A", "L", "U"}), "a delay of 0 sends the reply in the tick of the request, as before");
}
// [C.Y.B - C.O.A]

static void test_d1s2_purchase_sets_have_their_own_allowance() {
    std::printf("plan D1, stage 2: the client's Sets after a big purchase are not held to the bar budget\n");
    Harness h(bard_at(25));
    const int64_t bard = tree_guid(h, "Bard");
    std::vector<std::pair<int64_t, int64_t>> bought;   // (ability, rank) of the actives bought
    if (const tl::Tree* tree = catalog_of(h).tree(bard)) {
        for (const tl::Node& n : tree->nodes) {
            if (n.ranks.empty() || n.ranks.front().ability == 0 || !n.auto_hotbar) continue;
            const size_t before = h.s->talent_requests_applied();
            h.step(milliseconds(2100));   // the budgets refill
            h.request(purchase_rpc(build_of({{n.ranks.front().guid, bard, true}})), h.pawn_ch);
            if (h.s->talent_requests_applied() > before) bought.emplace_back(n.ranks.front().ability, n.ranks.front().guid);
        }
    }
    const size_t K = bought.size();
    std::printf("    (%zu active ranks bought)\n", K);
    CHECK(K >= 4, "(a level-25 Bard buys at least four actives one request at a time)");
    // the client puts each bought skill on a slot of its own (banks 1-3) and sends the Sets in one frame: 20 of them, more than the bar budget's 16
    const size_t N = 20;
    std::vector<fake::Bunch> burst;
    std::vector<repl::ActionbarSlot> sent;
    for (size_t i = 0; i < N; ++i) {
        repl::ActionbarSlot x;
        x.bank_index = 1 + static_cast<int>(i / 12);
        x.slot_index = static_cast<int>(i % 12);
        if (i < K) {
            x.use.use_type = repl::ActionbarUseType::Skill;
            x.use.ability_id = bought[i].first;
            x.use.skill_rank_id = bought[i].second;
        } else {
            x.use.use_type = repl::ActionbarUseType::Ability;
            x.use.ability_id = 5000 + static_cast<int64_t>(i);
        }
        sent.push_back(x);
        burst.push_back(h.client.actor(h.pc_ch, set_slot_rpc(x), true));
    }
    const size_t dropped = h.s->requests_dropped();
    h.client_sends(std::move(burst));
    h.settle();
    size_t held = 0;
    for (const repl::ActionbarSlot& x : sent) {
        const repl::ActionbarUseData* at = h.s->action_bar().at(x.bank_index, x.slot_index);
        held += (at && *at == x.use) ? 1 : 0;
    }
    CHECK(held == N && h.s->requests_dropped() == dropped && h.s->bar_sets_applied() == N,
          "all 20 Sets of the client are stored where it put them (the purchases' allowance, then the bar budget's 16): none dropped");
    size_t copies = 0;
    for (const repl::ActionbarSlot& x : h.s->action_bar().slots()) {
        if (x.bank_index < actionbar::kClassBanks && x.use.use_type == repl::ActionbarUseType::Skill) ++copies;
    }
    CHECK(copies <= K, "...and a bought skill is on the class pages once (the server's own placement moved with the client's Set)");
    // the allowance is spent: the next burst is the bar budget's again (16, then drops)
    std::vector<fake::Bunch> more;
    for (int i = 0; i < 24; ++i) {
        repl::ActionbarSlot x;
        x.bank_index = 3;
        x.slot_index = i % 12;
        x.use.use_type = repl::ActionbarUseType::Ability;
        x.use.ability_id = 9000 + i;
        more.push_back(h.client.actor(h.pc_ch, set_slot_rpc(x), true));
    }
    h.step(milliseconds(2100));
    const size_t dropped2 = h.s->requests_dropped();
    h.client_sends(std::move(more));
    h.settle();
    CHECK(h.s->requests_dropped() > dropped2 && !h.s->failed(), "(a burst with no purchase before it is still held to the budget)");
}

static void test_d1s2_level_grants() {
    std::printf("plan D1, stage 2: a level grants the class tree's ranks as well as the Universal tree's\n");
    Harness h(record_with_scrolls());
    const int64_t bard = tree_guid(h, "Bard");
    const size_t from = h.mirror.sequence.size();
    h.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));   // the level-10 scroll
    const auto seq = sequence_since(h, from);
    CHECK(h.s->progress().level == 10 && count_tag(seq, "A") == 1 && count_tag(seq, "U") == 5 && count_tag(seq, "E+") == 3,
          "level 10: one abilities update, five skill updates (Flourish, Resonant Weapon, Fortissimo of the Bard; Sprint Training I and Endurance Training I) and three effect items");
    CHECK(h.s->known_abilities().size() == 57 + 2 && h.mirror.known == h.s->known_abilities() && h.s->known_abilities()[57] == catalog_of(h).auto_ranks(*catalog_of(h).tree(bard), 10)[1].rank->ability,
          "Flourish and Fortissimo join KnownAbilities, appended (Resonant Weapon is a passive)");
    CHECK(client_list(h, bard).size() == 4 && client_list(h, content::kGuidUniversalTree).size() == 2, "the Bard tree's list has the four ranks of level 10, the Universal tree's two");
    CHECK(h.s->action_bar().at(0, 1) && h.s->action_bar().at(0, 2) && !h.s->action_bar().at(0, 3), "the granted skills with an ability (Flourish, Fortissimo) are on the bar after QuickWit: slots 1 and 2");
    CHECK(!h.sink.progress.empty() && h.sink.progress.back().second.replace_action_bar && h.sink.progress.back().second.action_bar.size() == 5 + 2, "the progress carries the bar");
}
// [C_Y_B + CoA]

static void test_d1s2_class_change() {
    std::printf("plan D1, stage 2: a class scroll switches the class live, and each class keeps its ranks\n");
    Harness h(bard_at(3));
    const int64_t bard = tree_guid(h, "Bard"), tank = tree_guid(h, "Tank");
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
    h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
    const std::vector<int64_t> bard_known = h.s->known_abilities();
    CHECK(h.s->talent_requests_applied() == 1 && h.s->action_bar().has_skill(menacing), "(a Bard with Menacing bought)");
    const auto quickwit = h.s->action_bar().at(0, 0);
    CHECK(quickwit && quickwit->use_type == repl::ActionbarUseType::Skill, "(and QuickWit on bank 0 slot 0)");
    // the rank id by value: `quickwit` points into the bar's map, and the class change erases that entry
    const int64_t quickwit_rank = quickwit ? quickwit->skill_rank_id : 0;

    const size_t from = h.mirror.sequence.size();
    const size_t payloads = h.mirror.bar_payloads.size();
    h.request_both(item_used(3, 6, guid_n(212)), item_activate(3, 6, guid_n(212)));   // the Tank scroll
    CHECK(h.s->class_id() == 7 && h.s->uses_applied() == 1 && h.mirror.unreadable == 0, "the Tank scroll is used and everything the server sent reads");
    // the abilities: the Bard's are gone, the Tank's are in, on the session and on the client
    const tl::Tree* ttree = catalog_of(h).tree(tank);
    const std::vector<int64_t> tank_abilities = h.s->talents()->abilities(*ttree);
    CHECK(!tank_abilities.empty(), "(the Tank tree lists abilities at level 3)");
    bool no_bard = true;
    for (const int64_t a : bard_known) {
        const bool bard_tree_ability = a == catalog_of(h).auto_ranks(*catalog_of(h).tree(bard), 3)[0].rank->ability || a == menacing_ability_of(h, menacing);
        if (bard_tree_ability) no_bard = no_bard && std::find(h.s->known_abilities().begin(), h.s->known_abilities().end(), a) == h.s->known_abilities().end();
    }
    CHECK(no_bard && h.mirror.known == h.s->known_abilities() && h.mirror.records.size() == h.s->record_abilities().size(), "the Bard tree's abilities left KnownAbilities and the records, on the client too");
    CHECK(std::find(h.s->known_abilities().begin(), h.s->known_abilities().end(), tank_abilities[0]) != h.s->known_abilities().end() &&
              h.s->known_abilities().front() == bs::general_unstuck() && std::equal(h.s->known_abilities().end() - 45, h.s->known_abilities().end(), bs::static_known_tail().begin()),
          "the Tank tree's abilities are in, Unstuck is first and the static tail is last");
    // the trees: the Bard's list left the client's map, the Tank's came
    CHECK(client_list(h, bard).empty() && client_list(h, tank).size() == h.s->talents()->list(*ttree).size() && !client_list(h, tank).empty(), "the XPTracker: the Bard tree's item is gone, the Tank tree's list is there");
    // the bar: the Bard's skills are sent cleared, the Tank's placed; one payload
    CHECK(h.mirror.bar_payloads.size() == payloads + 1 && count_tag(sequence_since(h, from), "P") == 1 && client_bar_is(h, h.s->action_bar()), "the bar goes out in one payload and the client's bar is the session's");
    bool cleared = false;
    for (const repl::ActionbarSlot& s : h.mirror.bar_payloads.back().slots) cleared = cleared || (s.use.use_type == repl::ActionbarUseType::None);
    CHECK(cleared && !h.s->action_bar().has_skill(menacing) && !h.s->action_bar().has_skill(quickwit_rank), "the payload holds cleared slots (UseType None) for the Bard's skills, which are off the bar");
    for (const auto& [key, use] : h.mirror.bar) {
        if (use.use_type == repl::ActionbarUseType::Skill) {
            const auto ref = catalog_of(h).find(tank, use.skill_rank_id);
            CHECK(ref.ok(), "every skill on the client's bar is a Tank skill");
        }
    }
    // the stats and the class
    CHECK(h.s->bootstrap().setup().class_record_guid == 108640201692798ll && h.s->character_stats()->class_enum() == 7 && stats_match(h, nullptr, true),
          "CombatInfo's class, the stats and the client's arrays are the Tank's");
    CHECK(h.s->talents()->purchased(bard) == (std::vector<int64_t>{kInstrumentFlute, menacing}), "the Bard's bought rank (and the Flute) is kept under the Bard tree");
    CHECK(h.sink.progress.back().second.class_id == 7 && h.sink.progress.back().second.skill_ranks.size() == 1 && h.sink.progress.back().second.skill_ranks[0].tree_guid == bard,
          "the saved progress: class 7, the Bard tree's ranks kept");

    // a Tank buys a rank of its own tree; the Bard's tree is refused now
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 1, "the Bard's tree cannot be bought by a Tank");
    // and back: the Bard scroll returns the Bard's ranks, ability and bar slot
    h.request_both(item_used(3, 7, guid_n(213)), item_activate(3, 7, guid_n(213)));   // the Bard scroll
    std::vector<int64_t> back_sorted = h.s->known_abilities(), before_sorted = bard_known;
    std::sort(back_sorted.begin(), back_sorted.end());
    std::sort(before_sorted.begin(), before_sorted.end());
    CHECK(h.s->class_id() == 2 && back_sorted == before_sorted && h.mirror.known == h.s->known_abilities(),
          "back to Bard: KnownAbilities are the Bard's again, with Menacing (the join's order, not the live append's)");
    CHECK(client_list(h, bard).size() == 4 && client_list(h, tank).empty() && h.s->action_bar().has_skill(menacing) && client_bar_is(h, h.s->action_bar()),
          "the Bard tree's list is back with the bought rank, the Tank tree's is gone, Menacing is on the bar again");
}

static void test_d1s2_relog() {
    std::printf("plan D1, stage 2: a relog keeps the ranks and the bar\n");
    svc::Character record = bard_at(3);
    std::vector<int64_t> known_before, records_before;
    actionbar::Bar bar_before;
    std::map<int64_t, std::vector<int64_t>> purchased_before;
    svc::Progress saved;
    {
        Harness h(record);
        const int64_t bard = tree_guid(h, "Bard");
        h.request(purchase_rpc(build_of({{rank_named(h, "Bard", "Bard_Music_Menacing"), bard, true}, {rank_named(h, "Bard", "Bard_Music_Leech"), bard, true}})), h.pawn_ch);
        repl::ActionbarSlot s;
        s.bank_index = 1;
        s.slot_index = 7;
        s.use.use_type = repl::ActionbarUseType::Ability;
        s.use.ability_id = content::kGuidSprint;
        s.use.combo_button = 3;
        h.step(milliseconds(150));
        h.request(set_slot_rpc(s), h.pc_ch);
        const tl::Node* un = node_where(h, "Universal", [](const tl::Node& n) { return n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity == 2; });
        if (un) h.request(purchase_rpc(build_of({{un->ranks.front().guid, content::kGuidUniversalTree, true}})), h.pawn_ch);
        known_before = h.s->known_abilities();
        records_before = h.s->record_abilities();
        bar_before = h.s->action_bar();
        purchased_before = h.s->talents()->purchased();
        h.s->finish("end");
        saved = h.sink.progress.back().second;
        record.level = saved.level;
        record.class_id = saved.class_id;
        record.experience = saved.experience;
        for (const svc::TreeRanks& t : saved.skill_ranks) record.skill_ranks.push_back(t);
        for (const svc::ActionbarSlot& a : saved.action_bar) record.action_bar.push_back(a);
    }
    CHECK(saved.replace_skill_ranks && !saved.skill_ranks.empty() && saved.replace_action_bar && !saved.action_bar.empty(), "(the session saved ranks and a bar)");
    Harness again(record);
    CHECK(again.s->talents() && again.s->talents()->purchased() == purchased_before, "the second login has the same bought ranks, tree by tree, in order");
    CHECK(again.s->action_bar() == bar_before, "...the same bar, slot for slot (the item guid and the combo button included)");
    // KnownAbilities are the join's order of the same ranks: the same set, the bought ranks in tree order
    std::vector<int64_t> a = known_before, b = again.s->known_abilities();
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    CHECK(a == b && again.mirror.known == again.s->known_abilities(), "...the same KnownAbilities as a set (the join lists them in tree order, a live purchase appended) and the client has them");
    const int64_t bard = tree_guid(again, "Bard");
    CHECK(client_list(again, bard) == again.s->talents()->list(*catalog_of(again).tree(bard)) && client_list(again, bard).size() == 5, "the client's Bard tree list: the two granted ranks, the Flute and the two bought");
    CHECK(client_list(again, content::kGuidUniversalTree).size() == 1 && again.mirror.unreadable == 0, "...and the Universal tree's: the one bought rank (the level grants none before 5)");
    const size_t payloads = again.mirror.bar_payloads.size();
    again.request(client_load_rpc(), again.pc_ch);
    CHECK(again.mirror.bar_payloads.size() == payloads + 1 && client_bar_is(again, bar_before), "the load sends the stored bar");
}

static void test_d1s2_weapon_swap() {
    std::printf("plan D1, stage 2: swapping weapons moves the combo to the end\n");
    Harness h(bard_at(1));
    const auto before = h.s->known_abilities();
    const size_t before_records = h.s->record_abilities().size();
    CHECK(before.size() == 57 && before[11] == 6064631845843960261ll, "(the Scepter's combo is KnownAbilities' element 11)");
    const size_t from = h.mirror.sequence.size();
    h.request(ranged_rpc(true), h.pawn_ch);
    CHECK(h.s->weapon_swaps() == 1 && h.s->using_ranged_weapon() && count_tag(sequence_since(h, from), "A") == 1, "ServerSetUseRangedWeapon(true): one abilities update");
    const auto after = h.s->known_abilities();
    CHECK(after.size() == 57 && after.back() == 6064631012536680449ll && std::find(after.begin(), after.end(), 6064631845843960261ll) == after.end() && h.mirror.known == after,
          "KnownAbilities keep their count: the Scepter's combo left, the Book's is the last element, on the client too");
    CHECK(h.s->record_abilities().back() == 6064631012536680449ll && h.s->record_abilities().size() == before_records && h.mirror.records == h.s->record_abilities() &&
              std::find(h.s->record_abilities().begin(), h.s->record_abilities().end(), 6064631845843960261ll) != h.s->record_abilities().end(),
          "the records: the Book's combo is the last element, the Scepter's stays carried, the count is the same");
    const repl::Subobject* owner = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.owner_info);
    const repl::Value* line = owner ? owner->obj.state.get(*owner->obj.schema, "WeaponSkillLineId.Guid") : nullptr;
    CHECK(line && line->get_if<int64_t>() && *line->get_if<int64_t>() == 6064631670991093890ll, "OwnerInfo.WeaponSkillLineId follows: Weapon_Ranged_Book");
    // plan E1, stage 3 fix: retail answers the swap with the pawn's CombatInfo too: bIsUsingRangedWeapon follows the request (capture 403, every one of
    // its 8 swaps: an 18-bit UPlayerCombatInfo block beside the AbilityComponent's, the StatsComponent's and OwnerInfo's); the client's weapon follows it
    const auto ranged_flag = [&]() -> std::optional<bool> {
        const repl::Subobject* combat = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.combat_info);
        const repl::Value* v = combat ? combat->obj.state.get(*combat->obj.schema, "bIsUsingRangedWeapon") : nullptr;
        if (!v || !v->get_if<bool>()) return std::nullopt;
        return *v->get_if<bool>();
    };
    CHECK(ranged_flag() == std::optional<bool>(true), "CombatInfo.bIsUsingRangedWeapon is true on the pawn (retail 403: the CombatInfo block of the swap's reply)");
    // swapping to the set it already uses is nothing
    const size_t again_from = h.mirror.sequence.size();
    h.step(milliseconds(120));
    h.request(ranged_rpc(true), h.pawn_ch);
    CHECK(h.s->weapon_swaps() == 1 && sequence_since(h, again_from).empty(), "the same set again: nothing changes, nothing is sent");
    // and back
    h.step(milliseconds(120));
    h.request(ranged_rpc(false), h.pawn_ch);
    CHECK(h.s->weapon_swaps() == 2 && h.s->known_abilities().back() == 6064631845843960261ll && h.s->known_abilities().size() == 57, "back to the melee set: the Scepter's combo is the last again");
    CHECK(ranged_flag() == std::optional<bool>(false), "...and CombatInfo.bIsUsingRangedWeapon is false again");
}
// [C-O-A x C Y B]

/// The talents: nodes that offer a choice of skills (a SkillTreeSlot) are ranks of the tree like any other (the first stage-2 draft left them out, and the
/// client's every talent purchase was refused: "entry 1 names a rank that is not in tree Bard"). What the client asks for is answered with the retail
/// shapes (A L U for an ability, E+ L U for a marker passive), the pick is one option of a slot at a time, a swap is one call, and a relog keeps it.
static void test_d1s2_talent_slots() {
    std::printf("plan D1, stage 2: talents (nodes that offer a choice of skills) are bought like any rank\n");
    using V = std::vector<std::string>;
    svc::Character record = record_with_scrolls();
    record.level = 25;   // no Instrument picked yet
    svc::Progress saved;
    std::map<int64_t, std::vector<int64_t>> purchased_before;
    actionbar::Bar bar_before;
    std::vector<int64_t> known_before;
    int64_t hymn_aoe_rank = 0;
    {
        Harness h(record);
        const int64_t bard = tree_guid(h, "Bard");
        std::vector<const tl::Node*> instrument, hymn;
        for (const tl::Node& n : catalog_of(h).tree(bard)->nodes) {
            if (n.name == "Bard_Passive_Instrument") instrument.push_back(&n);
            if (n.name == "Bard_Song_Hymn") hymn.push_back(&n);
        }
        CHECK(instrument.size() == 3 && hymn.size() == 2 && instrument[0]->slot != 0 && hymn[0]->slot != 0, "(the Bard's Instrument slot offers three skills and the Hymn slot two)");
        if (instrument.size() != 3 || hymn.size() != 2) return;
        const auto seq = [&](size_t from) {
            V out;
            for (const std::string& t : sequence_since(h, from)) {
                if (t != "S") out.push_back(t);
            }
            return out;
        };
        const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
        const int64_t flute = instrument[0]->ranks.front().guid, fiddle = instrument[1]->ranks.front().guid;
        const int64_t hymn_rank = hymn[0]->ranks.front().guid;
        hymn_aoe_rank = hymn[1]->ranks.front().guid;

        // an instrument song before an Instrument is picked: refused, nothing changes, the client's list is corrected
        const std::vector<int64_t> list0 = client_list(h, bard);
        size_t from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 1 && h.s->talent_requests_applied() == 0 && seq(from) == V{"L"} && client_list(h, bard) == list0,
              "a song that needs an Instrument, before one is picked: refused, and the client's unchanged list is sent again");

        // the Flute: a talent with a marker effect and no ability (retail shape S2: E+ L U)
        h.step(milliseconds(2100));
        from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{flute, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_applied() == 1 && seq(from) == (V{"E+", "L", "U"}) && client_list(h, bard).back() == flute && h.mirror.unreadable == 0,
              "picking the Flute (a talent of a slot): E+ L U, the rank is in the client's list");
        CHECK(h.mirror.skill_updates.size() == 1 && h.mirror.skill_updates[0].skill_id == instrument[0]->skill && h.mirror.skill_updates[0].skill_tree_id == bard &&
                  h.mirror.skill_updates[0].rank_id == 0 && !h.mirror.skill_updates[0].is_refunding && h.mirror.skill_updates[0].use_hint,
              "ClientUpdateSkillOnActionbar: the option's own Skill (not the slot's), the tree, the default rank id, the hint");
        // another option of the slot while the Flute stands: refused
        h.step(milliseconds(2100));
        h.request(purchase_rpc(build_of({{fiddle, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 2 && h.s->talents()->purchased(bard) == std::vector<int64_t>{flute}, "the Fiddle while the Flute is picked: refused (one option of a slot at a time)");
        // a swap in one call (the marker of the Flute has ended by then)
        advance(h, 1200);
        from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{flute, bard, false}, {fiddle, bard, true}})), h.pawn_ch);
        const std::vector<int64_t> swapped = client_list(h, bard);
        CHECK(h.s->talent_requests_applied() == 2 && seq(from) == (V{"E+", "L", "U"}) && swapped.back() == fiddle && std::count(swapped.begin(), swapped.end(), flute) == 0,
              "a swap Flute -> Fiddle is one call: one reply (E+ L U), the client's list has the Fiddle and not the Flute");
// (CoA | C-Y-B)

        // the song that needed an Instrument, and a talent with an ability: A L U (retail shape S1), the ability in both lists, on the bar
        advance(h, 1200);
        from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_applied() == 3 && seq(from) == (V{"A", "L", "U"}), "Menacing, once an Instrument is picked: A L U");
        h.step(milliseconds(150));
        from = h.mirror.sequence.size();
        const size_t updates = h.mirror.skill_updates.size();
        h.request(purchase_rpc(build_of({{hymn_rank, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_applied() == 4 && seq(from) == (V{"A", "L", "U"}), "the Hymn talent (a node of a slot with an ability, a point spent in the tree first): A L U");
        CHECK(h.mirror.skill_updates.size() == updates + 1 && h.mirror.skill_updates.back().skill_id == hymn[0]->skill && h.mirror.skill_updates.back().skill_tree_id == bard &&
                  h.mirror.skill_updates.back().use_hint,
              "...its skill update names the Hymn option's Skill");
        CHECK(h.mirror.known == h.s->known_abilities() && std::count(h.mirror.known.begin(), h.mirror.known.end(), hymn[0]->ranks.front().ability) == 1 &&
                  h.mirror.known.back() == hymn[0]->ranks.front().ability && h.mirror.records.back() == hymn[0]->ranks.front().ability,
              "the ability is appended to the client's KnownAbilities and records");
        CHECK(h.s->action_bar().has_skill(hymn_rank) && client_list(h, bard).back() == hymn_rank, "the Hymn is on the stored bar (a design choice) and in the tree's list");
        // the other Hymn option is refused; a swap to it is one call
        h.step(milliseconds(2100));
        h.request(purchase_rpc(build_of({{hymn_aoe_rank, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 3, "the AOE Hymn beside the Hymn: refused");
        h.step(milliseconds(2100));
        from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{hymn_rank, bard, false}, {hymn_aoe_rank, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_applied() == 5 && seq(from) == (V{"A", "L", "U"}) &&
                  std::count(h.mirror.known.begin(), h.mirror.known.end(), hymn[0]->ranks.front().ability) == 0 &&
                  std::count(h.mirror.known.begin(), h.mirror.known.end(), hymn[1]->ranks.front().ability) == 1 && !h.s->action_bar().has_skill(hymn_rank) &&
                  h.s->action_bar().has_skill(hymn_aoe_rank),
              "a swap Hymn -> Hymn (AOE): one call, one reply (A L U); the old ability leaves both lists and the bar, the new one is in them");
        // the client puts the skill where it wants it (ServerSetActionbarSlotUseData): the stored bar follows, the skill is on it once
        repl::ActionbarSlot slot;
        slot.bank_index = 1;
        slot.slot_index = 5;
        slot.use.use_type = repl::ActionbarUseType::Skill;
        slot.use.ability_id = hymn[1]->ranks.front().ability;
        slot.use.skill_rank_id = hymn_aoe_rank;
        h.step(milliseconds(150));
        h.request(set_slot_rpc(slot), h.pc_ch);
        const actionbar::Bar& bar = h.s->action_bar();
        int copies = 0;
        for (const repl::ActionbarSlot& x : bar.slots()) copies += x.use.skill_rank_id == hymn_aoe_rank ? 1 : 0;
        CHECK(bar.at(1, 5) && bar.at(1, 5)->skill_rank_id == hymn_aoe_rank && copies == 1, "the client's own placement of the skill is stored, and the skill is on the bar once");
        purchased_before = h.s->talents()->purchased();
        bar_before = h.s->action_bar();
        known_before = h.s->known_abilities();
        h.s->finish("end");
        saved = h.sink.progress.back().second;
        record.level = saved.level;
        record.class_id = saved.class_id;
        record.experience = saved.experience;
        for (const svc::TreeRanks& t : saved.skill_ranks) record.skill_ranks.push_back(t);
        for (const svc::ActionbarSlot& a : saved.action_bar) record.action_bar.push_back(a);
    }
    // a relog: the talents, the abilities and the bar the client is sent at ServerRequestClientLoad
    Harness again(record);
    const int64_t bard = tree_guid(again, "Bard");
    CHECK(again.s->talents() && again.s->talents()->purchased() == purchased_before, "the second login has the same talents (the Fiddle, Menacing and the AOE Hymn), in order");
    std::vector<int64_t> a = known_before, b = again.s->known_abilities();
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    CHECK(a == b && again.mirror.known == again.s->known_abilities() && std::count(b.begin(), b.end(), 0) == 0, "...the same KnownAbilities as a set, and the client has them");
    CHECK(again.s->action_bar() == bar_before && again.s->action_bar().at(1, 5) && again.s->action_bar().at(1, 5)->skill_rank_id == hymn_aoe_rank, "...the same bar, with the Hymn where the client put it");
    const size_t payloads = again.mirror.bar_payloads.size();
    again.request(client_load_rpc(), again.pc_ch);
    bool in_payload = false;
    if (again.mirror.bar_payloads.size() == payloads + 1) {
        for (const repl::ActionbarSlot& x : again.mirror.bar_payloads.back().slots) in_payload = in_payload || (x.bank_index == 1 && x.slot_index == 5 && x.use.skill_rank_id == hymn_aoe_rank);
    }
    CHECK(in_payload && client_bar_is(again, again.s->action_bar()), "ServerRequestClientLoad sends a bar with the talent's skill in the slot the client chose, and the client holds exactly the stored bar");
    CHECK(client_list(again, bard) == again.s->talents()->list(*catalog_of(again).tree(bard)), "the client's Bard list at the join is the character's, talents included");
}

static void test_d1s2_regeneration() {
    std::printf("plan D1, stage 2: Health and Mana regenerate every second\n");
    constexpr int64_t kHealth = content::kGuidHealth, kMaxHealth = content::kGuidMaxHealth, kHealthRegen = content::kGuidHealthRegeneration;
    // the heavy chest raises MaxHealth and leaves Health where it was: the character is hurt
    std::vector<svc::InventoryItem> items = bard_items();
    Harness h(record_with(items));
    CHECK(h.s->character_stats()->find(kHealthRegen)->value > 0, "(the Bard's HealthRegeneration is positive)");
    h.request(item_used(3, 3, guid_n(203)), h.pawn_ch);   // the heavy chest piece: Constitution +100
    const float regen = h.s->character_stats()->find(kHealthRegen)->value;   // (it follows Constitution)
    const float max = h.s->character_stats()->find(kMaxHealth)->value;
    const float hp = h.s->character_stats()->find(kHealth)->value;
    CHECK(max > hp, "the chest raised MaxHealth above Health");
    const size_t before = h.mirror.stat_updates.size();
    const size_t ticks = h.s->regen_ticks();
    advance(h, 1100);
    const size_t made = h.s->regen_ticks() - ticks;
    CHECK(made >= 1 && h.mirror.stat_updates.size() > before, "a second later: a regeneration tick, sent as a stat delta");
    const float now = h.s->character_stats()->find(kHealth)->value;
    if (std::fabs(now - std::min(max, hp + regen * static_cast<float>(made))) >= 1e-2f) {
        std::printf("    Health %f -> %f, regeneration %f, %zu tick(s), maximum %f\n", hp, now, regen, made, max);
    }
    CHECK(std::fabs(now - std::min(max, hp + regen * static_cast<float>(made))) < 1e-2f, "Health gained one HealthRegeneration for each tick, clamped at the maximum");
    std::string why;
    CHECK(stats_match(h, &why), "the client's arrays are the session's stats");
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    // it goes on until the maximum, and then it stops (the clock is stepped five seconds at a time: a tick makes up for at most five)
    for (int i = 0; i < 2000 && h.s->character_stats()->find(kHealth)->value < max; ++i) h.step(milliseconds(5000));
    CHECK(h.s->character_stats()->find(kHealth)->value == max && stats_match(h), "Health reaches its maximum, exactly, and the client has it");
    const size_t at_full = h.s->regen_ticks();
    for (int i = 0; i < 4; ++i) h.step(milliseconds(5000));
    CHECK(h.s->regen_ticks() == at_full && !h.s->failed(), "at the maximum nothing more is sent");
    // Stamina stays the ability runtime's
    CHECK(h.s->abilities() && h.s->abilities()->stamina() == h.s->abilities()->max_stamina(), "Stamina is the ability runtime's: untouched");
}
// (C.O.A : C.Y.B)

/// The reply shapes of the retail captures (research/stage2/evidence.md section 2, golden_picks.json), each by its tags in the order the server sends the pieces:
/// A (KnownAbilities and the records), E+ and E- (an effect item), S (stat items), L (the tree's list), U (ClientUpdateSkillOnActionbar), as the client's mirror reads them.
static void test_d1s2_shapes() {
    std::printf("plan D1, stage 2: the reply shapes of the retail captures\n");
    const auto seq = [](const Harness& h, size_t from) {
        std::vector<std::string> out;
        for (const std::string& t : sequence_since(h, from)) {
            if (t != "S") out.push_back(t);   // (a stat item an effect or a rank moves is not part of the shapes below)
        }
        return out;
    };
    using V = std::vector<std::string>;
    Harness h(bard_at(25));
    const int64_t bard = tree_guid(h, "Bard");
    const auto rank = [&](const char* name) { return rank_named(h, "Bard", name); };
    // S5: several ranks in one call: one abilities update, one list, a skill update for each
    size_t from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{rank("Bard_Music_Menacing"), bard, true}, {rank("Bard_Music_Leech"), bard, true}, {rank("Bard_Music_Hopeful"), bard, true}})), h.pawn_ch);
    CHECK(seq(h, from) == (V{"A", "L", "U", "U", "U"}), "S5 three ranks in one call: A L U U U (the abilities of all three in one update)");
    CHECK(h.mirror.skill_updates.size() >= 3 && h.mirror.skill_updates[h.mirror.skill_updates.size() - 3].skill_id == node_named(h, "Bard", "Bard_Music_Menacing")->skill &&
              h.mirror.skill_updates.back().skill_id == node_named(h, "Bard", "Bard_Music_Hopeful")->skill,
          "...the skill updates in the order of the request");
    // two more points of ranks that need none spent, then a class passive (4 points spent in the tree): S2 E+ L U with an infinite item
    h.step(milliseconds(150));
    h.request(purchase_rpc(build_of({{rank("Bard_Music_Pensive"), bard, true}})), h.pawn_ch);
    h.step(milliseconds(150));
    from = h.mirror.sequence.size();
    const int64_t passive = rank("Bard_Passive_QuickWit_Burning");
    h.request(purchase_rpc(build_of({{passive, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 3 && seq(h, from) == (V{"E+", "L", "U"}), "S2 a class passive: E+ L U");
    CHECK(h.mirror.effect_items.rbegin()->second.duration == 1.0f && h.s->effect_markers_pending() == 1, "...whose item lasts the record's own duration (this Bard passive's effect is a 1 s marker: the server ends it)");
    // S6b: a single refund of a passive: E- L
    h.step(milliseconds(150));
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{passive, bard, false}})), h.pawn_ch);
    CHECK(seq(h, from) == (V{"E-", "L"}) && h.mirror.effect_items.empty(), "S6b a refund of a passive: E- L, and its item is gone");
    // S8b: a respec of a tree with abilities and a passive: A, the removals, L
    h.step(milliseconds(150));
    h.request(purchase_rpc(build_of({{passive, bard, true}})), h.pawn_ch);
    h.step(milliseconds(150));
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{rank("Bard_Music_Menacing"), bard, false}, {rank("Bard_Music_Leech"), bard, false}, {rank("Bard_Music_Hopeful"), bard, false},
                                     {rank("Bard_Music_Pensive"), bard, false}, {passive, bard, false}})),
              h.pawn_ch);
    const std::map<int64_t, std::vector<int64_t>> only_flute = {{bard, {kInstrumentFlute}}};
    CHECK(seq(h, from) == (V{"A", "E-", "L"}) && h.s->talents()->purchased() == only_flute && h.s->known_abilities().size() == 57 + 2 && h.mirror.known == h.s->known_abilities(),
          "S8b a respec with a passive: A E- L, no skill update; KnownAbilities are the level's again (the captured 57, Flourish and Fortissimo)");
    // S3 and S3b: a Universal marker, then the same rank refunded and bought again inside its second: the new marker replaces the old
    const tl::Node* un = node_where(h, "Universal", [](const tl::Node& n) {
        return n.ranks.front().effect != 0 && !n.ranks.front().effect_infinite && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity == 2;
    });
    if (!un) {
        CHECK(false, "(a Universal marker rank to buy)");
        return;
    }
    const int64_t uni_rank = un->ranks.front().guid;
    h.step(milliseconds(150));
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, true}})), h.pawn_ch);
    CHECK(seq(h, from).front() == "E+" && count_tag(sequence_since(h, from), "L") == 1, "S3 a Universal rank: E+ L (U with the flag)");
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, false}, {uni_rank, content::kGuidUniversalTree, false}})), h.pawn_ch);   // twice: refused whole
    CHECK(h.s->talent_requests_refused() == 1 && h.s->effect_markers_pending() == 1, "(the same rank twice in a call is refused whole: the marker is still on)");
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, false}})), h.pawn_ch);
    CHECK(seq(h, from) == (V{"E-", "L"}), "a refund inside the marker's second removes its item: E- L");
    // S8d: a respec of a Universal tree whose marker has ended: L alone
    h.step(milliseconds(150));
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, true}})), h.pawn_ch);
    advance(h, 1200);
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, false}})), h.pawn_ch);
    CHECK(seq(h, from) == V{"L"}, "S8d / S6c a refund in a Universal tree after its marker ended: L alone (no item to remove, no skill update)");
    // S7b: the swap of two weapon ranks of the line in hand: E+ E- L U
    const tl::Node* w1 = node_where(h, "Weapon_1H_Scepter", [](const tl::Node& n) { return n.ranks.front().effect != 0 && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity == 1; });
    if (w1) {
        h.step(milliseconds(150));
        h.request(purchase_rpc(build_of({{w1->ranks.front().guid, tree_guid(h, "Weapon_1H_Scepter"), true}})), h.pawn_ch);
        from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{w1->ranks.front().guid, tree_guid(h, "Weapon_1H_Scepter"), false}})), h.pawn_ch);   // inside the 0.1 s: its item is removed
        const V s = seq(h, from);
        CHECK(s == (V{"L"}) || s == (V{"E-", "L"}), "a weapon rank refunded: L, and E- first when its item is still on the pawn");
    }
}
// == CoA | C:Y:B ==

static void test_d1s2_stored_garbage() {
    std::printf("plan D1, stage 2: what the record stores is judged by the catalog at the join\n");
    Harness probe(bard_at(3));
    const int64_t bard = tree_guid(probe, "Bard");
    const int64_t menacing = rank_named(probe, "Bard", "Bard_Music_Menacing");
    const int64_t rhetoric = rank_named(probe, "Bard", "Bard_Rhetoric");
    svc::Character r = bard_at(3);
    r.skill_ranks.push_back({bard, {999, menacing, 999, rhetoric, menacing}});   // an unknown rank twice, an auto-granted rank, a rank twice
    r.skill_ranks.push_back({424242, {1, 2}});                                    // a tree that is none
    svc::ActionbarSlot ok;
    ok.bank = 0;
    ok.slot = 4;
    ok.use_type = 1;
    ok.ability_id = content::kGuidSprint;
    svc::ActionbarSlot far = ok;
    far.bank = 99;
    svc::ActionbarSlot empty = ok;
    empty.slot = 5;
    empty.use_type = 0;
    svc::ActionbarSlot guid = ok;
    guid.slot = 6;
    guid.use_type = 3;
    guid.item_guid = "not 32 digits";
    svc::ActionbarSlot rarity = ok;   // the character service's CHECK is 0-15; the narrowing cast to the wire's byte must not turn 256 into a valid 0
    rarity.slot = 7;
    rarity.use_type = 3;
    rarity.item_rarity = 200;
    svc::ActionbarSlot wrapped = rarity;
    wrapped.slot = 8;
    wrapped.item_rarity = 256;
    svc::ActionbarSlot combo = ok;
    combo.slot = 9;
    combo.combo_button = 16;
    r.action_bar = {ok, far, empty, guid, rarity, wrapped, combo};
    Harness h(r);
    CHECK(h.s->talents()->purchased(bard) == std::vector<int64_t>{menacing} && h.s->talents()->purchased().size() == 1, "the join keeps the ranks the catalog knows, once each, in order (Menacing alone)");
    bool noted = false;
    for (const std::string& n : h.s->setup_notes()) noted = noted || n.find("stored rank(s)") != std::string::npos;
    CHECK(noted, "...and says what it left out");
    CHECK(h.s->action_bar().at(0, 4) && h.s->action_bar().size() == 2 && h.s->action_bar().at(0, 6) && !h.s->action_bar().at(0, 5), "the bar keeps the slots that are slots: a bank out of range and an empty slot are left out, a bad item guid is a zero guid");
    CHECK(!h.s->action_bar().at(0, 7) && !h.s->action_bar().at(0, 8) && !h.s->action_bar().at(0, 9),
          "...a rarity of 200, a rarity of 256 (a byte's 0) and a combo button of 16 are left out too (the service would refuse the whole save)");
    CHECK(h.mirror.unreadable == 0 && !h.s->failed() && client_list(h, bard).size() == 3, "the session joins and the client's list of the tree is the granted ranks and Menacing");
}

// ---- Gathering stage 3, "Resource storage": the resource containers on a connection -------------------------------------------------

namespace fr = content_fixture::resource;
static const ItemContainer R0 = ItemContainer::Resource0;

/// The Bard's kit and, in Resource0 (the Sanctus bag in slot 23: 6 x 4 cells), a stack of 7 oak (1 x 3) at (0, 0), 3 daffodils (1 x 2) at (1, 0) and 20 coal (1 x 1) at (2, 0).
static std::vector<svc::InventoryItem> resource_kit() {
    std::vector<svc::InventoryItem> out = bard_items();
    out.push_back(make_item(R0, 0, fr::kOak, 300, 7));
    out.push_back(make_item(R0, 1, fr::kDaffodil, 301, 3));
    out.push_back(make_item(R0, 2, fr::kCoal, 302, 20));
    return out;
}

static void test_resource_join() {
    std::printf("Gathering stage 3: the join opens the resource container with what it holds\n");
    Harness h(record_with(resource_kit()));
    const items::Inventory& inv = h.s->inventory();
    CHECK(inv.grid(R0).cells() == 24 && inv.grid(ItemContainer::Resource1).empty() && inv.count(R0) == 3, "Resource0 is the Sanctus bag's 6 x 4 and holds the three stacks; no other container");
    CHECK(mirror_matches(h), "the client was told every item of every container it holds, the resource ones included");
    const auto oak = h.mirror.storage.find({3, 0});
    CHECK(oak != h.mirror.storage.end() && oak->second.record_id == static_cast<uint64_t>(fr::kOak) && oak->second.quantity == 7, "the oak (x7) sits in the client's Resource0 slot 0");
    // OwnerInfo.ResourceContainers names the one container
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const repl::Subobject* owner = h.s->bootstrap().pawn().find(g.owner_info);
    const repl::Value* rc = owner ? owner->obj.state.get(*owner->obj.schema, "ResourceContainers") : nullptr;
    const auto* arr = rc ? rc->get_if<repl::ArrayValue>() : nullptr;
    bool named = arr && arr->elements.size() == 3;
    if (named) {
        const auto first = arr->elements[0].values.find(1);
        const auto* o = first == arr->elements[0].values.end() ? nullptr : first->second.get_if<emit::FIntrepidNetworkGUID>();
        named = o && *o == g.resource_storage[0] && arr->elements[1].values.empty() && arr->elements[2].values.empty();
    }
    CHECK(named, "OwnerInfo.ResourceContainers names Resource0 and leaves the other two null");
    CHECK(h.mirror.payloads >= 4, "the join sent the Backpack's, the Equipment's, the Quest storage's and Resource0's payloads");
}
// [CoA + C_Y_B]

static void test_resource_requests() {
    std::printf("Gathering stage 3: the client's drags inside the resource container\n");
    Harness h(record_with(resource_kit()));
    const bs::SessionGuids g = h.s->bootstrap().setup().guids;
    const std::string daffodil = guid_n(301), coal = guid_n(302);
    // a Move inside Resource0: the daffodils from (1, 0) to (3, 1)
    MoveSpec m;
    m.from_object = g.resource_storage[0];
    m.to_object = g.resource_storage[0];
    m.from_slot = 1;
    m.from_count = 3;
    m.to_slot = 9;
    m.source_guid = m.event_guid = daffodil;
    const size_t events = h.mirror.events.size();
    h.request(storage_move(m), h.pc_ch);
    CHECK(h.s->inventory().at(R0, 9) && !h.s->inventory().at(R0, 1) && h.s->inventory().at(R0, 9)->quantity == 3 && h.mirror.events.size() == events + 2 && mirror_matches(h),
          "a Move inside Resource0 is made: a Remove and an Add on the container's guid, and the client agrees");
    const bool on_container = h.mirror.events.size() == events + 2 && h.mirror.events[events + 1].targets[0].object == g.resource_storage[0] && h.mirror.events[events + 1].targets[0].slots[0].slot == 9;
    CHECK(on_container, "...the Add names Resource0's guid and slot 9");

    // refusals change nothing and the client is sent the containers again
    const items::Inventory before = h.s->inventory();
    const size_t refused = h.s->item_requests_refused(), payloads = h.mirror.payloads;
    const auto expect_refused = [&](MoveSpec spec, const char* what) {
        const size_t r0 = h.s->item_requests_refused();
        h.request(storage_move(spec), h.pc_ch);
        const bool ok = h.s->item_requests_refused() == r0 + 1 && h.s->inventory() == before;
        if (!ok) std::printf("    (%s: refused %zu -> %zu)\n", what, r0, h.s->item_requests_refused());
        return ok;
    };
    MoveSpec onto = m;   // the daffodils onto the oak
    onto.from_slot = 9;
    onto.to_slot = 0;
    CHECK(expect_refused(onto, "onto the oak"), "a Move onto another stack is refused (a swap or a fill)");
    MoveSpec swap = onto;
    swap.type = static_cast<uint8_t>(repl::StorageEventType::Swap);
    swap.target_guid = guid_n(300);
    swap.to_count = 7;
    CHECK(expect_refused(swap, "swap"), "a Swap in the container is refused");
    MoveSpec fill = onto;
    fill.type = static_cast<uint8_t>(repl::StorageEventType::Fill);
    CHECK(expect_refused(fill, "fill"), "a Fill is refused");
    MoveSpec destroy = onto;
    destroy.type = static_cast<uint8_t>(repl::StorageEventType::Remove);
    CHECK(expect_refused(destroy, "destroy"), "a Remove (destroying the stack by dragging it out) is refused");
    MoveSpec to_bag = m;   // the coal into the Backpack
    to_bag.from_slot = 2;
    to_bag.from_count = 20;
    to_bag.source_guid = to_bag.event_guid = coal;
    to_bag.to_object = g.backpack;
    to_bag.to_slot = 30;
    CHECK(expect_refused(to_bag, "to the Backpack"), "a resource stack dragged into the Backpack is refused");
    MoveSpec from_bag = m;   // a potion into the resource container
    from_bag.from_object = g.backpack;
    from_bag.from_slot = 1;
    from_bag.from_count = 5;
    from_bag.source_guid = from_bag.event_guid = guid_n(201);
    from_bag.to_object = g.resource_storage[0];
    from_bag.to_slot = 20;
    CHECK(expect_refused(from_bag, "from the Backpack"), "a potion dragged into the resource container is refused");
    MoveSpec third = m;   // a container with no bag
    third.from_slot = 9;
    third.to_object = g.resource_storage[2];
    third.to_slot = 0;
    CHECK(expect_refused(third, "third container"), "a drag to Resource2, which the character has no bag for, is refused");
    CHECK(h.s->item_requests_refused() == refused + 7, "seven refusals counted");
    CHECK(h.mirror.payloads > payloads && mirror_matches(h), "the client was sent its containers again after the refusals, and agrees");
}

static void test_resource_repair() {
    std::printf("Gathering stage 3: the login moves the Backpack's resource items to the container and saves that\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fr::kOak, 310, 5));
    items.push_back(make_item(Bag, 5, fr::kDaffodil, 311, 3));
    items.push_back(make_item(Bag, 6, fi::kPotion, 312, 2));
    Harness h(record_with(items));
    const items::Inventory& inv = h.s->inventory();
    CHECK(inv.at(R0, 0) && inv.at(R0, 0)->item_record_id == fr::kOak && inv.at(R0, 1) && inv.at(R0, 1)->item_record_id == fr::kDaffodil && !inv.at(Bag, 4) && !inv.at(Bag, 5) && inv.at(Bag, 6),
          "the oak and the daffodils moved from the Backpack to Resource0 (slots 0 and 1); the potion stayed");
    CHECK(mirror_matches(h) && h.mirror.storage.count({3, 0}) == 1 && h.mirror.storage.count({0, 4}) == 0, "the client was told the repaired layout at the join (no resource item in its Backpack)");
    CHECK(h.s->inventory_changed() && h.s->inventory_dirty(), "the session knows the layout differs from the record's and saves it");
    h.s->finish("the test");
    bool saved = false;
    for (const auto& s : h.sink.inventories) {
        bool oak = false, daffodil = false, none_in_bag = true;
        for (const svc::InventoryItem& it : s.items) {
            oak = oak || (it.container == R0 && it.slot == 0 && it.item_record_id == fr::kOak);
            daffodil = daffodil || (it.container == R0 && it.slot == 1 && it.item_record_id == fr::kDaffodil);
            none_in_bag = none_in_bag && !(it.container == Bag && (it.item_record_id == fr::kOak || it.item_record_id == fr::kDaffodil));
        }
        saved = saved || (oak && daffodil && none_in_bag);
    }
    CHECK(saved, "the inventory handed to the saver has the resource items in Resource0");
}
// -- C O A . Cyb --

static void test_resource_no_room_at_login() {
    std::printf("Gathering stage 3: a resource item that finds no room at the login stays in the Backpack\n");
    std::vector<svc::InventoryItem> items = bard_items();
    for (uint32_t i = 0; i < 7; ++i) {   // seven oaks: six fit a 6 x 4 grid
        svc::InventoryItem oak = make_item(Bag, 4 + static_cast<int>(i), fr::kOak, 320 + i, 20);
        oak.quality = static_cast<int32_t>(i + 1);
        items.push_back(oak);
    }
    Harness h(record_with(items));
    const items::Inventory& inv = h.s->inventory();
    CHECK(inv.count(R0) == 6 && inv.at(Bag, 10) && inv.at(Bag, 10)->item_record_id == fr::kOak, "six oaks moved, the seventh stays in the Backpack (nothing is deleted)");
    CHECK(inv.size() == bard_items().size() + 7, "every item is still there");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // (a crash must not take the last results with it)
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    test_join();
    test_unequip_and_equip_the_chest();
    test_two_hander_and_slots();
    test_moves_and_swaps();
    test_activate_alone();
    test_wire_forms();
    test_refusals();
    test_refusals_that_need_a_state();
    test_budgets();
    test_hostile();
    test_saving_in_the_session();
    test_saver_inventories();
    test_endpoint_saves();
    test_item_trigger();
    test_held_items_update_waits_for_the_window();
    test_review_naked_join_then_equip();
    test_review_unequip_everything_and_back();
    test_review_full_bag();
    test_review_empty_character();
    test_review_saves_after_equip();
    test_stats_at_the_join();
    test_stats_follow_the_chest();
    test_health_and_mana_clamp();
    test_nothing_changes_nothing_sent();
    test_stats_wait_for_the_window();
    test_stats_through_the_runtime();
    test_stats_follow_the_rod();
    test_stats_of_replication_zero_are_not_sent();
    test_gear_that_moves_only_unreplicated_stats();
    test_stats_equipment_changes_with_the_value_clamped();
    test_stamina_low_after_dodges_is_not_refilled_by_gear();
    test_stats_of_a_faulted_run_are_not_sent();
    test_equipment_overflow_behind_a_clamp_is_not_sent();
    test_stats_refused_by_the_pawn_stay_the_clients();
    test_d1_join_has_the_tracker();
    test_d1_level_scroll();
    test_d1_class_scroll();
    test_d1_gear_and_consumables();
    test_d1_refusals();
    test_d1_relog();
    test_d1_saver_progress();
    test_d1s2_join();
    test_d1s2_purchase();
    test_d1s2_passives();
    test_d1s2_refusals();
    test_d1s2_bar();
    test_d1s2_join_bar_lists_the_weapon_bank_three_times();
    test_d1s2_purchase_reply_waits_84_ms();
    test_d1s2_purchase_sets_have_their_own_allowance();
    test_d1s2_level_grants();
    test_d1s2_class_change();
    test_d1s2_relog();
    test_d1s2_talent_slots();
    test_d1s2_weapon_swap();
    test_d1s2_shapes();
    test_d1s2_stored_garbage();
    test_d1s2_regeneration();
    test_g2_join();
    test_g2_scrolls();
    test_g2_saver_round_trip();
    test_resource_join();
    test_resource_requests();
    test_resource_repair();
    test_resource_no_room_at_login();
    test_real_content_db();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

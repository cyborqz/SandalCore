// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_items.cpp: items and inventory, worldd plan B2
//
//  the rules (items/inventory.h)     wearing and taking off items: the events in the retail order and what each says (type, storage, slot,
//                                    instigator), the slot the server picks, two-handers and what they displace, a full bag, every refusal
//                                    leaving the inventory as it was; moves and swaps in the bag; guids and times as the wire has them
//  the retail equips                 20 captures of the retail server's answers (tests/capture_equip.h, from PCAPRepo equip_unequip/*): the same
//                                    events in the same order from the state each answer needs, the HeldItems and AppearanceIDs the pawn shows
//                                    after, and the HeldItems array key moving by 23 for each change of the Equipment storage
//  the visible gear                  the captured Bard's kit gives the captured HeldItems and AppearanceIDs (pkt#107)
//  the storage RPCs, bit for bit     the retail login's Backpack, Equipment and Quest payloads and its 109 ClientReceiveStorageEvent of those
//                                    three, and the original server's Bard's four payloads and 13 events, rebuilt from the items they hold: the
//                                    same bits, in the same order; and the pawn's second state of the captured Bard built from its kit equals
//                                    pkt#107 as before
//  the setup (E1)                    build_player_setup with items: the instances, HeldItems, AppearanceIDs and the Inventory; a record whose
//                                    items cannot be played is refused
//  content.db                        item() on the fixture and, with AOC_CONTENT_DB, on the real file (the Bard's kit, the equip facts)
//  hostile                           random requests, in and out of range, never break an invariant: one item per slot, one guid per item, no item
//                                    made or lost, a refusal changes nothing
// ============================================================================
#include "bootstrap/bootstrap.h"
#include "bootstrap/captured_character.h"
#include "bootstrap/captured_setup.h"
#include "bootstrap/character_setup.h"
#include "capture_blocks.h"
#include "capture_equip.h"
#include "capture_items.h"
#include "capture_resource.h"
#include "content_db_fixture.h"
#include "world_stats.h"
#include "cyb_appearance.h"
#include "items/acquired.h"
#include "items/inventory.h"
#include "protocol/repl/item_codec.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"
// ~ C_Y_B - C O A ~

#include <sqlite3.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <tuple>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace emit = aoc::protocol::emit;
namespace cap = capture_items;
namespace fs = std::filesystem;
using emit::BunchWriter;
using svc::ItemContainer;
using Inv = items::Inventory;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

namespace cf = content_fixture;
namespace fi = content_fixture::item;

// ---- helpers ------------------------------------------------------------------------------------------------------------------

static const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) { std::printf("  [FAIL] no class %s\n", name); std::exit(2); }
    return *c;
}

static std::string guid_n(uint32_t n) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xA0000000u + n, 0x1000u + n, 0x2000u + n, n);
    return buf;
}

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
// -- C.Y.B x C.O.A --

/// A catalog of the fixture's items (the records of content_db_fixture.h) and what a test adds.
struct FakeCatalog : items::ItemCatalog {
    std::map<int64_t, items::ItemInfo> m;
    mutable size_t asked = 0;
    std::optional<items::ItemInfo> info(int64_t record) const override {
        ++asked;
        const auto it = m.find(record);
        return it == m.end() ? std::nullopt : std::optional<items::ItemInfo>(it->second);
    }
    void add(int64_t record, std::vector<uint8_t> slots, bool two_handed = false) { m[record] = items::ItemInfo{std::move(slots), two_handed}; }
};

static FakeCatalog fixture_catalog() {
    FakeCatalog c;
    c.add(fi::kHelmet, {1});
    c.add(fi::kChest, {2});
    c.add(fi::kLegs, {5});
    c.add(fi::kFeet, {6});
    c.add(fi::kScepter, {15});
    c.add(fi::kFocus, {16, 18});
    c.add(fi::kBook, {17, 18}, true);
    c.add(fi::kBag, {23, 24, 25});
    c.add(fi::kPickaxe, {26});
    c.add(fi::kAxe, {29});
    c.add(fi::kSickle, {33});
    c.add(fi::kFishingRod, {35});
    c.add(fi::kBow, {38});
    c.add(fi::kSword2H, {15, 16}, true);
    c.add(fi::kMace1H, {15});
    c.add(fi::kShield, {16});
    c.add(fi::kHeavyChest, {2});
    c.add(fi::kRingA, {10, 11});
    c.add(fi::kRingB, {10, 11});
    c.add(fi::kEarLeftFirst, {14, 13});
    c.add(fi::kEarRightFirst, {13, 14});
    c.add(fi::kGatherCape, {8});
    c.add(fi::kCostume, {71});
    c.add(fi::kCloak, {79});
    c.add(fi::kShirt, {19});
    return c;   // kPotion is known to no one as wearable: it has no entry (an unknown record)
}

/// The Bard's kit as the starter items of the capture: slot and record (guids made up).
static std::vector<svc::InventoryItem> bard_kit() {
    const std::pair<int, int64_t> kit[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter},
                                            {16, fi::kFocus}, {17, fi::kBook}, {23, fi::kBag}, {26, fi::kPickaxe}, {29, fi::kAxe},
                                            {33, fi::kSickle}, {35, fi::kFishingRod}, {38, fi::kBow}};
    std::vector<svc::InventoryItem> out;
    uint32_t n = 100;
    for (const auto& [slot, record] : kit) out.push_back(make_item(ItemContainer::Equipment, slot, record, ++n));
    return out;
}

static Inv inventory_of(std::vector<svc::InventoryItem> v) {
    std::string error;
    auto inv = Inv::from_items(v, error);
    if (!inv) {
        std::printf("  [FAIL] test setup: %s\n", error.c_str());
        std::exit(2);
    }
    return *inv;
}

static bool same_event(const items::Event& e, repl::StorageEventType type, ItemContainer c, int slot, int64_t record, bool instigated) {
    return e.type == type && e.container == c && e.slot == slot && e.item.item_record_id == record && e.item.slot == slot && e.item.container == c &&
           e.instigated == instigated;
}

static const auto Add = repl::StorageEventType::Add;
static const auto Remove = repl::StorageEventType::Remove;
static const ItemContainer Bag = ItemContainer::Backpack;
static const ItemContainer Worn = ItemContainer::Equipment;
static constexpr uint32_t kAny = 0xFFFFFFFFu;

static bool unchanged_by(const Inv& before, const Inv& after) { return before == after; }

// ---- guids and times ----------------------------------------------------------------------------------------------------------
// [C O A x C.Y.B]

static void test_guids_and_ticks() {
    std::printf("guids and times as the wire has them\n");
    const auto g = items::guid_of("9cb881dd7fbb492eb9e6b0e19677c0f7");
    CHECK(g && g->a == 0x9cb881ddu && g->b == 0x7fbb492eu && g->c == 0xb9e6b0e1u && g->d == 0x9677c0f7u, "the capture's first instance guid reads as its four words");
    CHECK(g && items::guid_hex(*g) == "9cb881dd7fbb492eb9e6b0e19677c0f7", "and writes back to the same 32 digits");
    CHECK(items::guid_of("9CB881DD7FBB492EB9E6B0E19677C0F7").has_value(), "capitals are read (the table keeps lowercase)");
    CHECK(!items::guid_of("") && !items::guid_of("abc") && !items::guid_of(std::string(32, '0')) && !items::guid_of(std::string(31, 'a') + "g") &&
              !items::guid_of(std::string(33, 'a')),
          "nothing else is a guid: empty, short, long, a non-digit, the zero guid");
    // the captured Bard's creation time: 639068962740000000 ticks is a whole second of Unix time
    CHECK(items::ms_of_ticks(639068962740000000ll) == 1'771'299'474'000ll && items::ticks_of_ms(1'771'299'474'000ll) == 639068962740000000ll,
          "FDateTime ticks and Unix milliseconds convert both ways (the Bard's 639068962740000000)");
    svc::InventoryItem it = make_item(Worn, 2, fi::kChest, 7);
    it.durability = 11;
    const repl::ItemInstance inst = items::to_instance(it);
    repl::ItemInstance plain;
    plain.has_item = true;
    plain.record_id = static_cast<uint64_t>(fi::kChest);
    plain.quantity = 1;
    plain.quality = 1;
    plain.durability = 11;
    plain.guid = *items::guid_of(it.instance_guid);
    plain.creation_time = items::ticks_of_ms(it.created_at_ms);
    CHECK(inst == plain, "an item is the plain instance: record, quantity, quality, durability, guid and creation time, the rest at its default");
    const auto back = items::item_of(inst, Worn, 2);
    CHECK(back && *back == it, "and the instance gives the item back");
    CHECK(!items::item_of(repl::ItemInstance{}, Worn, 2), "the empty instance is no item");
    repl::ItemInstance bad = inst;
    bad.guid = {};
    CHECK(!items::item_of(bad, Worn, 2), "an instance with the zero guid is no item the table can hold");
    bad = inst;
    bad.quantity = 0;
    CHECK(!items::item_of(bad, Worn, 2), "nor one with quantity 0");

    // mounts, stage 1: what a mount item persists, as retail's login has it on its 24 mount items (capture 1198: Health 3572 and Mana 1 on a level 20
    // owner's terror bird, a LockExpiration in Unix seconds, in the order Health then Mana)
    svc::InventoryItem mount = make_item(Bag, 5, 6064631121016127508ll, 20);
    mount.persisted_health = 3572.0f;
    mount.persisted_mana = 1.0f;
    mount.lock_expiration = 1770131735;
    const repl::ItemInstance mi = items::to_instance(mount);
    CHECK(mi.stats.size() == 2 && mi.stats[0].stat_id == 108986352686712ull && mi.stats[0].value == 3572.0f && mi.stats[1].stat_id == 109183576135288ull &&
              mi.stats[1].value == 1.0f && mi.lock_expiration == 1770131735 && mi.cooldowns.empty() && mi.custom_data.empty(),
          "a mount item is the plain instance plus its two persisted stats (Health, Mana) and its LockExpiration");
    const auto mount_back = items::item_of(mi, Bag, 5);
    CHECK(mount_back && *mount_back == mount && mount_back->persisted_health == 3572.0f && mount_back->lock_expiration == 1770131735, "and the instance gives the mount item back, state and all");
    // the instance survives the item codec: its bits are the retail ones' shape (1,096 plain + two stats of 96 bits)
    {
        aoc::protocol::emit::BunchWriter w;
        const bool wrote = aoc::protocol::repl::write_item_instance(w, mi);
        CHECK(wrote && w.bit_pos() == 1096 + 2 * 96, "it writes as 1,288 bits, the smallest retail mount instance's size");
    }
    svc::InventoryItem dead = mount;
    dead.persisted_health = 0.0f;   // a mount that died is saved with no Health: present, not absent
    const auto dead_back = items::item_of(items::to_instance(dead), Bag, 5);
    CHECK(dead_back && dead_back->persisted_health && *dead_back->persisted_health == 0.0f && dead_back->persisted_mana && !(*dead_back == mount),
          "a persisted Health of 0 is a Health (a dead mount), and differs from the live one");
    svc::InventoryItem locked = make_item(Bag, 6, 6064634099174146054ll, 21);
    locked.lock_expiration = 1790000123;
    const repl::ItemInstance li = items::to_instance(locked);
    const auto locked_back = items::item_of(li, Bag, 6);
    CHECK(li.stats.empty() && locked_back && *locked_back == locked && !locked_back->persisted_health && !locked_back->persisted_mana,
          "an item with a lock and no persisted stats gives no stats, and reads back as it was");
    CHECK(!(make_item(Bag, 5, 6064631121016127508ll, 20) == mount), "an item that keeps none differs from the mount item of the same record");
    repl::ItemInstance hostile = mi;
    hostile.stats[0].value = -1.0f;
    CHECK(!items::item_of(hostile, Bag, 5), "a negative persisted Health is no item the table can hold");
    hostile = mi;
    hostile.stats[1].value = std::numeric_limits<float>::quiet_NaN();
    CHECK(!items::item_of(hostile, Bag, 5), "nor a Mana that is not a number");
    hostile = mi;
    hostile.lock_expiration = -5;
    CHECK(!items::item_of(hostile, Bag, 5), "nor a negative lock expiration");
    hostile = mi;
    hostile.lock_expiration = 253402300800ll;
    CHECK(!items::item_of(hostile, Bag, 5), "nor one past the year 9999");
    hostile = mi;
    hostile.stats.push_back({6064630000000000000ull, 5.0f});
    const auto extra = items::item_of(hostile, Bag, 5);
    CHECK(extra && *extra == mount, "a persisted stat that is neither Health nor Mana is ignored (the table keeps those two)");
}

// ---- building an inventory ----------------------------------------------------------------------------------------------------

static void test_from_items() {
    std::printf("building an inventory from the character's items\n");
    std::string error;
    auto inv = Inv::from_items({make_item(Bag, 3, fi::kPotion, 1), make_item(Worn, 2, fi::kChest, 2), make_item(ItemContainer::Quest, 99, 9, 3)}, error);
    CHECK(inv && inv->size() == 3 && inv->count(Bag) == 1 && inv->count(Worn) == 1 && inv->count(ItemContainer::Quest) == 1, "three items in three storages");
    CHECK(inv && inv->at(Worn, 2) && inv->at(Worn, 2)->item_record_id == fi::kChest && !inv->at(Worn, 3) && !inv->at(Worn, -1) && !inv->at(Worn, 83) &&
              !inv->at(Bag, 70) && !inv->at(ItemContainer::Quest, 100),
          "at() finds an item by storage and slot and nothing outside a storage");
    CHECK(inv && inv->first_free(Bag) == 0 && inv->first_free(Worn) == 0, "the lowest free slot");
    const auto all = inv->items();
    CHECK(all.size() == 3 && all[0].container == Bag && all[1].container == Worn && all[2].container == ItemContainer::Quest,
          "items() lists the Backpack's, the Equipment's, then the Quest storage's");
    // in any order, the same inventory
    auto other = Inv::from_items({make_item(ItemContainer::Quest, 99, 9, 3), make_item(Worn, 2, fi::kChest, 2), make_item(Bag, 3, fi::kPotion, 1)}, error);
    CHECK(other && *other == *inv, "the order of the list does not matter");
// C Y B - C | O | A

    struct Bad { const char* name; svc::InventoryItem it; const char* want; };
    std::vector<Bad> bad;
    auto with = [&](const char* name, const char* want, auto mod) { auto it = make_item(Bag, 3, fi::kPotion, 1); mod(it); bad.push_back({name, it, want}); };
    with("a bag slot 70", "slot 70", [](auto& it) { it.slot = 70; });
    with("a negative slot", "slot -1", [](auto& it) { it.slot = -1; });
    with("an equipment slot 83", "slot 83", [](auto& it) { it.container = Worn; it.slot = 83; });
    with("a quest slot 100", "slot 100", [](auto& it) { it.container = ItemContainer::Quest; it.slot = 100; });
    with("a storage that is none", "storage", [](auto& it) { it.container = static_cast<ItemContainer>(9); });
    with("record 0", "record", [](auto& it) { it.item_record_id = 0; });
    with("a guid in capitals", "lowercase", [](auto& it) { for (auto& c : it.instance_guid) c = static_cast<char>(std::toupper(c)); });
    with("a short guid", "guid", [](auto& it) { it.instance_guid = "abc"; });
    with("the zero guid", "guid", [](auto& it) { it.instance_guid = std::string(32, '0'); });
    with("quantity 0", "quantity", [](auto& it) { it.quantity = 0; });
    with("quantity past a million", "quantity", [](auto& it) { it.quantity = 1000001; });
    with("quality 0", "quality", [](auto& it) { it.quality = 0; });
    with("quality 201", "quality", [](auto& it) { it.quality = 201; });
    with("negative durability", "durability", [](auto& it) { it.durability = -1; });
    with("NaN durability", "durability", [](auto& it) { it.durability = std::nanf(""); });
    with("infinite durability", "durability", [](auto& it) { it.durability = INFINITY; });
    with("a negative creation time", "creation time", [](auto& it) { it.created_at_ms = -1; });
    with("a creation time past the year 2255", "creation time", [](auto& it) { it.created_at_ms = 9'000'000'000'001ll; });
    for (const Bad& b : bad) {
        std::string err;
        const auto r = Inv::from_items({b.it}, err);
        CHECK(!r && err.find(b.want) != std::string::npos, (std::string(b.name) + " is refused, saying why").c_str());
    }
    std::string err;
    CHECK(!Inv::from_items({make_item(Bag, 3, 1, 1), make_item(Bag, 3, 1, 2)}, err) && err.find("two items in Backpack slot 3") != std::string::npos, "two items in a slot are refused");
    CHECK(!Inv::from_items({make_item(Bag, 3, 1, 1), make_item(Bag, 4, 1, 1)}, err) && err.find("instance guid") != std::string::npos, "one guid on two items is refused");
    CHECK(Inv::from_items({make_item(Bag, 5, 1, 1), make_item(Worn, 5, 1, 2), make_item(ItemContainer::Quest, 5, 1, 3)}, err).has_value(), "slot 5 of three storages are three slots");
    std::vector<svc::InventoryItem> full;
    uint32_t n = 0;
    for (const ItemContainer c : {Bag, Worn, ItemContainer::Quest}) {
        for (int s = 0; s < items::slot_count(c); ++s) full.push_back(make_item(c, s, 1, ++n));
    }
    CHECK(full.size() == 253 && Inv::from_items(full, err).has_value() && Inv::from_items(full, err)->first_free(Bag) == -1, "every slot of every storage can be filled");
    full.push_back(make_item(Bag, 0, 1, ++n));
    CHECK(!Inv::from_items(full, err), "one item more than there are slots is refused");
}

// ---- wearing and taking off ----------------------------------------------------------------------------------------------------

static void test_equip_and_unequip() {
    std::printf("wearing an item and taking it off\n");
    const FakeCatalog catalog = fixture_catalog();
    const std::string chest_guid = guid_n(1);
    Inv inv = inventory_of({make_item(Bag, 18, fi::kHeavyChest, 1)});
    auto o = inv.use(Bag, 18, chest_guid, kAny, catalog);
    CHECK(o.ok && o.refusal.empty() && o.events.size() == 2 && o.equipment_changed, "a chest piece into the empty chest slot: two events and the equipment changed");
    CHECK(o.events.size() == 2 && same_event(o.events[0], Remove, Bag, 18, fi::kHeavyChest, false) && same_event(o.events[1], Add, Worn, 2, fi::kHeavyChest, true),
          "Remove it from the bag slot (no instigator), then Add it to Chest (the character instigates), as capture 1219");
    CHECK(o.events.size() == 2 && o.events[1].item.instance_guid == chest_guid && o.events[1].item.slot == 2 && o.events[1].item.container == Worn, "the Add carries the same instance, now in the equipment");
    CHECK(!inv.at(Bag, 18) && inv.at(Worn, 2) && inv.at(Worn, 2)->instance_guid == chest_guid && inv.size() == 1, "the item moved: it is worn, and the bag slot is empty");

    // and off: it goes to the lowest free bag slot, Add first
    inv = inventory_of({make_item(Bag, 0, fi::kPotion, 50), make_item(Bag, 1, fi::kPotion, 51), make_item(Bag, 3, fi::kPotion, 52), make_item(Worn, 2, fi::kHeavyChest, 1)});
    o = inv.use(Worn, 2, chest_guid, kAny, catalog);
    CHECK(o.ok && o.events.size() == 2 && o.equipment_changed, "an unequip is two events and changes the equipment");
    CHECK(o.events.size() == 2 && same_event(o.events[0], Add, Bag, 2, fi::kHeavyChest, true) && same_event(o.events[1], Remove, Worn, 2, fi::kHeavyChest, false),
          "Add it to the lowest free bag slot (2), then Remove it from Chest, as capture 1253");
    CHECK(inv.at(Bag, 2) && inv.at(Bag, 2)->instance_guid == chest_guid && !inv.at(Worn, 2), "the item is in the bag, and Chest is empty");

    // the same slot taken by a worn item: swapped, the old one to the bag
    inv = inventory_of({make_item(Bag, 5, fi::kHeavyChest, 2), make_item(Worn, 2, fi::kChest, 1)});
    o = inv.use(Bag, 5, guid_n(2), kAny, catalog);
    CHECK(o.ok && o.events.size() == 4, "a chest piece over a chest piece: four events");
    CHECK(o.events.size() == 4 && same_event(o.events[0], Remove, Bag, 5, fi::kHeavyChest, false) && same_event(o.events[1], Add, Bag, 0, fi::kChest, true) &&
              same_event(o.events[2], Remove, Worn, 2, fi::kChest, false) && same_event(o.events[3], Add, Worn, 2, fi::kHeavyChest, true),
          "Remove the new one, Add the old one to the lowest free bag slot (0, below the freed 5), Remove it from Chest, Add the new one");
    CHECK(inv.at(Bag, 0)->instance_guid == guid_n(1) && inv.at(Worn, 2)->instance_guid == guid_n(2) && inv.size() == 2, "the two changed places");

    // a request that names a slot the record lists
    inv = inventory_of({make_item(Bag, 0, fi::kFocus, 1)});
    o = inv.use(Bag, 0, guid_n(1), items::equip::RangedOffHand, catalog);
    CHECK(o.ok && o.events.size() == 2 && same_event(o.events[1], Add, Worn, 18, fi::kFocus, true), "a Focus with RangedOffHand asked for goes there");
    inv = inventory_of({make_item(Bag, 0, fi::kFocus, 1)});
    o = inv.use(Bag, 0, guid_n(1), items::equip::Primary, catalog);
    CHECK(!o.ok && o.refusal.find("not one of the item's slots") != std::string::npos && inv.at(Bag, 0), "a slot the record does not list is refused");
    o = inv.use(Bag, 0, guid_n(1), 300, catalog);
    CHECK(!o.ok, "and so is one past a byte");
}

static void test_slot_choice() {
    std::printf("the slot the server picks\n");
    const FakeCatalog catalog = fixture_catalog();
    // rings: the first free of the record's list, RingRight 10 then RingLeft 11
    Inv inv = inventory_of({make_item(Bag, 0, fi::kRingA, 1), make_item(Bag, 1, fi::kRingB, 2)});
    auto o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
    CHECK(o.ok && same_event(o.events.back(), Add, Worn, 10, fi::kRingA, true), "the first ring goes to RingRight (capture 1235)");
    o = inv.use(Bag, 1, guid_n(2), kAny, catalog);
    CHECK(o.ok && o.events.size() == 2 && same_event(o.events.back(), Add, Worn, 11, fi::kRingB, true), "a second ring goes to RingLeft (capture 1236), nothing is displaced");
    // a third ring: both taken, the first of its slots, and the old one to the bag
    inv = inventory_of({make_item(Bag, 0, fi::kRingA, 3), make_item(Worn, 10, fi::kRingA, 1), make_item(Worn, 11, fi::kRingB, 2)});
    o = inv.use(Bag, 0, guid_n(3), kAny, catalog);
    CHECK(o.ok && o.events.size() == 4 && same_event(o.events[1], Add, Bag, 0, fi::kRingA, true) && same_event(o.events[2], Remove, Worn, 10, fi::kRingA, false) &&
              same_event(o.events[3], Add, Worn, 10, fi::kRingA, true) && inv.at(Worn, 10)->instance_guid == guid_n(3) && inv.at(Bag, 0)->instance_guid == guid_n(1),
          "with both ring slots taken the new ring takes the first slot of its record and the old ring goes to the bag");
    // earrings: the record's order decides which ear is first
    inv = inventory_of({make_item(Bag, 0, fi::kEarLeftFirst, 1), make_item(Bag, 1, fi::kEarRightFirst, 2)});
    o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
    CHECK(o.ok && same_event(o.events.back(), Add, Worn, 14, fi::kEarLeftFirst, true), "an earring whose record lists EarLeft first goes to EarLeft (capture 1223)");
    o = inv.use(Bag, 1, guid_n(2), kAny, catalog);
    CHECK(o.ok && o.events.size() == 2 && same_event(o.events.back(), Add, Worn, 13, fi::kEarRightFirst, true), "the next, whose record says EarRight first, finds EarRight free (capture 1224)");
    // the Focus: Secondary, else RangedOffHand when Secondary is taken and the ranged slot is free
    inv = inventory_of({make_item(Bag, 0, fi::kFocus, 1), make_item(Worn, 16, fi::kShield, 2)});
    o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
    CHECK(o.ok && o.events.size() == 2 && same_event(o.events.back(), Add, Worn, 18, fi::kFocus, true) && inv.at(Worn, 16), "a Focus with a shield in Secondary goes to RangedOffHand, which is free");
    // but not under a worn Book, which covers RangedOffHand: the Focus takes Secondary (free) and the Book stays
    inv = inventory_of({make_item(Bag, 0, fi::kFocus, 1), make_item(Worn, 17, fi::kBook, 2)});
    o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
    CHECK(o.ok && o.events.size() == 2 && same_event(o.events.back(), Add, Worn, 16, fi::kFocus, true) && inv.at(Worn, 17), "a Focus beside a worn Book takes Secondary: the Book stays");
    // with Secondary taken as well, both covered: the first slot, the old one displaced
    inv = inventory_of({make_item(Bag, 0, fi::kFocus, 1), make_item(Worn, 17, fi::kBook, 2), make_item(Worn, 16, fi::kShield, 3)});
    o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
    CHECK(o.ok && o.events.size() == 4 && same_event(o.events[3], Add, Worn, 16, fi::kFocus, true) && inv.at(Bag, 0)->instance_guid == guid_n(3) && inv.at(Worn, 17),
          "with Secondary taken and RangedOffHand covered the Focus takes Secondary and the shield goes to the bag");
}
// built with C | Y | B and C O A

static void test_two_handers() {
    std::printf("two-handed items\n");
    const FakeCatalog catalog = fixture_catalog();
    // capture 1204: a two-hander over a main hand and an off hand: six events
    Inv inv = inventory_of({make_item(Bag, 0, fi::kPotion, 10), make_item(Bag, 1, fi::kPotion, 11), make_item(Bag, 2, fi::kPotion, 12), make_item(Bag, 3, fi::kPotion, 13),
                            make_item(Bag, 5, fi::kSword2H, 1), make_item(Worn, 15, fi::kMace1H, 2), make_item(Worn, 16, fi::kShield, 3)});
    auto o = inv.use(Bag, 5, guid_n(1), kAny, catalog);
    CHECK(o.ok && o.events.size() == 6, "a two-hander over a main hand and an off hand: six events");
    CHECK(o.events.size() == 6 && same_event(o.events[0], Remove, Bag, 5, fi::kSword2H, false) && same_event(o.events[1], Add, Bag, 4, fi::kMace1H, true) &&
              same_event(o.events[2], Remove, Worn, 15, fi::kMace1H, false) && same_event(o.events[3], Add, Bag, 5, fi::kShield, true) &&
              same_event(o.events[4], Remove, Worn, 16, fi::kShield, false) && same_event(o.events[5], Add, Worn, 15, fi::kSword2H, true),
          "Remove 2H, Add mace at bag slot 4, Remove mace, Add shield at bag slot 5, Remove shield, Add 2H at Primary (capture 1204)");
    CHECK(inv.at(Worn, 15) && inv.at(Worn, 15)->item_record_id == fi::kSword2H && !inv.at(Worn, 16) && inv.at(Bag, 4)->item_record_id == fi::kMace1H &&
              inv.at(Bag, 5)->item_record_id == fi::kShield,
          "the two-hander is in Primary, Secondary stays empty, the two went to the bag");
    // the two-hander sits in its first slot even when only Secondary is occupied
    inv = inventory_of({make_item(Bag, 0, fi::kSword2H, 1), make_item(Worn, 16, fi::kShield, 2)});
    o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
    CHECK(o.ok && o.events.size() == 4 && same_event(o.events.back(), Add, Worn, 15, fi::kSword2H, true) && !inv.at(Worn, 16), "a two-hander over a shield alone sits in Primary and sends the shield to the bag");
    // a shield over a worn two-hander: the two-hander is displaced even though Secondary is empty
    inv = inventory_of({make_item(Bag, 0, fi::kShield, 1), make_item(Worn, 15, fi::kSword2H, 2)});
    o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
    CHECK(o.ok && o.events.size() == 4 && same_event(o.events[1], Add, Bag, 0, fi::kSword2H, true) && same_event(o.events[2], Remove, Worn, 15, fi::kSword2H, false) &&
              same_event(o.events[3], Add, Worn, 16, fi::kShield, true) && !inv.at(Worn, 15),
          "a shield into Secondary under a worn two-hander takes the two-hander off");
    // a Book (Ranged, RangedOffHand) over a Focus in RangedOffHand: the Focus goes
    inv = inventory_of({make_item(Bag, 0, fi::kBook, 1), make_item(Worn, 18, fi::kFocus, 2), make_item(Worn, 16, fi::kShield, 3)});
    o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
    CHECK(o.ok && o.events.size() == 4 && same_event(o.events[1], Add, Bag, 0, fi::kFocus, true) && same_event(o.events[3], Add, Worn, 17, fi::kBook, true) && inv.at(Worn, 16),
          "a Book over a Focus in RangedOffHand sends the Focus to the bag and leaves the melee set alone");
    // the captured Bard: Scepter, Focus and Book worn together
    inv = inventory_of(bard_kit());
    CHECK(inv.at(Worn, 15) && inv.at(Worn, 16) && inv.at(Worn, 17) && !inv.at(Worn, 18), "the Bard wears Scepter, Focus and Book, and RangedOffHand is empty (the Book covers it)");
    // the bag is too small: nothing changes
    std::vector<svc::InventoryItem> crowded;
    uint32_t n = 100;
    for (int s = 0; s < 70; ++s) crowded.push_back(make_item(Bag, s, fi::kPotion, ++n));
    crowded[5] = make_item(Bag, 5, fi::kSword2H, 1);
    crowded.push_back(make_item(Worn, 15, fi::kMace1H, 2));
    crowded.push_back(make_item(Worn, 16, fi::kShield, 3));
    inv = inventory_of(crowded);
    const Inv before = inv;
    o = inv.use(Bag, 5, guid_n(1), kAny, catalog);
    CHECK(!o.ok && o.events.empty() && o.refusal.find("free slot") != std::string::npos && unchanged_by(before, inv), "a full bag with two items to take off: refused, nothing changed");
    // one item to take off fits in the slot the new item leaves
    crowded.pop_back();
    inv = inventory_of(crowded);
    o = inv.use(Bag, 5, guid_n(1), kAny, catalog);
    CHECK(o.ok && o.events.size() == 4 && same_event(o.events[1], Add, Bag, 5, fi::kMace1H, true) && inv.at(Bag, 5)->instance_guid == guid_n(2),
          "a full bag with one item to take off: the freed slot takes it");
    // unequip into a full bag is refused
    std::vector<svc::InventoryItem> full;
    for (int s = 0; s < 70; ++s) full.push_back(make_item(Bag, s, fi::kPotion, ++n));
    full.push_back(make_item(Worn, 2, fi::kChest, 1));
    inv = inventory_of(full);
    const Inv before2 = inv;
    o = inv.use(Worn, 2, guid_n(1), kAny, catalog);
    CHECK(!o.ok && o.refusal.find("bag is full") != std::string::npos && unchanged_by(before2, inv), "taking off into a full bag is refused");
}

static void test_refusals() {
    std::printf("what is refused changes nothing\n");
    const FakeCatalog catalog = fixture_catalog();
    Inv inv = inventory_of({make_item(Bag, 3, fi::kHeavyChest, 1), make_item(Bag, 4, fi::kPotion, 2), make_item(Bag, 5, 424242, 3), make_item(Worn, 2, fi::kChest, 4),
                            make_item(ItemContainer::Quest, 0, fi::kHeavyChest, 5)});
    const Inv before = inv;
    struct Case { const char* name; ItemContainer storage; int slot; std::string guid; uint32_t requested; const char* why; };
    const Case cases[] = {
        {"an empty slot", Bag, 9, guid_n(1), kAny, "no item in Backpack slot 9"},
        {"another instance's guid", Bag, 3, guid_n(2), kAny, "not the instance the client named"},
        {"the zero guid", Bag, 3, std::string(32, '0'), kAny, "not the instance"},
        {"a guid that is no guid", Bag, 3, "xyz", kAny, "not the instance"},
        {"an empty guid", Bag, 3, "", kAny, "not the instance"},
        {"a negative slot", Bag, -1, guid_n(1), kAny, "no item"},
        {"a slot past the bag", Bag, 70, guid_n(1), kAny, "no item"},
        {"a slot past the equipment", Worn, 83, guid_n(4), kAny, "no item"},
        {"an item that is not worn", Bag, 4, guid_n(2), kAny, "is not in content.db"},
        {"a record content.db does not know", Bag, 5, guid_n(3), kAny, "is not in content.db"},
        {"the Quest storage", ItemContainer::Quest, 0, guid_n(5), kAny, "Quest storage are not used"},
        {"a storage that is none", static_cast<ItemContainer>(7), 0, guid_n(5), kAny, "unknown storage"},
    };
    for (const Case& c : cases) {
        const items::Outcome o = inv.use(c.storage, c.slot, c.guid, c.requested, catalog);
        CHECK(!o.ok && o.events.empty() && !o.equipment_changed && o.refusal.find(c.why) != std::string::npos && unchanged_by(before, inv),
              (std::string(c.name) + ": refused (" + o.refusal + ")").c_str());
    }
    // a record whose list is empty: known, not worn
    FakeCatalog plain = fixture_catalog();
    plain.add(fi::kPotion, {});
    const items::Outcome o = inv.use(Bag, 4, guid_n(2), kAny, plain);
    CHECK(!o.ok && o.refusal.find("not worn in any slot") != std::string::npos && unchanged_by(before, inv), "a record that lists no slot is not worn");
}

// ---- moves in the bag ----------------------------------------------------------------------------------------------------------
// C Y B x CoA

static void test_move_and_swap() {
    std::printf("moving and swapping inside the bag\n");
    Inv inv = inventory_of({make_item(Bag, 40, fi::kPotion, 1, 1), make_item(Bag, 62, fi::kPotion, 2, 20), make_item(Bag, 55, fi::kRingA, 3, 1), make_item(Worn, 2, fi::kChest, 4)});
    auto o = inv.move(Bag, 40, 1, guid_n(1), Bag, 32);
    CHECK(o.ok && o.events.size() == 2 && same_event(o.events[0], Remove, Bag, 40, fi::kPotion, true) && same_event(o.events[1], Add, Bag, 32, fi::kPotion, true) &&
              !o.equipment_changed && inv.at(Bag, 32) && !inv.at(Bag, 40),
          "a move is Remove, then Add, both instigated by the character (capture 1169)");
    CHECK(inv.at(Bag, 32)->slot == 32 && inv.at(Bag, 32)->instance_guid == guid_n(1), "the item is in its new slot");
    // a stack moves whole, its quantity kept
    o = inv.move(Bag, 62, 20, guid_n(2), Bag, 63);
    CHECK(o.ok && o.events[1].item.quantity == 20 && inv.at(Bag, 63)->quantity == 20, "a stack of 20 moves whole");
    // a swap: Remove A, Remove B, Add B at A's slot, Add A at B's slot
    o = inv.swap(Bag, 63, 20, guid_n(2), Bag, 55, guid_n(3), 1);
    CHECK(o.ok && o.events.size() == 4 && same_event(o.events[0], Remove, Bag, 63, fi::kPotion, true) && same_event(o.events[1], Remove, Bag, 55, fi::kRingA, true) &&
              same_event(o.events[2], Add, Bag, 63, fi::kRingA, true) && same_event(o.events[3], Add, Bag, 55, fi::kPotion, true),
          "a swap is Remove A, Remove B, Add B where A was, Add A where B was (capture 1169's pair)");
    CHECK(inv.at(Bag, 63)->instance_guid == guid_n(3) && inv.at(Bag, 55)->instance_guid == guid_n(2) && inv.at(Bag, 55)->quantity == 20, "and the two changed places with their quantities");

    const Inv before = inv;
    struct Case { const char* name; items::Outcome o; };
    const Case cases[] = {
        {"a move onto an occupied slot", inv.move(Bag, 32, 1, guid_n(1), Bag, 55)},
        {"a move onto itself", inv.move(Bag, 32, 1, guid_n(1), Bag, 32)},
        {"a move from an empty slot", inv.move(Bag, 9, 1, guid_n(1), Bag, 10)},
        {"a move to a slot past the bag", inv.move(Bag, 32, 1, guid_n(1), Bag, 70)},
        {"a move to a negative slot", inv.move(Bag, 32, 1, guid_n(1), Bag, -1)},
        {"a move of another instance", inv.move(Bag, 32, 1, guid_n(9), Bag, 10)},
        {"a split", inv.move(Bag, 55, 5, guid_n(2), Bag, 10)},
        {"a count of 0", inv.move(Bag, 32, 0, guid_n(1), Bag, 10)},
        {"a move out of the bag", inv.move(Bag, 32, 1, guid_n(1), Worn, 10)},
        {"a move from the equipment", inv.move(Worn, 2, 1, guid_n(4), Bag, 10)},
        {"a swap with an empty slot", inv.swap(Bag, 32, 1, guid_n(1), Bag, 10, guid_n(3), 1)},
        {"a swap with itself", inv.swap(Bag, 32, 1, guid_n(1), Bag, 32, guid_n(1), 1)},
        {"a swap naming the wrong instance", inv.swap(Bag, 32, 1, guid_n(1), Bag, 55, guid_n(9), 20)},
        {"a swap naming the wrong count", inv.swap(Bag, 32, 1, guid_n(1), Bag, 55, guid_n(2), 3)},
        {"a swap of half a stack", inv.swap(Bag, 55, 10, guid_n(2), Bag, 32, guid_n(1), 1)},
        {"a swap into the equipment", inv.swap(Bag, 32, 1, guid_n(1), Worn, 2, guid_n(4), 1)},
    };
    for (const Case& c : cases) CHECK(!c.o.ok && c.o.events.empty() && !c.o.refusal.empty() && unchanged_by(before, inv), (std::string(c.name) + " is refused").c_str());
}

// ---- the visible gear ----------------------------------------------------------------------------------------------------------

static void test_visible_gear() {
    std::printf("the visible gear\n");
    CHECK(items::kHeldSlots.size() == 23 && items::kHeldSlots[0] == 15 && items::kHeldSlots[3] == 18 && items::kHeldSlots[4] == 26 && items::kHeldSlots[18] == 41 &&
              items::kHeldSlots[19] == 75 && items::kHeldSlots[22] == 78,
          "HeldItems has 23 keys: the weapons 15-18, the tools 26-39 and 41, the cosmetic weapons 75-78");
    // the captured keys, in order
    bool keys = true;
    for (size_t i = 0; i < 23; ++i) keys = keys && bs::captured::kHeldItems[i].key == items::kHeldSlots[i] && bs::captured::kHeldItems[i].id == static_cast<int32_t>(i + 1);
    CHECK(keys, "they are the captured Bard's keys with its ReplicationIDs 1 to 23");
    const Inv bard = inventory_of(bard_kit());
    const auto held = items::held_records(bard);
    bool same = true;
    for (size_t i = 0; i < 23; ++i) same = same && held[i] == bs::captured::kHeldItems[i].item_guid;
    CHECK(same, "the Bard's kit shows the captured HeldItems: Scepter, Focus and Book, the five tools, nothing else");
    const auto app = items::appearance_ids(bard);
    bool app_same = app.size() == 4;
    for (size_t i = 0; i < app.size() && i < 4; ++i) {
        app_same = app_same && static_cast<int64_t>(app[i].record_guid) == bs::captured::kAppearanceIds[i].record_guid &&
                   static_cast<int64_t>(app[i].type_id) == bs::captured::kAppearanceIds[i].type_id && app[i].enchantment_level == bs::captured::kAppearanceIds[i].enchantment &&
                   app[i].item_equip_slot == bs::captured::kAppearanceIds[i].equip_slot;
    }
    CHECK(app_same, "and the captured AppearanceIDs: the helmet, chest, legs and feet by slot, TypeId of an ItemRecord, enchantment 0");
    CHECK(items::shows_in_appearance(1) && items::shows_in_appearance(9) && items::shows_in_appearance(71) && items::shows_in_appearance(79) && !items::shows_in_appearance(0) &&
              !items::shows_in_appearance(10) && !items::shows_in_appearance(12) && !items::shows_in_appearance(19) && !items::shows_in_appearance(21) && !items::shows_in_appearance(74),
          "armor (1-9), the costume and the cloak are shown; rings, necklace, earrings, the artisan shirt, the tool belt and the lure are not");
    // a ring, a shirt and a bag worn: nothing shown
    const auto none = items::appearance_ids(inventory_of({make_item(Worn, 10, fi::kRingA, 1), make_item(Worn, 19, fi::kShirt, 2), make_item(Worn, 23, fi::kBag, 3)}));
    CHECK(none.empty(), "a ring, the artisan shirt and a bag add nothing to AppearanceIDs");
    const auto cosmetic = items::appearance_ids(inventory_of({make_item(Worn, 79, fi::kCloak, 1), make_item(Worn, 71, fi::kCostume, 2), make_item(Worn, 8, fi::kGatherCape, 3)}));
    CHECK(cosmetic.size() == 3 && cosmetic[0].item_equip_slot == 8 && cosmetic[1].item_equip_slot == 71 && cosmetic[2].item_equip_slot == 79, "armor, costume and cloak come by ascending slot");
    CHECK(items::held_records(Inv())[0] == 0 && items::appearance_ids(Inv()).empty(), "nothing worn: all 23 are 0 and nothing is shown");
    // the held items follow the slot, not the item: a Book in Ranged, nothing in RangedOffHand
    const auto only_book = items::held_records(inventory_of({make_item(Worn, 17, fi::kBook, 1)}));
    CHECK(only_book[2] == fi::kBook && only_book[3] == 0, "a two-hander is in its own slot only: Ranged has the Book, RangedOffHand 0");
}

// ---- the retail equips ---------------------------------------------------------------------------------------------------------

static const capture_equip::Record* record_of(int64_t record) {
    for (const auto& r : capture_equip::kRecords)
        if (r.record == record) return &r;
    return nullptr;
}

static FakeCatalog catalog_of_captures() {
    FakeCatalog c;
    for (const auto& r : capture_equip::kRecords) {
        std::vector<uint8_t> slots;
        for (int i = 0; i < r.nslots; ++i) slots.push_back(static_cast<uint8_t>(r.slots[i]));
        c.add(r.record, slots, r.two_handed);
    }
    return c;
}

static Inv inventory_before(const capture_equip::Scenario& sc) {
    std::vector<svc::InventoryItem> v;
    uint32_t n = 0;
    for (size_t i = 0; i < sc.nbefore; ++i) {
        const auto& p = sc.before[i];
        svc::InventoryItem it = make_item(p.container == 0 ? Bag : Worn, p.slot, p.record ? p.record : 5, ++n);
        it.instance_guid = p.guid;
        v.push_back(it);
    }
    return inventory_of(v);
}
// (C.Y.B | C-O-A)

static void test_retail_equips() {
    std::printf("the retail server's answers to equip and unequip (capture_equip.h)\n");
    const FakeCatalog catalog = catalog_of_captures();
    size_t events_checked = 0;
    for (const auto& sc : capture_equip::kScenarios) {
        const std::string name = "capture " + std::to_string(sc.capture) + ": " + sc.title;
        Inv inv = inventory_before(sc);
        std::vector<items::Event> got;
        bool all_ok = true;
        for (size_t i = 0; i < sc.nrequests; ++i) {
            const auto& r = sc.requests[i];
            const items::Outcome o = inv.use(r.storage == 3 ? Bag : Worn, r.slot, r.guid, kAny, catalog);
            all_ok = all_ok && o.ok;
            got.insert(got.end(), o.events.begin(), o.events.end());
        }
        bool events_equal = got.size() == sc.nevents;
        for (size_t i = 0; events_equal && i < sc.nevents; ++i) {
            const auto& e = sc.events[i];
            events_equal = same_event(got[i], e.type == 0 ? Add : Remove, e.container == 0 ? Bag : Worn, e.slot, e.record, e.instigated) && got[i].item.instance_guid == e.guid;
        }
        events_checked += sc.nevents;
        if (!all_ok || !events_equal) {
            std::printf("    %zu events, retail's %zu\n", got.size(), sc.nevents);
            for (size_t i = 0; i < got.size() || i < sc.nevents; ++i) {
                if (i < got.size()) std::printf("    ours   %s %s %d record %lld%s\n", got[i].type == Add ? "Add   " : "Remove", got[i].container == Bag ? "bag  " : "equip", got[i].slot,
                                                (long long)got[i].item.item_record_id, got[i].instigated ? " instigated" : "");
                if (i < sc.nevents) std::printf("    retail %s %s %d record %lld%s\n", sc.events[i].type == 0 ? "Add   " : "Remove", sc.events[i].container == 0 ? "bag  " : "equip",
                                                sc.events[i].slot, (long long)sc.events[i].record, sc.events[i].instigated ? " instigated" : "");
            }
        }
        CHECK(all_ok && events_equal, (name + ": the same events in the same order, instigators included").c_str());
        // what the pawn shows after
        const auto held = items::held_records(inv);
        bool held_same = true;
        for (size_t i = 0; i < 23; ++i) held_same = held_same && held[i] == sc.held[i].record && items::kHeldSlots[i] == sc.held[i].key;
        bool app_same = true;
        if (sc.has_appearance) {
            // a dynamic array's update names the elements that changed: each of them is worn in our answer too, and a shown slot the events
            // emptied (and nothing refilled) has no entry
            const auto app = items::appearance_ids(inv);
            for (size_t i = 0; i < sc.nshown; ++i) {
                bool found = false;
                for (const auto& a : app) found = found || (a.item_equip_slot == sc.shown[i].slot && static_cast<int64_t>(a.record_guid) == sc.shown[i].record);
                app_same = app_same && found;
            }
            for (size_t i = 0; i < sc.nevents; ++i) {
                const auto& e = sc.events[i];
                if (e.type != 1 || e.container != 1 || !items::shows_in_appearance(e.slot)) continue;
                bool refilled = false;
                for (size_t j = i + 1; j < sc.nevents; ++j) refilled = refilled || (sc.events[j].type == 0 && sc.events[j].container == 1 && sc.events[j].slot == e.slot);
                for (const auto& a : app) app_same = app_same && (refilled || a.item_equip_slot != e.slot);
            }
        }
        if (!held_same || !app_same) {
            for (size_t i = 0; i < 23; ++i)
                if (held[i] != sc.held[i].record) std::printf("    held key %d: ours %lld, retail %lld%s", items::kHeldSlots[i], (long long)held[i], (long long)sc.held[i].record, "\n");
            if (sc.has_appearance) {
                const auto app = items::appearance_ids(inv);
                std::printf("    appearance: ours %zu entries, retail %zu%s", app.size(), sc.nshown, "\n");
                for (const auto& a : app) std::printf("      ours slot %d record %lld%s", a.item_equip_slot, (long long)a.record_guid, "\n");
                for (size_t i = 0; i < sc.nshown; ++i) std::printf("      retail slot %d record %lld%s", sc.shown[i].slot, (long long)sc.shown[i].record, "\n");
            }
        }
        CHECK(held_same && app_same, (name + ": HeldItems and AppearanceIDs after equal the capture's").c_str());
    }
    std::printf("    %zu retail events compared\n", events_checked);
    CHECK(events_checked > 80, "more than eighty retail events compared");
    (void)record_of;
}

// ---- the storage RPCs against the captures --------------------------------------------------------------------------------------

/// The parameters of a captured RPC as the typed layer reads them.
static std::optional<repl::RpcCall> decode_captured(const cap::Rpc& c, const char* cls) {
    const schema::RpcSchema* rpc = class_named(cls).rpc(c.rpc);
    if (!rpc) return std::nullopt;
    const auto bytes = capture_blocks::bytes_of(c.hex);
    repl::CodecOptions opts;
    opts.inline_objects = true;
    size_t pos = 0;
    auto params = repl::read_rpc_params(*rpc, bytes.data(), c.bits, pos, opts);
    if (!params || pos != c.bits) return std::nullopt;
    repl::RpcCall call;
    call.rpc = rpc;
    call.params = std::move(*params);
    call.decoded = true;
    return call;
}

/// The bits of every RPC's parameters in a Send's content blocks, in order.
static std::vector<BunchWriter> param_bits_of(const bs::Send& s, const schema::ClassSchema& cls) {
    std::vector<BunchWriter> out;
    const auto blocks = repl::read_content_blocks(s.emission.payload.data(), s.emission.payload.bit_pos(), 0);
    if (!blocks) return out;
    for (const auto& b : *blocks) {
        size_t pos = b.payload_pos;
        const auto h = repl::read_field_header(cls, s.emission.payload.data(), pos + b.payload_bits, pos);
        if (!h) return {};
        BunchWriter w;
        w.write_bit_range(s.emission.payload.data(), pos, h->payload_bits);
        out.push_back(std::move(w));
    }
    return out;
}

static bool bits_equal(const BunchWriter& w, const cap::Rpc& c) {
    const auto want = capture_blocks::bytes_of(c.hex);
    return w.bit_pos() == c.bits && capture_blocks::same_bit_range(w.data(), want.data(), c.bits, nullptr);
}
// -- C | O | A . Cyb --

/// A corpus's items, as the rebuilding of its payloads and events needs them.
struct Corpus {
    std::vector<bs::StoredItem> items;                   // Backpack, Equipment and Quest items of the payloads
    std::map<int, repl::StoragePayload> payload;         // by bs::ItemStorage: the payload of the three storages
    std::vector<const cap::Rpc*> events;                 // the ClientReceiveStorageEvent of those three storages, in the capture's order
    std::vector<const cap::Rpc*> payloads;               // their payloads (pawn), by storage
    repl::GuidValue character;                           // the instigator of the events
    emit::FIntrepidNetworkGUID guids[3];                 // the storages' objects
    // Gathering stage 3: the resource containers of the login (24, 40 and 30 slots: the Sanctus, Mining T2 and Lumberjacking T2 bags), by container index
    std::map<int, repl::StoragePayload> resource_payload;
    std::map<int, const cap::Rpc*> resource_rpc;
    std::vector<const cap::Rpc*> resource_events[3];
    std::vector<bs::StoredItem> resource_items;
};

template <size_t N>
static std::optional<Corpus> read_corpus(const cap::Rpc (&rpcs)[N]) {
    Corpus out;
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    (void)pawn;
    out.payloads.assign(3, nullptr);
    std::map<int, const cap::Rpc*> by_storage;
    for (const cap::Rpc& c : rpcs) {
        if (std::string(c.rpc) != "ClientReceiveStoragePayload") continue;
        const auto call = decode_captured(c, "PlayerPawn_C");
        if (!call) return std::nullopt;
        const auto p = repl::parse_storage_payload(*call);
        if (!p) return std::nullopt;
        int storage = -1;
        if (p->maximum_items == 70) storage = static_cast<int>(bs::ItemStorage::Backpack);
        else if (p->maximum_items == 83) storage = static_cast<int>(bs::ItemStorage::Equipment);
        else if (p->maximum_items == 100) storage = static_cast<int>(bs::ItemStorage::Quest);
        if (storage < 0) {   // a resource container: 24 slots (the Sanctus bag), 40 (the Mining bag), 30 (the Lumberjacking bag)
            const int index = p->maximum_items == 24 ? 0 : p->maximum_items == 40 ? 1 : p->maximum_items == 30 ? 2 : -1;
            if (index < 0 || out.resource_payload.count(index)) continue;   // or the same payload sent again
            out.resource_payload[index] = *p;
            out.resource_rpc[index] = &c;
            for (const repl::StorageSlot& s : p->storage) {
                if (!s.item.has_item) continue;
                bs::StoredItem si;
                si.storage = static_cast<bs::ItemStorage>(static_cast<int>(bs::ItemStorage::Resource0) + index);
                si.slot = s.slot;
                si.item = s.item;
                out.resource_items.push_back(std::move(si));
            }
            continue;
        }
        if (out.payload.count(storage)) continue;   // the same payload sent again
        out.payload[storage] = *p;
        out.payloads[static_cast<size_t>(storage)] = &c;
        out.guids[storage] = p->target;
        for (const repl::StorageSlot& s : p->storage) {
            if (!s.item.has_item) continue;
            bs::StoredItem si;
            si.storage = static_cast<bs::ItemStorage>(storage);
            si.slot = s.slot;
            si.item = s.item;
            out.items.push_back(std::move(si));
        }
    }
    if (out.payload.size() != 3) return std::nullopt;
    bool have_character = false;
    for (const cap::Rpc& c : rpcs) {
        if (std::string(c.rpc) != "ClientReceiveStorageEvent" || c.pc == 0) continue;
        const auto call = decode_captured(c, "AoCPlayerControllerBP_C");
        if (!call) return std::nullopt;
        const auto e = repl::parse_storage_event(*call);
        if (!e || e->targets.size() != 1) return std::nullopt;
        bool ours = false;
        for (int s = 0; s < 3; ++s) ours = ours || e->targets[0].object == out.guids[s];
        if (!ours) {   // an event of a resource container
            for (const auto& [index, payload] : out.resource_payload) {
                if (e->targets[0].object == payload.target) out.resource_events[index].push_back(&c);
            }
            continue;
        }
        out.events.push_back(&c);
        if (!have_character && e->type == 0) {
            out.character = e->instigator;
            have_character = true;
        }
    }
    if (!have_character) return std::nullopt;
    return out;
}
// [C_Y_B ~ CoA]

static bs::PlayerSetup setup_of(const Corpus& c) {
    auto s = bs::captured_character_setup();
    if (!s) {
        std::printf("  [FAIL] the captured character does not build\n");
        std::exit(2);
    }
    s->character_guid = {c.character.a, c.character.b, c.character.c, c.character.d};
    s->guids.backpack = c.guids[0];
    s->guids.equipment = c.guids[1];
    s->guids.quest_storage = c.guids[2];
    s->items = c.items;
    // Gathering stage 3: the containers of the corpus's bags, with their grids and what they hold
    for (const auto& [index, payload] : c.resource_payload) {
        s->guids.resource_storage[static_cast<size_t>(index)] = payload.target;
        bs::ResourceBagSetup& bag = s->resource_bags[static_cast<size_t>(index)];
        bag.present = true;
        bag.columns = static_cast<int>(payload.dimension.x);
        bag.rows = static_cast<int>(payload.dimension.y);
        bag.override_matrix.clear();
        for (const std::vector<int32_t>& column : payload.override_matrix) bag.override_matrix.emplace_back(column.begin(), column.end());
    }
    for (const bs::StoredItem& it : c.resource_items) s->items.push_back(it);
    return std::move(*s);
}

template <size_t N>
static void test_corpus(const char* label, const cap::Rpc (&rpcs)[N], size_t expect_events, size_t expect_items) {
    std::printf("%s: the storage RPCs rebuilt from the items they hold\n", label);
    const auto c = read_corpus(rpcs);
    CHECK(c.has_value(), "the three storages' payloads and their events read");
    if (!c) return;
    CHECK(c->items.size() == expect_items && c->events.size() == expect_events, (std::to_string(c->items.size()) + " items in the payloads, " + std::to_string(c->events.size()) + " events for them").c_str());
    bs::PlayerBootstrap pb(setup_of(*c), bs::GameStateMode::Tier1);
    CHECK(pb.ok(), "the bootstrap builds from them");
    if (!pb.ok()) return;
    CHECK(pb.item_count(bs::ItemStorage::Backpack) + pb.item_count(bs::ItemStorage::Equipment) + pb.item_count(bs::ItemStorage::Quest) == expect_items, "and holds each of them");

    const schema::ClassSchema& pawn_cls = *pb.pawn().obj.schema;
    const schema::ClassSchema& pc_cls = *pb.player_controller().obj.schema;
    const struct { bs::StorageKind kind; const char* name; } kinds[] = {
        {bs::StorageKind::Backpack, "Backpack"}, {bs::StorageKind::Equipment, "Equipment"}, {bs::StorageKind::QuestStorage, "QuestStorage"}};
    for (size_t k = 0; k < 3; ++k) {
        const auto send = pb.storage_payload(kinds[k].kind, bs::Role::Pawn);
        const auto bits = send ? param_bits_of(*send, pawn_cls) : std::vector<BunchWriter>{};
        CHECK(bits.size() == 1 && bits_equal(bits[0], *c->payloads[k]), (std::string("the ") + kinds[k].name + " payload equals the capture's, " + std::to_string(c->payloads[k]->bits) + " bits").c_str());
    }
    // the events: the three storages' Adds, in order, one RPC each
    std::vector<BunchWriter> mine;
    for (const bs::ItemStorage s : {bs::ItemStorage::Backpack, bs::ItemStorage::Equipment, bs::ItemStorage::Quest}) {
        const auto sends = pb.storage_adds(s);
        CHECK(sends.has_value(), "the Adds of a storage build");
        if (!sends) return;
        for (const bs::Send& send : *sends) {
            CHECK(send.role == bs::Role::PlayerController && send.reliable() && !send.opens_channel && send.emission.exports.empty(), "a chain of Adds is reliable, on the PlayerController, with no export");
            for (BunchWriter& w : param_bits_of(send, pc_cls)) mine.push_back(std::move(w));
        }
    }
    bool order_ok = mine.size() == c->events.size();
    size_t first_bad = 0;
    for (size_t i = 0; order_ok && i < mine.size(); ++i) {
        if (!bits_equal(mine[i], *c->events[i])) {
            order_ok = false;
            first_bad = i;
        }
    }
    if (!order_ok) std::printf("    %zu events built, %zu captured; the first that differs is number %zu\n", mine.size(), c->events.size(), first_bad);
    CHECK(order_ok, "every ClientReceiveStorageEvent equals the capture's bit for bit, in the capture's order (the Backpack's, the Equipment's, the Quest storage's)");
    // Gathering stage 3: the resource containers -- each worn bag's payload (the bag's columns x rows slots, its Dimension and, for the Mining and Lumberjacking bags, the OverrideMatrix)
    // and its Adds equal the capture's bit for bit
    size_t resource_items = 0;
    for (const auto& [index, rpc] : c->resource_rpc) {
        const auto send = pb.storage_payload(bs::resource_kind(index), bs::Role::Pawn);
        const auto bits = send ? param_bits_of(*send, pawn_cls) : std::vector<BunchWriter>{};
        const std::string name = "Resource" + std::to_string(index);
        CHECK(bits.size() == 1 && bits_equal(bits[0], *rpc), (std::string("the ") + name + " payload equals the capture's, " + std::to_string(rpc->bits) + " bits").c_str());
        const auto sends = pb.storage_adds(static_cast<bs::ItemStorage>(static_cast<int>(bs::ItemStorage::Resource0) + index));
        std::vector<BunchWriter> mine_r;
        if (sends) {
            for (const bs::Send& send2 : *sends) {
                for (BunchWriter& w : param_bits_of(send2, pc_cls)) mine_r.push_back(std::move(w));
            }
        }
        bool same = sends.has_value() && mine_r.size() == c->resource_events[index].size();
        for (size_t i = 0; same && i < mine_r.size(); ++i) same = bits_equal(mine_r[i], *c->resource_events[index][i]);
        CHECK(same, (std::string("the ") + name + "'s " + std::to_string(c->resource_events[index].size()) + " Adds equal the capture's, bit for bit").c_str());
        resource_items += pb.item_count(static_cast<bs::ItemStorage>(static_cast<int>(bs::ItemStorage::Resource0) + index));
    }
    CHECK(resource_items == c->resource_items.size(), "the bootstrap holds every item of the resource containers");
    // not more than the chain limit in one chain
    const auto adds = pb.storage_adds(bs::ItemStorage::Backpack);
    bool small = adds.has_value();
    if (adds) for (const bs::Send& s : *adds) small = small && param_bits_of(s, pc_cls).size() <= bs::kMaxEventsPerChain;
    CHECK(small, "a chain holds at most kMaxEventsPerChain events");
    // building the Adds changes nothing: the payloads still hold what they held
    CHECK(pb.item_count(bs::ItemStorage::Backpack) + pb.item_count(bs::ItemStorage::Equipment) + pb.item_count(bs::ItemStorage::Quest) == expect_items, "building the join's events leaves the bootstrap's copy as it was");
}

static void test_captured_corpora() {
    test_corpus("the retail login (a level 20 Ranger)", cap::kLogin, 109, 64 + 29 + 16);
    test_corpus("the original server's Bard", cap::kBard, 13, 13);
}

// ---- the captured Bard and its kit: the pawn's second state and the join ------------------------------------------------------------

static bool bits_match(const BunchWriter& w, const capture_blocks::Bits& b) {
    const auto want = capture_blocks::bytes_of(b.hex);
    size_t diff = 0;
    if (w.bit_pos() != b.bits) {
        std::printf("    wrote %zu bits, expected %zu (%s)\n", w.bit_pos(), b.bits, b.name);
        return false;
    }
    if (!capture_blocks::same_bit_range(w.data(), want.data(), b.bits, &diff)) {
        std::printf("    first differing bit %zu (%s)\n", diff, b.name);
        return false;
    }
    return true;
}
// (CoA) (C_Y_B)

static std::vector<svc::InventoryItem> items_of(const Corpus& c) {
    std::vector<svc::InventoryItem> out;
    for (const bs::StoredItem& si : c.items) {
        const ItemContainer container = si.storage == bs::ItemStorage::Backpack ? Bag : si.storage == bs::ItemStorage::Equipment ? Worn : ItemContainer::Quest;
        const auto it = items::item_of(si.item, container, si.slot);
        if (!it) {
            std::printf("  [FAIL] a captured item does not make an inventory item\n");
            std::exit(2);
        }
        out.push_back(*it);
    }
    return out;
}

static void test_second_state_from_the_kit() {
    std::printf("the captured Bard from its kit: the pawn's second state is pkt#107 as before\n");
    const auto corpus = read_corpus(cap::kBard);
    if (!corpus) {
        CHECK(false, "the Bard's payloads read");
        return;
    }
    const Inv inv = inventory_of(items_of(*corpus));
    auto base = bs::captured_character_setup();
    if (!base) std::exit(2);
    bs::PlayerSetup setup = *base;
    // what the captured setup holds as constants is derived from the kit instead
    setup.held_items.items.clear();
    setup.appearance_ids.clear();
    setup.items = corpus->items;
    setup.held_items.array_key = bs::kInitialHeldItemsKey;
    setup.held_items.items = bs::held_items_for(inv);
    setup.appearance_ids = items::appearance_ids(inv);
    CHECK(setup.held_items.items.size() == base->held_items.items.size(), "23 HeldItems entries");
    bool held_same = true;
    for (size_t i = 0; i < setup.held_items.items.size() && i < base->held_items.items.size(); ++i) {
        const auto& a = setup.held_items.items[i];
        const auto& b = base->held_items.items[i];
        held_same = held_same && a.id == b.id && a.slot == b.slot && a.item_guid == b.item_guid && a.enchantment == b.enchantment;
    }
    CHECK(held_same && setup.held_items.array_key == base->held_items.array_key, "each with the captured id, slot and record, and the captured array key");
    CHECK(setup.appearance_ids == base->appearance_ids, "AppearanceIDs are the captured four");

    bs::PlayerBootstrap pb(setup, bs::GameStateMode::Tier2);
    CHECK(pb.ok(), "it builds");
    pb.player_controller_open();
    pb.load_tracker_before_possession();
    pb.pawn_open();
    pb.restart_camera_rotation();
    pb.possession_update();
    const auto second = pb.pawn_second_state();
    CHECK(second && second->emission.exports.size() == 1 && bits_match(second->emission.exports[0], capture_blocks::kPawnPossessedExports0) &&
              bits_match(second->emission.payload, capture_blocks::kPawnPossessedContent),
          "pkt#107: the export bunch and the 78235-bit payload (HeldItems, AppearanceIDs, the stats, the dynamic resource storage) match bit for bit");
}

/// The roles and names of a join's chains.
static std::vector<std::pair<bs::Role, std::string>> shape_of(const std::vector<bs::Send>& seq) {
    std::vector<std::pair<bs::Role, std::string>> out;
    for (const bs::Send& s : seq) out.emplace_back(s.role, s.what);
    return out;
}

static void test_join_with_items() {
    std::printf("the join sequence of a character with items\n");
    const auto corpus = read_corpus(cap::kLogin);
    if (!corpus) {
        CHECK(false, "the retail login's storages read");
        return;
    }
    bs::PlayerBootstrap pb(setup_of(*corpus), bs::GameStateMode::Tier2);
    const auto seq = pb.join_sequence();
    CHECK(seq.has_value(), "the sequence builds");
    if (!seq) return;
    const auto shape = shape_of(*seq);
    std::vector<std::string> names;
    for (const auto& [role, what] : shape) names.push_back(std::string(role == bs::Role::Pawn ? "pawn " : role == bs::Role::PlayerController ? "pc " : role == bs::Role::GameState ? "gs " : "ps ") + what);
    // PC open, HUD, two RPC bunches, GameState, tracker, then the events of the Backpack (64: four chains), the pawn's open, its payload, the Equipment's
    // events (29: two chains) and payload, the Quest storage's (16: one) and payload, the resource payload, restart, possession, second state, PlayerState
    const size_t at = 6;
    bool order = names.size() == at + 4 + 1 + 1 + 2 + 1 + 1 + 1 + 1 + 1 + 4 + 1 + 1 + 1 - 1 + 0;
    (void)order;
    auto is = [&](size_t i, const char* role_what) { return i < names.size() && names[i] == role_what; };
    size_t i = at;
    bool ok = true;
    for (int n = 0; n < 4; ++n) ok = ok && is(i++, "pc ClientReceiveStorageEvent");
    ok = ok && is(i++, "pawn pawn open") && is(i++, "pawn ClientReceiveStoragePayload");
    for (int n = 0; n < 2; ++n) ok = ok && is(i++, "pc ClientReceiveStorageEvent");
    ok = ok && is(i++, "pawn ClientReceiveStoragePayload") && is(i++, "pc ClientReceiveStorageEvent") && is(i++, "pawn ClientReceiveStoragePayload");
    for (int n = 0; n < 3; ++n) ok = ok && is(i++, "pc ClientReceiveStorageEvent") && is(i++, "pawn ClientReceiveStoragePayload");   // Gathering stage 3: each resource container's Adds, then its payload
    CHECK(ok, "the retail login's order: the Backpack's four chains of Adds, the pawn's open, the Backpack's payload; the Equipment's two and its payload; the Quest storage's one and its payload; each of the three resource containers' Adds and payload");
    CHECK(is(i, "pc ClientRestart, camera mode, rotation") && seq->size() == i + 5 && names.back() == "pc XPTracker", "then restart, possession, the pawn's second state, the PlayerState and (plan D1) the XPTracker");
    // the chains the client is sent are within the connection's limits
    bool limits = true;
    for (const bs::Send& s : *seq) {
        for (const BunchWriter& e : s.emission.exports) limits = limits && e.bit_pos() <= 7621;
        const size_t content = s.emission.payload.bit_pos();
        const size_t parts = s.emission.exports.size() + (content > 7621 ? (content + 7615) / 7616 : 1);
        limits = limits && parts <= 256 && content <= 524288;
    }
    CHECK(limits, "every chain has at most 256 parts, every export bunch at most 7621 bits and no payload is over 524,288 bits");
    // the events go out reliably on the PlayerController, in chains of at most 16
    bool events_ok = true;
    for (const bs::Send& s : *seq) {
        if (std::string(s.what) == "ClientReceiveStorageEvent") events_ok = events_ok && s.role == bs::Role::PlayerController && s.reliable() && !s.opens_channel;
    }
    CHECK(events_ok, "the events are reliable and on the PlayerController's open channel");
}

// ---- the retail answers through the bootstrap: what the client is sent and how the pawn's HeldItems moves ---------------------------
// [C:Y:B x CoA]

static int held_array_key(const bs::PlayerBootstrap& pb) {
    const repl::Subobject* sub = pb.pawn().find(pb.setup().guids.base_character_info);
    const schema::NetField* f = sub && sub->obj.schema ? sub->obj.schema->field("HeldItems") : nullptr;
    const repl::FastArrayState* a = f ? sub->obj.arrays.find(f->index) : nullptr;
    return a ? a->array_key() : -1;
}

static void test_retail_answers_through_the_bootstrap() {
    std::printf("the retail answers through the bootstrap: the events the client is sent, and the HeldItems array key\n");
    const FakeCatalog catalog = catalog_of_captures();
    size_t scenarios_ok = 0, sends_ok = 0;
    for (const auto& sc : capture_equip::kScenarios) {
        Inv inv = inventory_before(sc);
        auto base = bs::captured_character_setup();
        if (!base) std::exit(2);
        bs::PlayerSetup setup = *base;
        setup.items.clear();
        for (const svc::InventoryItem& it : inv.items()) {
            bs::StoredItem si;
            si.storage = it.container == Bag ? bs::ItemStorage::Backpack : it.container == Worn ? bs::ItemStorage::Equipment : bs::ItemStorage::Quest;
            si.slot = it.slot;
            si.item = items::to_instance(it);
            setup.items.push_back(si);
        }
        setup.held_items.array_key = bs::kInitialHeldItemsKey;
        setup.held_items.items = bs::held_items_for(inv);
        setup.appearance_ids = items::appearance_ids(inv);
        bs::PlayerBootstrap pb(setup, bs::GameStateMode::Tier2);
        if (!pb.ok() || !pb.join_sequence()) {
            std::printf("    %d: the bootstrap does not build or join: %s\n", sc.capture, pb.error().c_str());
            continue;
        }
        const int key_before = held_array_key(pb);
        const schema::ClassSchema& pc_cls = *pb.player_controller().obj.schema;
        size_t equipment_events = 0, event_index = 0;
        bool sends_match = true, applied = true;
        for (size_t r = 0; r < sc.nrequests; ++r) {
            const items::Outcome o = inv.use(sc.requests[r].storage == 3 ? Bag : Worn, sc.requests[r].slot, sc.requests[r].guid, kAny, catalog);
            std::vector<bs::ItemEvent> events;
            for (const items::Event& e : o.events) {
                bs::ItemEvent ev;
                ev.type = e.type;
                ev.storage = e.container == Bag ? bs::ItemStorage::Backpack : bs::ItemStorage::Equipment;
                ev.slot = e.slot;
                ev.item = items::to_instance(e.item);
                ev.instigated = e.instigated;
                events.push_back(ev);
                equipment_events += e.container == Worn ? 1 : 0;
            }
            const auto send = pb.item_events(events);
            sends_match = sends_match && send.has_value();
            if (!send) break;
            // each RPC of the chain, parsed, is the retail event of its position
            const auto rpcs = [&] {
                std::vector<repl::ItemStorageEvent> out;
                const auto blocks = repl::read_content_blocks(send->emission.payload.data(), send->emission.payload.bit_pos(), 0);
                if (!blocks) return out;
                for (const auto& b : *blocks) {
                    size_t pos = b.payload_pos;
                    const auto h = repl::read_field_header(pc_cls, send->emission.payload.data(), pos + b.payload_bits, pos);
                    const schema::RpcSchema* rpc = h ? pc_cls.rpc("ClientReceiveStorageEvent") : nullptr;
                    if (!rpc) return std::vector<repl::ItemStorageEvent>{};
                    const auto params = repl::read_rpc_params(*rpc, send->emission.payload.data(), pos + h->payload_bits, pos);
                    const auto ev = params ? repl::parse_storage_event(*rpc, *params) : std::nullopt;
                    if (!ev) return std::vector<repl::ItemStorageEvent>{};
                    out.push_back(*ev);
                }
                return out;
            }();
            sends_match = sends_match && rpcs.size() == events.size();
            for (size_t i = 0; sends_match && i < rpcs.size() && event_index + i < sc.nevents; ++i) {
                const auto& want = sc.events[event_index + i];
                const auto& got = rpcs[i];
                const bool inst_ok = !want.instigated ? got.instigator == repl::GuidValue{} : !(got.instigator == repl::GuidValue{});
                sends_match = sends_match && got.type == want.type && got.targets.size() == 1 && got.targets[0].slots.size() == 1 && got.targets[0].slots[0].slot == want.slot &&
                              got.targets[0].object == (want.container == 0 ? pb.setup().guids.backpack : pb.setup().guids.equipment) &&
                              static_cast<int64_t>(got.item.record_id) == want.record && items::guid_hex(got.item.guid) == want.guid && inst_ok;
            }
            event_index += rpcs.size();
            if (o.equipment_changed) {
                const auto held = bs::held_items_for(inv);
                const auto app = items::appearance_ids(inv);
                size_t marks = 0;
                for (const items::Event& e : o.events) marks += e.container == Worn ? 1 : 0;
                applied = applied && pb.apply_equipment(held, app, marks ? marks : 1);
            }
        }
        const int moved = held_array_key(pb) - key_before;
        const auto update = pb.equipment_update();
        const auto again = pb.equipment_update();
        const bool key_ok = moved == sc.key_moved;
        if (!sends_match || !applied || !key_ok) std::printf("    %d: sends %d applied %d key moved %d, retail %d\n", sc.capture, int(sends_match), int(applied), moved, sc.key_moved);
        sends_ok += sends_match ? 1 : 0;
        scenarios_ok += (sends_match && applied && key_ok && update && !again) ? 1 : 0;
    }
    CHECK(sends_ok == std::size(capture_equip::kScenarios), "in every capture the events the bootstrap builds read back as the retail events: type, storage object, slot, record, guid and instigator");
    CHECK(scenarios_ok == std::size(capture_equip::kScenarios),
          "and the HeldItems array key moves as the retail server's did (23 for each change of the Equipment storage: 23 for a chest piece, 69 for a two-hander over a main hand and an off hand), "
          "one update carries it and a second has nothing");
}
// (C_Y_B) (C O A)

/// restore(checkpoint) puts the HeldItems array back with the shadows: apply_equipment moves the array key before the update is built, and a chain the
/// connection did not take is built again from the checkpoint, so the retry must move the key once, to the same bits, not twice.
static void test_restore_undoes_the_held_items() {
    std::printf("restore(checkpoint) puts HeldItems back\n");
    const FakeCatalog catalog = fixture_catalog();
    const auto base = bs::captured_character_setup();
    if (!base) std::exit(2);
    // the Bard's kit worn; one run takes the chest off (key +23), another puts a two-hander on over the Scepter and the Focus (key +69)
    for (const bool two_hander : {false, true}) {
        std::vector<svc::InventoryItem> kit = bard_kit();
        if (two_hander) kit.push_back(make_item(Bag, 0, fi::kSword2H, 300));
        Inv inv = inventory_of(kit);
        bs::PlayerSetup setup = *base;
        setup.items.clear();
        for (const svc::InventoryItem& it : inv.items()) {
            bs::StoredItem si;
            si.storage = it.container == Bag ? bs::ItemStorage::Backpack : it.container == Worn ? bs::ItemStorage::Equipment : bs::ItemStorage::Quest;
            si.slot = it.slot;
            si.item = items::to_instance(it);
            setup.items.push_back(si);
        }
        setup.held_items.array_key = bs::kInitialHeldItemsKey;
        setup.held_items.items = bs::held_items_for(inv);
        setup.appearance_ids = items::appearance_ids(inv);
        bs::PlayerBootstrap pb(setup, bs::GameStateMode::Tier2);
        const char* what = two_hander ? "a two-hander over a main hand and an off hand" : "the chest off";
        if (!pb.ok() || !pb.join_sequence()) {
            CHECK(false, (std::string("the bootstrap does not join: ") + pb.error()).c_str());
            continue;
        }
        const int key0 = held_array_key(pb);
        const items::Outcome o = two_hander ? inv.use(Bag, 0, guid_n(300), kAny, catalog) : inv.use(Worn, 2, guid_n(102), kAny, catalog);
        size_t marks = 0;
        for (const items::Event& e : o.events) marks += e.container == Worn ? 1 : 0;
        const auto held = bs::held_items_for(inv);
        const auto app = items::appearance_ids(inv);
        const int moves = two_hander ? 69 : 23;
        CHECK(o.ok && o.equipment_changed && marks == (two_hander ? 3u : 1u),
              (std::string(what) + ": the request applies and changes the Equipment " + std::to_string(two_hander ? 3 : 1) + " time(s)").c_str());

        const auto cp = pb.checkpoint();
        CHECK(pb.apply_equipment(held, app, marks) && held_array_key(pb) == key0 + moves,
              (std::string(what) + ": the first build moves the array key by " + std::to_string(moves)).c_str());
        const auto first = pb.equipment_update();
        CHECK(first.has_value(), "the update builds");
        // the connection's window is full: the session restores the checkpoint, and builds the reply again later
        pb.restore(cp);
        CHECK(held_array_key(pb) == key0, (std::string(what) + ": restore puts the array key back").c_str());
        CHECK(pb.apply_equipment(held, app, marks) && held_array_key(pb) == key0 + moves,
              (std::string(what) + ": the retry moves it once more, by the same " + std::to_string(moves) + ", not twice").c_str());
        const auto second = pb.equipment_update();
        CHECK(first && second && first->emission.payload.bit_pos() == second->emission.payload.bit_pos() &&
                  capture_blocks::same_bit_range(first->emission.payload.data(), second->emission.payload.data(), first->emission.payload.bit_pos(), nullptr),
              "and builds the same update, bit for bit");
        CHECK(!pb.equipment_update(), "once sent, a second update has nothing");
    }
    {
        // the first update of a pawn that had nothing worn inserts the 23 entries (and marks the array dirty): restore empties the array again,
        // so the retry inserts again rather than changing what the abandoned attempt inserted
        std::vector<svc::InventoryItem> kit;
        for (auto it : bard_kit()) {
            it.container = Bag;
            it.slot = static_cast<int>(kit.size());
            kit.push_back(it);
        }
        Inv inv = inventory_of(kit);
        bs::PlayerSetup setup = *base;
        setup.items.clear();
        for (const svc::InventoryItem& it : inv.items()) {
            bs::StoredItem si;
            si.storage = bs::ItemStorage::Backpack;
            si.slot = it.slot;
            si.item = items::to_instance(it);
            setup.items.push_back(si);
        }
        setup.held_items.array_key = bs::kInitialHeldItemsKey;
        setup.held_items.items.clear();
        setup.appearance_ids.clear();
        bs::PlayerBootstrap pb(setup, bs::GameStateMode::Tier2);
        if (!pb.ok() || !pb.join_sequence()) {
            CHECK(false, "the bootstrap with nothing worn does not join");
            return;
        }
        const int key0 = held_array_key(pb);
        const Inv worn_one = [&] {
            Inv x = inv;
            x.use(Bag, 1, guid_n(102), kAny, catalog);   // the chest is the kit's second item
            return x;
        }();
        const auto held = bs::held_items_for(worn_one);
        const auto app = items::appearance_ids(worn_one);
        const auto cp = pb.checkpoint();
        CHECK(pb.apply_equipment(held, app, 1), "the first update of a naked pawn applies");
        const int key1 = held_array_key(pb);
        const auto first = pb.equipment_update();
        pb.restore(cp);
        CHECK(held_array_key(pb) == key0, "restore: the array is as it was before the first update (the key too)");
        CHECK(pb.apply_equipment(held, app, 1) && held_array_key(pb) == key1, "the retry inserts again and ends on the same key");
        const auto second = pb.equipment_update();
        CHECK(first && second && first->emission.payload.bit_pos() == second->emission.payload.bit_pos() &&
                  capture_blocks::same_bit_range(first->emission.payload.data(), second->emission.payload.data(), first->emission.payload.bit_pos(), nullptr),
              "and builds the same update, bit for bit");
    }
}
// Cyb - C O A

// ---- the setup of a joining character --------------------------------------------------------------------------------------------

static svc::Character kit_record() {
    svc::Character c;
    c.id = "bbec5c86eb101fe5f38c87f0fa22e7d0";
    c.account_id = 3;
    c.name = "cyb";
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    return c;
}

static void test_setup_with_items() {
    std::printf("build_player_setup with items\n");
    const fs::path dir = cf::make_temp_dir("worldd_items_");
    const fs::path path = dir / "content.db";
    if (!cf::make_db(path)) std::exit(2);
    std::string error;
    auto content = content::ContentDb::open(path.string(), error);
    if (!content) {
        std::printf("  [FAIL] the fixture does not open: %s\n", error.c_str());
        std::exit(2);
    }
    const bs::LoginIdentity login{"DESKTOP-TEST", "DESKTOP-TEST"};
    bs::SessionValues session;
    session.server_id = 60;
    svc::Character cyb = kit_record();
    cyb.items = bard_kit();
    auto r = bs::build_player_setup(cyb, *content, login, session, world_stats::of(*content));
    CHECK(r.setup.has_value() && r.inventory.has_value(), "a Bard with its kit builds, with its Inventory");
    if (r.setup && r.inventory) {
        CHECK(r.setup->items.size() == 13 && r.inventory->size() == 13, "thirteen items in the setup");
        const auto held = bs::held_items_for(*r.inventory);
        CHECK(r.setup->held_items.items.size() == 23 && r.setup->held_items.array_key == bs::kInitialHeldItemsKey && r.setup->held_items.items[0].item_guid == fi::kScepter &&
                  r.setup->held_items.items[1].item_guid == fi::kFocus && r.setup->held_items.items[2].item_guid == fi::kBook && r.setup->held_items.items[3].item_guid == 0,
              "HeldItems has the 23 slots with the Scepter, Focus and Book");
        bool same = held.size() == r.setup->held_items.items.size();
        for (size_t i = 0; same && i < held.size(); ++i) same = held[i].id == r.setup->held_items.items[i].id && held[i].item_guid == r.setup->held_items.items[i].item_guid;
        CHECK(same, "as held_items_for gives them");
        CHECK(r.setup->appearance_ids.size() == 4 && r.setup->appearance_ids[0].item_equip_slot == 1 && static_cast<int64_t>(r.setup->appearance_ids[1].record_guid) == fi::kChest,
              "AppearanceIDs: the helmet, chest, legs and feet");
        bool note = false;
        for (const std::string& n : r.notes) note = note || n.find("13") != std::string::npos || n.find("items:") != std::string::npos;
        CHECK(note, "the notes say how many items the character has");
    }
    // a record with no items: nothing is sent as before
    svc::Character bare = kit_record();
    r = bs::build_player_setup(bare, *content, login, session, world_stats::of(*content));
    CHECK(r.setup && r.setup->items.empty() && r.setup->held_items.items.empty() && r.setup->appearance_ids.empty() && r.inventory && r.inventory->size() == 0,
          "a character with no items has none in its setup, no HeldItems and no AppearanceIDs");
    // a bag item and a quest item are in the setup, and show nothing
    svc::Character bag = kit_record();
    bag.items = {make_item(Bag, 3, fi::kPotion, 1), make_item(ItemContainer::Quest, 0, 77, 2)};
    r = bs::build_player_setup(bag, *content, login, session, world_stats::of(*content));
    CHECK(r.setup && r.setup->items.size() == 2 && r.setup->held_items.items.empty() && r.setup->appearance_ids.empty(), "items in the bag and the quest storage are sent and show nothing");
    // items that cannot be played refuse the login, saying why
    for (const auto& [name, items_, want] : std::vector<std::tuple<const char*, std::vector<svc::InventoryItem>, const char*>>{
             {"two items in a slot", {make_item(Bag, 1, 5, 1), make_item(Bag, 1, 6, 2)}, "two items in Backpack slot 1"},
             {"a slot outside the storage", {make_item(Bag, 70, 5, 1)}, "slot 70"},
             {"one guid twice", {make_item(Bag, 1, 5, 1), make_item(Bag, 2, 6, 1)}, "instance guid"},
             {"a zero quantity", {[] { auto it = make_item(Bag, 1, 5, 1); it.quantity = 0; return it; }()}, "quantity"}}) {
        svc::Character broken = kit_record();
        broken.items = items_;
        const auto b = bs::build_player_setup(broken, *content, login, session, world_stats::of(*content));
        CHECK(!b.setup && !b.inventory && b.error.find("items cannot be played") != std::string::npos && b.error.find(want) != std::string::npos,
              (std::string(name) + " refuse the login: " + b.error).c_str());
    }
    std::error_code ec;
    content.reset();
    fs::remove_all(dir, ec);
}

// ---- content.db ------------------------------------------------------------------------------------------------------------------

static void test_content_items() {
    std::printf("content.db's items\n");
    const fs::path dir = cf::make_temp_dir("worldd_items_db_");
    const fs::path path = dir / "content.db";
    if (!cf::make_db(path)) std::exit(2);
    std::string error;
    auto db = content::ContentDb::open(path.string(), error);
    CHECK(db.has_value(), "the fixture opens, with the item tables");
    if (db) {
        const auto chest = db->item(fi::kChest, error);
        CHECK(chest && chest->guid == fi::kChest && chest->name == "Gear_Armor_Light_Starter_Chest" && !chest->can_equip && !chest->occupies_all_equip_slots &&
                  chest->equip_slots == std::vector<uint8_t>{2},
              "the starter chest: slot 2, CanEquip 0 (and worn)");
        const auto focus = db->item(fi::kFocus, error);
        CHECK(focus && focus->equip_slots == std::vector<uint8_t>({16, 18}), "a Focus lists Secondary and RangedOffHand, in the record's order");
        const auto ear = db->item(fi::kEarLeftFirst, error);
        CHECK(ear && ear->equip_slots == std::vector<uint8_t>({14, 13}), "an earring that lists EarLeft first");
        const auto book = db->item(fi::kBook, error);
        CHECK(book && book->occupies_all_equip_slots && book->equip_slots == std::vector<uint8_t>({17, 18}), "a Book is two-handed");
        const auto potion = db->item(fi::kPotion, error);
        CHECK(potion && potion->equip_slots.empty(), "a potion lists no slot");
        error = "stale";
        const auto none = db->item(1234567, error);
        CHECK(!none && error.empty(), "an item the file does not know is no item and no error");
        items::ContentItemCatalog catalog(*db);
        const auto info = catalog.info(fi::kBook);
        CHECK(info && info->two_handed && info->wearable() && info->equip_slots.size() == 2 && !catalog.info(1234567) && !catalog.info(fi::kPotion)->wearable(), "ContentItemCatalog is the same facts");
    }
    // a content.db without the item tables is refused at open, saying to rebuild it
    cf::DbOptions o;
    o.item_tables = false;
    const fs::path old_path = dir / "old.db";
    if (!cf::make_db(old_path, o)) std::exit(2);
    std::string why;
    const auto old = content::ContentDb::open(old_path.string(), why);
    CHECK(!old && why.find("rebuild it with this checkout's contentc") != std::string::npos, "a file built before plan B2 is refused with the way out");
    db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);

    // the real file: the Bard's kit and the equip facts, when AOC_CONTENT_DB names one
    const char* real = std::getenv("AOC_CONTENT_DB");
    if (!real || !*real) {
        std::printf("  (AOC_CONTENT_DB is not set: the real file's half is skipped)\n");
        return;
    }
    std::string e2;
    auto rdb = content::ContentDb::open(real, e2);
    CHECK(rdb.has_value(), "the real content.db opens");
    if (!rdb) return;
    items::ContentItemCatalog rc(*rdb);
    const auto chest = rdb->item(6064630214426886486ll, e2);
    CHECK(chest && !chest->can_equip && chest->equip_slots == std::vector<uint8_t>{2}, "the real starter chest: CanEquip 0 and slot 2");
    const auto book = rdb->item(6064631670974054529ll, e2);
    CHECK(book && book->occupies_all_equip_slots && book->equip_slots == std::vector<uint8_t>({17, 18}), "the real Book: two-handed, Ranged and RangedOffHand");
    const auto bag = rdb->item(6064629382556287782ll, e2);
    CHECK(bag && bag->equip_slots == std::vector<uint8_t>({23, 24, 25}), "the real resource bag: the three bag slots");
    // the Bard's kit through the rules: each piece is worn where the record says
    Inv kit = inventory_of({make_item(Bag, 0, 6064630214426886486ll, 1), make_item(Bag, 1, 6064631131688402948ll, 2), make_item(Bag, 2, 6064631670974054529ll, 3),
                            make_item(Bag, 3, 6064630695486029824ll, 4)});
    bool worn = true;
    for (int s = 0; s < 4; ++s) worn = worn && kit.use(Bag, s, guid_n(s + 1), kAny, rc).ok;
    CHECK(worn && kit.at(Worn, 2) && kit.at(Worn, 15) && kit.at(Worn, 17) && kit.at(Worn, 16) && !kit.at(Worn, 18), "from the real file: the chest, Scepter, Book and Focus are worn in Chest, Primary, Ranged and Secondary");
    const auto two = rdb->item(6064630695616315423ll, e2);
    CHECK(two && two->occupies_all_equip_slots && two->equip_slots == std::vector<uint8_t>({15, 16}), "the real Sword2H: Primary and Secondary, two-handed");
}
// (C:Y:B + CoA)

// ---- hostile requests ----------------------------------------------------------------------------------------------------------

static void test_fuzz() {
    std::printf("hostile requests\n");
    const FakeCatalog catalog = fixture_catalog();
    std::mt19937_64 rng(0xB2B2);
    size_t applied = 0, refused = 0, invariant_breaks = 0, lost = 0;
    for (int round = 0; round < 60; ++round) {
        std::vector<svc::InventoryItem> start = bard_kit();
        uint32_t n = 500;
        const int64_t pool[] = {fi::kPotion, fi::kRingA, fi::kRingB, fi::kEarLeftFirst, fi::kEarRightFirst, fi::kSword2H, fi::kMace1H, fi::kShield, fi::kHeavyChest,
                                fi::kGatherCape, fi::kCostume, fi::kCloak, fi::kShirt, fi::kFocus, fi::kBook, 99999, fi::kChest};
        const int bag_items = static_cast<int>(rng() % 70);
        std::set<int> used;
        while (static_cast<int>(used.size()) < bag_items) used.insert(static_cast<int>(rng() % 70));
        for (const int s : used) start.push_back(make_item(Bag, s, pool[rng() % (sizeof pool / sizeof pool[0])], ++n, 1 + static_cast<int32_t>(rng() % 3)));
        Inv inv = inventory_of(start);
        std::multiset<std::string> guids;
        for (const auto& it : inv.items()) guids.insert(it.instance_guid);
        for (int step = 0; step < 300; ++step) {
            const Inv before = inv;
            items::Outcome o;
            const auto pick_container = [&]() { return rng() % 8 == 0 ? static_cast<ItemContainer>(rng() % 5) : (rng() % 2 ? Bag : Worn); };
            const auto pick_slot = [&](ItemContainer c) {
                const int limit = c == Bag ? 70 : c == Worn ? 83 : 100;
                return rng() % 10 == 0 ? static_cast<int>(rng() % 300) - 100 : static_cast<int>(rng() % limit);
            };
            const auto guid_of_slot = [&](ItemContainer c, int s) -> std::string {
                const auto* it = inv.at(c, s);
                return it && rng() % 6 ? it->instance_guid : (rng() % 2 ? guid_n(static_cast<uint32_t>(rng() % 600)) : std::string("garbage"));
            };
            switch (rng() % 4) {
                case 0: {
                    const ItemContainer c = pick_container();
                    const int s = pick_slot(c);
                    o = inv.use(c, s, guid_of_slot(c, s), rng() % 4 == 0 ? static_cast<uint32_t>(rng() % 100) : kAny, catalog);
                    break;
                }
                case 1: {
                    const ItemContainer a = pick_container(), b = pick_container();
                    const int sa = pick_slot(a), sb = pick_slot(b);
                    const auto* it = inv.at(a, sa);
                    o = inv.move(a, sa, it && rng() % 6 ? it->quantity : static_cast<int32_t>(rng() % 5), guid_of_slot(a, sa), b, sb);
                    break;
                }
                case 2: {
                    const ItemContainer a = pick_container(), b = pick_container();
                    const int sa = pick_slot(a), sb = pick_slot(b);
                    const auto* ia = inv.at(a, sa);
                    const auto* ib = inv.at(b, sb);
                    o = inv.swap(a, sa, ia && rng() % 6 ? ia->quantity : 1, guid_of_slot(a, sa), b, sb, guid_of_slot(b, sb), ib && rng() % 6 ? ib->quantity : 1);
                    break;
                }
                default: {   // a legal move or use, most often
                    const auto all = inv.items();
                    if (all.empty()) break;
                    const svc::InventoryItem& it = all[rng() % all.size()];
                    if (it.container == Bag && rng() % 2) o = inv.move(Bag, it.slot, it.quantity, it.instance_guid, Bag, static_cast<int>(rng() % 70));
                    else if (it.container != ItemContainer::Quest) o = inv.use(it.container, it.slot, it.instance_guid, kAny, catalog);
                    break;
                }
            }
            if (!o.ok) {
                ++refused;
                if (!o.events.empty() || o.equipment_changed || !(before == inv)) ++invariant_breaks;   // a refusal changes nothing and says nothing
                continue;
            }
            ++applied;
            // what an applied request leaves: valid items, one per slot and guid, none made or lost, the events the inventory's difference
            std::multiset<std::string> now;
            std::string why;
            for (const auto& it : inv.items()) now.insert(it.instance_guid);
            if (now != guids) ++lost;
            if (!Inv::from_items(inv.items(), why) || inv.size() != before.size()) ++invariant_breaks;
            // the events, replayed on the earlier inventory, give the later one
            std::map<std::pair<int, int>, svc::InventoryItem> replay;
            for (const auto& it : before.items()) replay[{static_cast<int>(it.container), it.slot}] = it;
            for (const items::Event& e : o.events) {
                const std::pair<int, int> key{static_cast<int>(e.container), e.slot};
                if (e.type == Add) replay[key] = e.item;
                else {
                    const auto f = replay.find(key);
                    if (f == replay.end() || f->second.instance_guid != e.item.instance_guid) ++invariant_breaks;
                    else replay.erase(f);
                }
            }
            std::vector<svc::InventoryItem> replayed;
            for (const auto& [k, it] : replay) replayed.push_back(it);
            if (!(inventory_of(replayed) == inv)) ++invariant_breaks;
        }
    }
    CHECK(applied > 1000 && refused > 1000, (std::to_string(applied) + " requests applied and " + std::to_string(refused) + " refused").c_str());
    CHECK(invariant_breaks == 0 && lost == 0,
          "never a broken invariant: the items stay valid, one per slot and guid; none is made or lost; the events replayed on the earlier inventory give the later one; a refusal changes and says nothing");
}

// ---- the review's fuzz: long random sequences with the strongest invariants ------------------------------------------------------------

namespace {

/// A catalog that remembers answers (the real content.db is a SQLite file: asking a million times is slow).
struct CachedCatalog : items::ItemCatalog {
    const items::ItemCatalog& source;
    mutable std::map<int64_t, std::optional<items::ItemInfo>> cache;
    explicit CachedCatalog(const items::ItemCatalog& s) : source(s) {}
    std::optional<items::ItemInfo> info(int64_t record) const override {
        auto it = cache.find(record);
        if (it == cache.end()) it = cache.emplace(record, source.info(record)).first;
        return it->second;
    }
};

struct FuzzStats {
    size_t applied = 0, refused = 0, equips = 0, displacing = 0, unequips = 0, max_events = 0, violations = 0;
    std::vector<std::string> first;   // the first few violations, for the log
    void violation(const std::string& what) {
        if (++violations <= 8) first.push_back(what);
    }
};

/// `rounds` random inventories (worn items placed as the rules would, a bag filled to a random level, sometimes full, a few quest items), each put
/// through `steps` random requests: legal and hostile uses, moves and swaps. Checked after every one: a refusal changes nothing and says nothing;
/// an applied request makes or loses no item, leaves a valid inventory (one item per slot, one guid per item), wears every item only in a slot its
/// record lists and leaves no two-hander's other slots to another item; and its events, replayed on the earlier inventory (an Add never into an
/// occupied slot, a Remove only of the item that is there), give the later one, in at most 16 events (what one chain carries).
void fuzz_inventories(const items::ItemCatalog& catalog, const std::vector<int64_t>& pool, uint64_t seed, int rounds, int steps, FuzzStats& st) {
    std::vector<int64_t> wearable, twoh, plain;
    for (const int64_t r : pool) {
        const auto info = catalog.info(r);
        if (!info) continue;
        if (info->wearable()) {
            wearable.push_back(r);
            if (info->two_handed) twoh.push_back(r);
        } else {
            plain.push_back(r);
        }
    }
    if (wearable.empty() || plain.empty()) {
        st.violation("the pool has no wearable or no plain record");
        return;
    }
    std::mt19937_64 rng(seed);
    const auto pick = [&](uint64_t n) { return static_cast<size_t>(rng() % n); };
    for (int round = 0; round < rounds; ++round) {
        std::vector<svc::InventoryItem> start;
        uint32_t n = 1000u * static_cast<uint32_t>(round + 1);
        std::map<int, int64_t> worn;
        std::set<int> covered;
        const int want = 1 + static_cast<int>(pick(18));
        for (int k = 0; k < want; ++k) {
            const int64_t r = pick(5) == 0 && !twoh.empty() ? twoh[pick(twoh.size())] : wearable[pick(wearable.size())];
            const auto info = *catalog.info(r);
            const int slot = info.equip_slots[pick(info.equip_slots.size())];
            const std::vector<int> foot = info.two_handed ? std::vector<int>(info.equip_slots.begin(), info.equip_slots.end()) : std::vector<int>{slot};
            bool ok = true;
            for (const int s : foot) ok = ok && !covered.count(s) && !worn.count(s);
            const int place = info.two_handed ? info.equip_slots.front() : slot;   // a two-hander is placed in its first slot, as the rules do
            if (!ok || worn.count(place)) continue;
            worn[place] = r;
            for (const int s : foot) covered.insert(s);
            start.push_back(make_item(Worn, place, r, ++n));
        }
        const int levels[] = {0, 5, 30, 60, 66, 68, 69, 70, 70, 70};
        const int fill = levels[pick(sizeof levels / sizeof levels[0])];
        std::set<int> used;
        while (static_cast<int>(used.size()) < fill) used.insert(static_cast<int>(pick(70)));
        for (const int s : used) {
            const size_t k = pick(10);
            const int64_t r = k < 6 ? wearable[pick(wearable.size())] : k < 8 && !twoh.empty() ? twoh[pick(twoh.size())] : plain[pick(plain.size())];
            start.push_back(make_item(Bag, s, r, ++n, 1 + static_cast<int32_t>(pick(3))));
        }
        std::set<int> quest_slots;
        for (int q = static_cast<int>(pick(4)); q > 0; --q) {
            const int slot = static_cast<int>(pick(100));
            if (quest_slots.insert(slot).second) start.push_back(make_item(ItemContainer::Quest, slot, plain[pick(plain.size())], ++n));
        }
        std::string setup_error;
        const auto first = Inv::from_items(start, setup_error);
        if (!first) {
            st.violation("the setup is invalid: " + setup_error);
            return;
        }
        Inv inv = *first;
        std::multiset<std::string> guids;
        for (const auto& it : inv.items()) guids.insert(it.instance_guid);
        const size_t total = inv.size();
// C | Y | B.C | O | A

        for (int step = 0; step < steps; ++step) {
            const Inv before = inv;
            const auto all = inv.items();
            items::Outcome o;
            bool is_equip = false, is_unequip = false;
            switch (pick(6)) {
                case 0: case 1: case 2: {   // a legal use, sometimes naming the slot
                    if (all.empty()) break;
                    const auto& it = all[pick(all.size())];
                    if (it.container == ItemContainer::Quest) break;
                    uint32_t requested = kAny;
                    if (it.container == Bag && pick(5) == 0) {
                        const auto info = catalog.info(it.item_record_id);
                        if (info && info->wearable()) requested = info->equip_slots[pick(info->equip_slots.size())];
                    }
                    is_equip = it.container == Bag;
                    is_unequip = it.container == Worn;
                    o = inv.use(it.container, it.slot, it.instance_guid, requested, catalog);
                    break;
                }
                case 3: {   // a legal move or swap inside the bag
                    std::vector<const svc::InventoryItem*> in_bag;
                    for (const auto& it : all)
                        if (it.container == Bag) in_bag.push_back(&it);
                    if (in_bag.empty()) break;
                    const auto* a = in_bag[pick(in_bag.size())];
                    const int to = static_cast<int>(pick(70));
                    const auto* target = inv.at(Bag, to);
                    if (target) o = inv.swap(Bag, a->slot, a->quantity, a->instance_guid, Bag, to, target->instance_guid, target->quantity);
                    else o = inv.move(Bag, a->slot, a->quantity, a->instance_guid, Bag, to);
                    break;
                }
                case 4: {   // hostile: random containers, slots, guids and requested slots
                    const ItemContainer c = static_cast<ItemContainer>(pick(5));
                    const int slot = static_cast<int>(pick(300)) - 100;
                    std::string g = guid_n(static_cast<uint32_t>(pick(500000)));
                    if (const auto* it = inv.at(c, slot); it && pick(2)) g = it->instance_guid;
                    o = inv.use(c, slot, g, pick(3) == 0 ? static_cast<uint32_t>(pick(400)) : kAny, catalog);
                    break;
                }
                default: {   // hostile move or swap
                    const ItemContainer a = static_cast<ItemContainer>(pick(4)), b = static_cast<ItemContainer>(pick(4));
                    const int sa = static_cast<int>(pick(100)) - 10, sb = static_cast<int>(pick(100)) - 10;
                    const auto* ia = inv.at(a, sa);
                    const auto* ib = inv.at(b, sb);
                    const std::string ga = ia && pick(5) ? ia->instance_guid : guid_n(static_cast<uint32_t>(pick(500000)));
                    const std::string gb = ib && pick(5) ? ib->instance_guid : guid_n(static_cast<uint32_t>(pick(500000)));
                    if (pick(2)) o = inv.move(a, sa, ia ? ia->quantity : 1, ga, b, sb);
                    else o = inv.swap(a, sa, ia ? ia->quantity : 1, ga, b, sb, gb, ib ? ib->quantity : 1);
                    break;
                }
            }
            if (!o.ok) {
                if (!o.events.empty() || o.equipment_changed || !(before == inv)) st.violation("a refusal changed something or said something");
                ++st.refused;
                continue;
            }
            ++st.applied;
            if (is_equip) {
                ++st.equips;
                if (o.events.size() > 2) ++st.displacing;
            }
            if (is_unequip) ++st.unequips;
            st.max_events = std::max(st.max_events, o.events.size());
            if (o.events.size() > 16) st.violation("an outcome has more than 16 events (kMaxEventsPerChain): the client would get none");
            std::multiset<std::string> now;
            for (const auto& it : inv.items()) now.insert(it.instance_guid);
            if (now != guids || inv.size() != total) st.violation("items made or lost");
            std::string why;
            if (!Inv::from_items(inv.items(), why)) st.violation("invalid inventory after an applied request: " + why);
            std::map<int, const svc::InventoryItem*> in_slot;
            const auto after = inv.items();
            for (const auto& it : after)
                if (it.container == Worn) in_slot[it.slot] = &it;
            for (const auto& [slot, it] : in_slot) {
                const auto info = catalog.info(it->item_record_id);
                if (!info || std::find(info->equip_slots.begin(), info->equip_slots.end(), static_cast<uint8_t>(slot)) == info->equip_slots.end()) {
                    st.violation("an item is worn in a slot its record does not list: slot " + std::to_string(slot) + " record " + std::to_string(it->item_record_id));
                }
                if (info && info->two_handed) {
                    for (const uint8_t s : info->equip_slots) {
                        if (s != slot && in_slot.count(s)) st.violation("a two-hander in slot " + std::to_string(slot) + " covers slot " + std::to_string(s) + ", which another item holds");
                    }
                }
            }
            std::map<std::pair<int, int>, svc::InventoryItem> replay;
            for (const auto& it : before.items()) replay[{static_cast<int>(it.container), it.slot}] = it;
            for (const items::Event& e : o.events) {
                const std::pair<int, int> key{static_cast<int>(e.container), e.slot};
                if (e.type == Add) {
                    if (replay.count(key)) st.violation("an Add into an occupied slot (the client would overwrite without a Remove)");
                    if (e.slot < 0 || e.slot >= items::slot_count(e.container)) st.violation("an Add outside the storage");
                    replay[key] = e.item;
                } else {
                    const auto f = replay.find(key);
                    if (f == replay.end() || f->second.instance_guid != e.item.instance_guid) st.violation("a Remove of an item that is not there");
                    else replay.erase(f);
                }
            }
            std::vector<svc::InventoryItem> replayed;
            for (const auto& [k, it] : replay) replayed.push_back(it);
            std::string replay_error;
            const auto replayed_inv = Inv::from_items(replayed, replay_error);
            if (!replayed_inv) st.violation("the replay is invalid (two items in a slot or one guid twice): " + replay_error);
            else if (!(*replayed_inv == inv)) st.violation("the events replayed on the earlier inventory do not give the later one");
            (void)items::held_records(inv);       // the visible gear is derivable from every reachable inventory
            (void)items::appearance_ids(inv);
        }
    }
}

}  // namespace

static void report_fuzz(const char* what, const FuzzStats& st, size_t min_applied, double seconds) {
    std::printf("    %s: %zu applied, %zu refused (equips %zu, displacing %zu, unequips %zu), at most %zu events, %.2f s\n", what, st.applied, st.refused, st.equips,
                st.displacing, st.unequips, st.max_events, seconds);
    for (const std::string& v : st.first) std::printf("      [VIOLATION] %s\n", v.c_str());
    CHECK(st.violations == 0 && st.applied >= min_applied && st.refused > 0,
          (std::string(what) + ": never a violated invariant (no item made or lost, valid and worn only where the record lists, events replay to the later inventory)").c_str());
}
// -- C:Y:B & C.O.A --

static void test_review_fuzz() {
    std::printf("the review's fuzz: long sequences with the strongest invariants\n");
    {
        // the fixture's records: wearable and two-handed ones, a plain one, an unknown one
        FakeCatalog fixture = fixture_catalog();
        fixture.add(fi::kPotion, {});
        std::vector<int64_t> pool;
        for (const auto& [record, info] : fixture.m) pool.push_back(record);
        pool.push_back(99999);
        FuzzStats st;
        const auto started = std::chrono::steady_clock::now();
        for (const uint64_t seed : {0xB2C0FFEEull, 1ull, 2ull}) fuzz_inventories(fixture, pool, seed, 40, 300, st);
        report_fuzz("the fixture's catalog, three seeds", st, 5000, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    }
    const char* real = std::getenv("AOC_CONTENT_DB");
    if (!real || !*real) {
        std::printf("  (AOC_CONTENT_DB is not set: the real catalog's half is skipped)\n");
        return;
    }
    std::string error;
    auto db = content::ContentDb::open(real, error);
    CHECK(db.has_value(), "the real content.db opens");
    if (!db) return;
    // every fifth item record of the real file (about 2,100 of its 10,568), read straight from the file
    std::vector<int64_t> pool;
    {
        sqlite3* raw = nullptr;
        if (sqlite3_open_v2(real, &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            sqlite3_stmt* q = nullptr;
            if (sqlite3_prepare_v2(raw, "SELECT guid FROM item ORDER BY guid", -1, &q, nullptr) == SQLITE_OK) {
                for (size_t i = 0; sqlite3_step(q) == SQLITE_ROW; ++i)
                    if (i % 5 == 0) pool.push_back(sqlite3_column_int64(q, 0));
            }
            sqlite3_finalize(q);
        }
        sqlite3_close(raw);
    }
    items::ContentItemCatalog real_catalog(*db);
    CachedCatalog cached(real_catalog);
    CHECK(pool.size() > 1000, "the real file's records are read for the pool");
    FuzzStats st;
    const auto started = std::chrono::steady_clock::now();
    for (const uint64_t seed : {0xB2C0FFEEull, 7ull}) fuzz_inventories(cached, pool, seed, 50, 300, st);
    report_fuzz("the real catalog, two seeds", st, 5000, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

static void test_review_codec_noise() {
    std::printf("random bytes through the item readers\n");
    size_t read_ok = 0;
    std::mt19937_64 rng(99);
    for (int i = 0; i < 40000; ++i) {
        std::vector<uint8_t> bytes(1 + rng() % 300);
        for (auto& b : bytes) b = static_cast<uint8_t>(rng());
        if (rng() % 3 == 0) bytes[0] |= 1;   // has_item
        aoc::protocol::wire::PacketReader a(bytes.data(), bytes.size());
        if (repl::read_item_instance(a)) ++read_ok;
        aoc::protocol::wire::PacketReader b(bytes.data(), bytes.size());
        (void)repl::read_item_storage_event(b);
    }
    std::printf("    %zu instances read from 40,000 buffers of noise\n", read_ok);
    CHECK(true, "no read ran past its buffer (a build with AddressSanitizer would have stopped it)");
}

// ---- main ----------------------------------------------------------------------------------------------------------------------

// ---- the pick-up popup (Gathering stage 3) -----------------------------------------------------------------------------------

/// items/acquired.h against the 24 retail harvest events (tests/capture_resource.h, `full_hex`: the events as captured): the popup's callout made from the item, its name and the
/// time alone, put into the event, is the retail event bit for bit.  The name is what the retail text says between the brackets; the test_worldd_content_db and contentc's
/// TestRealItemDisplayNames tests check that content.db's item.display_name reads the same names.
static void test_acquired_popup() {
    std::printf("the pick-up popup: items/acquired.h against the 24 retail harvest events\n");
    const schema::RpcSchema* rpc = class_named("AoCPlayerControllerBP_C").rpc("ClientReceiveStorageEvent");
    CHECK(rpc != nullptr, "the PlayerController has ClientReceiveStorageEvent");
    if (!rpc) return;
    size_t n = 0, same_text = 0, same_bits = 0, bands = 0;
    for (const capture_resource::Harvest& h : capture_resource::kHarvests) {
        ++n;
        const auto bytes = capture_blocks::bytes_of(h.full_hex);
        repl::CodecOptions opts;
        opts.inline_objects = true;
        size_t pos = 0;
        auto params = repl::read_rpc_params(*rpc, bytes.data(), h.full_bits, pos, opts);
        if (!params || pos != h.full_bits) continue;
        repl::RpcCall call;
        call.rpc = rpc;
        call.params = std::move(*params);
        call.decoded = true;
        const auto retail = repl::parse_storage_event(call);
        if (!retail || !retail->callout) continue;
        const std::string& said = retail->callout->message;
        const size_t open = said.find("> [");
        const size_t close = said.find(']', open == std::string::npos ? 0 : open);
        if (open == std::string::npos || close == std::string::npos) continue;
        const std::string name = said.substr(open + 3, close - open - 3);
        // our event: the retail one with its callout taken out and made again
        repl::ItemStorageEvent mine = *retail;
        mine.callout = items::acquired_callout(mine.item, name, retail->callout->creation_time.ticks);
        same_text += mine.callout->message == said ? 1 : 0;
        bands += items::rarity_band_name(h.quality) == std::string_view(said).substr(said.find("Item_") + 5, said.find('"', said.find("Item_")) - said.find("Item_") - 5) ? 1 : 0;
        const auto mine_params = repl::storage_event_params(*rpc, mine);
        BunchWriter w;
        bool failed = false;
        if (mine_params) repl::write_rpc_params(*rpc, *mine_params, w, opts, &failed);
        if (mine_params && !failed && w.bit_pos() == h.full_bits && capture_blocks::same_bit_range(w.data(), bytes.data(), h.full_bits, nullptr)) ++same_bits;
    }
    CHECK(n == 24 && same_text == n, "the text acquired_text makes from the record, quality, quantity and name is the retail text, 24 of 24");
    CHECK(bands == n, "the style is the band of the quality (Common 1, Uncommon 6, Rare 16, Heroic 31) in all 24");
    CHECK(same_bits == n, "the event with acquired_callout's callout is the retail event to the last bit, 24 of 24 (the creation time is the capture's)");
    // the bands of the qualities no capture shows, and a quantity of 1 or 2
    CHECK(items::rarity_band_name(5) == "Common" && items::rarity_band_name(6) == "Uncommon" && items::rarity_band_name(15) == "Uncommon" && items::rarity_band_name(16) == "Rare" &&
              items::rarity_band_name(30) == "Rare" && items::rarity_band_name(31) == "Heroic" && items::rarity_band_name(50) == "Heroic" && items::rarity_band_name(51) == "Epic" &&
              items::rarity_band_name(71) == "Legendary" && items::rarity_band_name(101) == "Artifact" && items::rarity_band_name(200) == "Artifact" && items::rarity_band_name(0) == "Common",
          "the rarity bands: Common 1-5, Uncommon 6-15, Rare 16-30, Heroic 31-50, Epic 51-70, Legendary 71-100, Artifact 101-200");
    repl::ItemInstance one;
    one.has_item = true;
    one.record_id = 42;
    one.quality = 1;
    one.quantity = 1;
    repl::ItemInstance two = one;
    two.quantity = 2;
    CHECK(items::acquired_text(one, "A").find("[A]</>") != std::string::npos && items::acquired_text(one, "A").find("</>x") == std::string::npos &&
              items::acquired_text(two, "A").find("[A] </>x2") != std::string::npos,
          "a quantity of 1 ends [Name]</> and of 2 ends [Name] </>x2");
}

int main() {
    test_guids_and_ticks();
    test_from_items();
    test_equip_and_unequip();
    test_slot_choice();
    test_two_handers();
    test_refusals();
    test_move_and_swap();
    test_visible_gear();
    test_retail_equips();
    test_captured_corpora();
    test_second_state_from_the_kit();
    test_join_with_items();
    test_retail_answers_through_the_bootstrap();
    test_restore_undoes_the_held_items();
    test_setup_with_items();
    test_content_items();
    test_acquired_popup();
    test_fuzz();
    test_review_fuzz();
    test_review_codec_noise();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_combat.cpp
//
//  The player in the fights of the area: the combat side the world's fights see (player_combat_side.h), the hits creatures land on the player, the experience of kills,
//  the player's death and the loot window.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "crafting/job.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "world/city_nodes.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "items/acquired.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "world/zoi_surface.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

#include "session/player_combat_side.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

// ============================================================================================================================
//  plan E1, stage 3: this player in the fights of the area
// ============================================================================================================================

void PlayerSession::leave_combat() {
    if (combat_side_ && npc_world_ && npc_world_->combat()) npc_world_->combat()->remove_player(combat_side_.get());
    hit_port_.reset();
    if (pipeline_) pipeline_->set_combat(nullptr);
    combat_side_.reset();
}

bool PlayerSession::own_inline_object(const repl::InlineObject& o) const {
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    return o.outer == g.pawn || o.outer == g.collision_cylinder;
}

std::vector<uint64_t> PlayerSession::actors_of(const std::vector<protocol::emit::FIntrepidNetworkGUID>& objects, const std::vector<repl::InlineObject>& inline_objects) const {
    std::vector<uint64_t> out;
    if (!npc_world_ || !npc_world_->combat()) return out;
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    const auto add = [&](uint64_t object_id) {
        const uint64_t actor = npc_world_->combat()->actor_of_object(object_id);
        if (actor != 0 && std::find(out.begin(), out.end(), actor) == out.end()) out.push_back(actor);
    };
    for (const protocol::emit::FIntrepidNetworkGUID& o : objects) {
        if (o == g.collision_cylinder || o == g.pawn) continue;   // the caster's own components
        add(o.ObjectId);
    }
    // E1 stage 3 fix: a component named in place (the client has no GUID for an NPC's CollisionCylinder: worldd never exported it) belongs to its
    // outer, the actor; a play test named every NPC so and every cast "found nobody"
    for (const repl::InlineObject& o : inline_objects) {
        if (own_inline_object(o) || o.outer.ObjectId == 0) continue;
        add(o.outer.ObjectId);
    }
    return out;
}
// C.O.A with C | Y | B

void PlayerSession::take_hit(Connection& conn, const combat::IncomingHit& h, Clock::time_point now) {
    if (!pipeline_) return;
    // the mounts milestone, stage 3: a hit aimed at a mounted rider lands on the MOUNT's Health; the rider's own is untouched
    if (mounted()) return hit_on_mount(conn, h, now);
    pipeline_->receive_hit(h, ability_seconds(now));
    if (mounts_ && (h.flags & stats::kHitMiss) == 0) {
        for (const combat::ResolvedMod& m : h.mods) {
            if (m.stat_guid == content::kGuidHealth && m.value < 0) {
                mount_damaged(now);
                break;
            }
        }
    }
}

void PlayerSession::drain_combat(Connection& conn, Clock::time_point now) {
    if (!combat_side_ || !pipeline_) return;
    const double sec = ability_seconds(now);
    std::vector<combat::IncomingHit> hits;
    hits.swap(combat_side_->inbox);
    for (const combat::IncomingHit& h : hits) take_hit(conn, h, now);
    // the hits of the effects he put on creatures (a burn's damage each second): confirmed to his client, which draws the number
    std::vector<combat::EffectHitNote> notes;
    notes.swap(combat_side_->effect_notes);
    for (const combat::EffectHitNote& n : notes) pipeline_->confirm_effect_hit(n, sec);
    // the experience of the kills (a level-up sends a dozen chains: it waits for room)
    while (!combat_side_->kills.empty()) {
        if (!room_for(12 + (world_ ? world_->auto_grants().size() : 0))) break;
        const combat::KillReward k = combat_side_->kills.front();
        combat_side_->kills.erase(combat_side_->kills.begin());
        apply_kill(conn, k, now);
    }
    if (combat_side_->wants_in_combat != combat_flag_ && stats_ && stats_->started()) {
        combat_flag_ = combat_side_->wants_in_combat;
        stats_->set_in_combat(combat_flag_);
        if (abilities_) abilities_->set_in_combat(combat_flag_, sec);
        pipeline_->refresh(sec, combat_flag_ ? "combat starts" : "combat ends", true);   // the regeneration is five times faster out of combat
        if (pb_.set_in_combat(combat_flag_)) {
            queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("combat state"); }, conn, /*must_build=*/true);
        }
    }
}
// C.Y.B x C O A

double PlayerSession::take_damage(Connection& conn, double amount, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return 0;
    if (mounted()) {
        // a rider's damage lands on its mount (stage 3); nothing reaches the rider's Health
        if (amount > 0 && ride_) {
            const Ride::Hurt hurt = ride_->hurt(amount);
            ++mount_hits_;
            if (hurt.died) mount_died(conn, now);
        }
        return 0;
    }
    if (amount > 0) mount_damaged(now);
    const double through = pipeline_->take_damage(amount, ability_seconds(now));
    take_ability_events();
    flush_abilities(conn);
    return through;
}

// ============================================================================================================================
//  plan E1, stage 3, part 2: kills and loot
// ============================================================================================================================

namespace {

/// The instance guid of the stack in slot `slot` of the corpse `npc` (the window and the events agree on it; it is not a guid the bag holds).
repl::GuidValue loot_guid(uint64_t npc, size_t slot) {
    uint64_t s = npc * 0x9E3779B97F4A7C15ull + slot * 0xBF58476D1CE4E5B9ull + 0x5851F42D4C957F2Dull;
    const auto next = [&s]() {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    repl::GuidValue g;
    const uint64_t a = next(), b = next();
    g.a = static_cast<uint32_t>(a);
    g.b = static_cast<uint32_t>(a >> 32);
    g.c = static_cast<uint32_t>(b);
    g.d = static_cast<uint32_t>(b >> 32) | 1u;   // never the zero guid
    return g;
}

}  // namespace

// ============================================================================================================================
//  plan E1, stage 3, part 2: the player's death
// ============================================================================================================================

namespace {

/// Whether an item record is a material or currency (the retail PvE death takes about half of each stack of those, not the gear): its gameplay tags
/// include Item.Resource... (the Glint certificates, reagents, raw materials).
bool is_material(const content::ContentDb& db, int64_t record) {
    std::string err;
    const auto gear = db.item_gear(record, err);
    if (!gear) return false;
    for (const std::string& t : gear->tags) {
        std::string lower;
        for (const char c : t) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.rfind("item.resource", 0) == 0) return true;
    }
    return false;
}

}  // namespace

void PlayerSession::check_death(Connection& conn, Clock::time_point now) {
    if (dead_ || !stats_ || !stats_->started()) return;
    const stats::StatValue* h = stats_->find(content::kGuidHealth);
    if (!h || h->value > 0) return;
    die(conn, now);
}

void PlayerSession::die(Connection& conn, Clock::time_point now) {
    dead_ = true;
    ++deaths_;
    cancel_quick_make(&conn, "the player died");   // Crafting, step 3
    cancel_gather(&conn, "the player died", Interrupt::Death);   // Gathering stage 2
    if (pipeline_) pipeline_->set_dead(true);
    item_reserved_.clear();   // consumables: the casts that had not paid are gone with the death (set_dead dropped them): their items stay in the bag and can be used again
    // the mounts milestone, stage 3: the dead player is dismounted (the death frame below restarts its pawn) and its mount is desummoned
    if (mounted()) dismount(conn, now, DismountWhy::PlayerDied);
    if (mounts_ && abilities_) mounts_->desummon(ability_seconds(now));
    const GoodPosition at = checker_.good();
    death_x_ = at.x;
    death_y_ = at.y;
    death_z_ = at.z;
    loot_actor_ = 0;
    vendor_actor_ = 0;   // the windows close with the death: a purchase or a loot take from a dead character is refused (E1 review A4)
    vendor_by_talk_ = false;
    end_dialogue(conn, "the player died");
    // the frame of the retail captures: where it fell, the restart of the pawn, the pawn dead with no Health and no Mana
    set_current_value(content::kGuidMana, 0.0f);
    pb_.set_dead(true);
    const double x = at.x, y = at.y, z = at.z;
    queue("ClientSetLastDeathLocation", [x, y, z](bootstrap::PlayerBootstrap& pb) { return pb.death_location(x, y, z); }, conn, /*must_build=*/true);
    queue("ClientRestart (death)", [](bootstrap::PlayerBootstrap& pb) { return pb.death_restart(); }, conn, /*must_build=*/true);
    // what it costs: the experience debt and the materials of the bag (about half of each stack), which fall into a pile where it died
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    int64_t debt_added = 0;
    if (curve) {
        const leveling::Progress next = leveling::with_death_debt(*curve, progress_, 0.02);
        debt_added = next.debt - progress_.debt;
        progress_ = next;
        progress_changed_ = true;
    }
    std::vector<combat::LootStack> lost;
    items::Outcome all;
    all.ok = true;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const bool loss_on = !npc_world_ || npc_world_->aftermath_config().death_loss;
    if (db && loss_on) {
        for (const svc::InventoryItem& it : inventory_.items()) {
            // Gathering stage 3: the materials are in the resource containers now (the Backpack holds them only for a bag that had no room at the login)
            if ((it.container != svc::ItemContainer::Backpack && !items::is_resource(it.container)) || it.quantity <= 0 || !is_material(*db, it.item_record_id)) continue;
            const int32_t take = it.quantity == 1 ? 1 : (it.quantity + 1) / 2;
            const items::Outcome c = inventory_.consume(it.container, it.slot, it.instance_guid, take);
            if (!c.ok) continue;
            all.events.insert(all.events.end(), c.events.begin(), c.events.end());
            combat::LootStack s;
            s.item = it.item_record_id;
            s.quantity = take;
            s.quality = it.quality;
            lost.push_back(s);
        }
    }
    if (!all.events.empty()) {
        inventory_dirty_ = true;
        inventory_changed_ = true;
        ++items_applied_;
        const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
        for (size_t at2 = 0; at2 < events.size(); at2 += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at2),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at2 + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent (death)", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    queue("pawn state (death)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("death"); }, conn, /*must_build=*/true);
    if (curve) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        queue("XPTracker (death)", [total, debt](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            return pb.leveling_update();
        }, conn, /*must_build=*/true);
        hand_over_progress("a death");
    }
    // the pile
    pile_actor_ = 0;
    if (!lost.empty() && npc_world_ && npc_world_->aftermath()) {
        const std::array<uint32_t, 4>& g = pb_.setup().character_guid;
        std::string err;
        pile_actor_ = npc_world_->aftermath()->drop_pile(x, y, z, repl::GuidValue{g[0], g[1], g[2], g[3]}, pb_.setup().name, lost, npc_world_->world_clock(), err);
        if (pile_actor_ == 0) spdlog::warn("[session] {}: the pile of the death is not made: {} ({} stack(s) of materials are lost)", name(), err, lost.size());
    }
    // the bag goes to the saver with the death, not at the next periodic save: a crash within the minute would give the materials back to this
    // character while the pile's taker keeps them too (E1 review A5)
    if (inventory_dirty_) hand_over_inventory("a death", /*periodic=*/false);
    spdlog::log(level_for(death_logged_), "[session] {}: died at ({:.0f}, {:.0f}, {:.0f}): experience debt +{} (now {}), {} stack(s) of materials lost{}", name(), x, y, z, debt_added, progress_.debt,
                lost.size(), pile_actor_ != 0 ? " into a pile where it fell" : "");
    (void)now;
}
// -- C | O | A & Cyb --

void PlayerSession::apply_kill(Connection& conn, const combat::KillReward& k, Clock::time_point now) {
    ++kills_rewarded_;
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    const std::span<const content::AutoGrant> grants = world_ ? world_->auto_grants() : std::span<const content::AutoGrant>();
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (curve && k.adventuring_xp > 0) {
        xp = leveling::add_xp(*curve, grants, progress_, k.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    if (leveled) finish_level_up(xp, false, now, rank_effects, skill_updates);
    // weapon XP: the kill's Weapon experience goes to every weapon line of what is worn, each line once; a line has no debt to pay first and stops at its top
    const std::vector<int64_t> worn_lines = k.weapon_xp > 0 ? equipped_weapon_lines() : std::vector<int64_t>();
    std::vector<weaponxp::Change> weapon_changes;
    for (const int64_t type : worn_lines) {
        const weaponxp::Change c = weapon_.add_xp(type, k.weapon_xp);
        if (c.changed()) weapon_changes.push_back(c);
    }
    bool weapons_leveled = false;
    for (const weaponxp::Change& c : weapon_changes) weapons_leveled = weapons_leveled || c.leveled();
    if (!weapon_changes.empty()) progress_changed_ = true;
    const bool weapon_pawn = apply_weapon_levels(weapon_changes, now);   // the new levels' ranks, effect items and stats, before any reply is built
    if (xp.changed() || leveled || !weapon_changes.empty()) {
        // one XPTracker block with everything that moved: Adventuring when its total or debt did (a kill at the Adventuring cap moves only the lines) and each paid line, with the lists of
        // the weapon trees that gained a rank
        const bool adventuring = xp.changed() || leveled;
        const int64_t total = progress_.xp, debt = progress_.debt;
        std::vector<std::pair<int64_t, int64_t>> lines;
        for (const weaponxp::Change& c : weapon_changes) lines.emplace_back(c.line, c.xp_after);
        queue("XPTracker", [adventuring, total, debt, lines](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (adventuring && !pb.set_progress(total, debt)) return std::nullopt;
            for (const auto& [type, line_total] : lines) {
                if (!pb.set_weapon_line(type, line_total)) return std::nullopt;
            }
            return pb.leveling_update();
        }, conn, /*must_build=*/true);
    }
    if (leveled) {
        const int level = progress_.level;
        queue("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn);
    }
    for (const weaponxp::Change& c : weapon_changes) {
        if (!c.leveled()) continue;
        const int64_t type = c.line;
        const int level = c.level_after;
        queue("BroadcastPlayLevelUpEffects", [type, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(type, level); }, conn);
    }
    if (leveled) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, /*must_build=*/true);
    } else if (weapon_pawn) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("a weapon line"); }, conn, /*must_build=*/false);
    }
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, /*must_build=*/true);
    }
    std::string weapon_text;   // the log: what each worn line holds now
    if (k.weapon_xp > 0) {
        weapon_text = ", weapon lines +" + std::to_string(k.weapon_xp) + " each";
        if (!weapon_.valid()) weapon_text += " (this world has no weapon lines)";
        else if (worn_lines.empty()) weapon_text += " (no weapon is worn)";
        for (const int64_t type : worn_lines) {
            const weaponxp::Line* line = weapon_.lines()->find(type);
            const bool up = std::any_of(weapon_changes.begin(), weapon_changes.end(), [&](const weaponxp::Change& c) { return c.line == type && c.leveled(); });
            weapon_text += ", " + (line ? line->name : std::to_string(type)) + " " + std::to_string(weapon_.xp(type)) + " XP (level " + std::to_string(weapon_.level(type)) + (up ? ", level-up" : "") + ")";
        }
    }
    spdlog::log(level_for(use_logged_), "[session] {}: killed {} (level {}): Adventuring +{} XP{}{} (now {} XP, debt {}){}", name(), k.name.empty() ? k.blueprint : k.name, k.level,
                k.adventuring_xp, weapon_text, leveled ? ", level " + std::to_string(xp.after.level) : "", progress_.xp, progress_.debt, k.loot ? "; there is loot" : "");
    if (xp.changed() || !weapon_changes.empty()) hand_over_progress(leveled || weapons_leveled ? "a level-up" : "XP earned");
    quest_kill(conn, k, now);   // quests: the Kill objectives
}
// -- Cyb x C | O | A --

void PlayerSession::open_loot(Connection& conn, const combat::Corpse& corpse, const world::WorldActor& actor, Clock::time_point now) {
    const auto ignore = [&](const std::string& why) {
        ++loot_refused_;
        spdlog::log(level_for(loot_logged_), "[session] {}: the loot of {} is not opened: {}", name(), corpse.name, why);
    };
    if (!within_budget(loot_budget_, "a loot interaction", now)) return;
    if (!corpse.parts || corpse.remaining() == 0) return ignore("there is nothing left in it");
    if (!corpse.is_pile && !corpse.rights.count(pb_.setup().guids.pawn.ObjectId)) return ignore("the player did not fight it");
    const GoodPosition at = checker_.good();
    if (std::hypot(actor.x - at.x, actor.y - at.y) > kInteractRangeCm) return ignore("it is " + std::to_string(static_cast<int>(std::hypot(actor.x - at.x, actor.y - at.y) / 100.0)) + " m away");
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || !room_for(2)) return;
    repl::StoragePayload data;
    data.maximum_items = static_cast<int32_t>(corpse.loot.size());
    data.client_retrieve_only = true;
    const repl::GuidValue me{pb_.setup().character_guid[0], pb_.setup().character_guid[1], pb_.setup().character_guid[2], pb_.setup().character_guid[3]};
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (size_t i = 0; i < corpse.loot.size(); ++i) {
        repl::StorageSlot slot;
        const combat::LootStack& s = corpse.loot[i];
        if (!s.taken) {
            std::string err;
            const auto rec = db->item(s.item, err);
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = s.item;
            it.instance_guid = items::guid_hex(loot_guid(corpse.npc, i));
            it.quantity = static_cast<int32_t>(std::min<int64_t>(s.quantity, 1 << 20));
            it.quality = s.quality;
            it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
            it.created_at_ms = created;
            slot.item = items::to_instance(it);
            slot.slot = static_cast<int32_t>(i);
            slot.check_loot_permission = true;
            slot.loot_permission.push_back(me);
        }
        data.storage.push_back(std::move(slot));
    }
    const FIntrepidNetworkGUID loot_actor_guid = actor.actor.guid;
    loot_actor_ = corpse.npc;
    ++loot_opens_;
    spdlog::log(level_for(loot_logged_), "[session] {}: opens the loot of {} ({} stack(s))", name(), corpse.name, corpse.remaining());
    queue("ClientReceiveLootStoragePayload", [loot_actor_guid, data](bootstrap::PlayerBootstrap& pb) { return pb.loot_payload(loot_actor_guid, data); }, conn, /*must_build=*/true);
}

void PlayerSession::on_loot_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto events = repl::parse_loot_events(rpc.call);
    if (!events) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerRequestLootStorageEvents does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(loot_budget_, "ServerRequestLootStorageEvents", now)) return;
    const auto refuse = [&](const std::string& why) {
        ++loot_refused_;
        spdlog::log(level_for(loot_logged_), "[session] {}: a loot request is refused: {}", name(), why);
    };
    combat::Aftermath* after = npc_world_ ? npc_world_->aftermath() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!after || !db || !registry_) return refuse("the world has no loot");
    if (loot_actor_ == 0) return refuse("no loot window is open");
    const combat::Corpse* corpse = after->corpse_of(loot_actor_);
    const world::WorldActor* actor = registry_->find(loot_actor_);
    if (!corpse || !corpse->parts || !actor) {
        loot_actor_ = 0;
        return refuse("the corpse is gone");
    }
    const GoodPosition at = checker_.good();
    if (std::hypot(actor->x - at.x, actor->y - at.y) > kInteractRangeCm) {
        loot_actor_ = 0;
        return refuse("the corpse is too far");
    }
    if (!corpse->is_pile && !corpse->rights.count(pb_.setup().guids.pawn.ObjectId)) return refuse("the player may not take from it");
    if (events->empty() || events->size() > kMaxLootTakes) return refuse(events->empty() ? "no events" : "too many events");
    if (!room_for(4)) return;

    // what the client asks for, checked event by event: the container is the corpse's, the slot holds a stack that is still there, the item is that stack.
    // An event that fails is skipped and the others are taken (the game's server judges each event of a request on its own); a request with no event left is refused.
    std::vector<size_t> slots;
    size_t skipped = 0;
    std::string first_why;
    const auto skip = [&](const std::string& why) {
        ++skipped;
        if (first_why.empty()) first_why = why;
    };
    for (const repl::ItemStorageEvent& e : *events) {
        if (e.move_source.object.ObjectId != corpse->parts->container || e.move_source.slots.size() != 1) { skip("an event names another container"); continue; }
        const repl::StorageEventSlot& s = e.move_source.slots[0];
        if (s.slot < 0 || static_cast<size_t>(s.slot) >= corpse->loot.size()) { skip("an event names a slot that is none"); continue; }
        const size_t slot = static_cast<size_t>(s.slot);
        if (corpse->loot[slot].taken) { skip("a stack was taken already"); continue; }
        if (!e.item.has_item || e.item.record_id != static_cast<uint64_t>(corpse->loot[slot].item) || !(e.item.guid == loot_guid(corpse->npc, slot))) {
            skip("an event's item is not the stack of its slot");
            continue;
        }
        if (std::find(slots.begin(), slots.end(), slot) != slots.end()) { skip("a slot twice"); continue; }
        slots.push_back(slot);
    }
    if (slots.empty()) return refuse(first_why.empty() ? std::string("no event can be taken") : first_why);
    if (skipped > 0) spdlog::log(level_for(loot_logged_), "[session] {}: {} event(s) of the loot request are skipped ({}); the other {} are taken", name(), skipped, first_why, slots.size());
    // the bag takes each stack or leaves it (a full bag leaves the rest in the corpse)
    std::vector<bootstrap::LootRemoval> removed;
    items::Outcome all;
    all.ok = true;
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    size_t bag_full = 0;
    for (const size_t slot : slots) {
        const combat::LootStack* stack = after->take(corpse->npc, slot);
        if (!stack) continue;
        const auto rec = [&] {
            std::string err;
            return db->item(stack->item, err);
        }();
        const int32_t max_stack = rec ? std::max(1, rec->max_stack_size) : 1;
        std::vector<svc::InventoryItem> fresh;
        int64_t left = stack->quantity;
        while (left > 0) {
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = stack->item;
            it.instance_guid = fresh_guid();
            for (const svc::InventoryItem& other : fresh) {
                if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
            }
            it.quantity = static_cast<int32_t>(std::min<int64_t>(left, max_stack));
            it.quality = stack->quality;
            it.durability = rec ? rec->max_durability_at(it.quality) : 0.0f;
            it.created_at_ms = created;
            left -= it.quantity;
            fresh.push_back(std::move(it));
        }
        // Gathering stage 3: a stack goes where its kind says (a pile mixes both kinds) and stays in the corpse when that storage has no room for it
        const items::Outcome got = inventory_.grant(std::move(fresh), item_catalog());
        if (!got.ok) {
            after->put_back(corpse->npc, slot);
            ++bag_full;
            continue;
        }
        // the container's side: the stack leaves its slot (the same instance guid the window showed)
        svc::InventoryItem shown;
        shown.container = svc::ItemContainer::Backpack;
        shown.item_record_id = stack->item;
        shown.instance_guid = items::guid_hex(loot_guid(corpse->npc, slot));
        shown.quantity = static_cast<int32_t>(std::min<int64_t>(stack->quantity, 1 << 20));
        shown.quality = stack->quality;
        shown.durability = rec ? rec->max_durability_at(shown.quality) : 0.0f;
        shown.created_at_ms = created;
        removed.push_back({static_cast<int32_t>(slot), items::to_instance(shown)});
        all.events.insert(all.events.end(), got.events.begin(), got.events.end());
        ++loot_taken_;
    }
    if (bag_full > 0) spdlog::log(level_for(loot_logged_), "[session] {}: the bag has no room for {} stack(s): they stay in the corpse", name(), bag_full);
    if (removed.empty()) return;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    // retail's order: the removal from the loot container, the add to the bag, and the window closes when nothing is left
    const FIntrepidNetworkGUID container = corpse->parts->container_guid;
    queue("ClientReceiveStorageEvent (loot)", [container, removed](bootstrap::PlayerBootstrap& pb) { return pb.loot_removed(container, removed); }, conn, /*must_build=*/true);
    std::vector<bootstrap::ItemEvent> adds = item_events_for(all);
    add_item_toasts(adds, /*removals=*/false);   // every Add of the bag carries the "Acquired" popup of the item feed, in the stack's own quality and rarity
    for (size_t at2 = 0; at2 < adds.size(); at2 += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(adds.begin() + static_cast<std::ptrdiff_t>(at2),
                                                     adds.begin() + static_cast<std::ptrdiff_t>(std::min(adds.size(), at2 + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::log(level_for(loot_logged_), "[session] {}: takes {} stack(s) from {}: {} storage event(s) into the bag", name(), removed.size(), corpse->name, all.events.size());
    if (corpse->remaining() == 0) {
        const FIntrepidNetworkGUID loot_actor_guid = actor->actor.guid;
        queue("ClientRemoveListenerFromStorage", [loot_actor_guid](bootstrap::PlayerBootstrap& pb) { return pb.loot_listener_removed(loot_actor_guid); }, conn, /*must_build=*/true);
        after->emptied(corpse->npc, npc_world_->world_clock());
        loot_actor_ = 0;
    }
}
// ~ C.O.A - C Y B ~

/// The client calls of the loot window (session/rpc_table.h).
void PlayerSession::register_combat_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerRequestLootStorageEvents", [](PlayerSession& s, const RpcContext& c) { s.on_loot_request(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd

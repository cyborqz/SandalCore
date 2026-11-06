// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_progress.cpp
//
//  Experience and levels: the stats for what the character wears, the use of an item that gives experience or changes the class, the level-up, the professions and the weapon lines' experience.
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

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

void PlayerSession::refresh_rank_mods() {
    if (stats_ && book_) {
        stats_->set_passives(book_->passives(wielded_line_));
        stats_->set_ranks(book_->held_ranks(wielded_line_));
    }
    if (pipeline_ && book_) pipeline_->set_reaction_carriers(book_->reaction_carriers(wielded_line_));   // the reactions the ranks' effects carry
    sync_ability_values();   // (the ranks changed what the formulas give Sprint's speed)
}
// CoA with C_Y_B

std::vector<std::string> PlayerSession::weapon_tags() const {
    // the item in the weapon slot of the set in hand: the ranged slot while the ranged set is in hand, else the main hand
    if (!world_) return {};
    const stats::GearCatalog* gear = world_->stat_context().gear;
    const svc::InventoryItem* held = inventory_.at(svc::ItemContainer::Equipment, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
    if (!gear || !held) return {};
    const stats::ItemGear* item = gear->item(held->item_record_id);
    return item ? item->tags : std::vector<std::string>{};
}

void PlayerSession::apply_gear_stats(const char* what) {
    if (!stats_ || !stats_->started()) return;
    refresh_rank_mods();   // plan D1, stage 2: the ranks the character has (and the weapon line in hand) are an input like the gear
    // plan D2: a class change takes the old class's effects off and resolves the new class's resource rule (its own stat changes go out with it)
    if (pipeline_ && pipeline_->resource_class() != stats_->class_enum()) pipeline_->refresh(pipeline_->now(), "the class changed", false);
    std::vector<stats::StatValue> changed;
    stats::CharacterStats::Next next;
    std::string error;
    // The new stats are only prepared: they become the session's once the pawn's arrays took them, so that the session's stats are
    // always what the client has (a refused change leaves both as they were, and the next gear change is a diff against that).
    if (!stats_->prepare(bootstrap::worn_items_of(inventory_), changed, next, error)) {
        ++stat_updates_failed_;
        spdlog::error("[session] {}: the stats cannot be computed for {} ({}): the client keeps the stats it has", name(), what, error);
        return;
    }
    if (next.faults != 0) {   // as the join does: a run that faulted has stats that are not the character's (a fault is a 0)
        ++stat_updates_failed_;
        spdlog::error("[session] {}: the stat program faulted {} time(s) for {} (a division by zero, a bad curve read or a value out of range): "
                      "its stats are not sent, the client keeps the stats it has", name(), next.faults, what);
        return;
    }
    if (changed.empty()) {
        stats_->commit(std::move(next));
        spdlog::debug("[session] {}: {} changes no stat", name(), what);
        return;
    }
    const std::optional<AbilityRuntime::GearSnapshot> runtime_before = abilities_ ? std::optional(abilities_->gear_snapshot()) : std::nullopt;
    // what the owner's arrays carry; a stat the ability runtime holds goes through it (Stamina is its own)
    bootstrap::AbilityChange change;
    size_t owner = 0, owner_proxy = 0, everyone = 0, without_item = 0;
    for (const stats::StatValue& v : changed) {
        if (v.float_array() || (v.replication != 1 && v.replication != 5 && v.replication != 7)) continue;
        // plan D1, stage 2: a class change lists stats the arrays were not laid out for (the new class's own resource): each gets an item in the
        // array its audience names (a fast-array add: the client takes a new item like a changed one)
        const bool new_item = !pb_.has_stat_item(v.guid);
        if (new_item) ++without_item;
        (v.replication == 1 ? owner : v.replication == 5 ? owner_proxy : everyone) += 1;
        // the GM travel tools: the GM's run speed multiplier stays on top of what the engine says of MoveSpeedMult
        const auto shown = v.guid == spawns::kStatMoveSpeedMult ? static_cast<decltype(v.value)>(v.value * gm_speed_) : v.value;
        if (!new_item && abilities_ && abilities_->tracks(v.guid)) {
            abilities_->gear_stat(change, v.guid, shown);
        } else if (!(abilities_ && v.guid == content::kGuidStamina)) {
            bootstrap::StatChange sc;
            sc.record = v.guid;
            sc.value = shown;
            sc.buckets = v.buckets();
            sc.base = v.base;
            sc.equipment = v.equip;
            sc.add_to_replication = new_item ? v.replication : 0;
            change.stats.push_back(sc);
        }
    }
    if (change.empty()) {
        stats_->commit(std::move(next));
        return;
    }
    if (!pb_.apply_ability_change(change)) {
        if (runtime_before) abilities_->restore_gear(*runtime_before);
        ++stat_updates_failed_;
        spdlog::error("[session] {}: the pawn does not take the stats of {} ({}): the client keeps the stats it has", name(), what,
                      pb_.error().empty() ? std::string("a stat has no item in its arrays") : pb_.error());
        return;
    }
    stats_->commit(std::move(next));
    ++stat_updates_;
    stat_items_changed_ += change.stats.size();
    spdlog::log(level_for(stat_logged_), "[session] {}: {} changes {} stat(s): {} Owner, {} OwnerProxy, {} EveryoneProxy{}", name(), what,
                change.stats.size(), owner, owner_proxy, everyone,
                without_item ? " (" + std::to_string(without_item) + " of them new to the client's arrays: added)" : std::string());
}

// ============================================================================================================================
//  using an item (plan D1)
// ============================================================================================================================

double PlayerSession::xp_add_mult() const {
    if (!stats_) return 0;
    const stats::StatValue* v = stats_->find(content::kGuidAdventuringXPAddMult);
    return v && std::isfinite(v->value) ? v->value : 0.0;
}
// built by C-O-A with C_Y_B

// What a level gained does to the character (plan D1, stage 2): the pawn's level, the stats, the ranks and abilities the level grants, the passives' effect items,
// the TempHealth mark.  `class_changed`: the stats are computed once for both.
void PlayerSession::finish_level_up(const leveling::XpResult& xp, bool class_changed, Clock::time_point now, bootstrap::AbilityChange& rank_effects,
                                    std::vector<repl::SkillOnActionbar>& skill_updates) {
    const bool leveled = xp.leveled();
    if (leveled) {
        if (!pb_.set_level(progress_.level)) spdlog::error("[session] {}: the pawn does not take level {}", name(), progress_.level);
        if (stats_) stats_->set_level(progress_.level);
        if (book_) {
            // the ranks the new level grants by itself: the Universal tree's and the class tree's
            const talents::Catalog& catalog = book_->catalog();
            std::vector<talents::RankRef> granted;
            const int from = xp.before.level, to = xp.after.level;
            book_->set_level(to);
            for (const talents::Tree* tree : {catalog.universal(), book_->class_tree()}) {
                if (!tree) continue;
                const std::vector<talents::RankRef> had = catalog.auto_ranks(*tree, from);
                for (const talents::RankRef& r : catalog.auto_ranks(*tree, to)) {
                    const bool before = std::any_of(had.begin(), had.end(), [&](const talents::RankRef& h) { return h.node->skill == r.node->skill; });
                    if (!before) granted.push_back(r);
                }
            }
            std::stable_sort(granted.begin(), granted.end(), [](const talents::RankRef& a, const talents::RankRef& b) {
                return a.node->level_requirement < b.node->level_requirement;
            });
            grant_ranks(granted, now, rank_effects, skill_updates);
            for (const talents::Tree* tree : {catalog.universal(), book_->class_tree()}) {
                if (tree && !pb_.set_tree_ranks(tree->guid, book_->list(*tree))) spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", name(), tree->name);
            }
        }
    }
    if (leveled || class_changed) {
        apply_gear_stats(leveled && class_changed ? "the new level and class" : leveled ? "the new level" : "the new class");
    }
    if (leveled) {
        if (!pb_.mark_temp_health()) spdlog::debug("[session] {}: no TempHealth item to mark", name());
    }
    // A level gained gives Health, Mana and Stamina back, at the new maximums, but not to a player who is dead: the respawn gives them back. A player at 0 Health is dead here too, although the death is only
    // read at the end of the tick, after the tick's hits and kills: a kill that comes in the tick of the lethal hit must not give the Health back first.
    const stats::StatValue* health = stats_ ? stats_->find(content::kGuidHealth) : nullptr;
    const bool down = dead_ || (health && !(health->value > 0.0f));
    if (leveled && !down && stats_) {
        if (const stats::StatValue* hp = stats_->find(content::kGuidMaxHealth)) set_current_value(content::kGuidHealth, hp->value);
        if (const stats::StatValue* mp = stats_->find(content::kGuidMaxMana)) set_current_value(content::kGuidMana, mp->value);
        if (abilities_) {
            bootstrap::AbilityChange refill;
            abilities_->refill_stamina(refill);
            stats_->set_current(content::kGuidStamina, static_cast<float>(abilities_->stamina()));   // (the engine's copy agrees with the runtime's)
            if (pb_.apply_ability_change(refill)) abilities_dirty_ = true;
            else spdlog::warn("[session] {}: the pawn does not take the Stamina of the new level", name());
        } else if (const stats::StatValue* sp = stats_->find(content::kGuidMaxStamina)) {
            set_current_value(content::kGuidStamina, sp->value);
        }
    }
    if (!rank_effects.empty() && !pb_.apply_ability_change(rank_effects)) {
        spdlog::warn("[session] {}: the pawn does not take the granted passives' effect items ({})", name(), pb_.error().empty() ? "keys disagree" : pb_.error());
        for (const bootstrap::EffectItem& e : rank_effects.effects_added) {
            for (auto it = rank_effects_.begin(); it != rank_effects_.end();) it = it->second == e.effect_id ? rank_effects_.erase(it) : std::next(it);
            markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == e.effect_id; }), markers_.end());
        }
    }
    if (!rank_effects.empty()) abilities_dirty_ = true;
}

void PlayerSession::on_item_activated(Connection& conn, const repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now) {
    const char* what = "ServerItemUsed (a use)";
    const auto refuse = [&](const std::string& why) {
        ++uses_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    if (!db) return refuse("worldd has no content.db to read the item's actions from");
    use::Context ctx;
    ctx.class_enum = class_id_;
    ctx.progress = progress_;
    ctx.xp_add_mult = xp_add_mult();
    ctx.curve = curve;
    // Gathering stage 2: the professions, and the runner of a Profession reward's expression (made when the first one is asked for: it runs the stat graph once)
    ctx.professions = world_->professions();
    ctx.currencies = world_->currencies();
    ctx.held = professions_;
    const abilities::Catalog* ability_catalog = world_->ability_catalog();
    std::shared_ptr<professions::XpRunner> runner;
    if (ctx.professions && ability_catalog && stats_ && stats_->started()) {
        const std::vector<stats::WornItem> worn = bootstrap::worn_items_of(inventory_);
        runner = std::make_shared<professions::XpRunner>();
        ctx.profession_xp = [this, runner, ability_catalog, worn](int64_t expr, const std::map<std::string, int>& levels, std::string& why) -> std::optional<double> {
            if (!*runner) {
                *runner = professions::make_xp_runner(*ability_catalog, *stats_, worn, why);
                if (!*runner) return std::nullopt;
            }
            return (*runner)(expr, levels, why);
        };
    }
    const use::Plan plan = use::plan_use(*db, item.item_record_id, ctx);
    if (!plan.ok) return refuse(plan.refusal);
    // what the use grants, as fresh instances (their kind decides where the inventory puts them: a resource item to a resource container, the rest to the Backpack)
    std::vector<svc::InventoryItem> fresh;
    {
        const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
        for (const use::Granted& g : plan.items) {
            svc::InventoryItem it;
            it.container = svc::ItemContainer::Backpack;
            it.item_record_id = g.record;
            it.instance_guid = fresh_guid();
            it.quantity = g.quantity;
            it.quality = g.quality;
            it.durability = g.durability;
            it.created_at_ms = created;
            fresh.push_back(std::move(it));
        }
    }
    // the room: the grants need room in the storage of their kind; the consumed stack frees its slot only when it empties (tried on a copy)
    if (!fresh.empty()) {
        items::Inventory trial = inventory_;
        if (plan.consume) trial.consume(svc::ItemContainer::Backpack, use.slot, item.instance_guid, 1);
        if (!trial.can_grant(fresh, item_catalog())) {
            return refuse("the storages have no room for the use's " + std::to_string(plan.items.size()) + " item(s) (the bag has " +
                          std::to_string(static_cast<size_t>(items::kBackpackSlots) - inventory_.count(svc::ItemContainer::Backpack)) + " free slot(s)): nothing is used");
        }
    }
    const std::span<const content::AutoGrant> grants = world_->auto_grants();
    // weapon XP: a scroll's Weapon experience goes to every weapon line of what is worn, as a kill's does
    const std::vector<int64_t> worn_lines = plan.weapon_xp > 0 ? equipped_weapon_lines() : std::vector<int64_t>();
    // the steps: the events, the XPTracker, the broadcast, the pawn's state, the class bar, and a skill update for each rank a level or a class grants
    const size_t steps = 1 + (plan.items.size() + 1) / bootstrap::kMaxEventsPerChain + 1 + 1 + 1 + 1 + grants.size() + 8 + plan.profession_changes.size() + worn_lines.size();
    if (!room_for(steps)) return;
// (Cyb : C.O.A)

    // the inventory: the consumed stack, then the grants
    items::Outcome all;
    all.ok = true;
    if (plan.consume) {
        const items::Outcome c = inventory_.consume(svc::ItemContainer::Backpack, use.slot, item.instance_guid, 1);
        if (!c.ok) return refuse(c.refusal);
        all.events.insert(all.events.end(), c.events.begin(), c.events.end());
    }
    if (!fresh.empty()) {
        const items::Outcome g = inventory_.grant(std::move(fresh), item_catalog());
        if (!g.ok) {
            // the room was checked: a refusal here is a record the table refuses, not something the client can cause; the consumed stack stays consumed
            spdlog::error("[session] {}: the use's items are not granted ({}): the item was consumed and nothing came of it", name(), g.refusal);
        } else {
            all.events.insert(all.events.end(), g.events.begin(), g.events.end());
        }
    }
    const bool inventory_moved = !all.events.empty();
    if (inventory_moved) {
        inventory_dirty_ = true;
        inventory_changed_ = true;
    }

    // the progress: the XP, the levels it crosses and the ranks they grant
    leveling::XpResult xp;
    xp.before = xp.after = progress_;
    if (plan.adventuring_xp > 0 && curve) {
        xp = leveling::add_xp(*curve, grants, progress_, plan.adventuring_xp);
        progress_ = xp.after;
        progress_changed_ = true;
    }
    const bool leveled = xp.leveled();
    if (leveled) levels_gained_ += static_cast<size_t>(xp.after.level - xp.before.level);

    // the professions (Gathering stage 2): the XP, the Promotes and the levels they cross
    const std::vector<professions::Change> profession_changes = plan.profession_changes.empty() ? std::vector<professions::Change>() : set_professions(plan.professions_after);
    const bool professions_changed = !profession_changes.empty();

    // the class
    bool class_changed = false;
    std::string class_name;
    std::optional<content::ClassRecord> new_class;
    if (plan.set_class) {
        std::string error;
        const char* cname = bootstrap::client_class_name(*plan.set_class);
        const auto cls = cname ? db->class_for_name(cname, error) : std::nullopt;
        if (!cls) {
            spdlog::error("[session] {}: the class {} is not in content.db ({}): the class stays", name(), *plan.set_class, error);
        } else if (!pb_.set_class(cls->guid)) {
            spdlog::error("[session] {}: the pawn does not take class {}: the class stays", name(), cls->name);
        } else {
            class_id_ = *plan.set_class;
            class_name = cls->name;
            new_class = *cls;
            if (stats_) stats_->set_class(class_id_);
            class_changed = true;
            progress_changed_ = true;
        }
    }
    // plan D1, stage 2: what the ranks do (the abilities, the passives' items, the bar) when the class and the level change
    bootstrap::AbilityChange rank_effects;
    std::vector<repl::SkillOnActionbar> skill_updates;
    std::vector<std::pair<int, int>> cleared_slots;
    if (class_changed && book_) switch_class(*new_class, now, rank_effects, skill_updates, cleared_slots);
    finish_level_up(xp, class_changed, now, rank_effects, skill_updates);
    // weapon XP: each worn line takes the scroll's Weapon experience on top of its total (it stops at the top); the levels it crosses are applied before any reply is built
    std::vector<weaponxp::Change> weapon_changes;
    for (const int64_t type : worn_lines) {
        const weaponxp::Change c = weapon_.add_xp(type, plan.weapon_xp);
        if (c.changed()) weapon_changes.push_back(c);
    }
    bool weapons_leveled = false;
    for (const weaponxp::Change& c : weapon_changes) weapons_leveled = weapons_leveled || c.leveled();
    if (!weapon_changes.empty()) progress_changed_ = true;
    const bool weapon_pawn = apply_weapon_levels(weapon_changes, now);

    // what the client is told, in order: the storage events, the XPTracker, the level-up, the pawn's state, the granted skills
    ++uses_applied_;
    ++items_applied_;
    if (inventory_moved) {
        const std::vector<bootstrap::ItemEvent> events = item_events_for(all);
        for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    if (xp.changed() || leveled || class_changed || professions_changed || !weapon_changes.empty()) {
        const int64_t total = progress_.xp, debt = progress_.debt;
        const std::vector<ProfessionWire> wire = profession_wire(profession_changes);   // the professions that changed, in profession order
        std::vector<std::pair<int64_t, int64_t>> lines;   // weapon XP: each paid line's new total, in the same update
        for (const weaponxp::Change& c : weapon_changes) lines.emplace_back(c.line, c.xp_after);
        queue("XPTracker", [total, debt, wire, lines](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.set_progress(total, debt)) return std::nullopt;
            for (const ProfessionWire& w : wire) {
                if (!pb.set_profession(w.leveling_type, w.xp, w.certification)) return std::nullopt;
            }
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
    if (professions_changed) queue_profession_level_ups(conn, profession_changes);
    if (leveled || class_changed) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("level and stats"); }, conn, /*must_build=*/true);
    } else if (weapon_pawn) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("a weapon line"); }, conn, /*must_build=*/false);
    }
    if (class_changed && book_) {
        // the old class's slots are sent cleared and the new class's skills placed: the whole bar as it is now, in one payload (a slot of UseType
        // None is a clear; retail's bar payloads hold them too). ClientClearActionbarsForInstigators is not used: no capture shows its shape.
        repl::ActionbarPayload payload = bar_payload();
        for (const auto& [bank, slot] : cleared_slots) {
            if (bar_.at(bank, slot)) continue;   // taken again by a skill of the new class
            repl::ActionbarSlot none;
            none.bank_index = bank;
            none.slot_index = slot;
            payload.slots.push_back(none);
        }
        queue("ClientReceiveActionbarSlotPayload", [payload](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(payload); }, conn, /*must_build=*/true);
    }
    for (const repl::SkillOnActionbar& u : skill_updates) {
        queue("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, /*must_build=*/true);
    }

    std::string granted_names;
    for (const repl::SkillOnActionbar& u : skill_updates) {
        std::string skill = std::to_string(u.skill_id);
        if (book_) {
            for (const talents::Tree* t : book_->listed_trees()) {
                for (const talents::Node& n : t->nodes) {
                    if (n.skill == u.skill_id && t->guid == u.skill_tree_id) skill = n.skill_name;
                }
            }
        }
        granted_names += (granted_names.empty() ? "" : ", ") + skill;
    }
    std::string line = "used item " + std::to_string(item.item_record_id) + ": " + plan.summary;
    if (xp.changed()) line += "; Adventuring " + std::to_string(xp.after.xp) + " XP (debt " + std::to_string(xp.after.debt) + ")";
    if (leveled) line += ", level " + std::to_string(xp.before.level) + " -> " + std::to_string(xp.after.level);
    for (const professions::Change& c : profession_changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (!pf) continue;
        const professions::Book& pbook = *professions_.book();
        line += "; " + pf->name + " level " + std::to_string(c.before.level) + " -> " + std::to_string(c.after.level) + ", " + pbook.tier(c.before.tier).name + " -> " +
                pbook.tier(c.after.tier).name + ", " + std::to_string(c.after.xp) + " XP";
    }
    if (!granted_names.empty()) line += ", granted " + granted_names;
    if (plan.weapon_xp > 0) {   // weapon XP (the summary above says the amount): what each worn line holds now
        if (!weapon_.valid()) line += "; this world has no weapon lines";
        else if (worn_lines.empty()) line += "; no weapon is worn, so no line gets it";
        bool first = true;
        for (const int64_t type : worn_lines) {
            const weaponxp::Line* wl = weapon_.lines()->find(type);
            const bool up = std::any_of(weapon_changes.begin(), weapon_changes.end(), [&](const weaponxp::Change& c) { return c.line == type && c.leveled(); });
            line += (first ? "; " : ", ") + (wl ? wl->name : std::to_string(type)) + " " + std::to_string(weapon_.xp(type)) + " XP (level " + std::to_string(weapon_.level(type)) + (up ? ", level-up" : "") + ")";
            first = false;
        }
    }
    if (class_changed) line += "; the class is " + class_name + " now (its tree, abilities and bar are sent live)";
    line += "; " + std::to_string(all.events.size()) + " storage event(s)";
    for (const std::string& n : plan.notes) line += "; " + n;
    spdlog::log(level_for(use_logged_), "[session] {}: {}", name(), line);
    if (xp.changed() || class_changed || professions_changed || !weapon_changes.empty()) {
        hand_over_progress(leveled || weapons_leveled ? "a level-up" : class_changed ? "a class change" : professions_changed && !xp.changed() ? "a profession changed" : "XP earned");
    }
}
// C | O | A and Cyb

std::vector<professions::Change> PlayerSession::set_professions(const professions::Professions& after) {
    std::vector<professions::Change> changes = professions::Professions::diff(professions_, after);
    if (changes.empty()) return changes;
    professions_ = after;
    progress_changed_ = true;
    if (stats_) stats_->set_profession_levels(professions_.levels_by_name());
    for (const professions::Change& c : changes) {
        if (c.leveled()) profession_levels_gained_ += static_cast<size_t>(c.after.level - c.before.level);
    }
    return changes;
}

std::vector<PlayerSession::ProfessionWire> PlayerSession::profession_wire(const std::vector<professions::Change>& changes) const {
    std::vector<ProfessionWire> wire;
    if (!professions_.valid()) return wire;
    const professions::Book& book = *professions_.book();
    for (const professions::Change& c : changes) {
        if (const professions::Profession* pf = book.find(c.profession)) wire.push_back({pf->leveling_type, c.after.xp, book.tier(c.after.tier).guid});
    }
    return wire;
}

void PlayerSession::queue_profession_level_ups(Connection& conn, const std::vector<professions::Change>& changes) {
    if (!professions_.valid()) return;
    // a profession's level-up is the XP item and BroadcastPlayLevelUpEffects on the pawn with the profession's leveling type as the category (retail's artisan and weapon levels)
    for (const professions::Change& c : changes) {
        const professions::Profession* pf = professions_.book()->find(c.profession);
        if (!pf || !c.leveled()) continue;
        const int64_t category = pf->leveling_type;
        const int level = c.after.level;
        queue("BroadcastPlayLevelUpEffects", [category, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(category, level); }, conn);
    }
}

// ============================================================================================================================
//  weapon XP: the weapon lines of what is worn, and what a weapon line's new level does (a kill and the test command both end here)
// ============================================================================================================================

std::vector<int64_t> PlayerSession::equipped_weapon_lines() const {
    std::vector<int64_t> found;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!weapon_.valid() || !db) return found;
    // every weapon in the Equipment storage names its weapon line (armor, shields and tools name none); a line counts once, whichever slot or weapon set holds it
    for (const svc::InventoryItem& it : inventory_.items()) {
        if (it.container != svc::ItemContainer::Equipment) continue;
        std::string error;
        const auto w = db->item_weapon(it.item_record_id, error);
        if (!w || w->skill_line == 0 || w->skill_line_name.empty()) continue;
        const weaponxp::Line* line = weapon_.lines()->find_named(w->skill_line_name);
        if (line && std::find(found.begin(), found.end(), line->leveling_type) == found.end()) found.push_back(line->leveling_type);
    }
    std::vector<int64_t> ordered;   // in the lines' order, whatever the order of the slots
    for (const weaponxp::Line& l : weapon_.lines()->all()) {
        if (std::find(found.begin(), found.end(), l.leveling_type) != found.end()) ordered.push_back(l.leveling_type);
    }
    return ordered;
}

const talents::Tree* PlayerSession::weapon_tree_of(int64_t leveling_type) const {
    if (!book_) return nullptr;
    for (const talents::Tree& t : book_->catalog().trees()) {
        if (t.kind == talents::Kind::Weapon && t.leveling_type == leveling_type) return &t;
    }
    return nullptr;
}

bool PlayerSession::apply_weapon_levels(const std::vector<weaponxp::Change>& changes, Clock::time_point now) {
    if (!book_) return false;
    const auto holds = [](const std::vector<talents::RankRef>& v, const talents::RankRef& x) {
        return std::any_of(v.begin(), v.end(), [&](const talents::RankRef& y) { return y.rank->guid == x.rank->guid; });
    };
    bootstrap::AbilityChange effects;
    bool lists_changed = false, level_moved = false;
    for (const weaponxp::Change& c : changes) {
        if (c.line == 0 || c.level_before == c.level_after) continue;
        level_moved = true;
        const talents::Tree* tree = weapon_tree_of(c.line);
        const std::vector<talents::RankRef> had = tree ? book_->auto_ranks_of(*tree) : std::vector<talents::RankRef>();
        book_->set_line_level(c.line, c.level_after);
        if (!tree) continue;   // a line no item names has no weapon tree yet: its level is all there is
        const std::vector<talents::RankRef> has = book_->auto_ranks_of(*tree);
        for (const talents::RankRef& gone : had) {
            if (holds(has, gone)) continue;
            remove_rank_effect(gone.rank->guid, effects);
            if (gone.rank->ability != 0) {
                known_.erase(std::remove(known_.begin(), known_.end(), gone.rank->ability), known_.end());
                records_.erase(std::remove(records_.begin(), records_.end(), gone.rank->ability), records_.end());
                lists_changed = true;
            }
        }
        for (const talents::RankRef& granted : has) {
            if (holds(had, granted)) continue;
            if (granted.rank->ability != 0) {
                if (std::find(known_.begin(), known_.end(), granted.rank->ability) == known_.end()) known_.push_back(granted.rank->ability);
                if (std::find(records_.begin(), records_.end(), granted.rank->ability) == records_.end()) records_.push_back(granted.rank->ability);
                lists_changed = true;
            }
            add_rank_effect(granted, now, effects);   // a weapon rank's passive is on the pawn while the weapon of its line is in hand
        }
        if (!pb_.set_tree_ranks(tree->guid, book_->list(*tree))) spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", name(), tree->name);
    }
    if (lists_changed) push_abilities();
    if (!effects.empty() && !pb_.apply_ability_change(effects)) {
        spdlog::warn("[session] {}: the pawn does not take the weapon ranks' effect items ({}): the ranks stand without them", name(), pb_.error().empty() ? "keys disagree" : pb_.error());
        for (const bootstrap::EffectItem& e : effects.effects_added) {
            for (auto it = rank_effects_.begin(); it != rank_effects_.end();) it = it->second == e.effect_id ? rank_effects_.erase(it) : std::next(it);
            markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == e.effect_id; }), markers_.end());
        }
    }
    if (!effects.empty()) abilities_dirty_ = true;
    if (level_moved) apply_gear_stats("a weapon line's level");   // the ranks held are an input of the stats, and so are the passives of the line in hand
    return level_moved || !effects.empty();
}

PlayerSession::GmWeaponResult PlayerSession::gm_set_weapon_xp(Connection& conn, int64_t leveling_type, int64_t xp, Clock::time_point now, const std::string& by) {
    GmWeaponResult r;
    const weaponxp::Lines* lines = weapon_.lines();
    const weaponxp::Line* line = lines ? lines->find(leveling_type) : nullptr;
    if (!line) {
        r.refusal = weapon_.valid() ? "no weapon line has the leveling type " + std::to_string(leveling_type) : "this world has no weapon lines (its content.db lacks their leveling types)";
        return r;
    }
    r.line = line->name;
    r.xp_before = weapon_.xp(leveling_type);
    r.level_before = weapon_.level(leveling_type);
    if (xp < 0) {
        r.refusal = "an XP total cannot be negative";
        return r;
    }
    r.xp_after = std::min(xp, lines->max_xp());
    r.level_after = lines->level_of(r.xp_after);
    // the line's weapon tree (a line no item names has none yet)
    const talents::Tree* tree = weapon_tree_of(leveling_type);
    if (tree && r.level_after < r.level_before) {
        const std::string why = book_->line_shortfall(*tree, r.level_after);
        if (!why.empty()) {
            r.refusal = name() + "'s " + line->name + " cannot go down to level " + std::to_string(r.level_after) + ": " + why + " (refund them in the Skill Tree first)";
            return r;
        }
    }
    if (!room_for(4)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }

    // ---- the state: the total, and what the new level grants by itself in the line's weapon tree
    const weaponxp::Change change = weapon_.set_xp(leveling_type, r.xp_after);
    progress_changed_ = true;
    const bool pawn_changed = apply_weapon_levels({change}, now);
    if (book_ && tree) {
        const int64_t point = book_->catalog().point_type(talents::Point::Weapon);
        r.points_earned = book_->catalog().earned(tree->leveling_type, r.level_after, point);
        r.points_left = book_->points_left(*tree, point);
    } else if (book_) {
        r.points_earned = book_->catalog().earned(leveling_type, r.level_after, book_->catalog().point_type(talents::Point::Weapon));
        r.points_left = r.points_earned;
    }

    // ---- what the client is told: the line's total (and the weapon tree's list), the level-up effect, the pawn's changed state
    const int64_t total = r.xp_after;
    queue("XPTracker (a weapon line)", [leveling_type, total](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_weapon_line(leveling_type, total)) return std::nullopt;
        return pb.leveling_update();
    }, conn, /*must_build=*/true);
    if (change.leveled()) {
        const int level = r.level_after;
        queue("BroadcastPlayLevelUpEffects", [leveling_type, level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(leveling_type, level); }, conn);
    }
    if (pawn_changed) {
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("a weapon line"); }, conn, /*must_build=*/false);
    }
    r.ok = true;
    spdlog::info("[gm] {}: {} set by {}: {} XP, level {} -> {} XP, level {}; {} Weapon point(s) earned, {} left", name(), line->name, by, r.xp_before, r.level_before, r.xp_after, r.level_after,
                 r.points_earned, r.points_left);
    hand_over_progress("a game master set a weapon line");
    return r;
}

}}  // namespace aoc::worldd

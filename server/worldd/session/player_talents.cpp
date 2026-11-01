// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_talents.cpp
//
//  Talents, the action bar and the weapon in hand: ranks bought and refunded, the bar's slots, the ranged set, the abilities the ranks grant, and the marker effects that end by themselves.
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

void PlayerSession::on_set_slots(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!book_) return;
    ++slot_calls_;
    const auto slots = repl::parse_set_actionbar_slots(rpc.call);
    if (!slots) {
        if (++quiet_unreadable_logged_ <= kLoggedPerKind) spdlog::warn("[session] {}: a ServerSetActionbarMultiSlotUseData does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        return;
    }
    if (slots->empty()) return;
    if (slots->size() > kMaxMultiSlots) {
        ++bar_refused_;
        if (++quiet_refused_logged_ <= kLoggedPerKind) spdlog::info("[session] {}: ServerSetActionbarMultiSlotUseData names {} slots, more than {}: refused, nothing changed", name(), slots->size(), kMaxMultiSlots);
        return;
    }
    if (!within_budget(bar_budget_, "ServerSetActionbarMultiSlotUseData", now)) return;
    // each slot is judged and stored as the single-slot call would (the game's server runs that call for each), the progress is handed over once
    size_t stored = 0, refused = 0;
    for (const repl::ActionbarSlot& slot : *slots) {
        std::string why;
        if (!bar_.set(slot, &why)) {
            ++refused;
            ++bar_refused_;
            spdlog::log(level_for(bar_refused_logged_), "[session] {}: ServerSetActionbarMultiSlotUseData (bank {}, slot {}) refused, nothing changed: {}", name(), slot.bank_index, slot.slot_index, why);
            continue;
        }
        if (slot.use.use_type == repl::ActionbarUseType::Skill && slot.use.skill_rank_id != 0 && slot.bank_index < actionbar::kClassBanks) {
            bar_.remove_other_copies(slot.use.skill_rank_id, slot.bank_index, slot.slot_index);   // (a skill is on the class pages once: the client moved it)
        }
        ++stored;
        ++bar_sets_;
    }
    spdlog::log(level_for(slots_logged_, 20), "[session] {}: the client set {} action bar slot(s) at once: {} stored, {} refused", name(), slots->size(), stored, refused);
    if (stored > 0) {
        progress_changed_ = true;
        hand_over_progress("the action bar");
    }
}

// ============================================================================================================================
//  talents, the action bar and regeneration (plan D1, stage 2)
// ============================================================================================================================

repl::ActionbarPayload PlayerSession::bar_payload() const {
    repl::ActionbarPayload p;
    p.slots = bar_.wire_slots();   // the weapon bank three times, as retail lists it
    p.auto_granted = false;
    return p;
}

bool PlayerSession::push_abilities() {
    if (pipeline_) pipeline_->set_known(known_);   // plan D2: what the character may trigger follows what the client was told it knows
    std::vector<bootstrap::AbilityRecord> records;
    records.reserve(records_.size());
    for (const int64_t g : records_) records.push_back({g, content::kTypeAbilityRecord});
    if (!pb_.set_abilities(known_, records)) {
        spdlog::debug("[session] {}: the pawn does not take the ability lists yet", name());
        return false;
    }
    return true;
}

void PlayerSession::rebuild_ability_lists() {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!book_ || !db) return;
    const talents::AbilityLists lists =
        talents::join_abilities(*book_, bootstrap::general_unstuck(), combo_, bootstrap::carried_combos(*db, inventory_), bootstrap::static_known_tail(),
                                bootstrap::carried_item_abilities(world_->mounts() ? &world_->mounts()->data() : nullptr, inventory_, db));
    known_ = lists.known;
    records_ = lists.records;
}

void PlayerSession::sync_item_abilities(Connection& conn) {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!world_ || (!world_->mounts() && !db) || !phase_in_done_ || !join_.empty() || finished_ || failed_ || abilities_failed_) return;
    // what is carried is compared first: the items' abilities are looked up only when it changed
    const std::vector<int64_t> carried_records = inventory_.records();
    if (carried_records == item_ability_records_) return;
    const std::vector<int64_t> carried = bootstrap::carried_item_abilities(world_->mounts() ? &world_->mounts()->data() : nullptr, inventory_, db);
    std::vector<int64_t> missing;
    for (const int64_t a : carried) {
        if (std::find(records_.begin(), records_.end(), a) == records_.end()) missing.push_back(a);
    }
    if (missing.empty()) {
        item_ability_records_ = carried_records;
        return;
    }
    if (!room_for(1)) return;   // (no room now: the next tick tries again)
    for (const int64_t a : missing) records_.push_back(a);
    if (!push_abilities()) {
        for (const int64_t a : missing) records_.erase(std::remove(records_.begin(), records_.end(), a), records_.end());
        return;
    }
    item_ability_records_ = carried_records;
    ++item_ability_syncs_;
    spdlog::log(level_for(item_ability_logged_),
                "[session] {}: {} item ability record(s) added to ReplicatedRecordsToLoad (an item that casts an ability is carried: the client needs the ability loaded to send its trigger)", name(),
                missing.size());
    queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("item abilities"); }, conn);
}

void PlayerSession::add_rank_effect(const talents::RankRef& ref, Clock::time_point now, bootstrap::AbilityChange& effects) {
    const talents::Rank& rank = *ref.rank;
    if (rank.effect == 0 || rank_effects_.count(rank.guid)) return;
    // a weapon rank's passive applies while the weapon of its line is in hand (retail sent no item for a line that was not wielded)
    if (ref.tree->kind == talents::Kind::Weapon && ref.tree->weapon_line != wielded_line_) return;
    bootstrap::EffectItem e;
    e.effect_id = next_passive_effect_id_++;
    e.effect_guid = rank.effect;
    e.start_time = bootstrap::effect_clock(pb_.setup().world_time + session_seconds(now));
    e.duration = rank.effect_infinite ? std::numeric_limits<float>::max() : static_cast<float>(rank.effect_duration);
    e.tick_time = 0;
    e.stacks = 1;
    effects.effects_added.push_back(e);
    rank_effects_[rank.guid] = e.effect_id;
    // a marker effect (0.1 s of a weapon rank, 1 s of a Universal rank) is ended by the server
    if (!rank.effect_infinite) {
        markers_.push_back({now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(rank.effect_duration)), e.effect_id});
    }
}
// CoA captures, C-Y-B code

void PlayerSession::remove_rank_effect(int64_t rank_guid, bootstrap::AbilityChange& effects) {
    const auto it = rank_effects_.find(rank_guid);
    if (it == rank_effects_.end()) return;
    const int64_t key = it->second;
    rank_effects_.erase(it);
    markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == key; }), markers_.end());
    // an item this same change adds and that is not on the pawn yet: it never was
    const auto added = std::find_if(effects.effects_added.begin(), effects.effects_added.end(), [&](const bootstrap::EffectItem& e) { return e.effect_id == key; });
    if (added != effects.effects_added.end()) {
        effects.effects_added.erase(added);
        return;
    }
    effects.effects_removed.push_back(key);
}

void PlayerSession::grant_ranks(const std::vector<talents::RankRef>& granted, Clock::time_point now, bootstrap::AbilityChange& effects,
                                std::vector<repl::SkillOnActionbar>& updates) {
    bool lists_changed = false;
    for (const talents::RankRef& r : granted) {
        if (r.rank->ability != 0) {
            if (std::find(known_.begin(), known_.end(), r.rank->ability) == known_.end()) {
                known_.push_back(r.rank->ability);   // an append, as retail's: the live order is not the join's
                lists_changed = true;
            }
            if (std::find(records_.begin(), records_.end(), r.rank->ability) == records_.end()) {
                records_.push_back(r.rank->ability);
                lists_changed = true;
            }
        }
        add_rank_effect(r, now, effects);
        if (r.node->auto_hotbar) {
            if (r.rank->ability != 0 && bar_.place_skill(r.rank->ability, r.rank->guid)) progress_changed_ = true;   // a design choice (action_bar.h)
            repl::SkillOnActionbar u;
            u.skill_id = r.node->skill;
            u.skill_tree_id = r.tree->guid;
            u.rank_id = 0;
            u.is_refunding = false;
            u.use_hint = true;
            updates.push_back(u);
        }
    }
    if (lists_changed) push_abilities();
}

void PlayerSession::switch_class(const content::ClassRecord& cls, Clock::time_point now, bootstrap::AbilityChange& effects,
                                 std::vector<repl::SkillOnActionbar>& updates, std::vector<std::pair<int, int>>& cleared) {
    (void)updates;   // the whole bar goes in one payload (the caller), not a skill update per rank
    const talents::Tree* old_tree = book_->class_tree();
    std::set<int64_t> old_ranks;
    if (old_tree) {
        for (const talents::RankRef& r : book_->listed(*old_tree)) {
            old_ranks.insert(r.rank->guid);
            remove_rank_effect(r.rank->guid, effects);
        }
    }
    book_->set_class(cls.guid);
    const talents::Tree* new_tree = book_->class_tree();
    // the trees: the old class's list leaves the client's map, the new one's comes (each class keeps its bought ranks under its own tree)
    if (old_tree && old_tree != new_tree) pb_.set_tree_ranks(old_tree->guid, {});
    std::vector<talents::RankRef> listed;
    if (new_tree) {
        pb_.set_tree_ranks(new_tree->guid, book_->list(*new_tree));
        listed = book_->listed(*new_tree);
    }
    // the bar: the old class's skills leave, the new class's are placed
    for (const auto& slot : bar_.remove_skills(old_ranks)) cleared.push_back(slot);
    for (const talents::RankRef& r : listed) {
        add_rank_effect(r, now, effects);
        if (r.node->auto_hotbar && r.rank->ability != 0) bar_.place_skill(r.rank->ability, r.rank->guid);
    }
    // the abilities: the join's lists for the new class (the old class's go, the new class's come)
    rebuild_ability_lists();
    push_abilities();
    progress_changed_ = true;
}

void PlayerSession::refuse_purchase(Connection& conn, const repl::SkillBuild& build, const std::string& why, Clock::time_point now) {
    ++talents_refused_;
    spdlog::log(level_for(talent_refused_logged_), "[session] {}: ServerPurchaseSkillBuild ({} change(s)) refused, nothing changed: {}", name(), build.changes.size(), why);
    // the client is corrected: the unchanged list of each tree the request names (and the class tree when it names none we know), under the
    // resync budget
    if (!resync_budget_.take(now) || !room_for(1)) return;
    const talents::Catalog& catalog = book_->catalog();
    std::set<int64_t> trees;
    for (const repl::SkillRankChange& c : build.changes) {
        if (trees.size() >= 4) break;
        if (catalog.tree(c.tree.guid)) trees.insert(c.tree.guid);
    }
    if (trees.empty()) {
        if (const talents::Tree* ct = book_->class_tree()) trees.insert(ct->guid);
    }
    bool any = false;
    for (const int64_t t : trees) {
        const talents::Tree* tree = catalog.tree(t);
        if (!tree) continue;
        const std::vector<int64_t> list = book_->list(*tree);
        if (list.empty()) continue;
        if (!pb_.resend_tree(t)) pb_.set_tree_ranks(t, list);   // no item yet: it is added
        any = true;
    }
    if (any) queue("XPTracker", [](bootstrap::PlayerBootstrap& pb) { return pb.leveling_update(); }, conn);
}

void PlayerSession::on_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!book_) return;
    const auto build = repl::parse_purchase_skill_build(rpc.call);
    if (!build) {
        if (!talents_unreadable_logged_) {
            talents_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerPurchaseSkillBuild does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(talent_budget_, "ServerPurchaseSkillBuild", now)) return;
    // the answer's steps: the pawn's update, the XPTracker and a skill update for each bought rank; a request that cannot be answered whole is refused
    const size_t purchases = build->purchases();
    if (purchases + 3 > kMaxPendingReplies) return refuse_purchase(conn, *build, "more ranks than one reply can announce", now);
    if (!room_for(purchases + 3)) return;
    const talents::Plan plan = book_->plan(*build);
    if (!plan.ok) return refuse_purchase(conn, *build, plan.refusal, now);

    // ---- the state: what the ranks take, what they give
    bootstrap::AbilityChange effects;
    std::vector<repl::SkillOnActionbar> updates;
    bool lists_changed = false;
    bool bar_changed = false;
    for (const talents::Change& c : plan.changes) {
        if (c.purchase) continue;
        remove_rank_effect(c.ref.rank->guid, effects);
        if (c.ref.rank->ability != 0) {
            const int64_t a = c.ref.rank->ability;
            known_.erase(std::remove(known_.begin(), known_.end(), a), known_.end());
            records_.erase(std::remove(records_.begin(), records_.end(), a), records_.end());
            lists_changed = true;
        }
        if (!bar_.remove_skills({c.ref.rank->guid}).empty()) bar_changed = true;   // the client clears its own bar on a refund
    }
    book_->apply(plan);
    for (const talents::Change& c : plan.changes) {
        if (!c.purchase) continue;
        if (c.ref.rank->ability != 0) {
            const int64_t a = c.ref.rank->ability;
            if (std::find(known_.begin(), known_.end(), a) == known_.end()) known_.push_back(a);   // an append: the live order is not the join's
            if (std::find(records_.begin(), records_.end(), a) == records_.end()) records_.push_back(a);
            lists_changed = true;
        }
        add_rank_effect(c.ref, now, effects);
        if (c.ref.node->auto_hotbar) {
            if (c.ref.rank->ability != 0 && bar_.place_skill(c.ref.rank->ability, c.ref.rank->guid)) bar_changed = true;   // a design choice (action_bar.h)
            repl::SkillOnActionbar u;
            u.skill_id = c.ref.node->skill;
            u.skill_tree_id = c.ref.tree->guid;
            u.rank_id = 0;
            u.is_refunding = false;
            u.use_hint = true;
            updates.push_back(u);
        }
    }
    (void)bar_changed;
    for (const int64_t t : plan.trees) {
        const talents::Tree* tree = book_->catalog().tree(t);
        if (tree && !pb_.set_tree_ranks(t, book_->list(*tree))) spdlog::warn("[session] {}: the XPTracker does not take the list of tree {}", name(), tree->name);
    }
    if (lists_changed) push_abilities();
    if (!effects.empty()) {
        if (!pb_.apply_ability_change(effects)) {
            spdlog::warn("[session] {}: the pawn does not take the rank effects' items ({}): the ranks stand without them", name(),
                         pb_.error().empty() ? "keys disagree" : pb_.error());
            for (const bootstrap::EffectItem& e : effects.effects_added) {
                for (auto it = rank_effects_.begin(); it != rank_effects_.end();) it = it->second == e.effect_id ? rank_effects_.erase(it) : std::next(it);
                markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&](const Marker& m) { return m.key == e.effect_id; }), markers_.end());
            }
        }
    }
    apply_gear_stats("the talents");   // the ranks' constant mods are an input of the stats
// C:Y:B + CoA

    // ---- what the client is told, in the retail order: the pawn's update (abilities, effects, stats), the XPTracker's lists, the skill updates
    ++talents_applied_;
    // The reply goes out purchase_delay_ later (retail's 84 ms median): the state above is applied now, the steps are built when they go.
    queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("talents"); }, conn, now);
    queue_later("XPTracker", [](bootstrap::PlayerBootstrap& pb) { return pb.leveling_update(); }, conn, now, /*must_build=*/true);
    for (const repl::SkillOnActionbar& u : updates) {
        queue_later("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, now, /*must_build=*/true);
    }
    // the client reports each skill it puts on its bar with a ServerSetActionbarSlotUseData, all in one frame: they are not the bar budget's
    bar_allow_ = static_cast<uint32_t>(std::min<size_t>(kBarPurchaseAllowance, static_cast<size_t>(bar_allow_) + updates.size()));
    size_t bought = 0, refunded = 0;
    for (const talents::Change& c : plan.changes) (c.purchase ? bought : refunded) += 1;
    spdlog::log(level_for(talent_logged_), "[session] {}: ServerPurchaseSkillBuild: {} rank(s) bought, {} refunded{}; {} effect item(s) added, {} removed; KnownAbilities {}",
                name(), bought, refunded, build->refund_only() ? " (a respec)" : "", effects.effects_added.size(), effects.effects_removed.size(), known_.size());
    progress_changed_ = true;
    hand_over_progress("a talent change");
}

void PlayerSession::on_set_slot(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!book_) return;
    const auto slot = repl::parse_set_actionbar_slot(rpc.call);
    if (!slot) {
        if (!talents_unreadable_logged_) {
            talents_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerSetActionbarSlotUseData does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (bar_allow_ > 0) --bar_allow_;   // a Set a purchase made the client send
    else if (!within_budget(bar_budget_, "ServerSetActionbarSlotUseData", now)) return;
    const std::optional<std::pair<int, int>> server_slot = bar_.first_free();   // where the server puts a bought skill, for the log
    std::string why;
    if (!bar_.set(*slot, &why)) {
        ++bar_refused_;
        spdlog::log(level_for(bar_refused_logged_), "[session] {}: ServerSetActionbarSlotUseData (bank {}, slot {}) refused, nothing changed: {}", name(), slot->bank_index,
                    slot->slot_index, why);
        return;   // retail answers none, and nothing here is wrong with the client's own bar
    }
    // a skill is on the class pages once: the client moved it, not copied it
    if (slot->use.use_type == repl::ActionbarUseType::Skill && slot->use.skill_rank_id != 0 && slot->bank_index < actionbar::kClassBanks) {
        bar_.remove_other_copies(slot->use.skill_rank_id, slot->bank_index, slot->slot_index);
    }
    ++bar_sets_;
    // a probe of the D1 draft (the bar animation): every Set at info, with what the server's own placement would have been
    ++bar_logged_;
    spdlog::info("[session] {}: the client set action bar bank {} slot {} (use type {}, ability {}, rank {}): stored; the server's first free class slot was {}", name(),
                 slot->bank_index, slot->slot_index, static_cast<unsigned>(slot->use.use_type), slot->use.ability_id, slot->use.skill_rank_id,
                 server_slot ? std::to_string(server_slot->first) + "/" + std::to_string(server_slot->second) : std::string("none"));
    progress_changed_ = true;
    hand_over_progress("the action bar");
}

void PlayerSession::on_set_ranged(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!book_ || !world_ || !world_->content()) return;
    const auto use = repl::parse_set_use_ranged_weapon(rpc.call);
    if (!use) return;
    if (!within_budget(item_budget_, "ServerSetUseRangedWeapon", now) || !room_for(2)) return;
    if (*use == use_ranged_) return;
    use_ranged_ = *use;
    ++weapon_swaps_;
    // E1 stage 3 fix: the pawn's CombatInfo.bIsUsingRangedWeapon follows the request (retail 403, every swap: an 18-bit UPlayerCombatInfo block in the
    // same chain as the AbilityComponent's, the StatsComponent's and OwnerInfo's); the client's weapon in hand follows this flag, not KnownAbilities
    if (!pb_.set_using_ranged_weapon(use_ranged_)) spdlog::warn("[session] {}: the pawn does not take CombatInfo.bIsUsingRangedWeapon", name());
    const auto weapon = bootstrap::worn_weapon(*world_->content(), inventory_, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
    if (!weapon) {
        spdlog::info("[session] {}: the weapon set is the {} one now and nothing is worn in its slot: the combo stays", name(), use_ranged_ ? "ranged" : "melee");
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("weapon swap"); }, conn);
        return;
    }
    // KnownAbilities: the old combo leaves, the new one is the last element; the records: the new one is the last (the old one stays carried)
    put_weapon_in_hand(weapon->combo, weapon->line, weapon->line_name);
    apply_gear_stats("the weapon swap");   // a weapon tree's constant mods apply while its line is in hand
    spdlog::info("[session] {}: the {} weapon set: combo {} is the last known ability, weapon line {}", name(), use_ranged_ ? "ranged" : "melee", weapon->combo,
                 weapon->line_name);
    queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("weapon swap"); }, conn);
}

void PlayerSession::put_weapon_in_hand(int64_t combo, int64_t line, const std::string& line_name) {
    if (combo_ != 0) known_.erase(std::remove(known_.begin(), known_.end(), combo_), known_.end());
    known_.erase(std::remove(known_.begin(), known_.end(), combo), known_.end());
    known_.push_back(combo);
    records_.erase(std::remove(records_.begin(), records_.end(), combo), records_.end());
    records_.push_back(combo);
    combo_ = combo;
    wielded_line_ = line;
    push_abilities();   // (the pipeline answers to the new combo from here on, and not to the old one)
    pb_.set_weapon_skill_line(line, line_name);
}

void PlayerSession::follow_worn_weapon() {
    if (!book_ || !world_ || !world_->content()) return;
    const content::ContentDb& db = *world_->content();
    // The weapon swap fix (a wand worn over a longbow used to leave the longbow's combo the last known ability: the longbow kept shooting and the wand's combo was refused).  What a
    // ServerItemUsed wears or takes off in a weapon slot moves the weapon in hand the way the retail server's reply did, in the chain of the HeldItems
    // update [R: the recorded equips and unequips of a weapon; the replacement of the weapon in hand by another is the same reply, M]:
    //  - the set in hand holds a weapon: its combo is the last known ability, its line is the weapon line, and the combo that was in hand stops being answered; a weapon put into the set
    //    that is not in hand changes none of this (the swap to that set reads the slot) [R: a weapon worn into the other set changed no ability list, line or flag];
    //  - the set in hand holds no weapon and the other set does (the weapon in hand was taken off, or the first weapon was worn into the other set): the hand moves to that set and the
    //    flag follows [R: the weapon taken off; M: the first weapon worn, where one recording sends the flag that two like it do not];
    //  - nothing is left to hold: the combo leaves KnownAbilities and the line stays as the client has it [R; the records keep the carried combo, M].
    std::optional<bootstrap::WeaponInfo> weapon = bootstrap::worn_weapon(db, inventory_, use_ranged_ ? items::equip::Ranged : items::equip::Primary);
    bool moved_hand = false;
    if (!weapon) {
        if (const auto other = bootstrap::worn_weapon(db, inventory_, use_ranged_ ? items::equip::Primary : items::equip::Ranged)) {
            use_ranged_ = !use_ranged_;
            if (!pb_.set_using_ranged_weapon(use_ranged_)) spdlog::warn("[session] {}: the pawn does not take CombatInfo.bIsUsingRangedWeapon", name());
            weapon = other;
            moved_hand = true;
        }
    }
    if (weapon) {
        if (!moved_hand && weapon->combo == combo_) {   // the same combo stays in hand (another item of the same weapon, or a weapon in the other set)
            wielded_line_ = weapon->line;
            return;
        }
        put_weapon_in_hand(weapon->combo, weapon->line, weapon->line_name);
        spdlog::info("[session] {}: {}: the {} weapon set is in hand, combo {} is the last known ability, weapon line {}", name(),
                     moved_hand ? "the weapon in hand was taken off and the other set holds a weapon" : "the weapon worn in the set in hand changed", use_ranged_ ? "ranged" : "melee",
                     weapon->combo, weapon->line_name);
        return;
    }
    wielded_line_ = 0;
    if (combo_ == 0) return;
    const int64_t gone = combo_;
    known_.erase(std::remove(known_.begin(), known_.end(), gone), known_.end());
    combo_ = 0;
    push_abilities();
    spdlog::info("[session] {}: the weapon in hand was taken off and no other is worn: combo {} leaves KnownAbilities", name(), gone);
}

void PlayerSession::run_timers(Connection& conn, Clock::time_point now) {
    if (failed_ || finished_ || !join_.empty()) return;   // nothing before the join is on the wire
    // the replies whose time has come, in the order they were asked for
    while (!delayed_.empty() && delayed_.front().due <= now) {
        Delayed d = std::move(delayed_.front());
        delayed_.pop_front();
        queue(d.step.what, std::move(d.step.build), conn, d.step.must_build);
    }
    // marker effects end by themselves
    if (!markers_.empty()) {
        bootstrap::AbilityChange c;
        for (auto it = markers_.begin(); it != markers_.end();) {
            if (it->due > now) {
                ++it;
                continue;
            }
            c.effects_removed.push_back(it->key);
            for (auto r = rank_effects_.begin(); r != rank_effects_.end();) r = r->second == it->key ? rank_effects_.erase(r) : std::next(r);
            it = markers_.erase(it);
        }
        if (!c.empty()) {
            if (pb_.apply_ability_change(c)) abilities_dirty_ = true;
            else spdlog::warn("[session] {}: the pawn does not take the end of {} marker effect(s) ({})", name(), c.effects_removed.size(), pb_.error().empty() ? "keys disagree" : pb_.error());
        }
    }
    if (abilities_dirty_) flush_abilities(conn);
}

/// The client calls of talents, the action bar, the weapon in hand (session/rpc_table.h).
void PlayerSession::register_talents_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerSetActionbarSlotUseData", [](PlayerSession& s, const RpcContext& c) { s.on_set_slot(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerSetActionbarMultiSlotUseData", [](PlayerSession& s, const RpcContext& c) { s.on_set_slots(c.conn, c.rpc, c.now); });
    t.add(RpcScope::Pawn, "ServerPurchaseSkillBuild", [](PlayerSession& s, const RpcContext& c) { s.on_purchase(c.conn, c.rpc, c.now); });
    t.add(RpcScope::Pawn, "ServerSetUseRangedWeapon", [](PlayerSession& s, const RpcContext& c) { s.on_set_ranged(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd

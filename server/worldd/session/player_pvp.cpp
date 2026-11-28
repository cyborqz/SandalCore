// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.
// ============================================================================
//  session/player_pvp.cpp  (PvP: the flag between players)
//
//  The player's side of the rules of combat/pvp.h, kept by his session:
//    the flag       NonCombatant until an action of his flags him (the world decides it, CombatWorld::apply_hit_to_player: harming a non-combatant or a
//                   combatant, supporting a combatant); a Combatant for 60 s after the last such action (Combatant.RemovalTimer); Corrupted while his
//                   Corruption is above 0
//    the client     the pawn's AlignmentComponent.Alignments (the flag's alignment, and for a combatant the removal timer the client counts down from
//                   its SetTimeStamp on the synchronized clock), NotifyFlaggingReason at the first flagging ("Flagged as combatant for harming X"),
//                   CombatInfo.bInPVPCombat while he trades harmful hits with another player; the pawn the other players are shown carries the same Alignments, so
//                   their nameplates of him are white, purple or red as his own client's is
//    the force flag ServerSetPvpFlag (the PvP key) or the bPvpForceFlag of his last ServerTriggerAbility
//    the kill       the last harmful blow he took, when another player's (take_hit), is the killer the world is told at his death
//
//  Step 2, corruption (combat/pvp_corruption.h, GameStatProfile's formulas): a kill of a non-combatant outside a duel and his group adds corruption and blight
//  (NotifyCorruptionDeltaForKill); while above 0 he is Corrupted (red), the Corruption stat says how much (every client is told it) and he holds his tier's
//  debuff (Corruption_1 .. Corruption_5, the PvP effects); a creature's kill burns it off a little, a death a lot (NotifyCorruptionLostForDying); a corrupted
//  death costs a bigger debt, more resources and some of the worn gear (into the death's pile).  The record keeps corruption and blight (migration 0025).
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/constants.h"
#include "combat/combat_world.h"
#include "combat/pvp_corruption.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "session/player_combat_side.h"
#include "session/player_session_util.h"
#include "session/player_world.h"

#include <spdlog/spdlog.h>

#include <cmath>

namespace aoc { namespace worldd {

namespace pvp = combat::pvp;

namespace {
const char* flag_name(pvp::Flag f) {
    switch (f) {
        case pvp::Flag::Combatant: return "a combatant";
        case pvp::Flag::Corrupted: return "corrupted";
        default: return "a non-combatant";
    }
}
}  // namespace

double PlayerSession::pvp_world_now(Clock::time_point now) const { return pb_.setup().world_time + ability_seconds(now); }

void PlayerSession::pvp_side_of(pvp::Side& out, bool& force) const {
    out = {};
    out.actor = pb_.setup().guids.pawn.ObjectId;
    out.flag = pvp_state_;
    out.require_force_to_harm_combatants = pb_.controller_bool("CharacterInGameSettings.CombatSettings.PVP.bRequireForceFlagToHarmCombatants", false);
    out.require_force_to_support_unaffiliated = pb_.controller_bool("CharacterInGameSettings.CombatSettings.PVP.bRequireForceFlagToSupportUnaffiliatedCombatants", false);
    force = pvp_flag_ || pvp_trigger_flag_;
    if (duel_book_) out.duel = duel_book_->fighting_key(pb_.setup().character_guid);   // step 3: the duel he fights (Duel -> Duel: Hostile)
    out.protection = pvp_protected();   // step 4: an opt-in protection (no harm from another player outside a duel)
    out.guild = guild_ ? guild_->id : std::array<uint32_t, 4>{};   // social, step 9: one guild is the Friendly_Guild tier of the rules
}

bool PlayerSession::pvp_protected() const {
    return pipeline_ && (pipeline_->stacks_of(pvp::kEffectEmberspringProtection) > 0 || pipeline_->stacks_of(pvp::kEffectGeneralProtection) > 0);
}

bool PlayerSession::pvp_mark_flagged(double world_now) {
    if (pvp_state_ == pvp::Flag::Corrupted) return false;   // (a corrupted player is not made a combatant: he stays corrupted)
    // step 4: an action that flags him ends his opt-in protection (taken off at the session's tick: the world is in the middle of his hit)
    if (pvp_protected()) pvp_drop_protection_ = true;
    const bool was = pvp_state_ == pvp::Flag::Combatant;
    pvp_state_ = pvp::Flag::Combatant;
    pvp_combatant_since_ = world_now;
    ++pvp_flaggings_;
    return !was;
}

void PlayerSession::pvp_send_alignments(Connection& conn, const char* why) {
    const double since = pvp_state_ == pvp::Flag::Combatant ? pvp_combatant_since_ : -1;
    // step 3: while he fights a duel, the Duel alignment {its guid}, its timer from the fight's start
    std::array<uint32_t, 4> duel{};
    double duel_since = 0;
    if (duel_book_) {
        if (const auto d = duel_book_->fighting(pb_.setup().character_guid)) {
            duel = *d;
            if (const world::DuelBook::Duel* x = duel_book_->duel_of(pb_.setup().character_guid)) duel_since = bootstrap::effect_clock(std::max(0.0, x->began_at));
        }
    }
    if (pvp_state_ == pvp_sent_state_ && since == pvp_sent_since_ && duel == pvp_sent_duel_) return;
    pvp::Alignments a;
    a.flag = pvp_state_;
    a.combatant_since = bootstrap::effect_clock(std::max(0.0, since));   // (the removal timer's SetTimeStamp: the synchronized clock of ClientSetTime)
    a.self = pb_.setup().character_guid;
    a.duel = duel;
    a.duel_since = duel_since;
    const protocol::repl::AlignmentContainer shown = pvp::container(a);
    if (!pb_.set_alignments(shown)) return;   // (the pawn's own container: the flag's and the duel's rows change, every other instance it holds stays; before the possession it is tried again at the next tick)
    // the pawn the other players are shown takes the AlignmentComponent at this session's next tick, with the stance's other bits (session/player_stance.inc)
    note_shown_alignment();
    pvp_sent_state_ = pvp_state_;
    pvp_sent_since_ = since;
    pvp_sent_duel_ = duel;
    queue("pawn state (PvP flag)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("PvP flag"); }, conn, /*must_build=*/true);
    spdlog::log(level_for(pvp_logged_, 20), "[pvp] {}: is {} now ({}){}", name(), flag_name(pvp_state_), why, duel != std::array<uint32_t, 4>{} ? ", in a duel" : "");
}

void PlayerSession::drain_pvp(Connection& conn, Clock::time_point now) {
    if (!combat_side_) return;
    drain_duels(conn, now);   // step 3: the duels' notices (the alignments below follow the fight)
    if (pvp_drop_protection_ && pipeline_) {   // step 4: he flagged himself: his opt-in protection is over
        pvp_drop_protection_ = false;
        const double sec = pipeline_->now();
        for (const int64_t e : {pvp::kEffectEmberspringProtection, pvp::kEffectGeneralProtection}) {
            if (pipeline_->stacks_of(e) > 0) pipeline_->remove_effect(e, sec);
        }
        take_ability_events();
        spdlog::log(level_for(pvp_logged_, 20), "[pvp] {}: a flagging action of his ends his PvP protection", name());
    }
    const double world_now = pvp_world_now(now);
    // the flag ends 60 s after the last action that flagged
    if (pvp_state_ == pvp::Flag::Combatant && world_now - pvp_combatant_since_ >= pvp::kCombatantSeconds) pvp_state_ = pvp::Flag::NonCombatant;
    // why the flag came: the first flagging's notice (NotifyFlaggingReason), the target being the other player's pawn
    const auto notices = std::move(combat_side_->pvp_notices);
    combat_side_->pvp_notices.clear();
    for (const auto& [target, intention] : notices) {
        const world::WorldActor* other = registry_ ? registry_->find(target) : nullptr;
        if (!other) continue;
        const protocol::emit::FIntrepidNetworkGUID guid = other->actor.guid;
        const uint8_t why = intention;
        queue("NotifyFlaggingReason", [why, guid](bootstrap::PlayerBootstrap& pb) { return pb.notify_flagging_reason(why, guid); }, conn, /*must_build=*/true);
    }
    pvp_send_alignments(conn, notices.empty() ? "the flag's time" : "an action of his");
    pvp_sync_stats(conn);   // step 2: the corruption's stats and debuff (at the join they wait for the stats and the pawn's items)
    // CombatInfo.bInPVPCombat
    if (combat_side_->wants_pvp_combat != pvp_combat_) {
        pvp_combat_ = combat_side_->wants_pvp_combat;
        if (pb_.set_in_pvp_combat(pvp_combat_)) queue("pawn state (PvP combat)", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("PvP combat"); }, conn, /*must_build=*/true);
    }
    const auto kills = std::move(combat_side_->pvp_kills);
    combat_side_->pvp_kills.clear();
    for (const combat::PvpKill& k : kills) pvp_take_kill(conn, k, now);
}

void PlayerSession::pvp_take_kill(Connection& conn, const combat::PvpKill& kill, Clock::time_point now) {
    (void)now;
    ++pvp_kills_;
    spdlog::info("[pvp] {}: killed another player ({}, level {}){}", name(), flag_name(kill.victim_flag), kill.victim_level,
                 kill.duel ? " in a duel" : kill.grouped ? " of his own group" : "");
    // a non-combatant's death corrupts his killer (outside a duel, and never a friend's: the rules let no one harm a party member)
    if (kill.victim_flag != pvp::Flag::NonCombatant || kill.duel || kill.grouped) return;
    const int level = stats_ ? stats_->level() : 1;
    const double gain = pvp::corruption_for_kill(level, kill.victim_level, blight_);
    blight_ = std::min(blight_ + pvp::blight_for_kill(level, kill.victim_level), 999999.0);
    pvp_changed_ = true;
    pvp_set_corruption(conn, corruption_ + gain, "a kill of a non-combatant");
    const world::WorldActor* victim = registry_ ? registry_->find(kill.victim) : nullptr;
    const protocol::emit::FIntrepidNetworkGUID target = victim ? victim->actor.guid : protocol::emit::FIntrepidNetworkGUID{};
    const float delta = static_cast<float>(gain);
    queue("NotifyCorruptionDeltaForKill", [delta, target](bootstrap::PlayerBootstrap& pb) { return pb.notify_corruption_for_kill(delta, target); }, conn, /*must_build=*/true);
    spdlog::info("[pvp] {}: +{:.2f} corruption for killing a non-combatant (now {:.2f}, tier {}; blight {:.2f})", name(), gain, corruption_, pvp::corruption_tier(corruption_), blight_);
}

void PlayerSession::pvp_init(const svc::Character& character) {
    const auto valid = [](double v) { return std::isfinite(v) && v > 0 ? std::min(v, 999999.0) : 0.0; };
    corruption_ = valid(character.corruption);
    blight_ = valid(character.blight);
    if (corruption_ > 0) pvp_state_ = pvp::Flag::Corrupted;
    pvp_stats_dirty_ = true;
}

void PlayerSession::pvp_set_corruption(Connection& conn, double value, const char* why) {
    const double v = std::isfinite(value) ? std::clamp(value, 0.0, 999999.0) : 0.0;
    if (v < 1e-6 && corruption_ < 1e-6 && pvp_state_ != pvp::Flag::Corrupted) return;
    corruption_ = v < 1e-6 ? 0.0 : v;
    pvp_changed_ = true;
    pvp_stats_dirty_ = true;
    if (corruption_ > 0) {
        pvp_state_ = pvp::Flag::Corrupted;   // (assigning Corrupted removes Combatant: AssignAlignment's conflicting alignments)
        trade_interrupted(last_now_, "became corrupted");
    } else if (pvp_state_ == pvp::Flag::Corrupted) {
        pvp_state_ = pvp::Flag::NonCombatant;
        pvp_combatant_since_ = -1;
    }
    pvp_send_alignments(conn, why);
    pvp_sync_stats(conn);
}

bool PlayerSession::pvp_set_stat(int64_t record, double value) {
    if (pb_.has_stat_item(record)) return set_current_value(record, static_cast<float>(value));
    // the pawn's stat arrays were laid out without it (a stat at 0 at the join has no item): an item is added to the array its audience names (Corruption: every client,
    // 7; Corruption_Blight: the owner, 1), as a class change adds its new resource's (player_progress.cpp)
    const stats::StatValue* v = stats_ ? stats_->find(record) : nullptr;
    if (!v || !v->held() || !std::isfinite(value) || (v->replication != 1 && v->replication != 5 && v->replication != 7)) return false;
    bootstrap::AbilityChange c;
    bootstrap::StatChange sc;
    sc.record = record;
    sc.value = static_cast<float>(value);
    sc.buckets = v->buckets();
    sc.base = v->base;
    sc.equipment = v->equip;
    sc.add_to_replication = v->replication;
    c.stats.push_back(sc);
    if (!pb_.apply_ability_change(c)) return false;
    stats_->set_current(record, static_cast<float>(value));
    abilities_dirty_ = true;
    return true;
}

void PlayerSession::pvp_sync_stats(Connection& conn) {
    (void)conn;
    if (!pvp_stats_dirty_ || !pipeline_ || !stats_ || !stats_->started() || abilities_failed_) return;
    if (!pb_.has_stat_item(content::kGuidHealth)) return;   // (before the pawn's second state its stat items are not there: the next tick)
    // the two stats are held values of the character's (AbilityPipeline's resources): what every client reads (Corruption) and the owner's (Corruption_Blight)
    const std::pair<int64_t, double> wanted[] = {{pvp::kStatCorruption, corruption_}, {pvp::kStatBlight, blight_}};
    for (const auto& [stat, value] : wanted) {
        const stats::StatValue* v = stats_->find(stat);
        if (!v || std::fabs(v->value - value) <= 1e-4) continue;   // (a stat program without it: nothing to show)
        if (!pvp_set_stat(stat, value)) {
            spdlog::log(level_for(pvp_logged_, 20), "[pvp] {}: the stat {} cannot be shown ({:.2f}): {}", name(), stat, value,
                        pb_.error().empty() ? std::string("it is not held, or has no audience") : pb_.error());
        }
    }
    // the tier's debuff (its stat mods read the tier, which reads the Corruption stat just set)
    const double sec = pipeline_->now();
    const int64_t want = pvp::tier_effect(pvp::corruption_tier(corruption_));
    if (want != pvp_tier_effect_) {
        if (pvp_tier_effect_ != 0) pipeline_->remove_effect(pvp_tier_effect_, sec);
        pvp_tier_effect_ = 0;
        if (want != 0 && !pipeline_->grant_effect(want, 1, sec)) {
            spdlog::log(level_for(pvp_logged_, 20), "[pvp] {}: the corruption debuff {} is not in the ability tables (content.db older than the PvP effects?): the tier costs nothing", name(), want);
        } else {
            pvp_tier_effect_ = want;
        }
    }
    pipeline_->refresh(sec, "the corruption", true);   // (the tier and the debuff's mods follow the stat)
    take_ability_events();
    pvp_stats_dirty_ = false;   // (a stat that cannot be shown is logged, not retried at every tick)
}

void PlayerSession::pvp_npc_kill(Connection& conn, uint64_t npc, double base_xp) {
    if (!(corruption_ > 0)) return;
    const int level = stats_ ? stats_->level() : 1;
    const double lost = std::min(corruption_, pvp::corruption_lost_killing(base_xp, level));
    if (!(lost > 0)) return;
    pvp_set_corruption(conn, corruption_ - lost, "a creature's kill");
    const world::WorldActor* a = registry_ ? registry_->find(npc) : nullptr;
    const protocol::emit::FIntrepidNetworkGUID target = a ? a->actor.guid : protocol::emit::FIntrepidNetworkGUID{};
    const float delta = static_cast<float>(-lost);
    queue("NotifyCorruptionDeltaForKill", [delta, target](bootstrap::PlayerBootstrap& pb) { return pb.notify_corruption_for_kill(delta, target); }, conn, /*must_build=*/true);
}

void PlayerSession::pvp_corrupted_death(Connection& conn, int tier, std::vector<combat::LootStack>& lost) {
    gear_dropped_last_ = 0;
    if (tier > 0) {
        // CorruptedDeathNumItemsToDrop: RandIntRange(min, max) of the tier's curves; the worn armour, jewellery and weapons (not a shirt, a bag, a tool, a cosmetic or a crate)
        const int lo = pvp::item_drop_min(tier), hi = pvp::item_drop_max(tier);
        size_t n = static_cast<size_t>(lo + (hi > lo ? static_cast<int>(pvp_rng_.next() % static_cast<uint64_t>(hi - lo + 1)) : 0));
        std::vector<svc::InventoryItem> worn;
        for (const svc::InventoryItem& it : inventory_.items()) {
            if (it.container == svc::ItemContainer::Equipment && it.slot >= items::equip::Helmet && it.slot <= items::equip::RangedOffHand && it.quantity > 0) worn.push_back(it);
        }
        items::Outcome all;
        all.ok = true;
        while (n > 0 && !worn.empty()) {
            const size_t pick = static_cast<size_t>(pvp_rng_.next() % worn.size());
            const svc::InventoryItem it = worn[pick];
            worn.erase(worn.begin() + static_cast<std::ptrdiff_t>(pick));
            const items::Outcome c = inventory_.consume(svc::ItemContainer::Equipment, it.slot, it.instance_guid, it.quantity);
            if (!c.ok) continue;
            all.events.insert(all.events.end(), c.events.begin(), c.events.end());
            combat::LootStack s;   // [modelled: the pile keeps the item, its quality and the count; the durability it had is not kept]
            s.item = it.item_record_id;
            s.quantity = it.quantity;
            s.quality = it.quality;
            lost.push_back(s);
            ++gear_dropped_last_;
            --n;
        }
        if (gear_dropped_last_ > 0) {
            all.equipment_changed = true;
            apply_outcome(conn, all, "a corrupted death", /*with_toasts=*/false);
            spdlog::info("[pvp] {}: a corrupted death (tier {}) dropped {} worn item(s) into the pile", name(), tier, gear_dropped_last_);
        }
    }
    // CorruptionReductionOnDeath
    const double burn = pvp::corruption_lost_dying(corruption_);
    if (burn > 0) {
        pvp_set_corruption(conn, corruption_ - burn, "a death");
        const float delta = static_cast<float>(burn);
        queue("NotifyCorruptionLostForDying", [delta](bootstrap::PlayerBootstrap& pb) { return pb.notify_corruption_lost_for_dying(delta); }, conn, /*must_build=*/true);
    }
}

PlayerSession::GmPvpResult PlayerSession::gm_pvp(Connection& conn, const std::string& what, double amount, Clock::time_point now, const std::string& by) {
    GmPvpResult r;
    std::string w;
    for (const char c : what) w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const double world_now = pvp_world_now(now);
    if (w == "noncombatant" || w == "clear" || w == "off") {
        pvp_set_corruption(conn, 0, "a GM");
        pvp_state_ = pvp::Flag::NonCombatant;
        pvp_combatant_since_ = -1;
    } else if (w == "combatant" || w == "flag" || w == "on") {
        if (corruption_ > 0) {
            r.refusal = name() + " is corrupted: clear the corruption first (pvp " + name() + " noncombatant)";
            return r;
        }
        pvp_mark_flagged(world_now);
    } else if (w == "corruption" || w == "corrupt") {
        if (!(amount >= 0) || !std::isfinite(amount)) {
            r.refusal = "the corruption is a number of 0 or more";
            return r;
        }
        pvp_set_corruption(conn, amount, "a GM");
    } else {
        r.refusal = "the PvP state is noncombatant, combatant or corruption <amount>";
        return r;
    }
    pvp_send_alignments(conn, "a GM");
    spdlog::info("[pvp] {}: {} set the flag: {} (corruption {:.2f})", name(), by, flag_name(pvp_state_), corruption_);
    r.ok = true;
    char buf[96];
    std::snprintf(buf, sizeof buf, " (corruption %.2f, tier %d).", corruption_, pvp::corruption_tier(corruption_));
    r.text = name() + " is " + flag_name(pvp_state_) + buf;
    return r;
}

}}  // namespace aoc::worldd

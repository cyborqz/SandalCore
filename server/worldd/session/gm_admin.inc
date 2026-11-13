// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/gm_admin.inc  (included by session/gm_travel.cpp; see session/members/gm_admin.inc)
//
//  The session side of the game-master test commands the WorldAdmin run call reaches.  Every action goes out the way the same change in play goes out: the Health and Mana stats as
//  an ability update, a level's XPTracker and BroadcastPlayLevelUpEffects, a death's ClientRestart and death location, a revive's pawn state and correction, the quest component's
//  update, the pawn's Cooldowns items, and ClientSetTime.  Nothing here is sent when the character is dead, unless the command is the revive.
// ============================================================================

PlayerSession::GmValueResult PlayerSession::gm_set_health(Connection& conn, double value, Clock::time_point now, const std::string& by) {
    GmValueResult r;
    if (dead_) {
        r.refusal = name() + " is dead: revive it first";
        return r;
    }
    const stats::StatValue* maximum = stats_ ? stats_->find(content::kGuidMaxHealth) : nullptr;
    const stats::StatValue* current = stats_ ? stats_->find(content::kGuidHealth) : nullptr;
    if (!maximum || !current) {
        r.refusal = name() + " has no Health stat to set";
        return r;
    }
    r.maximum = static_cast<double>(maximum->value);
    r.before = static_cast<double>(current->value);
    const double want = std::clamp(value, 0.0, std::max(0.0, r.maximum));
    if (!room_for(4)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }
    if (!set_current_value(content::kGuidHealth, static_cast<float>(want))) {
        r.refusal = name() + "'s pawn does not take a Health change (its stat item or the ability runtime is missing)";
        return r;
    }
    flush_abilities(conn);
    r.after = want;
    r.ok = true;
    spdlog::info("[gm] {}: Health set to {:.0f} of {:.0f} by {} (was {:.0f})", name(), r.after, r.maximum, by, r.before);
    if (want <= 0.0) {
        check_death(conn, now);
        if (dead_) spdlog::info("[gm] {}: Health 0 by {}: it died", name(), by);
    }
    return r;
}

PlayerSession::GmValueResult PlayerSession::gm_set_mana(Connection& conn, double value, Clock::time_point now, const std::string& by) {
    (void)now;
    GmValueResult r;
    if (dead_) {
        r.refusal = name() + " is dead: revive it first";
        return r;
    }
    const stats::StatValue* maximum = stats_ ? stats_->find(content::kGuidMaxMana) : nullptr;
    const stats::StatValue* current = stats_ ? stats_->find(content::kGuidMana) : nullptr;
    if (!maximum || !current) {
        r.refusal = name() + " has no Mana stat to set";
        return r;
    }
    r.maximum = static_cast<double>(maximum->value);
    r.before = static_cast<double>(current->value);
    const double want = std::clamp(value, 0.0, std::max(0.0, r.maximum));
    if (!room_for(4)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }
    if (!set_current_value(content::kGuidMana, static_cast<float>(want))) {
        r.refusal = name() + "'s pawn does not take a Mana change (its stat item or the ability runtime is missing)";
        return r;
    }
    flush_abilities(conn);
    r.after = want;
    r.ok = true;
    spdlog::info("[gm] {}: Mana set to {:.0f} of {:.0f} by {} (was {:.0f})", name(), r.after, r.maximum, by, r.before);
    return r;
}

PlayerSession::GmHealResult PlayerSession::gm_heal(Connection& conn, Clock::time_point now, const std::string& by) {
    GmHealResult r;
    if (dead_) {
        r.refusal = name() + " is dead: revive it first";
        return r;
    }
    const stats::StatValue* hp = stats_ ? stats_->find(content::kGuidMaxHealth) : nullptr;
    const stats::StatValue* mp = stats_ ? stats_->find(content::kGuidMaxMana) : nullptr;
    if (!hp || !mp) {
        r.refusal = name() + " has no Health and Mana to fill";
        return r;
    }
    r.health = gm_set_health(conn, static_cast<double>(hp->value), now, by);
    if (!r.health.ok) {
        r.refusal = r.health.refusal;
        return r;
    }
    r.mana = gm_set_mana(conn, static_cast<double>(mp->value), now, by);
    if (!r.mana.ok) {
        r.refusal = r.mana.refusal;
        return r;
    }
    r.ok = true;
    spdlog::info("[gm] {}: healed to full Health and Mana by {} ({:.0f} / {:.0f})", name(), by, r.health.after, r.mana.after);
    return r;
}

PlayerSession::GmReviveResult PlayerSession::gm_revive(Connection& conn, Clock::time_point now, const std::string& by) {
    GmReviveResult r;
    if (!dead_) {
        r.refusal = name() + " is alive already";
        return r;
    }
    if (!room_for(5)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }
    PlayerWorld::RespawnSpot spot;
    if (npc_world_) {
        spot = npc_world_->respawn_spot(death_x_, death_y_);
    } else {
        spot.x = death_x_;
        spot.y = death_y_;
        spot.z = death_z_;
        spot.name = "where it fell";
    }
    finish_respawn(conn, spot, now);
    r.ok = true;
    r.place = spot.name;
    r.x = spot.x;
    r.y = spot.y;
    r.z = spot.z;
    spdlog::info("[gm] {}: revived by {} at {} ({:.0f}, {:.0f}, {:.0f}) with full Health and Mana", name(), by, spot.name, spot.x, spot.y, spot.z);
    return r;
}

PlayerSession::GmKillResult PlayerSession::gm_kill(Connection& conn, Clock::time_point now, const std::string& by) {
    GmKillResult r;
    if (dead_) {
        r.refusal = name() + " is dead already";
        return r;
    }
    const stats::StatValue* hp = stats_ ? stats_->find(content::kGuidHealth) : nullptr;
    if (!hp || !stats_ || !stats_->started()) {
        r.refusal = name() + " has no Health to take";
        return r;
    }
    if (!room_for(8)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }
    if (mounted()) dismount(conn, now, DismountWhy::PlayerDied);
    take_damage(conn, static_cast<double>(hp->value) + 1.0, now);
    check_death(conn, now);
    if (!dead_) {
        r.refusal = name() + " did not die (the death was not taken)";
        return r;
    }
    r.ok = true;
    r.deaths = deaths_;
    spdlog::info("[gm] {}: killed by {} ({} death(s) this session)", name(), by, deaths_);
    return r;
}

PlayerSession::GmXpResult PlayerSession::gm_give_experience(Connection& conn, int64_t amount, Clock::time_point now, const std::string& by) {
    GmXpResult r;
    if (amount < 1) {
        r.refusal = "the experience must be 1 or more";
        return r;
    }
    const leveling::LevelCurve* curve = world_ ? world_->level_curve() : nullptr;
    if (!curve) {
        r.refusal = "this world has no level curve (its content.db lacks the Adventuring levels)";
        return r;
    }
    if (!room_for(12 + (world_ ? world_->auto_grants().size() : 0))) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }
    const std::span<const content::AutoGrant> grants = world_->auto_grants();
    const leveling::XpResult xp = leveling::add_xp(*curve, grants, progress_, amount);
    r.before = progress_.xp;
    r.level_before = progress_.level;
    progress_ = xp.after;
    progress_changed_ = true;
    r.after = progress_.xp;
    r.level_after = progress_.level;
    r.leveled = xp.leveled();
    if (r.leveled) levels_gained_ += static_cast<size_t>(r.level_after - r.level_before);
    bootstrap::AbilityChange rank_effects;
    std::vector<protocol::repl::SkillOnActionbar> skill_updates;
    if (r.leveled) finish_level_up(xp, false, now, rank_effects, skill_updates);
    const int64_t total = progress_.xp, debt = progress_.debt;
    queue("XPTracker (a GM's experience)", [total, debt](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_progress(total, debt)) return std::nullopt;
        return pb.leveling_update();
    }, conn, /*must_build=*/true);
    if (r.leveled) {
        const int level = progress_.level;
        queue("BroadcastPlayLevelUpEffects", [level](bootstrap::PlayerBootstrap& pb) { return pb.level_up_broadcast(content::kGuidAdventuring, level); }, conn);
        queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("a GM's experience"); }, conn, /*must_build=*/true);
    }
    for (const protocol::repl::SkillOnActionbar& u : skill_updates) {
        queue("ClientUpdateSkillOnActionbar", [u](bootstrap::PlayerBootstrap& pb) { return pb.skill_on_actionbar(u); }, conn, /*must_build=*/true);
    }
    hand_over_progress("a GM's experience");
    spdlog::info("[gm] {}: {:+} Adventuring XP by {} ({} -> {}, level {} -> {})", name(), amount, by, r.before, r.after, r.level_before, r.level_after);
    r.ok = true;
    return r;
}

PlayerSession::GmCooldownResult PlayerSession::gm_clear_cooldowns(Connection& conn, Clock::time_point now, const std::string& by) {
    GmCooldownResult r;
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) {
        r.refusal = name() + " has no abilities to clear the cooldowns of";
        return r;
    }
    const size_t running = pipeline_->cooldowns_running();
    const bool dodge = runtime->clear_cooldowns(ability_seconds(now));
    if (running == 0 && !dodge) {
        r.refusal = name() + " has no cooldown running";
        return r;
    }
    const size_t generic = pipeline_->clear_cooldowns(ability_seconds(now));
    take_ability_events();
    flush_abilities(conn);
    r.cleared = generic + (dodge ? 1 : 0);
    r.ok = true;
    spdlog::info("[gm] {}: {} cooldown group(s) ended by {}", name(), r.cleared, by);
    return r;
}

const content::QuestRow* PlayerSession::gm_quest_named(const std::string& ref, std::string* why) const {
    const quest::QuestBook* book = world_ ? world_->quests() : nullptr;
    if (!quests_on_ || !book) {
        if (why) *why = "quests are off for " + name();
        return nullptr;
    }
    if (ref.empty()) {
        if (why) *why = "no quest is named";
        return nullptr;
    }
    const content::QuestRow* def = book->find(ref);
    if (!def && why) *why = "no quest is named \"" + ref + "\"";
    return def;
}

PlayerSession::GmQuestResult PlayerSession::gm_start_quest(Connection& conn, const std::string& quest, bool bypass, Clock::time_point now, const std::string& by) {
    GmQuestResult r;
    std::string why;
    const content::QuestRow* def = gm_quest_named(quest, &why);
    if (!def) {
        r.refusal = why;
        return r;
    }
    r.quest = def->name;
    r.quest_guid = def->guid;
    if (quest_log_.find(def->guid)) {
        r.refusal = name() + " has taken " + def->name + " already";
        return r;
    }
    const dialogue::Book* book = world_->dialogue();
    if (!book) {
        r.refusal = "this world has no conversations, so no quest can be started";
        return r;
    }
    const auto facts = dialogue_facts(nullptr);
    dialogue::Evaluator ev(*book, *facts);
    const quest::Create c = quest_log_.can_start(def, ev, bypass);
    if (c != quest::Create::Ok) {
        r.refusal = def->name + " cannot be started" + (bypass ? std::string() : " (its rules are asked; bypass=yes skips them)") + ": " + quest::create_name(c);
        return r;
    }
    start_quest(conn, *def, 0, "a GM started it", now);
    r.completions = quest_log_.completions(def->guid);
    r.ok = true;
    spdlog::info("[gm] {}: started quest {} ({}) by {}{}", name(), def->name, def->guid, by, bypass ? " with its rules bypassed" : "");
    return r;
}

PlayerSession::GmQuestResult PlayerSession::gm_complete_quest(Connection& conn, const std::string& quest, Clock::time_point now, const std::string& by) {
    GmQuestResult r;
    std::string why;
    const content::QuestRow* def = gm_quest_named(quest, &why);
    if (!def) {
        r.refusal = why;
        return r;
    }
    r.quest = def->name;
    r.quest_guid = def->guid;
    if (!quest_log_.find(def->guid)) {
        r.refusal = name() + " has not taken " + def->name;
        return r;
    }
    r.completions = static_cast<int32_t>(quest_log_.completions(def->guid));
    if (!end_quest(conn, def->guid, "finish", now)) {
        r.refusal = def->name + " is not in " + name() + "'s quest log";
        return r;
    }
    r.completions = static_cast<int32_t>(quest_log_.completions(def->guid));
    r.ok = true;
    spdlog::info("[gm] {}: completed quest {} ({}) by {}", name(), def->name, def->guid, by);
    return r;
}

PlayerSession::GmObjectiveResult PlayerSession::gm_complete_objective(Connection& conn, int64_t objective, Clock::time_point now, const std::string& by, int64_t only_quest) {
    GmObjectiveResult r;
    r.objective = objective;
    const quest::QuestBook* book = world_ ? world_->quests() : nullptr;
    if (!quests_on_ || !book) {
        r.refusal = "quests are off for " + name();
        return r;
    }
    int64_t quest = 0;
    for (const quest::QuestInstance& q : quest_log_.instances()) {
        if (only_quest != 0 && q.record != only_quest) continue;
        for (const quest::ObjectiveState& o : q.objectives) {
            if (o.record == objective) {
                quest = q.record;
                break;
            }
        }
        if (quest != 0) break;
    }
    if (quest == 0) {
        r.refusal = "no quest of " + name() + " has the objective " + std::to_string(objective);
        return r;
    }
    const content::QuestRow* def = book->quest(quest);
    r.quest = quest;
    r.quest_name = def ? def->name : std::to_string(quest);
    bool done = false;
    quest_engine(conn, now, "a GM completed an objective", [&](quest::Engine& eng) { done = eng.succeed(quest, objective); });
    if (!done) {
        r.refusal = "the objective " + std::to_string(objective) + " of " + r.quest_name + " is not active or is done already";
        return r;
    }
    r.ok = true;
    spdlog::info("[gm] {}: completed objective {} of quest {} ({}) by {}", name(), objective, r.quest_name, quest, by);
    return r;
}

PlayerSession::GmResetQuestsResult PlayerSession::gm_reset_quests(Connection& conn, Clock::time_point /*now*/, const std::string& by) {
    GmResetQuestsResult r;
    if (!quests_on_) {
        r.refusal = "quests are off for " + name();
        return r;
    }
    r.completed = quest_log_.completed().size();
    r.taken = quest_log_.instances().size();
    r.variables = quest_log_.variables().size();
    quest_log_.clear(0);
    quests_touched_ = true;
    quest_sync(conn, "a GM cleared the finished quests");
    r.ok = true;
    spdlog::info("[gm] {}: forgot {} finished quest(s), {} variable(s) and kept {} taken quest(s), by {}", name(), r.completed, r.variables, r.taken, by);
    return r;
}

bool PlayerSession::gm_send_clock(Connection& conn, Clock::time_point now) {
    if (!world_ || !room_for(1)) return false;
    const double world_time = pb_.setup().world_time + session_seconds(now);
    const world::WorldClock clock = world_->day_clock();
    queue("clock (a GM's time of day)", [world_time, clock](bootstrap::PlayerBootstrap& pb) { return pb.set_time(world_time, clock); }, conn);
    ++time_syncs_sent_;
    return true;
}

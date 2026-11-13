// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/gm_admin.inc: the game-master test commands the WorldAdmin run call reaches (session/gm_admin.inc holds them)
//
//  Each acts on a character who is in the world, and sends the client what a change in play sends: the Health and Mana items, the level-up's XPTracker and BroadcastPlayLevelUpEffects,
//  the death's ClientRestart and death location, the respawn's pawn state and correction, the quest component's update, the cooldowns removed from the pawn, and ClientSetTime.
//  The results say what changed, or why nothing did.
// ============================================================================
public:
    /// Setting a current value (Health or Mana): what it was, what it is and the character's maximum.
    struct GmValueResult {
        bool        ok = false;
        std::string refusal;
        double      before = 0, after = 0, maximum = 0;
    };
    GmValueResult gm_set_health(Connection& conn, double value, Clock::time_point now, const std::string& by);
    GmValueResult gm_set_mana(Connection& conn, double value, Clock::time_point now, const std::string& by);
    /// Both Health and Mana to their maximum; `health` and `mana` are the two values (and their maximums).
    struct GmHealResult {
        bool        ok = false;
        std::string refusal;
        GmValueResult health, mana;
    };
    GmHealResult gm_heal(Connection& conn, Clock::time_point now, const std::string& by);

    /// The revive: where the character came back.
    struct GmReviveResult {
        bool        ok = false;
        std::string refusal;
        std::string place;
        double      x = 0, y = 0, z = 0;
    };
    GmReviveResult gm_revive(Connection& conn, Clock::time_point now, const std::string& by);

    /// The kill: the deaths the session counted when it returned.
    struct GmKillResult {
        bool        ok = false;
        std::string refusal;
        size_t      deaths = 0;
    };
    GmKillResult gm_kill(Connection& conn, Clock::time_point now, const std::string& by);

    /// Experience added: the totals and levels before and after, and whether a level was crossed.
    struct GmXpResult {
        bool        ok = false;
        std::string refusal;
        int64_t     before = 0, after = 0;
        int32_t     level_before = 1, level_after = 1;
        bool        leveled = false;
    };
    GmXpResult gm_give_experience(Connection& conn, int64_t amount, Clock::time_point now, const std::string& by);

    /// The cooldowns ended: how many groups were running.
    struct GmCooldownResult {
        bool        ok = false;
        std::string refusal;
        size_t      cleared = 0;
    };
    GmCooldownResult gm_clear_cooldowns(Connection& conn, Clock::time_point now, const std::string& by);

    /// A quest command's answer: the quest it found and the completions the character has of it now.
    struct GmQuestResult {
        bool        ok = false;
        std::string refusal;
        std::string quest;
        int64_t     quest_guid = 0;
        int32_t     completions = 0;
    };
    GmQuestResult gm_start_quest(Connection& conn, const std::string& quest, bool bypass, Clock::time_point now, const std::string& by);
    GmQuestResult gm_complete_quest(Connection& conn, const std::string& quest, Clock::time_point now, const std::string& by);

    /// Completing one objective: the quest it belongs to and its name.
    struct GmObjectiveResult {
        bool        ok = false;
        std::string refusal;
        int64_t     quest = 0, objective = 0;
        std::string quest_name;
    };
    /// `quest` (a quest record, 0: none) limits the search to that quest's instance: two quests may share an objective's Guid.
    GmObjectiveResult gm_complete_objective(Connection& conn, int64_t objective, Clock::time_point now, const std::string& by, int64_t quest = 0);

    /// Forgetting the finished quests and the variables: what was there before.
    struct GmResetQuestsResult {
        bool        ok = false;
        std::string refusal;
        size_t      completed = 0, taken = 0, variables = 0;
    };
    GmResetQuestsResult gm_reset_quests(Connection& conn, Clock::time_point now, const std::string& by);

    /// Sends the client its clock now, with the world's clock as it is (the set-time command).
    bool gm_send_clock(Connection& conn, Clock::time_point now);
    /// Finds the content quest a command names (its record name or Guid); nullptr with `why` when it is none or quests are off.
    const content::QuestRow* gm_quest_named(const std::string& ref, std::string* why) const;
private:

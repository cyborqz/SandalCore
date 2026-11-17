// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.
// ============================================================================
//  session/members/pvp.inc: the members of PlayerSession for PvP (session/player_pvp.cpp, the rules in combat/pvp.h)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    /// PvP: the player's flag (what his pawn's Alignments say: white, purple or red), his corruption and whether he is in a fight with another player.
    combat::pvp::Flag pvp_flag_state() const { return pvp_state_; }
    double            corruption() const { return corruption_; }
    double            blight() const { return blight_; }
    /// PvP, step 2: the debuff of his corruption's tier he holds now (Corruption_1 .. Corruption_5; 0: none), and how many worn items his last death dropped.
    int64_t           corruption_debuff() const { return pvp_tier_effect_; }
    size_t            gear_dropped_last() const { return gear_dropped_last_; }
    bool              in_pvp_combat() const { return pvp_combat_; }
    /// Step 4: he holds a PvP opt-in protection (a respawn's Emberspring_PVPOptInProtection, or General_PVPOptInProtection), and how many respawns gave him one.
    bool              pvp_protected() const;
    uint64_t          pvp_protections() const { return pvp_protections_; }
    /// How many times an action of his flagged him, and how many players he killed (the tests and the log).
    uint64_t          pvp_flaggings() const { return pvp_flaggings_; }
    uint64_t          pvp_kills() const { return pvp_kills_; }
    /// The pawn that landed the last harmful blow he took, when it was another player's (0: a creature's or none).
    uint64_t          last_pvp_blow() const { return last_blow_by_; }
    /// What the admin command pvp did.
    struct GmPvpResult {
        bool        ok = false;
        std::string refusal;
        std::string text;
    };
    /// The admin command pvp (session/admin_commands.h): noncombatant, combatant, or corruption <amount>, the flag of the player as a GM sets it (testing the nameplates and the rules).
    GmPvpResult gm_pvp(Connection& conn, const std::string& what, double amount, Clock::time_point now, const std::string& by);

private:
    /// The player's side of the PvP rules as he is now, and his force flag (the PvP key, or the flag his last trigger carried).
    void pvp_side_of(combat::pvp::Side& out, bool& force) const;
    /// An action of his flagged him at `world_now` (the world's clock): he is a combatant from now for the flag's time (a corrupted player stays corrupted).
    /// True when he was not a combatant before (the client is told why: NotifyFlaggingReason).
    bool pvp_mark_flagged(double world_now);
    /// The client's side of PvP at the session's tick: the flag's notices, its end, the pawn's Alignments, bInPVPCombat, the kills.
    void drain_pvp(Connection& conn, Clock::time_point now);
    /// The pawn's Alignments as the flag is now, sent when they changed.
    void pvp_send_alignments(Connection& conn, const char* why);
    /// The world's clock now (the clock the combat world is ticked with, and the flag's time is kept on).
    double pvp_world_now(Clock::time_point now) const;
    /// He killed another player (the world said so): the count and the log; a non-combatant's death outside a duel and his group corrupts him
    /// (CorruptionAddedOnKill, BlightAddedOnKill).
    void pvp_take_kill(Connection& conn, const combat::PvpKill& kill, Clock::time_point now);
    // ---- step 2: corruption (combat/pvp_corruption.h)
    /// The record's corruption and blight at the join (migration 0025): a corrupted character comes back red.
    void pvp_init(const svc::Character& character);
    /// His corruption becomes `value` (0 or more): the flag (Corrupted while above 0), the pawn's Alignments, the Corruption stat and the tier's debuff follow; the save carries it.
    void pvp_set_corruption(Connection& conn, double value, const char* why);
    /// The Corruption and Corruption_Blight stats and the tier's debuff as the numbers say (retried at each tick until the stats and the pawn's items are there).
    void pvp_sync_stats(Connection& conn);
    /// One of the two stats as the client is told it (set_current_value; an item in the array its audience names when the pawn's arrays have none for it).
    bool pvp_set_stat(int64_t record, double value);
    /// A creature's kill burns his corruption off (CorruptionReductionOnKillNPC): `base_xp` is the kill's experience before the server's rate.
    void pvp_npc_kill(Connection& conn, uint64_t npc, double base_xp);
    /// What a corrupted death costs besides the debt and the resources (die): the worn items the tier drops (into `lost`, the death's pile), then the corruption the
    /// death burns (CorruptionReductionOnDeath).
    void pvp_corrupted_death(Connection& conn, int tier, std::vector<combat::LootStack>& lost);

    combat::pvp::Flag pvp_state_ = combat::pvp::Flag::NonCombatant;
    double   pvp_combatant_since_ = -1;    // the world time of the last action that flagged him (< 0: none since the login)
    double   corruption_ = 0;               // the Corruption stat (kept over a relog: migration 0025)
    double   blight_ = 0;                   // Corruption_Blight
    bool     pvp_changed_ = false;          // the corruption or blight moved in this session: the save carries them
    bool     pvp_stats_dirty_ = true;       // the stats and the tier's debuff are to be brought in line
    int64_t  pvp_tier_effect_ = 0;          // the tier's debuff on him now
    size_t   gear_dropped_last_ = 0;
    stats::SplitMix pvp_rng_{0x5eed0c0bb1e5ca1eull};   // the dice of a corrupted death (how many worn items, which)
    bool     pvp_trigger_flag_ = false;     // the PvP force flag his last ServerTriggerAbility carried
    bool     pvp_combat_ = false;           // CombatInfo.bInPVPCombat as the client was told
    combat::pvp::Flag pvp_sent_state_ = combat::pvp::Flag::NonCombatant;   // what the pawn's Alignments say
    double   pvp_sent_since_ = -1;
    std::array<uint32_t, 4> pvp_sent_duel_{};   // the duel the pawn's Alignments name (step 3)
    bool     pvp_drop_protection_ = false;   // step 4: a flagging action of his: the protection comes off at the next tick
    uint64_t pvp_protections_ = 0;
    uint64_t last_blow_by_ = 0;
    uint64_t pvp_flaggings_ = 0, pvp_kills_ = 0;
    size_t   pvp_logged_ = 0;
private:

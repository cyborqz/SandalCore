// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/stance.inc: the members of PlayerSession that show the player's combat stance to the other players (Social, seeing other players: their combat
//  stances) and answer the client's draw request.  Included at the end of the class (session/members_registered.inc); it sets its own public: and private:.
//  session/player_stance.inc holds the code; session/shown_to_others.h says where a player is shown.
// ============================================================================
public:
    /// The draw (CombatInfo.bIsTargeting) the client last asked for, and what the session heard: the calls, the changes it made and the calls the budget dropped (tests).
    bool     targeting() const { return targeting_; }
    size_t   targeting_calls() const { return targeting_calls_; }
    size_t   targeting_changes() const { return targeting_changes_; }
    size_t   targeting_refused() const { return targeting_refused_; }

private:
    /// Every tick after sync_shown_death: the five stance bits of the pawn the other players are shown (bIsTargeting, bInCombat, bPvPForceFlag, InPvpSuppressionArea and
    /// bInPVPCombat), and the Alignment component when the PvP flag changed; one update carries them with whatever else moved in the tick.  Each bit is kept as the
    /// shown pawn carries it, so a change writes them once.  A dead player's combat and draw bits are written 0 in the same tick as his death bit.
    void sync_shown_stance();
    /// The client's ServerSetTargeting(bNewTargeting): the player's own pawn takes bIsTargeting and the pawn the others are shown follows at the next tick.  A request over
    /// the budget is dropped, and one that repeats the bit changes nothing.
    void on_set_targeting(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// The PvP flag changed: the pawn the other players are shown takes the Alignment that goes with it at the next tick (the name colour).
    void note_shown_alignment() { shown_alignment_dirty_ = true; }
    /// bIsTargeting as the player's own pawn carries it (the client's draw).
    bool     targeting_ = false;
    /// The draw requests: a burst of sixteen, then twenty a second, as the other quiet requests (a keyboard toggle cannot come faster).
    RequestBudget targeting_budget_{kQuietBurst, kQuietInterval};
    size_t targeting_calls_ = 0, targeting_changes_ = 0, targeting_refused_ = 0;
    size_t stance_logged_ = 0;
    /// The stance the pawn the other players are shown carries now, bit by bit.
    bool shown_targeting_ = false;
    bool shown_in_combat_ = false;
    bool shown_pvp_flag_ = false;
    bool shown_suppression_ = false;
    bool shown_pvp_combat_ = false;
    bool shown_alignment_dirty_ = false;   // the PvP flag changed the Alignment, which goes out with the next stance write
private:

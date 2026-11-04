// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Players seeing each other (stage A): written by unspike in #14.
// Moved here from session/player_session.h so that file stays within its size limit (scripts/hotspot_limits.txt).
// ============================================================================
//  session/members/presence.inc: the members of PlayerSession that put this player into the world's registry for the other players (session/player_presence.cpp)
//  Included at the end of the class (session/members_registered.inc).
// ============================================================================
private:
    // Multiplayer visibility, stage A: this player in the world's registry, as the other players' views open it (ActorKind::Player; 0: not there)
    // #14 review: world_player_state_ is gone with the PlayerState (the other players are shown the pawn alone)
    uint64_t                       world_pawn_ = 0;
    bool                           presence_failed_ = false;     // the observer's pawn does not build: the other players never see this one (said once)
    GoodPosition                   presence_at_;                 // the place the registry's pawn was last put
    bool                           presence_moving_ = false;     // ... with a velocity (an update with none follows when the moves stop)
    Clock::time_point              presence_moved_at_{};
    /// Puts the player's pawn into the world's registry once the phase-in is built, then keeps the pawn where the last good position is
    /// (ReplicatedMovement with the velocity of the step, unreliable as an NPC's walk).
    /// #14 review: called once per tick, from tick_logic (session/player_view_phases.cpp) and from nowhere else, so that the registry is written in the first part of the
    /// server's tick, before every view pass; it used to be called from tick(), which the server does not call with view threads on, and from every accepted move.
    void sync_presence(Clock::time_point now);
    /// Takes the pawn out of the registry: every view that holds it closes it (Destroyed).
    void leave_presence(const char* why);
private:

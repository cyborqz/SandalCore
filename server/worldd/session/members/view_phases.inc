// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/members/view_phases.inc: the members of PlayerSession that cut its tick into the parts the server's phases run apart (session/player_view_phases.cpp)
//  (included at the end of the class by session/members_registered.inc)
// ============================================================================
public:
    /// What the first part of a session's tick found.
    struct TickPlan {
        bool   stopped = false;    // the session is finished or failed: nothing more is done this tick
        bool   view = false;       // the player's view of the world is to be ticked (the join and the phase-in are out)
        bool   view_due = false;   // ... and its relevancy pass is due (decided by tick_logic when it was asked to)
        double x = 0, y = 0;       // where the view looks from
    };
    /// Everything PlayerSession::tick does before the player's view: the session's own work.  With `plan_view` it also decides, in turn, whether the view's pass is due (ActorView::plan).
    TickPlan tick_logic(Connection& conn, Clock::time_point now, bool plan_view);
    /// The view's part of the tick (ActorView::run); it can run on any thread, alone for this session.
    void run_view(Connection& conn, const TickPlan& plan, Clock::time_point now);
    /// What follows the view in the tick: the NPC world told where the player is, the position line of the log, the periodic save.
    void tick_after(Clock::time_point now, const TickPlan& plan);
private:

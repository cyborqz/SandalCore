// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_view_phases.cpp: a session's tick in the parts the server's phases run apart (the declarations are in session/members/view_phases.inc)
//
//  PlayerSession::tick is the session's own work (the abilities, the mounts, the quests ... up to the player's view of the world), then the view's tick, then what follows it (the NPC
//  world told where the player is, the position line of the log, the periodic save).  A server that runs the players' view passes on several threads (net/endpoint.cpp, --view-threads)
//  runs the first part of every session, then all the passes at once, then the last part of every session; these are the same lines of the same function, cut where the view's tick was:
//  tick_logic up to it (and, when asked, the serial decision whether the pass is due), run_view for the view's own part, tick_after for the rest.  PlayerSession::tick calls the three in a row.
// ============================================================================
#include "session/player_session.h"

namespace aoc::worldd {

PlayerSession::TickPlan PlayerSession::tick_logic(Connection& conn, Clock::time_point now, bool plan_view) {
    TickPlan plan;
    last_now_ = now;
    pump(conn); send_move_ack(conn, now);   // (the held answer to the latest good move, session/player_move_ack.cpp)
    if (finished_ || failed_) {
        plan.stopped = true;
        return plan;
    }
    session_seconds(now);   // the clock starts at the first tick
    run_abilities(conn, now);
    run_pets(conn, now);
    run_mounts(conn, now);
    run_ride(conn, now);
    run_timers(conn, now);
    run_gather(conn, now);
    run_foliage(conn, now);
    run_floor_net(conn, now);
    run_embersprings(conn, now);   // Embersprings: a shrine whose radius holds the player is discovered
    run_props(conn, now);          // quests part 3: a use of a quest object that takes time
    run_dialogue(conn, now);
    run_quests(conn, now);
    run_warehouse(conn, now);
    run_crafting(conn, now);
    run_quick_make(conn, now);
    run_cargo(conn, now);
    run_nodes(conn);
    sync_item_abilities(conn);
    // Players seeing each other (stage A, unspike's #14): this player's pawn in the registry, for the other players' views.  #14 review: once a tick, here and nowhere else.  This is the first part of the
    // server's tick (before every view pass of the tick), after everything that can move the player (the moves, a teleport, a respawn, the floor net), so the registry is never written while the views
    // read it, and it makes no difference whether the server runs the view passes on threads.
    sync_presence(now);
    // E1 stage 3 fix: the client's synchronized clock, set again after the phase-in and every 120 s (see the phase-in above)
    if (phase_in_done_ && !time_syncs_.empty() && now >= time_syncs_.front() && room_for(1)) {
        time_syncs_.pop_front();
        if (time_syncs_.empty()) time_syncs_.push_back(now + kTimeSyncInterval);
        const double world_time = pb_.setup().world_time + session_seconds(now);
        ++time_syncs_sent_;
        // the game time it carries is the world's clock at this moment (world/world_clock.h), the clock the join's ClientSetTime read
        const world::WorldClock clock = world_ ? world_->day_clock() : pb_.setup().clock;
        queue("clock", [world_time, clock](bootstrap::PlayerBootstrap& pb) { return pb.set_time(world_time, clock); }, conn);
    }
    // A town's scenery (its ANodeLayoutReplicator) is opened in the join, before the phase-in.  The client's pawn phases in when a 10 m trace down finds a blocking object, or after 120 s,
    // and in a town's bowl the only ground the client has is what the replicator builds: opened by the pass below, after the phase-in, it left the player waiting out the cap
    // (about two minutes, against about 30 s without a bowl).  Every join asks, whether or not it stands in a bowl: the replicator is relevant within its own radius
    // (1.45 km) and a join farther from every town finds none, so nothing changes there.  Only the scenery: the NPCs and every other actor keep waiting for the phase-in (below).
    if (view_ && !phase_in_done_ && !scenery_done_ && join_.empty()) {
        const GoodPosition at = checker_.good();
        size_t opened = 0;
        scenery_done_ = view_->open_scenery(conn, at.x, at.y, now, &opened);
        if (opened > 0) {   // (each open has its own "[scenery] open for player" line; the join's one line is the phase-in's below)
            scenery_opened_ += opened;
            scenery_at_ = session_seconds(now);
        }
    }
    // plan E1: the other actors, once the join and the phase-in are out (their opens need the client's world to exist)
    if (view_ && phase_in_done_ && join_.empty() && replies_.empty() && delayed_.empty()) {
        const GoodPosition at = checker_.good();
        plan.view = true;
        plan.x = at.x;
        plan.y = at.y;
        if (plan_view) plan.view_due = view_->plan(now);   // (the views' shared count of phases changes here, in turn, never on two threads at once)
    }
    return plan;
}

void PlayerSession::run_view(Connection& conn, const TickPlan& plan, Clock::time_point now) {
    if (view_ && plan.view) view_->run(conn, plan.x, plan.y, now, plan.view_due);
}

void PlayerSession::tick_after(Clock::time_point now, const TickPlan& plan) {
    if (plan.view) {
        if (npc_world_) npc_world_->note_player(static_cast<uint64_t>(pb_.setup().player_id), plan.x, plan.y);   // the NPCs near the player are awake
        if (!position_log_started_) {
            position_log_started_ = true;
            last_position_log_ = now;
        } else if (now - last_position_log_ >= kPositionLogInterval) {
            last_position_log_ = now;
            log_position(now);
        }
    }
    if (!save_clock_started_) {
        save_clock_started_ = true;
        last_save_ = now;
        return;
    }
    if (now - last_save_ < save_interval_) return;
    last_save_ = now;
    if (position_changed(/*with_yaw=*/false)) hand_over("periodic save", /*periodic=*/true);
    if (inventory_dirty_) hand_over_inventory("periodic save", /*periodic=*/true);
}

}  // namespace aoc::worldd

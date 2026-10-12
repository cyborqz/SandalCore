// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  npc/npc_world.h  (plan E1, stage 2)
//
//  The NPCs' life in the area: standing on the ground, wandering within their radius, walking their patrol, going home.  One NpcWorld holds the
//  motion of every NPC of the registry (world/actor_registry.h) and advances it on a fixed 15 Hz step (retail's server tick, 67 ms) from a clock it
//  is given; each NPC has a Mover (npc/mover.h) for its walking and a small state machine on top.
//
//  What moves.  Only an NPC near a player: within the awake ring (15,000 cm, the relevancy default, with a margin) of some player the sessions
//  noted this second.  The others stand still where they are, frozen mid-state, until a player comes near ("dormant ones stay still until woken":
//  the client of a snapshot-ring NPC sees it as it was).  An NPC that leaves every ring stops at once and reports a zero velocity.
//
//  Replication.  A step moves the registry's actor (grid and version: Change::Movement) and writes what the retail server writes: ReplicatedMovement
//  (location at 0.01 cm, velocity in whole cm/s, yaw as one byte), its timestamp, CurrentMovementInput (1 while walking forward, 0 standing).  The
//  actor view sends the changed handles on unreliable chains, so a walking NPC costs what the retail ones did.
//
//  Behaviours (a PopulationInstance's NpcAmbientBehavior).
//    Stationary   stands where it spawned.
//    Wander       idles (retail: gaps of 1.5 to 9 s, median 4.5), walks a leg (retail: a median of 250 cm, 740 at the 90th percentile, drawn log-normally
//                 and kept within MinRadius .. MaxRadius = 100 .. 2,000 cm of home: a leg that would leave the radius turns back towards home), at the
//                 speed the retail captures show for its archetype (else a quarter of its Blueprint's walk speed), idles again.  A point with no path is
//                 dropped and another is tried.
//    Patrol       walks its PathingRoute's waypoints in order, pausing at each as the route says; a looping route goes round, any other goes
//                 there and back.  A route placed by a city node or an encounter is not walked in the open world: its NPC stands.
//    Return home  an NPC that is further from home than twice its radius (or that stage 3's AI sends) walks back at a run.
//  "Default" (7,335 instances) is Wander: the retail captures show Default instances wandering.
//
//  The seams of stage 3.  `order_move` / `release` / `order_return_home` hand an NPC's feet to the caller (the AI chases, keeps a range, goes
//  home); `mover()` is the service itself.  The state machine here runs only while nothing has ordered the NPC.
//
//  Without a navmesh (the file is missing or broken) there is no NpcWorld to build: NPCs keep standing where the loader put them, and worldd's log
//  says so once.
// ============================================================================
#pragma once

#include "npc/mover.h"
#include "world/actor_registry.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::npc {

enum class Ambient : uint8_t { Stationary, Wander, Patrol };

/// What an NPC's life needs of its data, cm and seconds.
struct NpcMotionSetup {
    Ambient ambient = Ambient::Stationary;
    double  home_yaw = 0;               // degrees
    double  foot_offset = 88.0;         // the capsule's half height times the actor's scale
    double  yaw_rate = 360.0;           // degrees per second
    double  wander_min = 100.0, wander_max = 2000.0;     // cm from home
    double  leg_median = 250.0, leg_p90 = 740.0;        // cm: how far one wander leg goes (the retail captures' median and 90th percentile), within wander_max of home
    double  wander_delay = 1.0, wander_delay_deviation = 0.5;   // the data's (seconds); see Config::idle_per_delay
    double  walk_speed = 150.0;         // cm/s while wandering
    double  patrol_speed = 240.0;       // cm/s while on a route
    double  run_speed = 300.0;          // cm/s going home
    double  arrival_distance = 100.0;   // within this of a waypoint counts as there
    std::vector<nav::Vec3> route;       // world waypoints (ground), for Patrol
    std::vector<double>    pauses;      // seconds at each waypoint (shorter: the rest 0)
    bool    loop = false;
    bool    start_closest = false;
    double  awake_radius = 0;           // 0: the config's
};
// built with C | Y | B and CoA

struct NpcWorldConfig {
    double step_seconds = 1.0 / 15.0;    // retail's server tick
    int    max_catch_up_steps = 4;       // a clock that jumped ahead is not made up for beyond this
    double awake_cm = 15000.0;           // the relevancy default
    double awake_margin_cm = 2000.0;     // a margin so that an NPC at the ring's edge does not freeze and thaw with every step
    double player_ttl = 2.0;             // seconds a noted player position counts
    /// The wander idle is `delay * idle_per_delay` +- `deviation * idle_spread` seconds, clamped to [min_idle, max_idle]: the data's WanderDelay is 1.0 with 0.5,
    /// the retail NPCs idle for 4.5 seconds at the median (1.5 to 9), so the delay is read in units of 4.5 s (modelled).
    double idle_per_delay = 4.5;
    double idle_spread = 5.0;
    double min_idle = 1.5;
    double max_idle = 12.0;
    size_t max_pathfinds_per_step = 24;  // the paths asked for in one step (the rest decide on the next)
    int    max_search_iterations_per_step = 60000;   // the A* node expansions of one step's searches together (review A I5: 24 searches at the old cap of 2,500; about 43 ms at 0.7 us a node): the rest decide on the next step
    double max_wander_path_factor = 3.0; // a wander path longer than this times the walk's straight distance is dropped
};

class NpcWorld {
public:
    struct Stats {
        size_t npcs = 0;
        size_t moving_now = 0;
        size_t active_now = 0;
        uint64_t steps = 0;
        uint64_t moves_started = 0;
        uint64_t no_path = 0;           // a wander or patrol target with no path
        uint64_t wander_asks = 0, wander_no_point = 0, wander_no_path = 0, wander_deferred = 0, patrol_asks = 0, patrol_no_path = 0, patrol_deferred = 0;
        /// The searches that found no path, by why (nav::PathResult::Fail, as an index): the start off the loaded navmesh, the end off it, the search out of expansions, no way, too long
        uint64_t wander_fail[6] = {}, patrol_fail[6] = {};
        uint64_t returns_home = 0;
        uint64_t writes = 0;            // registry updates made
        double   max_step_ms = 0, max_refresh_ms = 0;   // the longest step of the world, and of its first part (who is awake)
        size_t   max_active = 0, max_pathfinds_in_step = 0;   // the most NPCs awake in a step, the most paths asked in one
        // the NPCs by what they are doing now (all NPCs, awake or not)
        size_t standing = 0, idling = 0, wandering = 0, patrolling = 0, patrol_pausing = 0, returning = 0, ordered = 0;
    };

    /// `nav` must outlive the NpcWorld (null: add() refuses every NPC).  `registry` is the world's.
    NpcWorld(world::ActorRegistry& registry, const nav::AreaNav* nav, NpcWorldConfig config = {});
    ~NpcWorld();
    NpcWorld(const NpcWorld&) = delete;
    NpcWorld& operator=(const NpcWorld&) = delete;

    /// Takes an NPC of the registry under its care: stands it on the ground (the nearest floor or terrain to the height it has, within 15 m), writes
    /// its place and starts its behaviour.  False for an unknown actor, one that is not an NPC, one already held, or a setup that is not a number.
    /// `ground_found` (optional) says whether a ground was there (when not, the NPC stays at the height it had and never moves).
    /// A city node's people (`NpcInfo::ground_by_data`) keep the height the data gave them: the terrain is never their ground (the baked terrain under a town's bowl is 50 m below its floor), so
    /// they stand at that height while their chunk is not resident and when no surface is within 15 m of it.
    /// With a chunk window (nav/area_nav.h, G1 stage 1) the ground of an NPC whose chunk is not resident cannot be asked yet: it stands on the terrain, is dormant, `pending` (optional) says so, and
    /// it is grounded for good when `chunks_resident` reports its chunk (the same standing as above).
    bool add(uint64_t actor_id, const NpcMotionSetup& setup, bool* ground_found = nullptr, bool* pending = nullptr);
    bool has(uint64_t actor_id) const { return npcs_.count(actor_id) != 0; }
    /// Drops an NPC's motion (the registry's removal of the actor does it by itself: a dead NPC of stage 3).  False when it is not held.
    bool remove(uint64_t actor_id);
    size_t size() const { return npcs_.size(); }

    /// A session tells where its player is (every tick); the position counts for `player_ttl` seconds.
    void note_player(uint64_t player, double x, double y, double now);
    void forget_player(uint64_t player);
    /// The noted positions that still count at `now` (the chunk window's interest).
    std::vector<std::pair<double, double>> player_points(double now) const;
    /// The ground's window made these chunks resident: the NPCs waiting for them are put on the ground (and the ones the chunk's tiles gave a floor to are lifted onto it).
    void chunks_resident(const std::vector<nav::ChunkKey>& chunks);
    /// NPCs that wait for their chunk.
    size_t pending_ground() const { return pending_count_; }
    /// Grounding that moves a waiting NPC by more than this (cm) is a reliable change of the actor (a Snapshot or Dormant view of it is sent again).
    static constexpr double kRegroundReliableCm = 30.0;

    /// Advances the world to `now` (seconds, a clock that never goes back) in steps of `step_seconds`.
    void tick(double now);

    // ---- the movement service (stage 3's AI) ------------------------------------------------------------------------------
    /// Takes the NPC's feet: it walks to `dest` and stands there until released (or ordered again).  False when no path.
    bool order_move(uint64_t actor_id, const nav::Vec3& dest, double speed_cm_s, double stop_distance = 0);
    /// Gives the feet back to the NPC's own behaviour.
    bool release(uint64_t actor_id);
    /// Stops the NPC where it stands and writes that it is still (a zero velocity); the NPC then stands for good (stage 3: it died).
    bool halt(uint64_t actor_id);
    /// Orders a walk home at the NPC's run speed; its behaviour takes over on arrival.  False when no path.
    bool order_return_home(uint64_t actor_id);
    const Mover* mover(uint64_t actor_id) const;
    /// True when the NPC has no life of its own: it stands where it is (a Stationary instance, no ground, no usable route) until the AI orders it.
    bool         stands(uint64_t actor_id) const;
    bool         is_ordered(uint64_t actor_id) const;
    nav::Vec3    home_of(uint64_t actor_id) const;
    /// Moves an NPC's home (a leash that is re-set, a respawn point): its wander and its walk home are about the new place.  False for an unknown NPC or a
    /// place that is not a number.
    bool         set_home(uint64_t actor_id, const nav::Vec3& home);
// C O A with Cyb

    Stats stats() const;
    const NpcWorldConfig& config() const { return cfg_; }
    double now() const { return now_; }

private:
    enum class Mode : uint8_t { Stand, Idle, Wander, Patrol, PatrolPause, Return, Ordered };
    struct Npc {
        uint64_t     id = 0;
        NpcMotionSetup setup;
        nav::Vec3    home;
        Mover        mover;
        Mode         mode = Mode::Stand;
        double       timer = 0;               // seconds to the next decision
        uint64_t     rng = 0;
        size_t       wp = 0;                  // the waypoint it walks to
        int          wp_dir = 1;
        int          fails = 0;               // paths that failed in a row
        bool         active = false;
        bool         reported_moving = false; // what the registry last got: walking or still
        bool         ordered_return = false;
        bool         first_leg = false;       // a patrol's first leg goes to its start (waypoint `wp`) before it goes round
        bool         pending_ground = false;  // its chunk is not resident: it stands on the terrain and sleeps until `chunks_resident` says its ground is there
        bool         pending_ground_done = false;
        double       data_z = 0;              // the height the data gave it (an origin), kept while it waits: the ground it is stood on is the surface nearest to that
    };

    double next_unit(Npc& n);
    void   step(double t);
    void   advance_npc(Npc& n, double t);
    void   decide(Npc& n, double t);
    void   start_wander(Npc& n);
    void   start_patrol_leg(Npc& n);
    void   go_idle(Npc& n);
    void   commit(Npc& n, double t, bool force);
    void   refresh_active(double t);
    /// Stands the NPC on the ground nearest the height it has and gives it its behaviour; true when a ground was found.
    bool   ground_npc(Npc& n, world::WorldActor& a);
    bool   awake_for_someone(const world::WorldActor& a) const;

    world::ActorRegistry&         registry_;
    const nav::AreaNav*           nav_;
    NpcWorldConfig                cfg_;
    std::unordered_map<uint64_t, Npc> npcs_;
    struct PlayerNote { double x, y, at; };
    std::map<uint64_t, PlayerNote> players_;
    std::vector<uint64_t>         active_;        // the NPCs that moved last step (sorted: a deterministic order)
    std::map<nav::ChunkKey, std::vector<uint64_t>> pending_by_chunk_;   // NPCs waiting for a chunk of the ground's window
    size_t                        pending_count_ = 0;
    uint32_t                      remove_listener_ = 0;
    double                        now_ = 0;
    double                        next_step_ = -1;
    size_t                        pathfinds_this_step_ = 0;
    int                           iterations_this_step_ = 0;

    Stats                         stats_;
};

}  // namespace aoc::worldd::npc

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
//                 there and back.  A route placed by a city node is walked at that node's record (a town person); one placed by an encounter is not walked: its NPC stands.
//    Return home  an NPC that is further from home than twice its radius (or that stage 3's AI sends) walks back at a run.
//  "Default" (7,335 instances) is Wander: the retail captures show Default instances wandering.
//
//  The seams of stage 3.  `order_move` / `release` / `order_return_home` hand an NPC's feet to the caller (the AI chases, keeps a range, goes
//  home); `mover()` is the service itself.  The state machine here runs only while nothing has ordered the NPC.
//
//  Root motion.  A creature's attack animation can carry the creature with it (a lunge: the montage's root bone travels 1.5 to 2.5 m).  The client plays the montage and
//  moves the creature along it by itself, so the server has to move its own copy the same way, at the same 15 Hz, and say where it is: `begin_root_motion` takes the NPC's
//  feet for the montage; each step puts the creature where the montage's root is, as far along the way there as the creature can go (it stops where it touches a capsule the
//  caller names, and at the edge of the walkable ground, and stays there while the root's place is beyond: the recorded creatures stop at the player and do not step back when the
//  animation does); it writes ReplicatedMovement as for any walk and RepRootMotion (the montage, its position, the creature's place and facing: what the client puts the creature
//  back to), and `end_root_motion` (or the montage's end) gives the feet back with the montage marked over.  An ability whose lunge pauses on contact is held where it is while a body is in the
//  box ahead of it (the montage goes on playing, the creature's velocity is zero) and `bRootMotionObstructed` goes out with those updates, which is what makes the client hold its own copy
//  too; when the box is clear the creature goes on by what the montage still travels (the travel it skipped is lost, as on the client).  The client's copy was moved by the montage on its own and is put back where the
//  server has it only while its montage plays; when its montage lets go the ordinary placement is back, and it must be given the place once more: the retail server sends ReplicatedMovement
//  (the place, the facing, no velocity) at every update after the end of a Snap or a Fury, the first 0.03 to 0.2 s after it, and so does this one, for `settle_seconds` or until the creature walks or
//  a montage takes it again.
//
//  Without a navmesh (the file is missing or broken) there is no NpcWorld to build: NPCs keep standing where the loader put them, and worldd's log
//  says so once.
// ============================================================================
#pragma once

#include "npc/mover.h"
#include "world/actor_registry.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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
    /// The reach of the ground of a city node's person (`ground_by_data`): its data height is within 1 to 168 cm of the town's floor, so a surface 3 m under it or 1.5 m over it is its ground, and a
    /// roof or a ledge farther than that is not (the open world's reach is 15 m each way).
    double data_ground_below_cm = 300.0;
    double data_ground_above_cm = 150.0;
    /// Within the agent's radius (35 cm) of a wall the walking map has no polygon at a place, and a posted person stands that close to a stall or a house: where there is none at its exact place, the
    /// floor of the nearest polygon within this radius (cm), in the same window, is its ground.
    double data_ground_near_cm = 100.0;
    /// After a montage lets go of a creature its place goes out again at every step (ReplicatedMovement with the creature's place and facing and no velocity, even though nothing about it changed) for this many
    /// seconds, unless it walks or a montage takes it first: the client's copy, moved by the montage's root motion and no longer held to the server's by its montage, is put back by it.  The recorded retail
    /// creatures do it at every update from the first one after the end (0.03 to 0.2 s) until the next cast or a walk (1 to 7 updates in the 6 recorded Snap and Fury ends); the longest the client's catch-up
    /// waits for its montage (the age of a move it still uses) is 0.5 s, so a client whose montage ends late is still given the place after it.
    double settle_seconds = 0.5;
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
    /// they stand at that height while their chunk is not resident and when no surface is within the tight reach of NpcWorldConfig::data_ground_below_cm / above of it.  The exception is a creature
    /// whose data says SnapToGround (`NpcInfo::snaps_to_ground`): with no surface in that reach it takes the ground nearest its height within 15 m (a flyer, `NpcInfo::flies`, only when that ground is over it).
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
    /// Turns a standing NPC to face the point (x, y) at once and publishes it (the facing a creature attacks with; it keeps it while it stands).  False for an NPC the world does not hold,
    /// and for one a montage is carrying (it keeps the facing it began with).
    bool face_toward(uint64_t actor_id, double x, double y);
    /// Turns a standing NPC towards the point (x, y) at its rotation rate (or `rate` degrees a second), one step at a time: unlike `face_toward` its facing never jumps.  An NPC that walks turns
    /// with its walk; an order that takes its feet (a walk, a halt) ends the turn.  False for an NPC the world does not hold.
    bool turn_toward(uint64_t actor_id, double x, double y, double rate = 0);
    /// Where a target is now; false when it is gone.
    using Where = std::function<bool(nav::Vec3&)>;
    /// The NPC keeps turning towards the place `where` names, at `rate` degrees a second (<= 0: its rotation rate), until the world clock reaches `until` (<= 0: until `stop_tracking`): while it stands,
    /// and while a montage carries it, when the rest of the montage's travel goes the way it faces then.
    bool track_target(uint64_t actor_id, Where where, double rate, double until);
    void stop_tracking(uint64_t actor_id);
    /// Orders a walk after a target that moves: at every step the walk is re-planned to the target's place when it has moved off the walk's end (or the NPC has stopped short of it), so the NPC
    /// never runs to where the target was and waits there.  It ends `stop_distance` cm short of the target (centres) and sets off again at once when the target steps away; any other order ends the pursuit.
    bool order_pursue(uint64_t actor_id, Where where, double speed_cm_s, double stop_distance);

    // ---- root motion: a montage that carries its creature -----------------------------------------------------------------
    /// How far a montage carries the creature that plays it: its root bone's place, `x` cm ahead and `y` cm to the right of where the creature faces, from where it stood when the
    /// montage began, `time` seconds into the montage at a play rate of 1.
    struct RootMotionStep {
        double time = 0, x = 0, y = 0;
    };
    /// A place the carried creature's centre may not come nearer to than `radius` (another body's capsule plus its own).  `body` is the other body's own radius and `target` says it is the one
    /// the montage's ability is aimed at (the freeze box below looks at it).
    struct RootMotionBlock {
        double x = 0, y = 0, radius = 0;
        double body = 0;
        bool   target = false;
    };
    struct RootMotionPlay {
        uint64_t                    montage = 0;     // the montage object's id (what RepRootMotion names)
        std::vector<RootMotionStep> steps;           // from time 0, in time order
        double                      length = 0;      // seconds the montage plays at a play rate of 1
        double                      rate = 1;        // montage seconds per second (its rate scale times the play rate)
        double                      blend_out = 0.25;   // montage seconds at its end in which it no longer carries the creature (the engine's default blend out)
        /// Asked at every step: appends the places to keep out of.
        std::function<void(std::vector<RootMotionBlock>&)> blockers;
        /// The ability record's RootMotionFreeze (StopOnContact): the lunge ends for good, for the rest of the montage, once a body is in the box ahead of the creature, which reaches
        /// `freeze_distance` past the creature's own radius times `freeze_scale` and is as wide as that, each plus the body's own radius; a lunge that begins with a body in it does not begin.
        /// Only the ability's target counts when `ignore_non_target`.
        bool   freeze_stop = false;
        /// ... and PauseOnContact: the creature is held where it is for as long as a body is in that box (the step that would take it into the box takes it as far as the box's edge, and it is held
        /// there), and goes on by what the montage travels from then on once the box is clear.  `bRootMotionObstructed` is written 1 in every update of the pause (and of a stop), 0 otherwise.
        bool   freeze_pause = false;
        double freeze_distance = 0, freeze_scale = 1, own_radius = 0;
        bool   ignore_non_target = true;
    };
    /// Whether a body whose centre is at (bx, by) and whose own radius is `body` is in the freeze box of a creature whose centre is at (cx, cy), facing `yaw_degrees`: the box reaches `distance` ahead
    /// of the creature's own radius `wide` (the radius times the record's scale), is `wide` to either side of its facing, and counts the body's own radius on every side.
    static bool in_freeze_box(double cx, double cy, double yaw_degrees, double distance, double wide, double bx, double by, double body);
    /// A montage begins on the NPC: from now it, not the NPC's own walking, moves it.  The montage's first state is written at once (the attack that names it goes out with it).  False
    /// for an NPC the world does not hold, one still waiting for its ground, or a play with no steps or no length.
    bool begin_root_motion(uint64_t actor_id, RootMotionPlay play);
    /// The montage is over (or the NPC died or was sent home): writes the creature's place with the montage marked inactive, and gives its feet back.  False when none was playing.
    bool end_root_motion(uint64_t actor_id);
    bool root_motion_active(uint64_t actor_id) const;
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

    /// A game master's move: stands the NPC at (x, y) at once, on the ground there, its feet taken from whatever it was doing (a walk, a turn, a montage; a chase it was ordered
    /// goes on from the new place), and its home (the centre its wander and its walk home are about) put at the new place.  The place it was made at (`spawn_point`) is not touched.
    /// `z` is the height the caller gives: the NPC's origin when `z_is_origin` (a game master's own z, believed), else a height of the ground under the point (the server's: the landscape's or a
    /// town's floor), which the walking map's floor within 4 m of it replaces when there is one.  The facing is kept.  A patrolling NPC becomes a wandering one (it stays about the new place).
    /// The NPC's change is a reliable one, so a view that holds it dormant or as a snapshot is sent it again.
    enum class Relocated : uint8_t { Ok, UnknownNpc, WaitingForGround, NotLoaded, NotANumber };
    struct RelocateResult {
        Relocated status = Relocated::Ok;
        nav::Vec3 ground;     // where its feet stand now
        double    origin_z = 0;
    };
    RelocateResult relocate(uint64_t actor_id, double x, double y, double z, bool z_is_origin);
    /// Where the NPC was made, as the world put it on the ground: false for an NPC the world does not hold.  `ai_home` is the home its NpcInfo had then (the data's point).
    bool         spawn_point(uint64_t actor_id, nav::Vec3* ground_home, nav::Vec3* ai_home) const;
    /// Puts the NPC's home back at the place it was made at; returns it (false for an unknown NPC).  The NPC is not moved.
    bool         restore_home(uint64_t actor_id, nav::Vec3* ground_home);
// C O A with Cyb

    Stats stats() const;
    const NpcWorldConfig& config() const { return cfg_; }
    double now() const { return now_; }

    /// How a group of NPCs (a town's people) stand on the ground, for the log and the tests.  The height the data gave an NPC is what it is judged against.
    struct StandReport {
        size_t people = 0;       // the NPCs asked about that this world holds
        size_t waiting = 0;      // ... whose chunk of the ground is not resident yet (their ground cannot be asked): counted in no other field
        size_t on_floor = 0;     // ... on a floor within kStandNearCm of the height the data gave them
        size_t moved = 0;        // ... on a floor farther than that (within the reach of the window)
        size_t no_floor = 0;     // ... with no floor within reach: they stand at the data's height
        size_t blocked = 0;      // ... of those, the ones with no walkable ground within kBlockedCm sideways and from kBlockedBelowCm under their feet to kBlockedAboveCm over: in a wall, a stall, under a floor or in the air
        size_t stuck = 0;        // ... of those with no floor, the ones whose data gives them a life (they would wander or patrol): they stand for want of ground
        double max_moved_cm = 0; // the farthest any of them moved to reach its floor
    };
    static constexpr double kStandNearCm = 30.0;
    static constexpr double kBlockedCm = 100.0, kBlockedBelowCm = 150.0, kBlockedAboveCm = 300.0;
    StandReport stand_report(const std::vector<uint64_t>& ids) const;

private:
    enum class Mode : uint8_t { Stand, Idle, Wander, Patrol, PatrolPause, Return, Ordered };
    struct RootMotion {
        RootMotionPlay play;
        double         start = 0;                 // the world clock when the montage began
        double         yaw = 0;                   // degrees: the facing it is carried in, kept to the end
        double         ox = 0, oy = 0;            // where the creature stood when the montage began (the montage's root travels from here)
        double         position = 0;              // montage seconds at the last step
        bool           frozen = false;            // the freeze box has stopped the lunge
        double         skip_x = 0, skip_y = 0;    // the part of the montage's travel (ahead, to the right) the creature did not take while a pause held it: the montage's root minus this is where it goes
        double         last_tx = 0, last_ty = 0;  // where the last step put the creature in the montage's frame (the root minus the skipped part): a turn of the facing carries the rest of the travel on from here
        std::optional<uint64_t> saved_mode;       // the creature's ReplicatedMovementMode before the montage took it (it walks while the montage carries it)
    };
    struct Npc {
        uint64_t     id = 0;
        NpcMotionSetup setup;
        nav::Vec3    home;
        nav::Vec3    spawn_home;              // where it was made (its home as grounded then): a game master's move changes `home`, never this
        nav::Vec3    spawn_ai_home;           // ... and the home its NpcInfo had then (the data's point)
        bool         relocated = false;       // a game master has moved it
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
        bool         grounded = false;        // the last grounding found a surface for it
        double       data_feet = 0;           // where the data put its feet at that grounding (the height the surface was searched around)
        double       ground_dz = 0;           // how far the surface it was stood on is from that (cm, over is positive)
        std::optional<RootMotion> rm;         // the montage that carries it now
        bool         turning = false;         // it turns on the spot to `turn_yaw` (degrees) at `turn_rate` degrees a second (0: its own rate)
        double       turn_yaw = 0, turn_rate = 0;
        Where        track;                   // it keeps turning to this place ...
        double       track_rate = 0, track_until = 0;   // ... at this rate, until this time
        Where        pursue;                  // it walks after this place (the AI's chase) ...
        double       pursue_speed = 0, pursue_stop = 0;   // ... at this speed, ending this far from it
        double       pursue_x = 0, pursue_y = 0;          // ... the place the walk ends at now ...
        double       pursue_plan_x = 0, pursue_plan_y = 0;   // ... and the place it was last planned to (path-finding)
        double       settle_began = 0;        // after a montage: when it let go (its place goes out again at every step after that one) ...
        double       settle_until = 0;        // ... until then (NpcWorldConfig::settle_seconds) ...
        int          settle_sent = 0;         // ... and how many times it has
    };

    double next_unit(Npc& n);
    void   step(double t);
    void   advance_npc(Npc& n, double t);
    void   decide(Npc& n, double t);
    void   start_wander(Npc& n);
    void   start_patrol_leg(Npc& n);
    void   go_idle(Npc& n);
    void   commit(Npc& n, double t, bool force);
    void   advance_root_motion(Npc& n, double t);
    /// One step of the NPC's turning on the spot or tracking its target, and of its pursuit: before it walks.
    void   steer(Npc& n, double t);
    void   pursue_step(Npc& n, double t);
    void   finish_root_motion(Npc& n, double t);
    /// A montage has let go of the creature: its place will go out again at the following steps.
    void   begin_settle(Npc& n, double t);
    /// One step of that: the creature's place, facing and no velocity are written again (or the settling is over).
    void   settle(Npc& n, double t);
    void   refresh_active(double t);
    /// Stands the NPC on the ground nearest the height it has and gives it its behaviour; true when a ground was found.
    bool   ground_npc(Npc& n, world::WorldActor& a);
    /// The ground of a city node's person (`ground_by_data`) whose feet the data puts at `hint`: the floor nearest the feet within the tight window at its place, or, where the map has no polygon at
    /// the exact place (within the agent's radius of a wall), the floor of the nearest polygon within data_ground_near_cm.
    bool   ground_by_data_at(double x, double y, double hint, double* gz) const;
    bool   awake_for_someone(const world::WorldActor& a) const;

    world::ActorRegistry&         registry_;
    const nav::AreaNav*           nav_;
    NpcWorldConfig                cfg_;
    std::unordered_map<uint64_t, Npc> npcs_;
    struct PlayerNote { double x, y, at; };
    std::map<uint64_t, PlayerNote> players_;
    std::vector<uint64_t>         active_;        // the NPCs that moved last step (sorted: a deterministic order)
    std::vector<uint64_t>         rooted_;        // the NPCs a montage is carrying
    std::vector<uint64_t>         settling_;      // the NPCs a montage let go of lately: their place goes out again at each step (NpcWorldConfig::settle_seconds)
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

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  npc/ai/npc_ai.h  (plan E1, stage 3, part 1)
//
//  The decision of one NPC in a fight: idle (stage 2's own wandering and patrolling) -> aggro -> chase -> attack -> leash -> reset.  Pure logic on a clock
//  the caller gives: it sees what the world tells it (where it is and where home is, who is near, whether it can see them) and says what to do (walk
//  there, stand, walk home, attack now); the CombatWorld does it through stage 2's movement service and the hit rules.
//
//    aggro   a hostile, living combatant inside the aggro radius (the instance's, else the asset's, else kDefaultAggroCm) that the NPC has a line of sight
//            to, or anyone that hurt it, adds to its threat table and starts the fight.  Proximity aggro never starts beyond the leash.
//    chase   it walks to its top target (threat.h picks it) until the target is within the attack's range, repathing as the target moves.
//    attack  it stands and attacks while the target stays in range (a little farther once started, so it does not flicker between the two).
//    leash   it gives up when it is farther than the leash radius from home, or when the table is empty (every attacker died or left).
//    reset   it walks home at a run, its threat is cleared, and when it is there stage 2's behaviour takes it back (the CombatWorld heals it to full).
//
//  [modelled: no capture shows an aggro by proximity, a leash or a reset (research/combat 6 and 9); the numbers are the data's where it has them (the
//  asset's AggroRadius with its override flag: 500 to 2,500 cm; LeashRadius is 0 for every NPC type of the area) and kDefault... where it does not.]
// ============================================================================
#pragma once

#include "combat/fighter.h"
#include "npc/ai/threat.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace aoc::worldd::npc::ai {

enum class State : uint8_t { Idle, Chase, Attack, Reset, Dead };
const char* state_name(State s);

struct Params {
    double aggro_radius_cm = 800.0;       // a Wolf's (the data's 800; 0 in the data means the Blueprint's, which is not in it)
    /// false: it never starts a fight by noticing someone (an NPC friendly to players, or one whose data switches its aggro off: a Timid Grem); it still fights what hurts it.
    bool   proximity_aggro = true;
    double leash_radius_cm = 3000.0;      // from home
    double sense_interval = 0.25;         // how often an idle NPC looks for a target
    double attack_hysteresis = 1.15;      // an attack goes on to this multiple of its range
    double repath_interval = 0.5;         // seconds between two orders to walk to a moving target
    double repath_distance_cm = 150.0;    // ... unless the target moved less than this
    double reset_arrive_cm = 150.0;       // within this of home it is home
    double proximity_threat = 1.0;        // what coming into the aggro radius adds
    double chase_speed_cm_s = 600.0;
    double run_home_speed_cm_s = 900.0;
};

/// Someone the NPC may fight (a player): where, whether it lives, and whether the NPC may attack it.
struct Candidate {
    uint64_t       id = 0;
    combat::Vec3   pos;
    double         radius_cm = 50.0;
    bool           alive = true;
    bool           hostile = true;
};
// C.Y.B : C O A

struct Sense {
    double                       now = 0;
    combat::Vec3                 self;
    double                       self_radius_cm = 50.0;
    combat::Vec3                 home;
    std::vector<Candidate>       known;              // everyone near enough to matter (the threat table's entries that are not here have left)
    /// The ground's line of sight between two points (Detour's raycast); null: always.
    std::function<bool(const combat::Vec3&, const combat::Vec3&)> line_of_sight;
    /// How far the next attack reaches from the surface of the NPC (cm).
    double                       attack_range_cm = 200.0;
};

/// What the NPC does next.
struct Orders {
    enum class Move : uint8_t { None, ChaseTo, Stand, Home, Release } move = Move::None;
    combat::Vec3 dest;
    double       stop_distance_cm = 0;
    double       speed_cm_s = 0;
    uint64_t     target = 0;          // who it fights now (0: nobody): its CombatInfo.ActiveTargets
    bool         attack = false;      // it is in range: start an attack if one is ready
    bool         in_combat = false;
    bool         began_fight = false; // the first order of a fight (the flags go up)
    bool         began_reset = false; // the leash broke or the table emptied
    bool         reset_done = false;  // it reached home
};

class NpcAi {
public:
    explicit NpcAi(Params params = {}) : params_(params) {}

    /// Someone hurt it (or healed its enemy): threat for the table; an idle NPC starts the fight from any distance within the leash of home.
    void notice(uint64_t id, double threat, double now);
    /// The NPC died: it stops thinking.
    void kill();
    /// It is alive again at home (a respawn).
    void revive();

    Orders think(const Sense& sense);

    State state() const { return state_; }
    uint64_t target() const { return target_; }
    const ThreatTable& threat() const { return threat_; }
    ThreatTable& threat() { return threat_; }
    const Params& params() const { return params_; }
    void set_params(const Params& p) { params_ = p; }
    bool in_combat() const { return state_ == State::Chase || state_ == State::Attack; }

private:
    bool within_leash(const combat::Vec3& self, const combat::Vec3& home) const;

    Params      params_;
    State       state_ = State::Idle;
    ThreatTable threat_;
    uint64_t    target_ = 0;
    double      next_sense_ = 0;
    double      last_order_at_ = -1e9;
    combat::Vec3 last_order_dest_;
    bool        pending_fight_ = false;
};

}  // namespace aoc::worldd::npc::ai

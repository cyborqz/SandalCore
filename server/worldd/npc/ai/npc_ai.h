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
//    aggro   a hostile, living combatant that the NPC has a line of sight to and that it notices, or anyone that hurt it, adds to its threat table and starts the fight.
//            A creature with a senses table row (Params::senses) notices someone only within its aggro radius, and then by hearing (all round, up to its hearing radius)
//            or by sight (inside the cone in front of it, up to its sight radius), each with its own vertical limit, and it looks for players once every
//            sense_interval.  A creature without a row notices anyone within a flat radius, in the plane, every quarter second, and never beyond its leash from home.
//    chase   it walks to its top target (threat.h picks it) until the target is within the attack's range, repathing as the target moves.
//    attack  it stands and attacks while the target stays in range (a little farther once started, so it does not flicker between the two).
//    leash   it gives up when it is farther than the leash radius from its anchor (the place the fight began, or its home), when its target is farther than the leash
//            radius from it, or when the table is empty (every attacker died or left).
//    reset   it walks home at a run, its threat is cleared, and when it is there stage 2's behaviour takes it back (the CombatWorld heals it to full).
//
//  [modelled: no capture shows an aggro by proximity, a leash or a reset (research/combat 6 and 9); the numbers are the data's where it has them (a creature's
//  intelligence settings from its Blueprint, tools/npcabilities; the asset's AggroRadius with its override flag: 500 to 2,500 cm; LeashRadius is 0 for every NPC type of
//  the area) and kDefault... where it does not.]
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

/// What a creature notices with (the intelligence component of its Blueprint): the older flat radius does not apply to it.
struct Senses {
    bool   enabled = false;               // false: the flat aggro radius (the planar distance plus the candidate's radius) decides
    double hearing_radius_cm = 0;         // all round; 0: it does not hear
    double hearing_vertical_cm = 0;       // how far above or below it can hear (0: no limit)
    double sight_radius_cm = 0;           // in front of it only; 0: it does not see
    double sight_vertical_cm = 0;         // how far above or below it can see (0: no limit)
    double sight_cone_deg = 45.0;         // the half angle either side of its facing
};

struct Params {
    double aggro_radius_cm = 800.0;       // a Wolf's (the data's 800; 0 in the data means the Blueprint's, which is not in it); with Senses: the most it can notice anyone at, after the level curve
    Senses senses;
    /// true: the leash is measured from the place the fight began (the creature's tree saves it), not from home.
    bool   anchor_at_fight_start = false;
    /// false: it never starts a fight by noticing someone (an NPC friendly to players, or one whose data switches its aggro off: a Timid Grem); it still fights what hurts it.
    bool   proximity_aggro = true;
    double leash_radius_cm = 3000.0;      // from home
    double sense_interval = 0.25;         // how often an idle NPC looks for a target
    double attack_hysteresis = 1.15;      // an attack goes on to this multiple of its range
    double chase_stop_fraction = 0.8;     // a chase ends when the target is within this share of the attack's range
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
    double                       self_yaw_degrees = 0;   // the way it faces (the same angle as atan2(dy, dx) in degrees)
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
    const char*  reset_reason = "";   // why (for the log), set with began_reset
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
    /// A game master's reset: the threat table is cleared and the NPC goes home as after a broken leash (its walk home and its healing are the caller's).  No effect on a dead NPC.
    void reset_now();

    Orders think(const Sense& sense);

    State state() const { return state_; }
    uint64_t target() const { return target_; }
    const ThreatTable& threat() const { return threat_; }
    ThreatTable& threat() { return threat_; }
    const Params& params() const { return params_; }
    /// Where its leash is measured from: the place its fight began, or `home`.
    combat::Vec3 leash_anchor(const combat::Vec3& home) const { return params_.anchor_at_fight_start ? anchor_ : home; }
    void set_params(const Params& p) { params_ = p; }
    bool in_combat() const { return state_ == State::Chase || state_ == State::Attack; }

private:
    bool within_leash(const combat::Vec3& self, const combat::Vec3& home) const;
    /// Whether the creature notices the candidate: its distance from the creature in `distance` (the plane, or all three directions with Senses).
    bool notices(const Sense& s, const Candidate& c, double& distance) const;

    Params      params_;
    State       state_ = State::Idle;
    ThreatTable threat_;
    uint64_t    target_ = 0;
    double      next_sense_ = 0;
    double      last_order_at_ = -1e9;
    combat::Vec3 last_order_dest_;
    combat::Vec3 anchor_;               // where the fight began
    bool        pending_fight_ = false;
};

}  // namespace aoc::worldd::npc::ai

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_world.h  (plan E1, stage 3, part 1)
//
//  The fights of the area: the NPCs' stats and combat state, their AI (npc/ai), their attacks, what a player's hits do to them and where each hit is
//  confirmed.  One CombatWorld per worldd (PlayerWorld owns it); the sessions of the players join it as PlayerPorts.  Pure logic on the clock it is
//  ticked with: no sockets, no Connection.  It reads the registry (where the NPCs are), moves them through stage 2's NpcWorld (the AI's feet) and writes
//  their replicated combat state into the registry (combat/npc_replicator.h); a player's side of a hit goes through the PlayerPort.
//
//  An NPC becomes a combatant (`NpcCombatant`: its stat engine run for its kind and level, its alignments, its AI, its kit) the first time it matters:
//  when a player targets or hits it, or when a player comes within the scan radius of an NPC that is hostile to players.  The thousands of others stay
//  plain registry actors.
//
//  The seams of part 2.  `set_death_hook`: an NPC whose Health reaches 0 calls it once (who killed it, where, what it was), its AI stops, its Health item
//  and bIsDead are written in the same step, the movement stops and the NPC stays in the registry as a corpse; part 2 rolls the loot, gives the XP, closes
//  the channel after the corpse's life and respawns it.  `ai_state(id)` exposes the AI.  A player at 0 Health is dead to the AI (it drops its threat);
//  what a player's death is, is part 2's.
// ============================================================================
#pragma once

#include "combat/combat_data.h"
#include "combat/fighter.h"
#include "combat/hit_resolver.h"
#include "combat/npc_ability_data.h"
#include "combat/npc_stats.h"
#include "nav/area_nav.h"
#include "npc/ai/npc_ai.h"
#include "npc/motion_table.h"
#include "npc/npc_world.h"
#include "session/ability_catalog.h"
#include "world/actor_registry.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::combat {

/// Whether an NPC may start a fight by noticing a player (retest 3): it is hostile to players, and its data does not switch its aggro off (the instance overrides the radius with 0: a Timid
/// Grem).  A creature friendly to players (a huntable) never attacks first; either one still fights back what hurts it.
inline bool notices_players(bool hostile_to_players, const world::NpcInfo& npc) { return hostile_to_players && !npc.aggro_off; }

/// What an NPC attack did to a player: the hit as the confirmation and the player's Health need it.
struct IncomingHit {
    uint64_t                 instigator = 0;      // the NPC's actor id (its ObjectId)
    int64_t                  hit_guid = 0;
    int64_t                  source_type = 0;     // content::kSourceType...: the ability or projectile that made the hit
    int64_t                  source_guid = 0;
    uint8_t                  flags = 0;           // Miss, Block, Crit (Lethal is the player's session's: its Health decides)
    std::vector<ResolvedMod> mods;
    double                   at = 0;
};
// built with C-Y-B and CoA

/// What a kill gives a player (plan E1, stage 3, part 2: combat/aftermath.h): the experience of the creature's worth after the decay of the player's level.
struct KillReward {
    uint64_t    npc = 0;
    std::string name;             // the creature's nameplate
    std::string blueprint;        // its Blueprint's leaf ("Wolf")
    int         level = 0;
    int64_t     adventuring_xp = 0;
    int64_t     weapon_xp = 0;    // every equipped weapon line gets this (the data's Weapon experience; not modelled by the session yet)
    int64_t     node_xp = 0;      // the node's share (not modelled)
    bool        loot = false;     // there is a corpse the player may loot
};

/// What the world asks of a player's session.
class PlayerPort {
public:
    virtual ~PlayerPort() = default;
    /// The pawn's ObjectId: a player's actor id (ActiveTargets and the confirmations name it).
    virtual uint64_t actor_id() const = 0;
    virtual Fighter& fighter() = 0;
    /// An NPC's hit on the player: the session applies it (shields first, the confirmation, the replicated arrays).
    virtual void receive_hit(const IncomingHit& hit, double now) = 0;
    /// The player enters or leaves combat (the first exchange, 6 s after the last): the session sets its stats' flag (the out-of-combat regeneration
    /// ends and returns) and its CombatInfo.
    virtual void set_in_combat(bool in_combat, double now) = 0;
    /// The player's CharacterGuid (its four words): the guid the loot's interaction is offered to.
    virtual std::array<uint32_t, 4> character_guid() const { return {}; }
    /// The player killed a creature or helped to: the experience it is worth (the session applies it to its progress).
    virtual void receive_kill(const KillReward& /*kill*/, double /*now*/) {}
};

/// What a player asks about a target before it acts on it.
struct TargetInfo {
    bool     exists = false;
    bool     alive = false;
    Relation relation = Relation::Neutral;
    bool     in_line_of_sight = true;
    double   surface_distance_cm = 0;   // from the surfaces of the two bodies
    Vec3     position;
    double   radius_cm = 50.0;
    bool     is_player = false;
};

/// What applying a player's resolved hit to an NPC did.
struct Delivery {
    bool                applied = false;     // the target was a living combatant
    uint8_t             flags = 0;           // the resolved flags plus Lethal when the hit killed it
    std::vector<double> effective;           // per resolved mod: the part that took effect (a loss is negative; 0 when the target had none to lose)
    bool                lethal = false;
    uint64_t            target_object = 0;   // what the confirmation names as HitComp
};

struct DeathEvent {
    uint64_t               npc = 0;
    uint64_t               killer = 0;        // the player whose hit killed it (0: none)
    double                 at = 0;
    int                    level = 0;
    Vec3                   position;
    std::string            blueprint;         // the Blueprint's leaf without "_C": "Wolf"
    const world::NpcInfo*  info = nullptr;    // valid during the call
    const npc::MotionStats* motion = nullptr; // the Blueprint's row (its reward tables ...), null when the table has none
    std::set<uint64_t>     attackers;         // everyone with threat on it: who may loot and gets XP
    bool                   gm = false;        // a game master killed it (CombatWorld::gm_kill): nobody fought it
};

struct CombatWorldConfig {
    bool   enabled = true;                  // the kill switch (worldd --no-combat): no AI, no hits taken, no stats
    double think_seconds = 0.2;             // the AI's rate
    double scan_radius_cm = 3500.0;         // NPCs this near a player are looked at
    double default_aggro_cm = 800.0;
    double default_leash_cm = 3000.0;
    double default_attack_range_cm = 220.0; // a creature with no kit ability: the generic slash
    double default_attack_interval = 2.5;   // seconds between two of its attacks when the captures give none
    double default_windup = 0.5;
    double attack_variance = 0.25;          // +- this share of the interval
    double player_reach_cm = 4500.0;        // a player's ability reaches this far past the surface of its target (OutOfRangeDistance + the activation leeway)
    double melee_reach_cm = 1500.0;         // ... a weapon combo's hit (ThirdPartyHitDistance)
    uint64_t seed = 1;
    /// Multiplies the Health an NPC's hit takes from a player (1: the data's own; worldd's --npc-damage-mult sets it, 1 by default).  Misses, heals and every other stat are untouched.
    double npc_damage_mult = 1.0;
    /// The ability a creature with no kit attacks with: a plain basic attack that does 1.0 x the shared damage equation, so its damage follows the creature's level and stats, the player's
    /// mitigation and the level difference.  (The data's NPC_Bandit_Melee_Slash_ABL_1 takes a flat 50 Health whatever the creature, the player and their levels are.)
    std::string fallback_ability = "Ramkin_Hunter_SpearStrike";
};

class CombatWorld {
public:
    using DeathHook = std::function<void(const DeathEvent&)>;

    /// Everything outlives the world.  `npcs` and `nav` may be null (no movement: NPCs fight where they stand, and a null nav sees everything).
    CombatWorld(world::ActorRegistry& registry, npc::NpcWorld* npcs, const nav::AreaNav* nav, const CombatData& data, const abilities::Catalog& catalog,
                const NpcStatBook& book, const npc::MotionTable& motion, CombatWorldConfig config = {});

    ~CombatWorld();
    CombatWorld(const CombatWorld&) = delete;
    CombatWorld& operator=(const CombatWorld&) = delete;

    const CombatWorldConfig& config() const { return cfg_; }
    const CombatData& data() const { return data_; }
    HitResolver& resolver() { return resolver_; }

    void set_death_hook(DeathHook hook) { death_hook_ = std::move(hook); }

    /// The creatures that attack with their own animation (combat/npc_ability_data.h): a creature whose Blueprint the tables list starts each attack with the ability's montage, lands
    /// each hit when the montage's notify says and starts its next attack when the montage is over; every other creature swings as it always did.  Null (the default) for none.
    /// The tables outlive the world.
    void set_ability_data(const NpcAbilityData* data) { anim_ = data; }
    /// How many of the creature attacks the tables list and the world can animate land by each kind of rule (see combat/bite_area.h), and how many keep the single swing because a volume of theirs
    /// cannot be placed.  An attack none of whose hits is a hit of its montage (a thrown rock, a bolt) has no area to decide by: `unplayed` counts those.  One attack counts once, whichever
    /// Blueprints use it.
    struct GateCensus {
        size_t any = 0, validity = 0, volumes = 0, kept_swing = 0, unplayed = 0;
        std::vector<std::string> any_names, validity_names, volume_names, kept_names, unplayed_names;   // the attacks' names, by kind
        std::string text() const;
    };
    GateCensus gate_census() const;

    /// Gathering stage 3, "Hunting" (retest 3): which creatures are huntable.  A huntable is no combatant (it fights nothing and its CombatInfo is the living huntable's, not a fighter's) but its
    /// stats are filled as a combatant's are, the first time a player is near: retail's living huntable opens with the full list (914 packet 1561 and a hostile Raven's both carry 1563 bits of
    /// them), so the hunt's done update can write Health 0 into one item, as the five captured hunts do (a 278-bit stats block beside the CombatInfo's and the interactable's).
    using HuntablePredicate = std::function<bool(const world::NpcInfo&)>;
    void set_huntable(HuntablePredicate fn) { huntable_ = std::move(fn); }

    // ---- players
    void add_player(PlayerPort* port);
    void remove_player(PlayerPort* port);
    /// The port of the player whose pawn is `actor_id` (null: none joined).
    PlayerPort* find_player(uint64_t actor_id) { return player(actor_id); }
    const std::vector<PlayerPort*>& players() const { return players_; }

    // ---- NPCs
    /// The NPC's fighter (its combatant made when it is not yet); null for an actor that is no NPC of a kind that has stats.
    Fighter* fighter_of(uint64_t actor_id);
    /// The registry actor an object id names: the actor itself, or a component the registry indexes (its CollisionCylinder, minted in the actor's own
    /// block: NpcInfo::collision_cylinder; its InteractableComponent). 0 for an id that names nothing.
    uint64_t actor_of_object(uint64_t object_id) const;
    /// What `from` (a player's fighter) would be acting on in `target`: for the validations of a targeted cast and a hit.
    TargetInfo inspect(uint64_t target_actor, const Fighter& from);
    /// The ObjectId of the NPC target's collision cylinder: what the ability items name as Targets and the confirmations export as the actor's
    /// "CollisionCylinder" (NpcInfo::collision_cylinder, the actor's + 4 as retail mints it); 0 for an actor that has none.
    uint64_t hit_component_of(uint64_t actor_id) const;

    /// Applies a player's resolved hit to an NPC: Health (and Mana) move, the threat, the combat flags, the death. Returns what happened.
    Delivery apply_hit_to_npc(uint64_t npc_actor, uint64_t attacker, const Resolved& resolved, double now);

    /// Advances the world to `now`: the AI of every combatant near a player, the attacks, the flags, the NPCs' regeneration.
    void tick(double now);

    // ---- state, for the tests and the log
    bool is_combatant(uint64_t actor_id) const { return npcs_.count(actor_id) != 0; }
    std::optional<npc::ai::State> ai_state(uint64_t actor_id) const;
    const npc::ai::NpcAi* ai_of(uint64_t actor_id) const;
    double health_of(uint64_t actor_id) const;
    double max_health_of(uint64_t actor_id) const;
    bool   dead(uint64_t actor_id) const;
    /// What the world stream (world/map_stream.h) shows of an NPC that has been in a fight: its health as a percentage of its maximum, whether the clients are told it is in combat, and
    /// whether it is dead.  `summaries` appends one row per combatant, in no particular order; an NPC that has never been in a fight has no row (it is alive, at full health and out of combat).
    struct Summary {
        uint64_t id = 0;
        uint8_t  health_pct = 100;
        bool     in_combat = false;
        bool     dead = false;
    };
    void summaries(std::vector<Summary>& out) const;
    size_t combatants() const { return npcs_.size(); }
    struct Stats {
        uint64_t aggros = 0, resets = 0, kills = 0, npc_attacks = 0, npc_hits = 0, npc_misses = 0, player_hits = 0, refused_no_attack = 0;
    };
    const Stats& stats() const { return stats_; }

    /// The attack an NPC would start next (its ability's name), for the tests.
    std::string next_attack_name(uint64_t actor_id) const;
    /// Gives an NPC combatant a hand: threat from outside (a test, a pet).
    void add_threat(uint64_t npc_actor, uint64_t attacker, double amount, double now);
// C O A - Cyb

    /// A shield on an NPC (an effect's TempHealth amount): damage takes from the oldest first; it ends at `now + seconds` (0: when used up).
    void add_shield(uint64_t npc_actor, double amount, double seconds, double now);
    /// What the NPC's shields hold now.
    double shield_of(uint64_t npc_actor, double now) const;

    /// Forgets a dead NPC's combatant (part 2: the corpse closed or the NPC respawned).
    void forget(uint64_t actor_id);

    // ---- a game master's hands on an NPC (the admin API's NPC actions; the network thread)
    /// Kills the NPC as a lethal hit would (its death, its flags and the death hook, which gets `gm` set: nobody fought it).  False, with `why`, for an NPC that has no stats to fight with
    /// or that is dead already.
    bool gm_kill(uint64_t actor_id, double now, std::string& why);
    /// Puts the NPC's health at its maximum.  False for a dead NPC; an NPC that has never been in a fight is at full health already (true).
    bool gm_heal(uint64_t actor_id);
    /// How a reset ended: it runs home on foot, it stands (no way home on foot, or it never fought: the caller puts it at home), or it is dead.
    enum class GmReset : uint8_t { Walking, Standing, Dead };
    /// The NPC's fight is over as after a broken leash: its threat table is cleared, it is healed and its flags go down, and it is sent home at a run (the NPC world's home: the caller has set
    /// it).  A combatant that cannot walk home is left idle and healed (Standing).
    GmReset gm_reset(uint64_t actor_id, double now);

private:
    struct Attack {
        const abilities::AbilityDef* ability = nullptr;
        int    windup_ms = 500;
        int    range_cm = 220;
        const CreatureAbility* task = nullptr;   // the Blueprint's task that picked it (a creature with its own animation)
    };
    struct Pending {
        double   due = 0;
        uint64_t npc = 0, target = 0;
        const abilities::AbilityDef* ability = nullptr;
        std::vector<int64_t> hits;
        // an attack with its own animation: every hit at the time of its notify, then the end of the montage
        bool     animated = false;
        std::vector<std::pair<double, int64_t>> timed_hits;   // (when, hit record), in time order
        size_t   next_hit = 0;
        double   end_at = 0;
        bool     logged = false;    // the log has the line of its beginning, so it gets the line of its end
        std::vector<BiteArea> timed_areas;   // where each of those hits lands (the targeting record of the hit's key): a hit that reaches nobody sends nothing
    };
    /// Why the tree allowed none of a creature's attacks from where it stands: its target is farther than the attacks reach, or the right distance but not within the angle of its facing.
    struct TreeWhy {
        bool too_far = false, off_angle = false;
    };
    /// How an attack with its own animation plays: the montage, how long it runs and the hits it lands (seconds after it starts, hit record).
    struct PlannedHit {
        double  delay = 0;   // seconds after the attack begins
        int64_t hit = 0;     // the hit record
        int     key = 0;     // the key of the ability's hit, which finds its targeting record
    };
    struct CastPlan {
        const MontageData* montage = nullptr;
        double             seconds = 0;
        std::vector<PlannedHit> hits;
    };
    class Npc;
    Npc*  combatant(uint64_t actor_id);
    Npc*  make_combatant(const world::WorldActor& a);
    Attack choose_attack(Npc& n);
    Attack choose_animated(Npc& n, const Fighter* target, TreeWhy* why = nullptr);
    /// Whether a hit of `area` lands on `body` now (the creature as it stands and faces): see combat/bite_area.h.  `is_target`: the body is the one the record's target part finds.
    bool  hit_reaches(const Npc& n, const Fighter& body, const BiteArea& area, bool is_target) const;
    /// The player the target part of `area` finds for a hit of an attack aimed at `cast_target`: the creature's own target and the one the cast was aimed at, in the order the record says (0: none).
    uint64_t aimed_at(const Npc& n, const BiteArea& area, uint64_t cast_target);
    /// Whether the creature has a line of sight between the two places (the walking map's: off the map the ground says nothing and it sees).
    bool  line_clear(const Vec3& from, const Vec3& to) const;
    /// Whether every hit of the ability that hurts an enemy has a targeting record the world can decide by; an ability with one it cannot decide by is not animated.
    bool  areas_usable(const abilities::AbilityDef& def) const;
    /// Whether the ability's montages are read, with their hit notifies and a root motion the tables could read (what any creature needs to play it).
    bool  montages_readable(const abilities::AbilityDef& def) const;
    void  think(Npc& n, double now);
    /// Sends the creature after its target (the NPC world re-plans the walk to the target's place at every step, so it never runs to where the target was and waits there).
    void  pursue(Npc& n, uint64_t target, double speed_cm_s, double stop_distance_cm);
    /// Whether the creature faces `at` to within `tolerance_degrees`.
    bool  faces(const Npc& n, const Vec3& at, double tolerance_degrees) const;
    /// `picked` is the attack the tree allowed a moment ago (its conditions, a roll among them, are not asked twice); null: the tree is asked here.
    void  start_attack(Npc& n, uint64_t target, double now, const Attack* picked = nullptr);
    /// Ends the animation of an attack under way: the montage that carries the creature stops carrying it, and the cast's element says the montage is over.
    void  stop_cast(Npc& n);
    void  resolve_pending(double now);
    bool  animates(const Npc& n, const abilities::AbilityDef& def) const;
    /// Whether any task of the creature's tree that a fight can use is an attack it can play (its ability is in the data, its open lists it, its montage is read): a creature with none keeps the
    /// single swing every creature had before the trees.
    bool  can_animate(const Npc& n) const;
    bool  plan_cast(Npc& n, const abilities::AbilityDef& def, CastPlan& out);
    void  deliver_hit(Npc& n, PlayerPort& port, const abilities::AbilityDef& ability, int64_t hit_guid, double now);
    void  kill(Npc& n, uint64_t killer, double now, bool gm = false);
    void  begin_reset(Npc& n, double now);
    void  set_flags(Npc& n, bool in_combat, uint64_t target_actor);
    void  fill_huntable_stats(const world::WorldActor& a);
    PlayerPort* player(uint64_t actor_id);
    void  player_exchange(uint64_t player_id, double now);

    world::ActorRegistry&  registry_;
    npc::NpcWorld*         npcs_world_;
    const nav::AreaNav*    nav_;
    const CombatData&      data_;
    const abilities::Catalog& catalog_;
    const NpcStatBook&     book_;
    const npc::MotionTable& motion_;
    CombatWorldConfig      cfg_;
    HitResolver            resolver_;
    DeathHook              death_hook_;
    std::unordered_map<uint64_t, std::unique_ptr<Npc>> npcs_;
    std::vector<PlayerPort*> players_;
    std::map<uint64_t, double> player_combat_until_;   // actor id -> when its combat flag ends
    std::vector<Pending>   pending_;
    const NpcAbilityData*  anim_ = nullptr;
    std::unordered_map<std::string, bool> hostile_by_archetype_;
    HuntablePredicate      huntable_;
    std::set<uint64_t>     huntable_stats_;   // the huntables whose stats were filled
    double                 next_think_ = 0;
    double                 now_ = 0;
    Stats                  stats_;
    stats::SplitMix        rng_;
    size_t                 npc_hits_logged_ = 0;                 // the NPC hits on players written to the log so far (the first kLoggedNpcHits)
    static constexpr size_t kLoggedNpcHits = 400;
    size_t                 npc_casts_logged_ = 0;                // the attacks with their own animation written to the log so far (the first kLoggedNpcCasts)
    static constexpr size_t kLoggedNpcCasts = 200;
};

}  // namespace aoc::worldd::combat

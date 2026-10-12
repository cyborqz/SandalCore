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
    /// The ability a creature with no kit attacks with.
    std::string fallback_ability = "NPC_Bandit_Melee_Slash_ABL_1";
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

private:
    struct Attack {
        const abilities::AbilityDef* ability = nullptr;
        int    windup_ms = 500;
        int    range_cm = 220;
    };
    struct Pending {
        double   due = 0;
        uint64_t npc = 0, target = 0;
        const abilities::AbilityDef* ability = nullptr;
        std::vector<int64_t> hits;
    };
    class Npc;
    Npc*  combatant(uint64_t actor_id);
    Npc*  make_combatant(const world::WorldActor& a);
    Attack choose_attack(Npc& n);
    void  think(Npc& n, double now);
    void  start_attack(Npc& n, uint64_t target, double now);
    void  resolve_pending(double now);
    void  kill(Npc& n, uint64_t killer, double now);
    void  begin_reset(Npc& n, double now);
    void  set_flags(Npc& n, bool in_combat, uint64_t target_actor);
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
    std::unordered_map<std::string, bool> hostile_by_archetype_;
    double                 next_think_ = 0;
    double                 now_ = 0;
    Stats                  stats_;
    stats::SplitMix        rng_;
};

}  // namespace aoc::worldd::combat

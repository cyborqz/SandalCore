// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/aftermath.h  (plan E1, stage 3, part 2)
//
//  What follows an NPC's death, in the order the retail captures show it (research/combat 1 to 4):
//
//    the death frame    CombatWorld::kill wrote Health 0, bIsDead and the cleared combat state in one step and called this module's hook
//    experience         each player that fought it gets what its Blueprint's reward tables say (combat/rewards.h) after the decay of the level difference:
//                       the player's session applies it (PlayerPort::receive_kill)
//    loot               the tables' items are rolled at the death; when there is any, the corpse gets its loot components (world/corpse.h) and the players
//                       that fought it may take it through the loot window; a kill with nothing to give gets no components (retail: the first Lancer of
//                       capture 552)
//    the corpse         stays 15.0 s after the death (retail: 7 of 7 with nothing to loot), or 4.0 s after its loot was emptied (11 of 11); a corpse with
//                       loot nobody takes lasts kLootCorpseSeconds [modelled: no capture shows one]; its channel then closes (Destroyed) with the actor's
//                       removal from the registry
//    the respawn        the spawn point makes the NPC again respawn_time (the instance's, with its variance) after the death, as a new actor with a new id,
//                       while the corpse may still stand (retail: 178.7 to 181.0 s for 180, 1.07 and 1.47 s for 1.0 +- 0.2); the rule NoRespawn never
//                       does, AllDead and Refill are the same here [modelled: no capture shows AllDead]
//
//  A huntable creature (Gathering stage 3, "Hunting": set_huntable says which) is hunted ALIVE, standing, with a Bow (session/player_gather.cpp; the five captured hunts start at the client's
//  interact with no kill before it): hunt_started makes its record (`living`) for the channel and a few seconds more, hunt_cancelled takes the record away (the creature is as it was),
//  hunt_done is the creature's end (the done update killed it for its clients): its record is a hunted one, it goes hunt_done_seconds later [modelled like an emptied loot's 4.0 s], and
//  its spawn point refills it as after a kill.  A friendly huntable cannot be attacked (the damage abilities reach the hostile only), but one that dies anyway (a GM's kill, the tests')
//  dies as any other, gives its kill experience, and leaves NO loot: its corpse is hunted the same way and stays hunt_corpse_seconds unharvested [modelled].  The Glint certificate and
//  Bezoar rolls of its Blueprint's Hunting_Base table are not dropped.
//
//  Pure logic on the clock it is ticked with: nothing here sends anything.  The session of a player that may loot reads the corpse (`corpse_of`), takes
//  stacks (`take`) and says when nothing is left (`emptied`).
// ============================================================================
#pragma once

#include "combat/combat_world.h"
#include "combat/rewards.h"
#include "world/actor_registry.h"
#include "world/corpse.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::combat {

struct AftermathConfig {
    bool   enabled = true;                  // the kill switch (worldd --no-loot): no experience, loot or respawn
    bool   respawn = true;
    double corpse_seconds = 15.0;           // [R] 15.0 s after the death (7 of 7 corpses with nothing to loot)
    double emptied_seconds = 4.0;           // [R] 3.97 to 4.08 s after the loot was emptied (11 of 11)
    double loot_corpse_seconds = 120.0;     // [modelled] a corpse with loot nobody takes
    double hunt_corpse_seconds = 120.0;     // [modelled] a huntable's corpse nobody has hunted (the hunt takes 14 s; extended while one runs)
    double hunt_done_seconds = 4.0;         // [modelled like an emptied loot's, R: 11 of 11] a hunted corpse goes this long after the hunt
    bool   death_loss = true;               // the PvE death takes about half of each stack of materials into a pile (--no-death-loss: nothing)
    double pile_seconds = 600.0;            // [modelled] a dead player's pile (no capture shows one close unemptied)
    double default_respawn_seconds = 60.0;  // [modelled] a data NPC whose instance says 0
    double loot_chance_mult = 1.0;          // [modelled] x the chance of every RollEachByPercent loot draw (never the quantities): worldd --loot-chance-mult, AOC_LOOT_CHANCE_MULT
    double min_respawn_seconds = 1.0;       // [R] the shortest retail respawn: 1.07 s
    uint64_t seed = 1;
};

/// A stack of a corpse's loot.
struct LootStack {
    int64_t item = 0;
    int64_t variation = 0;
    int64_t quantity = 0;
    int     quality = 1;       // the item record's default quality (1, 21, 41 for Glint's tiers)
    bool    taken = false;     // a slot keeps its index when its stack is taken
};

struct Corpse {
    uint64_t                     npc = 0;
    std::string                  name;
    double                       died_at = 0;
    double                       close_at = 0;
    bool                         emptied = false;
    std::vector<LootStack>       loot;          // by slot
    std::set<uint64_t>           rights;        // the pawns of the players that may take from it
    std::optional<world::CorpseLoot> parts;     // its components, when it has loot
    int                          top_rarity = 0;
    bool                         is_pile = false;   // a dead player's pile (public loot: anyone may take from it)
    bool                         huntable = false;  // a huntable creature's corpse: no loot, hunted with a Bow
    bool                         hunted = false;    // ... and the hunt is done
    bool                         living = false;    // a huntable creature that is being hunted ALIVE (no death yet): the record holds the hunt's time; the hunt's end is its death
    size_t remaining() const {
        size_t n = 0;
        for (const LootStack& s : loot) n += s.taken ? 0 : 1;
        return n;
    }
};
// [C-O-A :: C-Y-B]

class Aftermath {
public:
    struct Stats {
        uint64_t piles = 0, deaths = 0, corpses_with_loot = 0, corpses_closed = 0, respawns = 0, loot_stacks = 0, loot_taken = 0, xp_awarded = 0;
    };

    /// `book` may be null (no reward tables: a corpse with nothing and no experience).  Everything outlives the module.
    Aftermath(world::ActorRegistry& registry, CombatWorld& combat, const RewardBook* book, world::Minter mint, AftermathConfig config = {});
    ~Aftermath();
    Aftermath(const Aftermath&) = delete;
    Aftermath& operator=(const Aftermath&) = delete;

    const AftermathConfig& config() const { return cfg_; }

    /// Makes an NPC of a spawn point again (the spawn_ref of its NpcInfo): its new actor id, 0 when it could not.  Without one nothing respawns.
    void set_respawner(std::function<uint64_t(uint32_t spawn_ref, double now)> fn) { respawner_ = std::move(fn); }
    /// The default quality of an item record (what a rolled stack's instance has); without one every stack is quality 1.
    void set_quality_of(std::function<int(int64_t item)> fn) { quality_of_ = std::move(fn); }
    /// The name of an item record, for the log's death line (without one the record's number is logged).
    void set_name_of(std::function<std::string(int64_t item)> fn) { name_of_ = std::move(fn); }
    /// Nodes 1, stage 1: the city nodes.  A death is rolled in the zone of influence of its position (world/city_nodes.h node_at): its biome and node
    /// answer the reward tables' GetNodeBiome and the other node reads.  Without them (null) the node reads answer as in a world with no nodes.  Outlives the module.
    void set_city_nodes(const world::CityNodes* nodes) { nodes_ = nodes; }

    /// Which creatures are huntable (their corpse is hunted, not looted): the predicate sees the NpcInfo of the creature that died.  Without one every creature is looted.
    using HuntablePredicate = std::function<bool(const world::NpcInfo&)>;
    void set_huntable(HuntablePredicate fn) { huntable_ = std::move(fn); }

    /// A hunt of `npc` starts at `now` and takes `seconds`.  A huntable creature is hunted as it stands, ALIVE (the user, 2026-10-12; captures 1456 1457 1458 1178 1179 start at the client's interact
    /// with no kill before it): the call makes its record (`living`), which keeps it in the world until a few seconds after the channel.  A huntable corpse (a creature that was killed some other
    /// way) stays until a few seconds after it, too.  False when `npc` is neither.
    bool hunt_started(uint64_t npc, double now, double seconds);
    /// The hunt ended without a reward (the player moved, the client interrupted, a disconnect): a living creature is just alive again, its record goes; a corpse keeps its own.
    void hunt_cancelled(uint64_t npc);
    /// The hunt is done: the creature is dead for its clients now (the done update), goes hunt_done_seconds after `now`, and its spawn point refills it as after a kill.  False when `npc` has no
    /// hunt record.
    bool hunt_done(uint64_t npc, double now);

    /// Takes the death hook of the combat world.
    void install();
    /// The hook itself (the tests call it with a DeathEvent).
    void on_death(const DeathEvent& d);

    /// Closes the corpses whose time has come and respawns the NPCs whose time has come.
    void tick(double now);

    // ---- the loot, for the sessions
    const Corpse* corpse_of(uint64_t npc) const;
    /// The corpse a client's interaction names (the InteractableComponent's object id), null when it names none.
    const Corpse* corpse_by_interactable(uint64_t object_id) const;
    /// Takes the stack in `slot` out of the corpse (null when there is none or it was taken); the corpse is not closed by it.
    const LootStack* take(uint64_t npc, size_t slot);
    /// Puts a taken stack back (the player's bag could not hold it).
    void put_back(uint64_t npc, size_t slot);
    /// The corpse holds nothing more: its loot components say so and it closes emptied_seconds later.
    void emptied(uint64_t npc, double now);

    /// A dead player's pile (research/combat 5): an actor where the player fell, holding `stacks`, public loot that stays pile_seconds and closes
    /// emptied_seconds after it is emptied.  The pile's actor id (0, with `error`, when it cannot be made).
    uint64_t drop_pile(double x, double y, double z, const protocol::repl::GuidValue& owner, const std::string& name, std::vector<LootStack> stacks, double now,
                       std::string& error);

    const Stats& stats() const { return stats_; }
    size_t corpses() const { return corpses_.size(); }
    size_t respawns_pending() const { return respawns_.size(); }

private:
    struct Respawn {
        double   at = 0;
        uint32_t spawn_ref = 0;
    };

    /// The spawn point of `info` refills the creature `at` plus its respawn time (a death's, and a hunted creature's end).
    void schedule_respawn(const world::NpcInfo& info, double at);

    world::ActorRegistry&      registry_;
    CombatWorld&               combat_;
    const RewardBook*          book_;
    world::Minter              mint_;
    AftermathConfig            cfg_;
    std::function<uint64_t(uint32_t, double)> respawner_;
    std::function<int(int64_t)> quality_of_;
    std::function<std::string(int64_t)> name_of_;
    HuntablePredicate          huntable_;
    const world::CityNodes*    nodes_ = nullptr;
    std::map<uint64_t, Corpse> corpses_;
    std::vector<Respawn>       respawns_;
    stats::SplitMix            rng_;
    Stats                      stats_;
};

}  // namespace aoc::worldd::combat

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/hit_port.h  (plan E1, stage 3, part 1)
//
//  What a player's ability runtime (session/ability_pipeline.h) asks of the world when a cast or a hit names an actor other than its caster: is the target
//  there, alive, an enemy, in range and in sight; its stats (the target's are the actor of GetTarget() in the hit's formulas); the hit rules; and where to put
//  what the hit did (Health, the combat flags, the death).  CombatWorld has the answers; WorldHitPort puts them behind this interface for one player so the
//  runtime knows nothing of the registry.
// ============================================================================
#pragma once

#include "combat/combat_world.h"
#include "protocol/emit/intrepid_netguid.h"

#include <cstdint>

namespace aoc::worldd::combat {

class HitPort {
public:
    virtual ~HitPort() = default;
    virtual TargetInfo inspect(uint64_t target_actor) = 0;
    virtual Fighter* fighter(uint64_t target_actor) = 0;
    virtual HitResolver& resolver() = 0;
    virtual const CombatData& data() const = 0;
    /// Applies a resolved hit to the target (see CombatWorld::apply_hit_to_npc).
    virtual Delivery deliver(uint64_t target_actor, const Resolved& hit, double now) = 0;
    /// A shield (a TempHealth effect's amount) on the target for `seconds` (0: until it is used up).
    virtual void shield(uint64_t target_actor, double amount, double seconds, double now) = 0;
    /// The target's NetGUID and its HitComp (the collision cylinder) for a confirmation.
    virtual protocol::emit::FIntrepidNetworkGUID guid_of(uint64_t actor) = 0;
    virtual protocol::emit::FIntrepidNetworkGUID hit_component_of(uint64_t actor) = 0;
    /// How far (cm) past its target's surface a cast of the ability reaches, and a combo hit.
    virtual double reach_cm(bool melee) const = 0;
    /// The player's own fighter and actor id.
    virtual const Fighter& self() const = 0;
};
// (C O A ~ C-Y-B)

/// A HitPort over a CombatWorld for one player.
///
/// Two clocks meet here [E1 stage 3 fix 2]: the player's ability runtime counts seconds from its own start (the session's join), the combat world and
/// the aftermath are ticked with the server's world time (seconds since worldd started, PlayerWorld::tick_world).  `clock_offset` is the world time at the
/// runtime's zero (the join's world time): every `now` the runtime passes is translated before it reaches the world, so a death's time, a shield's end and
/// the player's in-combat window are on the clock the world is ticked with.  Without it (the user's run of 2026-10-06, worldd 102 s old at the join) every
/// corpse closed and every spawn point refilled at the very next world tick: no death animation, an NPC "respawning in place".
class WorldHitPort final : public HitPort {
public:
    WorldHitPort(CombatWorld& world, world::ActorRegistry& registry, const Fighter& self, double clock_offset = 0.0)
        : world_(world), registry_(registry), self_(self), clock_offset_(clock_offset) {}
    TargetInfo inspect(uint64_t target_actor) override { return world_.inspect(target_actor, self_); }
    Fighter* fighter(uint64_t target_actor) override { return world_.fighter_of(target_actor); }
    HitResolver& resolver() override { return world_.resolver(); }
    const CombatData& data() const override { return world_.data(); }
    Delivery deliver(uint64_t target_actor, const Resolved& hit, double now) override {
        return world_.apply_hit_to_npc(target_actor, self_.id(), hit, world_time(now));
    }
    void shield(uint64_t target_actor, double amount, double seconds, double now) override { world_.add_shield(target_actor, amount, seconds, world_time(now)); }
    /// The world's clock for a time of the runtime's.
    double world_time(double runtime_now) const { return runtime_now + clock_offset_; }
    double clock_offset() const { return clock_offset_; }
    protocol::emit::FIntrepidNetworkGUID guid_of(uint64_t actor) override {
        const world::WorldActor* a = registry_.find(actor);
        return a ? a->actor.guid : protocol::emit::FIntrepidNetworkGUID{};
    }
    protocol::emit::FIntrepidNetworkGUID hit_component_of(uint64_t actor) override {
        protocol::emit::FIntrepidNetworkGUID g = guid_of(actor);
        g.ObjectId = world_.hit_component_of(actor);
        return g;
    }
    double reach_cm(bool melee) const override { return melee ? world_.config().melee_reach_cm : world_.config().player_reach_cm; }
    const Fighter& self() const override { return self_; }

private:
    CombatWorld&          world_;
    world::ActorRegistry& registry_;
    const Fighter&        self_;
    double                clock_offset_ = 0.0;
};

}  // namespace aoc::worldd::combat

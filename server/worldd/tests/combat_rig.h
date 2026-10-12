// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_rig.h  (plan E1, stage 3, part 1)
//
//  What the tests of the combat world share: a player (the ability rig's character) as a Fighter and a PlayerPort that applies what NPCs do to it,
//  a registry NPC made from a retail open of the Wolf (the real replicated state: stat arrays and CombatInfo), and the small navmesh world of
//  nav_fixture.h to stand them on.
// ============================================================================
#pragma once

#include "ability_rig.h"
#include "combat/combat_world.h"
#include "combat/npc_replicator.h"
#include "nav_fixture.h"
#include "npc/area_npcs.h"
#include "spawns/captured_spawns.h"

#include <memory>
#include <string>
#include <vector>

namespace combat_rig {

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace emit = aoc::protocol::emit;

/// The ability rig's character as a fighter.
class RigFighter final : public combat::Fighter {
public:
    RigFighter(ability_rig::Rig& rig, uint64_t id, combat::Vec3 pos) : rig_(rig), id_(id), pos_(pos), aligns_(combat::CombatData::player_alignments()) {}
    uint64_t id() const override { return id_; }
    bool is_player() const override { return true; }
    int level() const override { return rig_.cs->level(); }
    combat::Vec3 position() const override { return pos_; }
    double radius_cm() const override { return 34.0; }
    bool alive() const override {
        const stats::StatValue* h = rig_.cs->find(content::kGuidHealth);
        return h && h->value > 0;
    }
    const std::vector<int64_t>& alignments() const override { return aligns_; }
    stats::CharacterStats& stats() override { return *rig_.cs; }
    void move_to(const combat::Vec3& p) { pos_ = p; }

private:
    ability_rig::Rig&    rig_;
    uint64_t             id_;
    combat::Vec3         pos_;
    std::vector<int64_t> aligns_;
};

/// A player's session as the world sees it: it takes the NPCs' hits off the character's Health through the pipeline and keeps what it was told.
class FakePlayer final : public combat::PlayerPort {
public:
    FakePlayer(ability_rig::Rig& rig, uint64_t id, combat::Vec3 pos) : rig_(rig), fighter_(rig, id, pos) {}
    uint64_t actor_id() const override { return fighter_.id(); }
    combat::Fighter& fighter() override { return fighter_; }
    RigFighter& rig_fighter() { return fighter_; }
    void receive_hit(const combat::IncomingHit& hit, double now) override {
        hits.push_back(hit);
        for (const combat::ResolvedMod& m : hit.mods) {
            if (m.stat_guid == content::kGuidHealth && m.value < 0) rig_.pipe->take_damage(-m.value, rig_.clock = std::max(rig_.clock, now));
        }
        rig_.take();
    }
    void set_in_combat(bool in_combat, double now) override {
        combat_flips.emplace_back(now, in_combat);
        this->in_combat = in_combat;
    }
    std::array<uint32_t, 4> character_guid() const override { return guid; }
    void receive_kill(const combat::KillReward& kill, double now) override {
        kills.push_back(kill);
        (void)now;
    }
    std::array<uint32_t, 4>                  guid = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    std::vector<combat::KillReward>          kills;
    std::vector<combat::IncomingHit>         hits;
    std::vector<std::pair<double, bool>>     combat_flips;
    bool                                     in_combat = false;
// -- CoA & C Y B --

private:
    ability_rig::Rig& rig_;
    RigFighter        fighter_;
};

/// An NPC of the Blueprint made from a retail open (the Wolf's when no capture opened the Blueprint: the stat arrays and CombatInfo are the same shape), put
/// in `registry` at (x, y, origin_z) and given to `npcs` (standing).  0 when it cannot be made.
inline uint64_t spawn_npc(world::ActorRegistry& registry, npc::NpcWorld* npcs, const spawns::CapturedData& captured, uint64_t& next_id, const std::string& archetype_path, double x,
                          double y, double origin_z, int level, double aggro_cm = 0, double half_height = 88.0) {
    const spawns::OpenTemplate* t = captured.template_for(archetype_path);
    if (!t) {
        for (const spawns::OpenTemplate& o : captured.opens()) {
            if (o.category == "npc" && o.archetype_path.find("/Wolf.Default__Wolf_C") != std::string::npos) t = &o;
        }
    }
    if (!t) return 0;
    const spawns::Minter mint = [&]() {
        emit::FIntrepidNetworkGUID g;
        g.ObjectId = next_id;
        g.ServerId = 60;
        g.Randomizer = 0x6ee87ee4u;
        next_id += 2;
        return g;
    };
    spawns::InstanceParams p;
    p.x = x;
    p.y = y;
    p.z = origin_z;
    p.level = level;
    p.server_id = 60;
    p.seed = next_id;
    std::string err;
    auto w = spawns::instantiate(*t, mint, p, err);
    if (!w) return 0;
    w->npc.archetype = archetype_path;
    w->npc.level = level;
    w->npc.home_x = x;
    w->npc.home_y = y;
    w->npc.home_z = origin_z;
    w->npc.aggro_radius_cm = aggro_cm;
    const uint64_t id = w->id;
    if (!registry.add(std::move(*w))) return 0;
    if (npcs) {
        npc::NpcMotionSetup setup;
        setup.ambient = npc::Ambient::Stationary;
        setup.foot_offset = half_height;
        npcs->add(id, setup);
    }
    return id;
}

/// A registry of one world: the NPCs' ground, the Wolf's open and a minter.
struct World {
    world::ActorRegistry           registry;
    std::unique_ptr<npc::NpcWorld> npcs;
    std::optional<spawns::CapturedData> captured;
    uint64_t                       next_id = 4096;
    std::string                    problem;

    World() {
        std::string err;
        captured = spawns::CapturedData::load(err);
        if (!captured) problem = "the captured data: " + err;
        npcs = std::make_unique<npc::NpcWorld>(registry, &navfix::shared_nav());
    }

    /// An NPC of the Blueprint (archetype path) standing at (x, y) of the plain, at `level`; its aggro radius and home as the data would say.  0 when it
    /// cannot be made.
    uint64_t spawn(const std::string& archetype_path, double x, double y, int level, double aggro_cm = 0, double half_height = 88.0) {
        if (!captured) return 0;
        return spawn_npc(registry, npcs.get(), *captured, next_id, archetype_path, x, y, 1000.0 + half_height, level, aggro_cm, half_height);
    }
};

/// A bool member of an NPC's CombatInfo ("bInCombat", "bIsDead").
inline bool combat_flag(const world::WorldActor& a, const char* name) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UNPCCombatInfo") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, name);
        const bool* b = v ? v->get_if<bool>() : nullptr;
        return b && *b;
    }
    return false;
}

/// ActiveTargets' first object (0: none).
inline uint64_t active_target(const world::WorldActor& a) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UNPCCombatInfo") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, "ActiveTargets.ActiveTargets");
        const repl::ArrayValue* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
        if (!arr || arr->elements.empty()) return 0;
        const auto it = arr->elements[0].values.find(1);
        const auto* g = it == arr->elements[0].values.end() ? nullptr : it->second.get_if<emit::FIntrepidNetworkGUID>();
        return g ? g->ObjectId : 0;
    }
    return 0;
}

}  // namespace combat_rig

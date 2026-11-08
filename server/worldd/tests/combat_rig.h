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
#include "combat/knockback.h"
#include "combat/npc_replicator.h"
#include "nav_fixture.h"
#include "npc/area_npcs.h"
#include "spawns/captured_spawns.h"

#include <memory>
#include <spdlog/sinks/ostream_sink.h>   // (the tests of the crowd control read the log lines the world writes)
#include <spdlog/spdlog.h>
#include <sstream>
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
    double facing_degrees() const override { return facing_; }
    bool alive() const override {
        const stats::StatValue* h = rig_.cs->find(content::kGuidHealth);
        return h && h->value > 0;
    }
    const std::vector<int64_t>& alignments() const override { return aligns_; }
    stats::CharacterStats& stats() override { return *rig_.cs; }
    void move_to(const combat::Vec3& p) { pos_ = p; }
    /// The way the player faces (degrees, 0 along +X, turning towards +Y); a knock-back, a push and a pull work from it.
    void face(double degrees) { facing_ = degrees; }

private:
    ability_rig::Rig&    rig_;
    uint64_t             id_;
    combat::Vec3         pos_;
    std::vector<int64_t> aligns_;
    double               facing_ = 0.0;
};

/// The NetGUIDs the rig's player is known by, with the ServerId and Randomizer of a session: his pawn, and the pawn's collision cylinder (the pawn's ObjectId + 20, as in the captured session).
inline emit::FIntrepidNetworkGUID rig_pawn_guid(uint64_t id) {
    emit::FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = 60;
    g.Randomizer = 0x5eed1234u;
    return g;
}
inline emit::FIntrepidNetworkGUID rig_cylinder_guid(uint64_t id) {
    emit::FIntrepidNetworkGUID g = rig_pawn_guid(id);
    g.ObjectId = id + 20;
    return g;
}

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
    emit::FIntrepidNetworkGUID pawn_guid() const override { return rig_pawn_guid(fighter_.id()); }
    emit::FIntrepidNetworkGUID cylinder_guid() const override { return rig_cylinder_guid(fighter_.id()); }
    void receive_kill(const combat::KillReward& kill, double now) override {
        kills.push_back(kill);
        (void)now;
    }
    /// A hit of an effect the player put on a creature: queued, as the session queues it (it confirms it at its next tick, never inside the cast that made the hit).
    void confirm_effect_hit(const combat::EffectHitNote& note, double) override { effect_notes.push_back(note); }
    /// What the session does at its next tick: the queued notes become confirmations of the pipeline.
    void deliver_effect_notes() {
        std::vector<combat::EffectHitNote> notes;
        notes.swap(effect_notes);
        for (const combat::EffectHitNote& n : notes) {
            confirmed_notes.push_back(n);
            rig_.pipe->confirm_effect_hit(n, rig_.clock = std::max(rig_.clock, n.at));
        }
        rig_.take();
    }
    std::vector<combat::EffectHitNote>       effect_notes, confirmed_notes;
    void hit_dealt(const combat::DealtHit& hit, double) override { dealt.push_back(hit); }
    void hit_from_pet(int64_t hit_guid, int64_t, double) override { from_pet.push_back(hit_guid); }
    std::vector<combat::DealtHit>            dealt;
    std::vector<int64_t>                     from_pet;
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
                          double y, double origin_z, int level, double aggro_cm = 0, double half_height = 88.0, const spawns::OpenTemplate* made = nullptr, const npc::MotionStats* body = nullptr) {
    // `made`: the open the world itself makes for the Blueprint (spawns::GenericNpcs::template_for), when the test is about that one
    const spawns::OpenTemplate* t = made ? made : captured.template_for(archetype_path);
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
        if (body) {   // what a throw or a push works on: the Blueprint's own body constants where its table row has them (make_setup does the same)
            if (body->ground_friction >= 0) setup.ground_friction = body->ground_friction;
            if (body->braking_deceleration >= 0) setup.braking = body->braking_deceleration;
            if (body->mass > 0) setup.mass = body->mass;
        }
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
    uint64_t spawn(const std::string& archetype_path, double x, double y, int level, double aggro_cm = 0, double half_height = 88.0, const spawns::OpenTemplate* made = nullptr,
                   const npc::MotionStats* body = nullptr) {
        if (!captured) return 0;
        return spawn_npc(registry, npcs.get(), *captured, next_id, archetype_path, x, y, 1000.0 + half_height, level, aggro_cm, half_height, made, body);
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

/// ActiveTargets' first object in full: its ObjectId, ServerId and Randomizer (an empty NetGUID: none).
inline emit::FIntrepidNetworkGUID active_target_guid(const world::WorldActor& a) {
    for (const repl::Subobject& s : a.actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UNPCCombatInfo") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, "ActiveTargets.ActiveTargets");
        const repl::ArrayValue* arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
        if (!arr || arr->elements.empty()) return {};
        const auto it = arr->elements[0].values.find(1);
        const auto* g = it == arr->elements[0].values.end() ? nullptr : it->second.get_if<emit::FIntrepidNetworkGUID>();
        return g ? *g : emit::FIntrepidNetworkGUID{};
    }
    return {};
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

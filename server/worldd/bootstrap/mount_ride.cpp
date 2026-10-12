// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/mount_ride.cpp  (the mounts milestone, stage 3)
//
//  What PlayerBootstrap builds for riding: the PlayerController's and the pawn's side of getting on and off a mount, and the move acknowledgement and
//  correction of a possessed mount's channel.  The mount's own side (its Owner, its owner stat arrays ...) is the registry actor's (spawns/mount_spawn.h
//  `possess`), sent by the actor view.  The wire is retail's: captures 910 (getting on), 912 (getting off), 893 (the mounted desummon's two restarts).
// ============================================================================
#include "bootstrap/bootstrap.h"

#include "bootstrap/constants.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/schema/generated/index.h"

#include <bit>
#include <cmath>

namespace aoc::worldd::bootstrap {

using protocol::emit::BunchWriter;
using protocol::emit::FIntrepidNetworkGUID;
using repl::Emission;
using repl::Value;
namespace schema = ::aoc::protocol::schema;
namespace gen = ::aoc::protocol::schema::gen;

namespace {

Value object(const FIntrepidNetworkGUID& g) { return Value(g); }

uint16_t yaw16(double degrees) {
    const float v = static_cast<float>(degrees) * 65536.0f / 360.0f;
    return static_cast<uint16_t>(static_cast<int>(std::floor(v + 0.5f)) & 0xFFFF);
}
// -- C O A . C_Y_B --

}  // namespace

std::optional<Send> PlayerBootstrap::move_ack_on(const repl::Actor& actor, float time_stamp) {
    const schema::ClassSchema* cls = actor.obj.schema;
    if (!cls) return std::nullopt;
    const schema::NetField* f = cls->field("ClientMoveResponsePacked");
    if (!f) return std::nullopt;
    BunchWriter params;   // FCharacterMoveResponsePackedBits: 10 bits of header (a good move), then the move's TimeStamp
    params.write_bits(645, 10);
    params.write_bits(std::bit_cast<uint32_t>(time_stamp), 32);
    BunchWriter field;
    if (repl::write_field(*cls, f->index, params, field) == 0) return std::nullopt;
    Emission e;
    e.reliable = false;
    repl::BlockHeader header;
    if (repl::write_content_block(e.payload, header, field) == 0) return std::nullopt;
    Send s;
    s.role = Role::Pawn;
    s.what = "ClientMoveResponsePacked (mount)";
    s.emission = std::move(e);
    return s;
}

std::optional<Send> PlayerBootstrap::correction_on(const repl::Actor& actor, float time_stamp, const repl::Vec3d& location, const repl::Vec3d& velocity,
                                                   uint8_t movement_mode) const {
    if (!ok_ || !actor.obj.schema) return std::nullopt;
    const schema::RpcSchema* rpc = actor.obj.schema->rpc("ClientAdjustPosition");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "TimeStamp", Value::f32(time_stamp)) || !repl::set_rpc_param(*rpc, params, "NewLoc", Value(location)) ||
        !repl::set_rpc_param(*rpc, params, "NewVel", Value(velocity)) || !repl::set_rpc_param(*rpc, params, "ServerMovementMode", Value::u64(movement_mode))) {
        return std::nullopt;
    }
    return finish(Role::Pawn, false, "ClientAdjustPosition (mount)", repl::call_rpc(actor, nullptr, "ClientAdjustPosition", params, {}, acks_));
}

std::optional<Send> PlayerBootstrap::end_ability_on(const repl::Actor& actor, const FIntrepidNetworkGUID& ability_component, int32_t client_instance_id) const {
    if (!ok_) return std::nullopt;
    const repl::Subobject* ability = actor.find(ability_component);
    if (!ability) return std::nullopt;
    return finish(Role::Pawn, false, "ClientEndAbility (mount)", repl::client_end_ability(actor, *ability, client_instance_id, acks_));
}

std::optional<Send> PlayerBootstrap::interaction_animation_on(const repl::Actor& actor, const FIntrepidNetworkGUID& interactable, uint16_t input_index) const {
    if (!ok_) return std::nullopt;
    const repl::Subobject* comp = actor.find(interactable);
    if (!comp || !comp->obj.schema) return std::nullopt;
    const schema::RpcSchema* rpc = comp->obj.schema->rpc("ClientSideHandleInteractionAnimation");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "InteractingCharacter", object(setup_.guids.pawn)) || !repl::set_rpc_param(*rpc, params, "InputIndex", Value::u64(input_index))) return std::nullopt;
    return finish(Role::Pawn, false, "ClientSideHandleInteractionAnimation", repl::call_rpc(actor, comp, "ClientSideHandleInteractionAnimation", params, {}, acks_));
}

std::optional<Send> PlayerBootstrap::possess_mount(const FIntrepidNetworkGUID& mount, int64_t dash_ability, int32_t dash_slot) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::ClassSchema& cls = *pc_.obj.schema;
    const schema::RpcSchema* restart = cls.rpc("ClientRestart");
    const schema::RpcSchema* camera = cls.rpc("ClientSetCameraMode");
    if (!restart || !camera) return std::nullopt;
    std::vector<repl::RpcRequest> calls(2);
    calls[0].rpc = "ClientRestart";
    calls[1].rpc = "ClientSetCameraMode";
    if (!repl::set_rpc_param(*restart, calls[0].params, "NewPawn", object(mount)) ||
        !repl::set_rpc_param(*camera, calls[1].params, "NewCamMode", Value(repl::NameValue::hardcoded_name(kCameraModeEName)))) {
        return std::nullopt;
    }
    if (!mount_bar_calls(dash_ability, dash_slot, calls)) return std::nullopt;
    return rpcs(Role::PlayerController, "ClientRestart (the mount), camera mode and the mount bar", calls);
}

std::optional<Send> PlayerBootstrap::restart_own_pawn(int times) {
    if (!ok_ || !second_state_added_ || times < 1 || times > 4) return std::nullopt;
    const schema::ClassSchema& cls = *pc_.obj.schema;
    const schema::RpcSchema* restart = cls.rpc("ClientRestart");
    const schema::RpcSchema* camera = cls.rpc("ClientSetCameraMode");
    if (!restart || !camera) return std::nullopt;
    std::vector<repl::RpcRequest> calls(static_cast<size_t>(times) * 2);
    for (size_t i = 0; i < calls.size(); i += 2) {
        calls[i].rpc = "ClientRestart";
        calls[i + 1].rpc = "ClientSetCameraMode";
        if (!repl::set_rpc_param(*restart, calls[i].params, "NewPawn", object(setup_.guids.pawn)) ||
            !repl::set_rpc_param(*camera, calls[i + 1].params, "NewCamMode", Value(repl::NameValue::hardcoded_name(kCameraModeEName)))) {
            return std::nullopt;
        }
    }
    return rpcs(Role::PlayerController, times == 1 ? "ClientRestart (the own pawn) and camera mode" : "ClientRestart (the own pawn) and camera mode, again", calls);
}

std::optional<Send> PlayerBootstrap::controller_attach(const FIntrepidNetworkGUID& actor, const FIntrepidNetworkGUID& cylinder) {
    if (!ok_ || !second_state_sent_) return std::nullopt;
    auto set = [&](std::string_view path, Value v) { return pc_.obj.state.set(*pc_.obj.schema, path, std::move(v)); };
    if (!set("Pawn", object(actor)) || !set("AttachmentReplication.AttachParent", object(actor)) ||
        !set("AttachmentReplication.RotationOffset", Value(repl::Rot{0, 0, 0})) || !set("AttachmentReplication.AttachComponent", object(cylinder))) {
        return std::nullopt;
    }
    // AttachComponent names the actor's CollisionCylinder, a default component the client finds by name and outer: its export goes with the block
    std::vector<repl::ContentBlock> blocks;
    repl::ShadowUndo undo;
    if (!repl::collect_update_blocks(pc_, pc_shadow_, blocks, &undo) || blocks.empty() || blocks[0].sub) {
        undo.rollback(pc_shadow_);
        return std::nullopt;
    }
    blocks[0].exports.push_back(repl::component_export(actor, cylinder, "CollisionCylinder", kChecksumCollisionCylinder));
    auto e = repl::assemble_blocks(pc_, blocks, acks_);
    if (!e) undo.rollback(pc_shadow_);
    return finish(Role::PlayerController, false, "PlayerController attached to the possessed actor", std::move(e));
}

bool PlayerBootstrap::ensure_interact_info(const FIntrepidNetworkGUID& guid) {
    if (!ok_ || !guid.is_valid()) return false;
    for (const repl::Subobject& s : pawn_.subobjects) {
        if (s.name == "InteractInfo") return s.guid == guid;
    }
    repl::Subobject info;
    info.guid = guid;
    info.name = "InteractInfo";
    info.checksum = kChecksumInteractInfo;
    info.obj.schema = gen::find_component_schema("InteractInfo");
    if (!info.obj.schema || !info.obj.schema->command("MountPtr")) return false;
    pawn_.subobjects.push_back(std::move(info));
    return true;
}
// built by Cyb with C-O-A

std::optional<Send> PlayerBootstrap::pawn_mountable_pet(const FIntrepidNetworkGUID& mount) {
    if (!ok_ || !second_state_sent_) return std::nullopt;
    repl::Subobject* info = nullptr;
    for (repl::Subobject& s : pawn_.subobjects) {
        if (s.name == "InteractInfo") info = &s;
    }
    if (!info || !info->obj.schema || !info->obj.schema->command("MountablePet")) return std::nullopt;
    if (!info->obj.state.set(*info->obj.schema, "MountablePet", object(mount))) return std::nullopt;
    return update(Role::Pawn, "the pawn's mountable pet", pawn_, pawn_shadow_);
}

std::optional<Send> PlayerBootstrap::pawn_mounted(const FIntrepidNetworkGUID& mount, const FIntrepidNetworkGUID& mesh, int64_t scale_percent) {
    if (!ok_ || !second_state_sent_ || !mount.is_valid() || !mesh.is_valid()) return std::nullopt;
    repl::Subobject* info = nullptr;
    for (repl::Subobject& s : pawn_.subobjects) {
        if (s.name == "InteractInfo") info = &s;
    }
    if (!info) return std::nullopt;
    auto set = [&](repl::ObjectState& o, std::string_view path, Value v) { return o.state.set(*o.schema, path, std::move(v)); };
    repl::ObjectState& a = pawn_.obj;
    const FIntrepidNetworkGUID none;
    const bool ok =
        set(a, "RemoteRole", Value::u64(1)) &&   // SimulatedProxy: the pawn follows its attachment
        set(a, "AttachmentReplication.AttachParent", object(mount)) &&
        set(a, "AttachmentReplication.RelativeScale3D", Value(repl::QuantVec{repl::QuantVec::Mode::Scaled, scale_percent, scale_percent, scale_percent})) &&
        set(a, "AttachmentReplication.AttachSocket", Value(repl::NameValue::plain("MountSocket"))) &&
        set(a, "AttachmentReplication.AttachComponent", object(mesh)) && set(a, "PlayerState", object(none)) && set(a, "Controller", object(none)) &&
        set(a, "ReplicatedMovementMode", Value::u64(0)) && set(a, "AttachmentState.InteractableZoneID", Value::u64(kMountInteractableZone)) &&
        set(a, "AttachmentState.bClientLerpToAttachment", Value::boolean(true)) && set(a, "AttachmentState.bAttachParentValid", Value::boolean(true)) &&
        set(info->obj, "MountPtr", object(mount));
    if (!ok) return std::nullopt;
    std::vector<repl::ContentBlock> blocks;
    repl::ShadowUndo undo;
    if (!repl::collect_update_blocks(pawn_, pawn_shadow_, blocks, &undo) || blocks.empty() || blocks[0].sub) {
        undo.rollback(pawn_shadow_);
        return std::nullopt;
    }
    // AttachComponent names the mount's CharacterMesh0 (a default component of the mount, found by name and outer): exported with the actor's block
    blocks[0].exports.push_back(repl::component_export(mount, mesh, "CharacterMesh0", kChecksumPawnMesh));
    auto e = repl::assemble_blocks(pawn_, blocks, acks_);
    if (!e) {
        undo.rollback(pawn_shadow_);
        return std::nullopt;
    }
    pawn_mounted_ = true;
    return finish(Role::Pawn, false, "the rider's pawn is attached to its mount", std::move(e));
}

std::optional<Send> PlayerBootstrap::pawn_dismounted(const repl::Vec3d& detach_location, double yaw_degrees) {
    if (!ok_ || !second_state_sent_ || !std::isfinite(detach_location.x) || !std::isfinite(detach_location.y) || !std::isfinite(detach_location.z) ||
        !std::isfinite(yaw_degrees)) {
        return std::nullopt;
    }
    repl::Subobject* info = nullptr;
    for (repl::Subobject& s : pawn_.subobjects) {
        if (s.name == "InteractInfo") info = &s;
    }
    auto set = [&](repl::ObjectState& o, std::string_view path, Value v) { return o.state.set(*o.schema, path, std::move(v)); };
    repl::ObjectState& a = pawn_.obj;
    const FIntrepidNetworkGUID none;
    const SessionGuids& g = setup_.guids;
    const bool ok =
        set(a, "RemoteRole", Value::u64(2)) && set(a, "AttachmentReplication.AttachParent", object(none)) &&
        set(a, "AttachmentReplication.AttachComponent", object(none)) && set(a, "RemoteViewPitch16", Value::u64(0)) &&
        set(a, "PlayerState", object(g.player_state)) && set(a, "Controller", object(g.player_controller)) &&
        set(a, "AttachmentState.InteractableZoneID", Value::u64(0)) && set(a, "AttachmentState.bClientLerpToAttachment", Value::boolean(false)) &&
        set(a, "AttachmentState.bAttachParentValid", Value::boolean(false)) && set(a, "RepDetachmentLocation", Value(detach_location)) &&
        set(a, "RepDetachmentRotation", Value(repl::Rot{0, yaw16(yaw_degrees), 0})) && (!info || set(info->obj, "MountPtr", object(none)));
    if (!ok) return std::nullopt;
    pawn_mounted_ = false;
    return update(Role::Pawn, "the rider's pawn is detached from its mount", pawn_, pawn_shadow_);
}

}  // namespace aoc::worldd::bootstrap

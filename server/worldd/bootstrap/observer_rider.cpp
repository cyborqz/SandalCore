// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/observer_rider.cpp  (Social, seeing other players: riders on their mounts)
//
//  See observer_rider.h.  The values are the ones the player's own pawn gets when it mounts (bootstrap/mount_ride.cpp pawn_mounted), less what only its owner is sent.
// ============================================================================
#include "bootstrap/observer_rider.h"

#include "bootstrap/constants.h"
#include "protocol/schema/generated/index.h"

#include <string_view>

namespace aoc::worldd::bootstrap {

using protocol::emit::FIntrepidNetworkGUID;
using repl::Value;
namespace gen = ::aoc::protocol::schema::gen;

namespace {

Value object(const FIntrepidNetworkGUID& g) { return Value(g); }

bool set(repl::ObjectState& o, std::string_view path, Value v) { return o.schema && o.state.set(*o.schema, path, std::move(v)); }

repl::Subobject* interact_info(repl::Actor& pawn, const FIntrepidNetworkGUID& info) {
    for (repl::Subobject& s : pawn.subobjects) {
        if (s.name == "InteractInfo") return s.guid == info ? &s : nullptr;
    }
    return nullptr;
}

}  // namespace

bool observer_ensure_interact_info(repl::Actor& pawn, const FIntrepidNetworkGUID& info) {
    if (!info.is_valid()) return false;
    for (const repl::Subobject& s : pawn.subobjects) {
        if (s.name == "InteractInfo") return s.guid == info;
    }
    repl::Subobject sub;
    sub.guid = info;
    sub.name = "InteractInfo";
    sub.checksum = kChecksumInteractInfo;
    sub.obj.schema = gen::find_component_schema("InteractInfo");
    if (!sub.obj.schema || !sub.obj.schema->command("MountPtr") || !sub.obj.schema->command("MountablePet")) return false;
    pawn.subobjects.push_back(std::move(sub));
    return true;
}

bool observer_mountable_pet(repl::Actor& pawn, const FIntrepidNetworkGUID& info, const FIntrepidNetworkGUID& pet) {
    if (!observer_ensure_interact_info(pawn, info)) return false;
    repl::Subobject* sub = interact_info(pawn, info);
    return sub && set(sub->obj, "MountablePet", object(pet));
}

bool observer_mount_up(repl::Actor& pawn, const FIntrepidNetworkGUID& info, const FIntrepidNetworkGUID& mount, const FIntrepidNetworkGUID& mesh,
                       std::vector<repl::ExportChain>& exports) {
    if (!mount.is_valid() || !mesh.is_valid() || !observer_ensure_interact_info(pawn, info)) return false;
    repl::Subobject* sub = interact_info(pawn, info);
    if (!sub) return false;
    const FIntrepidNetworkGUID none;
    repl::ObjectState& a = pawn.obj;
    const bool ok = set(a, "RemoteRole", Value::u64(1)) &&   // SimulatedProxy: the pawn follows its attachment
                    set(a, "AttachmentReplication.AttachParent", object(mount)) &&
                    set(a, "AttachmentReplication.RelativeScale3D", Value(repl::QuantVec{repl::QuantVec::Mode::Scaled, 100, 100, 100})) &&
                    set(a, "AttachmentReplication.AttachSocket", Value(repl::NameValue::plain("MountSocket"))) &&
                    set(a, "AttachmentReplication.AttachComponent", object(mesh)) && set(a, "PlayerState", object(none)) && set(a, "Controller", object(none)) &&
                    set(a, "ReplicatedMovementMode", Value::u64(0)) && set(a, "AttachmentState.InteractableZoneID", Value::u64(kMountInteractableZone)) &&
                    set(a, "AttachmentState.bClientLerpToAttachment", Value::boolean(true)) && set(a, "AttachmentState.bAttachParentValid", Value::boolean(true)) &&
                    set(sub->obj, "MountPtr", object(mount));
    if (!ok) return false;
    // AttachComponent names the mount's CharacterMesh0, a default component of the mount that the client finds by its name and its outer: the update says where
    exports.assign(1, repl::component_export(mount, mesh, "CharacterMesh0", kChecksumPawnMesh));
    return true;
}

bool observer_get_off(repl::Actor& pawn, const FIntrepidNetworkGUID& info) {
    repl::Subobject* sub = interact_info(pawn, info);
    const FIntrepidNetworkGUID none;
    repl::ObjectState& a = pawn.obj;
    return set(a, "AttachmentReplication.AttachParent", object(none)) && set(a, "AttachmentReplication.AttachComponent", object(none)) &&
           set(a, "AttachmentState.InteractableZoneID", Value::u64(0)) && set(a, "AttachmentState.bClientLerpToAttachment", Value::boolean(false)) &&
           set(a, "AttachmentState.bAttachParentValid", Value::boolean(false)) && (!sub || set(sub->obj, "MountPtr", object(none)));
}

}  // namespace aoc::worldd::bootstrap

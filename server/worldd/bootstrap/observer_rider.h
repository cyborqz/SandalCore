// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/observer_rider.h  (Social, seeing other players: riders on their mounts)
//
//  What the pawn the OTHER players are shown (PlayerBootstrap::observer_pawn, bootstrap/observer_actor.cpp) says about a mount: the mount it names as its own (InteractInfo.MountablePet:
//  the summoned mount, ridden or not) and the mount it rides (InteractInfo.MountPtr and the attachment at the mount's saddle).  The functions change the registry's copy of the pawn
//  (a repl::Actor the caller keeps in a WorldActor and changes through ActorRegistry::modify); the actor view sends every connection that holds it what changed.
//
//  What retail sends a third player when another player rides [R: the recordings in which the capturing player sees other players ride]:
//    mount-up     one update of the rider's pawn: AttachmentReplication.AttachParent (the mount) and .AttachComponent (the mount's CharacterMesh0, exported with the update, the socket
//                 "MountSocket" and the scale 1 with the first), AttachmentState (InteractableZoneID 2, bClientLerpToAttachment, bAttachParentValid), ReplicatedMovementMode 0 and, in
//                 the InteractInfo subobject, MountPtr.  The pawn's ReplicatedMovement stays where it was (it is not gathered while attached).
//    dismount     one update: AttachParent and AttachComponent none, AttachmentState zero, ReplicatedMovementMode 3 (falling), the pawn's ReplicatedMovement at the place it left
//                 the saddle, MountPtr none; the walking updates follow.  No RepDetachmentLocation (that is the owner's).
//    while it rides  nothing more: the seated pawn is sent no update of its own, and the rider's view pitch is the mount's (RemoteViewPitch16 of the mount's motion updates), not the pawn's.
//    summon, desummon   the pawn's InteractInfo.MountablePet is the mount's guid one packet before the mount opens, and none when the mount is gone.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/replicator.h"

#include <cstdint>
#include <vector>

namespace aoc::worldd::bootstrap {

namespace repl = ::aoc::protocol::repl;

/// The pawn's InteractInfo subobject (`info` is its GUID), added when the pawn has none.  False when the class lacks the mount fields.
bool observer_ensure_interact_info(repl::Actor& pawn, const protocol::emit::FIntrepidNetworkGUID& info);

/// InteractInfo.MountablePet: the mount the player has summoned (an invalid guid: none).
bool observer_mountable_pet(repl::Actor& pawn, const protocol::emit::FIntrepidNetworkGUID& info, const protocol::emit::FIntrepidNetworkGUID& pet);

/// The rider is on `mount`, seated at its MountSocket: the attachment (to the mount's CharacterMesh0, `mesh`), the attachment state and MountPtr.  `exports` gets the export of the mesh
/// (the update that carries the attachment must carry it).
bool observer_mount_up(repl::Actor& pawn, const protocol::emit::FIntrepidNetworkGUID& info, const protocol::emit::FIntrepidNetworkGUID& mount, const protocol::emit::FIntrepidNetworkGUID& mesh,
                       std::vector<repl::ExportChain>& exports);

/// The rider is off its mount: the attachment, the attachment state and MountPtr go.  The pawn's place is the caller's.
bool observer_get_off(repl::Actor& pawn, const protocol::emit::FIntrepidNetworkGUID& info);

}  // namespace aoc::worldd::bootstrap

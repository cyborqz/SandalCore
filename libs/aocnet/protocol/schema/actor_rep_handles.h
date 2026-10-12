// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/actor_rep_handles.h
//
//  RepLayout handles of the replicated properties every AActor (and APawn)
//  shares.  A handle is the 1-based index of a RepLayout command:
//  FRepLayout::InitFromClass walks UClass::ClassReps (super classes first,
//  each native class in declaration order) and gives every leaf property a
//  command; a struct without NetSerialize is flattened into one command per
//  member (sorted by offset), a NetSerialize struct is one command.
//
//  AActor's 13 CPF_Net properties from the client exe
//  (tools/ghidra/ue_class_props.py AActor --net-only):
//    AuthServerIDReplicated, bReplicateMovement, bHidden, bTearOff,
//    bCanBeDamaged, bReplicates, ReplicatedMovement (FRepMovement,
//    NetSerialize), RemoteRole, AttachmentReplication (FRepAttachment, six
//    members), Owner, Role, NetDormancy, Instigator.
//  APawn adds RemoteViewPitch16, RemoteViewPitch, PlayerState, Controller.
//
//  Checked against the original server's PlayerPawn_C updates in
//  replay_data.bin: Owner at 15, Role at 16, Instigator at 18, PlayerState
//  at 21 and Controller at 22 all carry the expected objects
//  (test_rep_layout_writer).
// ============================================================================
#pragma once

#include <cstdint>

namespace aoc { namespace protocol { namespace schema {

namespace actor_rep {
constexpr uint32_t AuthServerIDReplicated = 1;   // uint32
constexpr uint32_t bReplicateMovement     = 2;   // bool
constexpr uint32_t bHidden                = 3;   // bool
constexpr uint32_t bTearOff               = 4;   // bool
constexpr uint32_t bCanBeDamaged          = 5;   // bool
constexpr uint32_t bReplicates            = 6;   // bool
constexpr uint32_t ReplicatedMovement     = 7;   // FRepMovement (NetSerialize)
constexpr uint32_t RemoteRole             = 8;   // ENetRole
// 9..14: AttachmentReplication — AttachParent, LocationOffset,
//        RelativeScale3D, RotationOffset, AttachSocket, AttachComponent
constexpr uint32_t Owner                  = 15;  // AActor*
constexpr uint32_t Role                   = 16;  // ENetRole
constexpr uint32_t NetDormancy            = 17;  // ENetDormancy
constexpr uint32_t Instigator             = 18;  // APawn*
}  // namespace actor_rep
// -- C.Y.B . CoA --

namespace pawn_rep {
constexpr uint32_t RemoteViewPitch16      = 19;  // uint16
constexpr uint32_t RemoteViewPitch        = 20;  // uint8
constexpr uint32_t PlayerState            = 21;  // APlayerState*
constexpr uint32_t Controller             = 22;  // AController*
}  // namespace pawn_rep

/// AAoCPlayerControllerBP_C: AController, APlayerController and
/// AAoCPlayerController after AActor's 18 commands
/// (tools/ghidra/ue_replayout.py AAoCPlayerController).  Checked against the
/// original server's PC blocks on ch=3: its open (pkt#22) decodes to the last
/// bit through PlayerState, SpawnLocation, CaravanLaunchNode and
/// SummonCooldownTimer; pkt#46 and #72 carry only CharacterLoadTracker.
namespace pc_rep {
constexpr uint32_t PlayerState           = 19;  // APlayerState*
constexpr uint32_t Pawn                  = 20;  // APawn*
constexpr uint32_t TargetViewRotation    = 21;  // FRotator (NetSerialize)
constexpr uint32_t SpawnLocation         = 22;  // FVector: three doubles
constexpr uint32_t CaravanLaunchNode     = 24;  // int64
constexpr uint32_t CharacterLoadTracker  = 51;  // FCharacterLoadTracker (NetSerialize)
constexpr uint32_t SummonCooldownTimer   = 52;  // float
}  // namespace pc_rep

/// AoC's ENetRole: ROLE_LightweightProxy sits between AutonomousProxy and
/// Authority, so Authority is 4 (client exe, Z_Construct_UEnum_Engine_ENetRole).
///
/// The receiver swaps the pair: the value sent under RemoteRole becomes the
/// client's local Role (AutonomousProxy for the player's own pawn), the value
/// sent under Role becomes its RemoteRole.
enum class ENetRole : uint8_t {
    None             = 0,
    SimulatedProxy   = 1,
    AutonomousProxy  = 2,
    LightweightProxy = 3,
    Authority        = 4,
};

/// FByteProperty with an enum: CeilLogTwo(GetMaxEnumValue() + 1) bits,
/// ROLE_MAX = 5 -> 3 bits.
constexpr int kNetRoleBits = 3;

}}}  // namespace aoc::protocol::schema

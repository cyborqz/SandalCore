// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/generated/ADuelCenter.h
//
//  From the client's class layout (AOCClient-Win64-Shipping.exe): ADuelCenter (GameSystemsPlugin), the duel's banner. Duel: FDuel, CPF_Net, RepNotify
//  (OnRep_DuelState); its members by offset: InstanceGuid (FGuid), Participants (TArray<FDuelParticipant>: Role, CharacterGuid, ExitRadiusTimestamp),
//  SyncBeginTime (double, the countdown's end on the synchronized clock), bIsActive.  Its five net functions are server-to-server (InterServer*), never
//  sent to a client, but they are net fields: they count in SerializeInt's max.
//  AActor's commands and fields as AActor.h has them.  BP_DuelCenter_C (/Game/GameBlueprints/PC/BP_DuelCenter) adds no net field of its own (its
//  components, IntrepidAk and DuelBannerDrop, are not replicated).
//
//  ADuelCenter: 25 RepLayout commands, 19 net fields (SerializeInt max 20), 5 RPCs.
//  Own: 1 replicated property from handle 19, 5 RPCs from field 14.
//  Class chain:
//    UObject
//    AActor
//    ADuelCenter
// ============================================================================
#pragma once

#include "protocol/schema/generated/schema_types.h"

namespace aoc { namespace protocol { namespace schema { namespace gen {

namespace ADuelCenter_tables {

inline constexpr RepCommand kCommands[] = {
    {    1,    0, "AuthServerIDReplicated", "AActor", PropType::UInt32, 0,  32, nullptr, nullptr, 0, 0},
    {    2,    1, "bReplicateMovement", "AActor", PropType::Bool, 4,   1, nullptr, nullptr, 0, 0},
    {    3,    2, "bHidden", "AActor", PropType::Bool, 0,   1, nullptr, nullptr, 0, 0},
    {    4,    3, "bTearOff", "AActor", PropType::Bool, 0,   1, nullptr, nullptr, 0, 0},
    {    5,    4, "bCanBeDamaged", "AActor", PropType::Bool, 0,   1, nullptr, nullptr, 0, 0},
    {    6,    5, "bReplicates", "AActor", PropType::Bool, 4,   1, nullptr, nullptr, 0, 0},
    {    7,    6, "ReplicatedMovement", "AActor", PropType::NetSerializeStruct, 4,   0, "FRepMovement", nullptr, 0, 0},
    {    8,    7, "RemoteRole", "AActor", PropType::Enum, 0,   3, nullptr, "ENetRole", 0, 0},
    {    9,    8, "AttachmentReplication.AttachParent", "AActor", PropType::Object, 4, 128, nullptr, nullptr, 0, 0},
    {   10,    8, "AttachmentReplication.LocationOffset", "AActor", PropType::NetSerializeStruct, 4,   0, "FVector_NetQuantize100", nullptr, 0, 0},
    {   11,    8, "AttachmentReplication.RelativeScale3D", "AActor", PropType::NetSerializeStruct, 4,   0, "FVector_NetQuantize100", nullptr, 0, 0},
    {   12,    8, "AttachmentReplication.RotationOffset", "AActor", PropType::NetSerializeStruct, 4,   0, "FRotator", nullptr, 0, 0},
    {   13,    8, "AttachmentReplication.AttachSocket", "AActor", PropType::Name, 4,   0, nullptr, nullptr, 0, 0},
    {   14,    8, "AttachmentReplication.AttachComponent", "AActor", PropType::Object, 4, 128, nullptr, nullptr, 0, 0},
    {   15,    9, "Owner", "AActor", PropType::Object, 4, 128, nullptr, nullptr, 0, 0},
    {   16,   10, "Role", "AActor", PropType::Enum, 0,   3, nullptr, "ENetRole", 0, 0},
    {   17,   11, "NetDormancy", "AActor", PropType::Enum, 4,   3, nullptr, "ENetDormancy", 0, 0},
    {   18,   12, "Instigator", "AActor", PropType::Object, 4, 128, nullptr, nullptr, 0, 0},
    {   19,   13, "Duel.InstanceGuid.A", "ADuelCenter", PropType::Int32, 4,  32, nullptr, nullptr, 0, 0},
    {   20,   13, "Duel.InstanceGuid.B", "ADuelCenter", PropType::Int32, 4,  32, nullptr, nullptr, 0, 0},
    {   21,   13, "Duel.InstanceGuid.C", "ADuelCenter", PropType::Int32, 4,  32, nullptr, nullptr, 0, 0},
    {   22,   13, "Duel.InstanceGuid.D", "ADuelCenter", PropType::Int32, 4,  32, nullptr, nullptr, 0, 0},
    {   23,   13, "Duel.Participants", "ADuelCenter", PropType::DynamicArray, 4,   0, nullptr, nullptr, 0, 6},
    {   24,   13, "Duel.SyncBeginTime", "ADuelCenter", PropType::Double, 4,  64, nullptr, nullptr, 0, 0},
    {   25,   13, "Duel.bIsActive", "ADuelCenter", PropType::Bool, 4,   1, nullptr, nullptr, 0, 0},
};

inline constexpr RepCommand kElementCommands[] = {
    {    1,    0, "Participants.Role", "", PropType::Enum, 0,   2, nullptr, "EDuelParticipantRole", 0, 0},
    {    2,    0, "Participants.CharacterGuid.A", "", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    3,    0, "Participants.CharacterGuid.B", "", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    4,    0, "Participants.CharacterGuid.C", "", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    5,    0, "Participants.CharacterGuid.D", "", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    6,    0, "Participants.ExitRadiusTimestamp", "", PropType::Double, 0,  64, nullptr, nullptr, 0, 0},
};

inline constexpr NetField kFields[] = {
    {    0, "AuthServerIDReplicated", "AActor", FieldKind::Property, 0, 0x00000000, 1, kNoRpc},
    {    1, "bReplicateMovement", "AActor", FieldKind::Property, 4, 0x00000000, 2, kNoRpc},
    {    2, "bHidden", "AActor", FieldKind::Property, 0, 0x00000000, 3, kNoRpc},
    {    3, "bTearOff", "AActor", FieldKind::Property, 0, 0x00000000, 4, kNoRpc},
    {    4, "bCanBeDamaged", "AActor", FieldKind::Property, 0, 0x00000000, 5, kNoRpc},
    {    5, "bReplicates", "AActor", FieldKind::Property, 4, 0x00000000, 6, kNoRpc},
    {    6, "ReplicatedMovement", "AActor", FieldKind::Property, 4, 0x00000000, 7, kNoRpc},
    {    7, "RemoteRole", "AActor", FieldKind::Property, 0, 0x00000000, 8, kNoRpc},
    {    8, "AttachmentReplication", "AActor", FieldKind::Property, 4, 0x00000000, 9, kNoRpc},
    {    9, "Owner", "AActor", FieldKind::Property, 4, 0x00000000, 15, kNoRpc},
    {   10, "Role", "AActor", FieldKind::Property, 0, 0x00000000, 16, kNoRpc},
    {   11, "NetDormancy", "AActor", FieldKind::Property, 4, 0x00000000, 17, kNoRpc},
    {   12, "Instigator", "AActor", FieldKind::Property, 4, 0x00000000, 18, kNoRpc},
    {   13, "Duel", "ADuelCenter", FieldKind::Property, 4, 0x00000000, 19, kNoRpc},
    {   14, "InterServerAcceptDuel", "ADuelCenter", FieldKind::Function, 0, 0x00820cd0, 0, 0},
    {   15, "InterServerDeclineDuel", "ADuelCenter", FieldKind::Function, 0, 0x00820cd0, 0, 1},
    {   16, "InterServerForfeit", "ADuelCenter", FieldKind::Function, 0, 0x00820cd0, 0, 2},
    {   17, "InterserverInterruptDuel", "ADuelCenter", FieldKind::Function, 0, 0x00820cd0, 0, 3},
    {   18, "InterserverWinDuel", "ADuelCenter", FieldKind::Function, 0, 0x00820cd0, 0, 4},
};

inline constexpr RpcParam kRpcParams[] = {
    {"RecipientGuid", PropType::Struct, 0, "FGuid", true, 0, 4},
    {"RecipientGuid", PropType::Struct, 0, "FGuid", true, 0, 4},
    {"ParticipantGuid", PropType::Struct, 0, "FGuid", true, 0, 4},
    {"InterruptingPlayerGuid", PropType::Struct, 0, "FGuid", true, 0, 4},
    {"WinningParticipantGuid", PropType::Struct, 0, "FGuid", true, 0, 4},
};

inline constexpr RepCommand kRpcCommands[] = {
    {    1,    0, "RecipientGuid.A", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    2,    0, "RecipientGuid.B", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    3,    0, "RecipientGuid.C", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    4,    0, "RecipientGuid.D", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    1,    0, "RecipientGuid.A", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    2,    0, "RecipientGuid.B", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    3,    0, "RecipientGuid.C", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    4,    0, "RecipientGuid.D", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    1,    0, "ParticipantGuid.A", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    2,    0, "ParticipantGuid.B", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    3,    0, "ParticipantGuid.C", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    4,    0, "ParticipantGuid.D", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    1,    0, "InterruptingPlayerGuid.A", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    2,    0, "InterruptingPlayerGuid.B", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    3,    0, "InterruptingPlayerGuid.C", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    4,    0, "InterruptingPlayerGuid.D", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    1,    0, "WinningParticipantGuid.A", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    2,    0, "WinningParticipantGuid.B", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    3,    0, "WinningParticipantGuid.C", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
    {    4,    0, "WinningParticipantGuid.D", "ADuelCenter", PropType::Int32, 0,  32, nullptr, nullptr, 0, 0},
};

inline constexpr RpcSchema kRpcs[] = {
    {"InterServerAcceptDuel", "ADuelCenter", 0x00820cd0, 14, std::span<const RpcParam>{kRpcParams + 0, 1}, std::span<const RepCommand>{kRpcCommands + 0, 4}, std::span<const RepCommand>{}},
    {"InterServerDeclineDuel", "ADuelCenter", 0x00820cd0, 15, std::span<const RpcParam>{kRpcParams + 1, 1}, std::span<const RepCommand>{kRpcCommands + 4, 4}, std::span<const RepCommand>{}},
    {"InterServerForfeit", "ADuelCenter", 0x00820cd0, 16, std::span<const RpcParam>{kRpcParams + 2, 1}, std::span<const RepCommand>{kRpcCommands + 8, 4}, std::span<const RepCommand>{}},
    {"InterserverInterruptDuel", "ADuelCenter", 0x00820cd0, 17, std::span<const RpcParam>{kRpcParams + 3, 1}, std::span<const RepCommand>{kRpcCommands + 12, 4}, std::span<const RepCommand>{}},
    {"InterserverWinDuel", "ADuelCenter", 0x00820cd0, 18, std::span<const RpcParam>{kRpcParams + 4, 1}, std::span<const RepCommand>{kRpcCommands + 16, 4}, std::span<const RepCommand>{}},
};

inline constexpr uint16_t kCommandsByPath[] = {
    14, 9, 13, 10, 11, 12, 1, 19, 20, 21, 22, 23, 24, 25, 18, 17,
    15, 8, 7, 16, 5, 3, 2, 6, 4,
};

inline constexpr uint16_t kFieldsByName[] = {
    8, 0, 13, 12, 14, 15, 16, 17, 18, 11, 9, 7, 6, 10, 4, 2,
    1, 5, 3,
};

}  // namespace ADuelCenter_tables

inline constexpr ClassSchema kADuelCenter{
    "ADuelCenter", "AActor", true, 19, 13,
    std::span<const RepCommand>{ADuelCenter_tables::kCommands},
    std::span<const RepCommand>{ADuelCenter_tables::kElementCommands},
    std::span<const NetField>{ADuelCenter_tables::kFields},
    std::span<const RpcSchema>{ADuelCenter_tables::kRpcs},
    std::span<const uint16_t>{ADuelCenter_tables::kCommandsByPath},
    std::span<const uint16_t>{ADuelCenter_tables::kFieldsByName},
};

}}}}  // namespace aoc::protocol::schema::gen

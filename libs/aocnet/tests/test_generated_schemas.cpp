// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_generated_schemas.cpp
//
//  The generated class schemas (tools/ghidra/schemagen.py) against every fact
//  established before them:
//    - AActor/APawn handles 1-22 as protocol/schema/actor_rep_handles.h;
//    - PC handles 19-24, 51, 52 and ENetRole's 3 bits (pkt#22, #46, #72);
//    - PC field indices as protocol/wire/possession.h and level_visibility.h;
//    - pawn field 77 ServerClientIsReadyToPhaseIn;
//    - UAoCAbilityComponent 24/28 with SerializeInt max 31;
//    - UAoCStatsComponent 3/5/6/10 with SerializeInt max 23;
//    - ULevelingComponent (plan D1, the PlayerController's XPTracker): five fast maps (fields 2-6) and three RPCs, with SerializeInt max 11;
//    - UMountComponent (mounts, stage 1): 12 commands, 11 net fields, the three export names of a mount's component and the alias MountComponent_C;
//  plus the invariants the replicator relies on: handle == index + 1, field
//  index == position, sorted lookup tables, RPC <-> field cross references,
//  element ranges in bounds, and the index's native / Blueprint / component
//  names.
// ============================================================================
#include "protocol/schema/generated/AActor.h"
#include "protocol/schema/generated/AAoCPlayerController.h"
#include "protocol/schema/generated/AAutonomousPlayerCharacter.h"
#include "protocol/schema/generated/APawn.h"
#include "protocol/schema/generated/APlayerCharacter.h"
#include "protocol/schema/generated/UAoCAbilityComponent.h"
#include "protocol/schema/generated/UAoCStatsComponent.h"
#include "protocol/schema/generated/ULevelingComponent.h"
#include "protocol/schema/generated/UMountComponent.h"
#include "protocol/schema/generated/UPlayerInfo.h"
#include "protocol/schema/generated/index.h"
#include "protocol/schema/actor_rep_handles.h"
#include "protocol/repl/codecs.h"
#include "protocol/wire/level_visibility.h"
#include "protocol/wire/possession.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace aoc::protocol::schema;
using namespace aoc::protocol::schema::gen;
namespace wire = aoc::protocol::wire;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static bool same(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }
// Cyb : C-O-A

/// Handle `h` of `s` is `path` of `type` and `width` bits.
static bool cmd_is(const ClassSchema& s, uint32_t h, const char* path, PropType type, uint16_t width,
                   const char* struct_name = nullptr) {
    const RepCommand* c = s.command(h);
    return c && same(c->path, path) && c->type == type && c->width_bits == width &&
           (!struct_name || same(c->struct_name, struct_name));
}

static bool field_is(const ClassSchema& s, uint32_t index, const char* name, FieldKind kind) {
    const NetField* f = s.field(index);
    return f && same(f->name, name) && f->kind == kind && s.field(name) == f;
}

static void test_actor_and_pawn_handles() {
    std::printf("AActor / APawn handles (actor_rep_handles.h)\n");
    const ClassSchema& s = kAPawn;
    CHECK(kAActor.commands.size() == 18 && s.commands.size() == 22, "AActor has 18 commands, APawn 22");
    CHECK(cmd_is(s, actor_rep::AuthServerIDReplicated, "AuthServerIDReplicated", PropType::UInt32, 32), "1 AuthServerIDReplicated uint32");
    CHECK(cmd_is(s, actor_rep::bReplicateMovement, "bReplicateMovement", PropType::Bool, 1), "2 bReplicateMovement");
    CHECK(cmd_is(s, actor_rep::bHidden, "bHidden", PropType::Bool, 1), "3 bHidden");
    CHECK(cmd_is(s, actor_rep::bTearOff, "bTearOff", PropType::Bool, 1), "4 bTearOff");
    CHECK(cmd_is(s, actor_rep::bCanBeDamaged, "bCanBeDamaged", PropType::Bool, 1), "5 bCanBeDamaged");
    CHECK(cmd_is(s, actor_rep::bReplicates, "bReplicates", PropType::Bool, 1), "6 bReplicates");
    CHECK(cmd_is(s, actor_rep::ReplicatedMovement, "ReplicatedMovement", PropType::NetSerializeStruct, 0, "FRepMovement"),
          "7 ReplicatedMovement FRepMovement (NetSerialize)");
    CHECK(cmd_is(s, actor_rep::RemoteRole, "RemoteRole", PropType::Enum, 3), "8 RemoteRole enum, 3 bits");
    CHECK(cmd_is(s, 9, "AttachmentReplication.AttachParent", PropType::Object, 128), "9 AttachmentReplication.AttachParent");
    CHECK(cmd_is(s, 10, "AttachmentReplication.LocationOffset", PropType::NetSerializeStruct, 0, "FVector_NetQuantize100"),
          "10 AttachmentReplication.LocationOffset");
    CHECK(cmd_is(s, 11, "AttachmentReplication.RelativeScale3D", PropType::NetSerializeStruct, 0, "FVector_NetQuantize100"),
          "11 AttachmentReplication.RelativeScale3D");
    CHECK(cmd_is(s, 12, "AttachmentReplication.RotationOffset", PropType::NetSerializeStruct, 0, "FRotator"),
          "12 AttachmentReplication.RotationOffset");
    CHECK(cmd_is(s, 13, "AttachmentReplication.AttachSocket", PropType::Name, 0), "13 AttachmentReplication.AttachSocket");
    CHECK(cmd_is(s, 14, "AttachmentReplication.AttachComponent", PropType::Object, 128), "14 AttachmentReplication.AttachComponent");
    CHECK(cmd_is(s, actor_rep::Owner, "Owner", PropType::Object, 128), "15 Owner");
    CHECK(cmd_is(s, actor_rep::Role, "Role", PropType::Enum, 3), "16 Role enum, 3 bits");
    CHECK(cmd_is(s, actor_rep::NetDormancy, "NetDormancy", PropType::Enum, 3), "17 NetDormancy");
    CHECK(cmd_is(s, actor_rep::Instigator, "Instigator", PropType::Object, 128), "18 Instigator");
    CHECK(cmd_is(s, pawn_rep::RemoteViewPitch16, "RemoteViewPitch16", PropType::UInt16, 16), "19 RemoteViewPitch16 uint16");
    CHECK(cmd_is(s, pawn_rep::RemoteViewPitch, "RemoteViewPitch", PropType::UInt8, 8), "20 RemoteViewPitch uint8");
    CHECK(cmd_is(s, pawn_rep::PlayerState, "PlayerState", PropType::Object, 128), "21 PlayerState");
    CHECK(cmd_is(s, pawn_rep::Controller, "Controller", PropType::Object, 128), "22 Controller");
    const RepCommand* role = s.command(actor_rep::Role);
    CHECK(role && same(role->enum_name, "ENetRole") && role->width_bits == kNetRoleBits, "ENetRole is 3 bits (kNetRoleBits)");
    CHECK(s.command("AttachmentReplication.AttachSocket") == s.command(13u), "lookup by path finds handle 13");
    CHECK(kAPawn.first_own_handle == 19 && kAPawn.first_own_field == 13, "APawn's own entries start at handle 19 / field 13");
    CHECK(same(kAPawn.super_name, "AActor") && kAPawn.is_actor, "APawn's super is AActor");
}

static void test_player_controller() {
    std::printf("AAoCPlayerController handles and fields (pkt#22, #46, #72, possession.h)\n");
    const ClassSchema& s = kAAoCPlayerController;
    CHECK(cmd_is(s, pc_rep::PlayerState, "PlayerState", PropType::Object, 128), "19 PlayerState");
    CHECK(cmd_is(s, pc_rep::Pawn, "Pawn", PropType::Object, 128), "20 Pawn");
    CHECK(cmd_is(s, pc_rep::TargetViewRotation, "TargetViewRotation", PropType::NetSerializeStruct, 0, "FRotator"),
          "21 TargetViewRotation FRotator (NetSerialize)");
    CHECK(cmd_is(s, pc_rep::SpawnLocation, "SpawnLocation", PropType::NetSerializeStruct, 192, "FVector"),
          "22 SpawnLocation FVector: one NetSerialize command of three doubles (192 bits)");
    CHECK(cmd_is(s, 23, "bRegisteredForDamageMeter", PropType::Bool, 1), "23 bRegisteredForDamageMeter");
    CHECK(cmd_is(s, pc_rep::CaravanLaunchNode, "CaravanLaunchNode", PropType::Int64, 64), "24 CaravanLaunchNode int64");
    CHECK(cmd_is(s, pc_rep::CharacterLoadTracker, "CharacterLoadTracker", PropType::NetSerializeStruct, 0, "FCharacterLoadTracker"),
          "51 CharacterLoadTracker (NetSerialize)");
    CHECK(cmd_is(s, pc_rep::SummonCooldownTimer, "SummonCooldownTimer", PropType::Float, 32), "52 SummonCooldownTimer float");
    CHECK(s.commands.size() == 170 && s.first_own_handle == 23, "170 commands, own from 23");
    // ETabTargetMethod has enumerators 0..3 and no MAX: UEnum::SetEnums adds
    // ETabTargetMethod_MAX = 4, so the enum takes 3 bits (pkt#620's settings
    // block only decodes to its last bit with 3).
    CHECK(cmd_is(s, 102, "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.TabTargetMethod", PropType::Enum, 3),
          "102 TabTargetMethod: 3 bits from the implicit _MAX entry");
    // FVector2D has a NetSerializer (two doubles), so AAoCGameStateBase's
    // AutoFollowDistanceRange is one command; with it flattened the GameState
    // open (pkt#28) stopped decoding at its FText neighbours.
    const ClassSchema* gs = find_class_schema("AAoCGameStateBase");
    CHECK(gs && cmd_is(*gs, 153, "CombatSettings.AutoFollow.AutoFollowDistanceRange", PropType::NetSerializeStruct, 128, "FVector2D") &&
          gs->commands.size() == 179 && gs->num_fields() == 35,
          "GameState 153 AutoFollowDistanceRange FVector2D (128 bits); 179 commands, 35 fields");

    CHECK(field_is(s, wire::kPcFieldIdx_ClientRestart, "ClientRestart", FieldKind::Function), "field 45 ClientRestart");
    CHECK(field_is(s, wire::kPcFieldIdx_ClientRetryClientRestart, "ClientRetryClientRestart", FieldKind::Function),
          "field 46 ClientRetryClientRestart");
    CHECK(field_is(s, wire::kPcFieldIdx_ClientSetHUD, "ClientSetHUD", FieldKind::Function), "field 53 ClientSetHUD");
    CHECK(field_is(s, wire::kPcFieldIdx_ServerAcknowledgePossession, "ServerAcknowledgePossession", FieldKind::Function),
          "field 73 ServerAcknowledgePossession");
    CHECK(field_is(s, wire::kPcFieldIdx_ServerCheckClientPossession, "ServerCheckClientPossession", FieldKind::Function),
          "field 77 ServerCheckClientPossession");
    CHECK(field_is(s, wire::kPcFieldIdx_ServerCheckClientPossessionReliable, "ServerCheckClientPossessionReliable",
                   FieldKind::Function), "field 78 ServerCheckClientPossessionReliable");
    CHECK(field_is(s, wire::kPcFieldIdx_ClientAckUpdateLevelVisibility, "ClientAckUpdateLevelVisibility", FieldKind::Function),
          "field 21 ClientAckUpdateLevelVisibility");
    CHECK(field_is(s, wire::kPcFieldIdx_ServerUpdateLevelVisibility, "ServerUpdateLevelVisibility", FieldKind::Function),
          "field 92 ServerUpdateLevelVisibility");
    CHECK(field_is(s, wire::kPcFieldIdx_ServerUpdateMultipleLevelsVisibility, "ServerUpdateMultipleLevelsVisibility",
                   FieldKind::Function), "field 93 ServerUpdateMultipleLevelsVisibility");
    // AActor 13 properties, AController 2 + 2 RPCs, APlayerController 2: its RPCs start at 19.
    CHECK(field_is(s, 19, "BroadcastRPC", FieldKind::Function) && same(s.field(19u)->owner, "APlayerController"),
          "APlayerController's first RPC is field 19");
    CHECK(field_is(s, 15, "ClientSetLocation", FieldKind::Function) && field_is(s, 16, "ClientSetRotation", FieldKind::Function),
          "AController's RPCs are fields 15 and 16 (after its PlayerState / Pawn at 13, 14)");
    CHECK(field_is(s, 17, "TargetViewRotation", FieldKind::Property) && field_is(s, 18, "SpawnLocation", FieldKind::Property),
          "APlayerController's properties are fields 17 and 18");
    CHECK(field_is(s, 13, "PlayerState", FieldKind::Property) && s.field(13u)->handle == pc_rep::PlayerState,
          "property field 13 PlayerState -> handle 19");
    CHECK(field_is(s, 107, "CharacterLoadTracker", FieldKind::Property) && s.field(107u)->handle == 51,
          "property field 107 CharacterLoadTracker -> handle 51");
// ~ C-O-A - Cyb ~

    // The native class has 1025 fields; AoCPlayerControllerBP_C adds six
    // replicated Blueprint events (cooked package, bp_class_props.py) sorted
    // by name after them: 1031 fields, SerializeInt(field, 1032).
    // possession.h's 1025 gives the same bit count for every field from 8 up,
    // and fields 0..12 are AActor's properties, never in a field loop.
    CHECK(s.num_fields() == 1025 && s.field_max() == 1026, "native: 1025 net fields");
    const ClassSchema* bp = find_class_schema("AoCPlayerControllerBP_C");
    CHECK(bp && same(bp->name, "AoCPlayerControllerBP_C") && same(bp->super_name, "AAoCPlayerController") &&
          bp->num_fields() == 1031 && bp->field_max() == 1032 && bp->first_own_field == 1025 &&
          bp->commands.size() == s.commands.size(),
          "AoCPlayerControllerBP_C: the native tables plus 6 fields, SerializeInt max 1032");
    CHECK(bp && field_is(*bp, 1025, "CLIENT Select Target from SERVER", FieldKind::Function) &&
          field_is(*bp, 1026, "Client_HideAllCharactersInWorld", FieldKind::Function) &&
          field_is(*bp, 1030, "TargetMember", FieldKind::Function) &&
          field_is(*bp, 45, "ClientRestart", FieldKind::Function),
          "Blueprint events are fields 1025..1030 in case-insensitive name order; native fields unchanged");
    // FBitWriter::SerializeInt stops once the bits written so far plus the
    // next mask reach the max, so 1025 and 1032 only part company where the
    // low ten bits of the field are 1..7: fields 1-7 (AActor properties,
    // never in a field loop) and the Blueprint events 1025-1031, which 1025
    // cannot encode at all.  Every native RPC keeps its width; the
    // replicator still has to use field_max() for the Blueprint events.
    bool same_bits = true, bp_differ = true;
    for (uint32_t f = 8; f < 1025; ++f)
        same_bits = same_bits && wire::detail::serialize_int_bits(f, 1032) == wire::detail::serialize_int_bits(f, wire::kPcMaxFieldIdx);
    for (uint32_t f = 1025; f < 1031; ++f)
        bp_differ = bp_differ && wire::detail::serialize_int_bits(f, 1032) != wire::detail::serialize_int_bits(f, wire::kPcMaxFieldIdx);
    CHECK(same_bits, "fields 8..1024 take the same header bits with max 1032 as with kPcMaxFieldIdx 1025");
    CHECK(bp_differ, "the Blueprint event fields 1025..1030 do not");
    const RpcSchema* hide = bp ? bp->rpc("Client_HideAllCharactersInWorld") : nullptr;
    CHECK(hide && hide->params.size() == 2 && hide->params[0].type == PropType::Bool && !hide->params[0].send_bit &&
          hide->params[1].type == PropType::DynamicArray && (hide->function_flags & kFuncNetClient),
          "Client_HideAllCharactersInWorld(bool bHide, TArray<AActor*> IgnoreActors)");

    const RpcSchema* restart = s.rpc("ClientRestart");
    CHECK(restart && restart->field_index == 45 && restart->params.size() == 1 &&
          same(restart->params[0].name, "NewPawn") && restart->params[0].type == PropType::Object &&
          restart->params[0].width_bits == 128 && restart->params[0].send_bit,
          "ClientRestart(APawn* NewPawn): one object parameter with a send bit");
    const RpcSchema* check = s.rpc("ServerCheckClientPossession");
    CHECK(check && check->params.empty() && (check->function_flags & kFuncNetServer) && !(check->function_flags & kFuncNetReliable),
          "ServerCheckClientPossession(): no parameters, Server, unreliable");
    const RpcSchema* hud = s.rpc("ClientSetHUD");
    CHECK(hud && hud->params.size() == 1 && hud->params[0].type == PropType::Object, "ClientSetHUD(TSubclassOf<AHUD>): one object");
}

static void test_pawn() {
    std::printf("player pawn (AAutonomousPlayerCharacter / APlayerCharacter)\n");
    const ClassSchema& s = kAAutonomousPlayerCharacter;
    CHECK(field_is(s, 77, "ServerClientIsReadyToPhaseIn", FieldKind::Function) && same(s.field(77u)->owner, "ABaseCharacter"),
          "field 77 ServerClientIsReadyToPhaseIn (ABaseCharacter)");
    CHECK(field_is(kAPlayerCharacter, 77, "ServerClientIsReadyToPhaseIn", FieldKind::Function),
          "... also under APlayerCharacter");
    CHECK(s.commands.size() == 70 && kAPlayerCharacter.commands.size() == 70, "70 commands (AAutonomousPlayerCharacter adds none)");
    CHECK(s.num_fields() == 193 && kAPlayerCharacter.num_fields() == 193, "193 net fields (SerializeInt max 194)");
    CHECK(field_is(s, 38, "ServerMove", FieldKind::Function) && field_is(s, 42, "ServerMoveNoBase", FieldKind::Function),
          "ACharacter's ServerMove is field 38, ServerMoveNoBase 42");
    CHECK(field_is(s, 28, "ClientAckGoodMove", FieldKind::Function), "ACharacter's first RPC is field 28 (13 + 4 + 11 properties)");
    CHECK(cmd_is(s, 22, "Controller", PropType::Object, 128) && s.command(23u) && same(s.command(23u)->owner, "ACharacter"),
          "ACharacter's commands start at 23");
    // PlayerPawn_C adds four replicated Blueprint events (no variables).
    const ClassSchema* bp = find_class_schema("PlayerPawn_C");
    CHECK(bp && same(bp->name, "PlayerPawn_C") && same(bp->super_name, "AAutonomousPlayerCharacter") &&
          bp->commands.size() == 70 && bp->num_fields() == 197 && bp->field_max() == 198 && bp->first_own_field == 193,
          "PlayerPawn_C: 70 commands, 197 fields (SerializeInt max 198)");
    CHECK(bp && field_is(*bp, 193, "DialogueServerEvent", FieldKind::Function) &&
          field_is(*bp, 194, "Left Click On Hover", FieldKind::Function) &&
          field_is(*bp, 195, "StartCameraLag", FieldKind::Function) &&
          field_is(*bp, 196, "StopCameraLag", FieldKind::Function) &&
          field_is(*bp, 77, "ServerClientIsReadyToPhaseIn", FieldKind::Function),
          "Blueprint events are fields 193..196; ServerClientIsReadyToPhaseIn stays 77");
    const RpcSchema* lag = bp ? bp->rpc("StartCameraLag") : nullptr;
    CHECK(lag && lag->params.size() == 3 && lag->params[0].type == PropType::Double && lag->params[0].send_bit &&
          lag->params[1].type == PropType::Bool && !lag->params[1].send_bit,
          "StartCameraLag(double, bool, bool): send bits on the double only");
}

static void test_components() {
    std::printf("UAoCAbilityComponent / UAoCStatsComponent fields\n");
    const ClassSchema& ab = kUAoCAbilityComponent;
    CHECK(field_is(ab, 28, "ServerTriggerAbility", FieldKind::Function), "ability field 28 ServerTriggerAbility");
    CHECK(field_is(ab, 24, "ServerNotifyHeldAbilityCancelled", FieldKind::Function), "ability field 24 ServerNotifyHeldAbilityCancelled");
    CHECK(ab.num_fields() == 30 && ab.field_max() == 31, "ability: 30 net fields -> SerializeInt max 31");
    CHECK(ab.commands.size() == 6 && field_is(ab, 0, "bReplicates", FieldKind::Property) && field_is(ab, 1, "bIsActive", FieldKind::Property),
          "UActorComponent's bReplicates / bIsActive are fields 0 and 1 (handles 1 and 2)");
    const RpcSchema* trigger = ab.rpc("ServerTriggerAbility");
    CHECK(trigger && trigger->field_index == 28 && !trigger->params.empty() && (trigger->function_flags & kFuncNetServer),
          "ServerTriggerAbility has parameters and Server flags");

    const ClassSchema& st = kUAoCStatsComponent;
    CHECK(field_is(st, 3, "StatRepInt32Owner", FieldKind::Property), "stats field 3 StatRepInt32Owner");
    CHECK(field_is(st, 5, "StatRepInt32OwnerProxy", FieldKind::Property), "stats field 5 StatRepInt32OwnerProxy");
    CHECK(field_is(st, 6, "StatRepInt32EveryoneProxy", FieldKind::Property), "stats field 6 StatRepInt32EveryoneProxy");
    CHECK(field_is(st, 10, "StatRepFloatArrayEveryoneProxy", FieldKind::Property), "stats field 10 StatRepFloatArrayEveryoneProxy");
    CHECK(st.num_fields() == 22 && st.field_max() == 23, "stats: 22 net fields -> SerializeInt max 23");
    const NetField* owner = st.field(3u);
    CHECK(owner && (owner->flags & kRepCustomDelta) && (owner->flags & kRepFastArray), "StatRepInt32Owner is a fast array (custom delta)");
    const RepCommand* items = st.command(owner ? owner->handle : 0u);
    CHECK(items && items->type == PropType::DynamicArray && (items->flags & kRepFastArray) && st.elements(*items).size() > 0,
          "its first command is the Items array with element commands");
    const NetField* cc = st.field("CrowdControlState");
    CHECK(cc && cc->kind == FieldKind::Property && !(cc->flags & kRepCustomDelta) && st.command(cc->handle) &&
          std::string(st.command(cc->handle)->path).rfind("CrowdControlState.", 0) == 0,
          "CrowdControlState is a flattened struct");
}

static void test_leveling_component() {
    std::printf("ULevelingComponent (the PlayerController's XPTracker)\n");
    const ClassSchema& lv = kULevelingComponent;
    CHECK(same(lv.super_name, "UIntrepidRepKeyComponent") && !lv.is_actor && lv.commands.size() == 7 && lv.num_fields() == 10 && lv.field_max() == 11,
          "ULevelingComponent: 7 commands, 10 net fields -> SerializeInt max 11");
    CHECK(field_is(lv, 0, "bReplicates", FieldKind::Property) && field_is(lv, 1, "bIsActive", FieldKind::Property), "UActorComponent's two properties are fields 0 and 1");
    const char* maps[] = {"ExperienceFastMap", "SkillPointFastMap", "ExperienceDebtFastMap", "LevelingCertificationsFastMap", "SkillTreePurchasesFastMap"};
    bool fast = true;
    for (size_t i = 0; i < 5; ++i) {
        const NetField* f = lv.field(static_cast<uint32_t>(2 + i));
        fast = fast && f && same(f->name, maps[i]) && f->kind == FieldKind::Property && (f->flags & kRepCustomDelta) && (f->flags & kRepFastArray);
        const RepCommand* items = f ? lv.command(f->handle) : nullptr;
        fast = fast && items && items->type == PropType::DynamicArray && (items->flags & kRepFastArray) && lv.elements(*items).size() >= 4;
    }
    CHECK(fast, "fields 2 to 6 are the five fast maps (custom delta), each with an Items array whose items start with the key Guid, TypeId and Name");
    const NetField* purchases = lv.field("SkillTreePurchasesFastMap");
    const RepCommand* pitems = purchases ? lv.command(purchases->handle) : nullptr;
    const auto pelems = pitems ? lv.elements(*pitems) : std::span<const RepCommand>{};
    CHECK(pelems.size() == 4 && same(pelems[3].path, "Items.Value.IDs") && pelems[3].type == PropType::DynamicArray && lv.elements(pelems[3]).size() == 3,
          "the purchases' value is a nested array of FDesignDataObjectId (three commands)");
    const NetField* xp = lv.field("ExperienceFastMap");
    const auto xelems = xp ? lv.elements(*lv.command(xp->handle)) : std::span<const RepCommand>{};
    CHECK(xelems.size() == 4 && xelems[3].type == PropType::Int64 && lv.elements(*lv.command(lv.field("SkillPointFastMap")->handle))[3].type == PropType::UInt16,
          "an experience value is an int64 and a skill point balance a uint16");
    const RpcSchema* demote = lv.rpc("ServerDemoteCertification");
    CHECK(demote && demote->field_index == 9 && (demote->function_flags & kFuncNetServer) && demote->params.size() == 3 && same(demote->params[0].name, "InteractActorGuid") && same(demote->params[1].name, "Id") &&
              demote->params[1].type == PropType::Struct && demote->params[2].type == PropType::UInt8,
          "ServerDemoteCertification (field 9): the interact actor's FGuid, an FDesignDataObjectId and a Key byte");
    CHECK(lv.rpc("SendDeltaDataToClient") && lv.rpc("SendDeltaDataToClient")->field_index == 8 && lv.rpc("InterServerAwardXpType") &&
              lv.rpc("InterServerAwardXpType")->field_index == 7,
          "SendDeltaDataToClient is field 8 and InterServerAwardXpType field 7");
    CHECK(find_component_schema("XPTracker") == &kULevelingComponent && find_class_schema("ULevelingComponent") == &kULevelingComponent,
          "the PlayerController's component named XPTracker resolves to it");
    // plan D2, review: the actor of a lingering zone (a Bard dance's visual effect), whose retail open decodes with these handles (capture 44, packet 302)
    const ClassSchema* le = find_class_schema("AAoCAbilityLingeringEffect");
    CHECK(le && le->is_actor && same(le->super_name, "AActor") && le->commands.size() == 32 && le->num_fields() == 21 && le->command("LingeringEffectId") &&
              le->command("LingeringEffectId")->handle == 24 && le->command("SourceRecordGuid")->handle == 25 && le->command("SourceRecordType")->handle == 26 &&
              le->command("bFadingOut") && le->command("bFadingOut")->handle == 23 && le->command("LingeringZoneInstGuid.A")->handle == 28 &&
              le->command("AttachmentReplication.AttachComponent")->handle == 14,
          "AAoCAbilityLingeringEffect: 32 commands, 21 fields; LingeringEffectGuid 19-22, bFadingOut 23, LingeringEffectId 24, SourceRecordGuid 25, SourceRecordType 26, LingeringZoneInstGuid 28-31");
}
// (C_Y_B : C.O.A)

/// plan E1, stage 1: the other actors of the living area.  The handles are the ones the retail NPC and gatherable opens decode with
/// (capture 957, ch 21, packet 106: ReplicatedMovement H7, Controller H22, ReplicatedServerLastTransformUpdateTimeStamp H31,
/// ReplicatedMovementMode H33, CurrentMovementInput H67; UNPCInfo CreatureOwnerName H9, Level H13, AssociatedNode H21; UNPCCombatInfo bIsDead H5,
/// EliteRank H23).
static void test_npc_and_gatherable_schemas() {
    std::printf("NPC, gatherable and interactable schemas (plan E1)\n");
    const ClassSchema* npc = find_class_schema("ANPCCharacter");
    CHECK(npc && npc->is_actor && same(npc->super_name, "ABaseCharacter") && npc->commands.size() == 70 && npc->num_fields() == 91,
          "ANPCCharacter: an ABaseCharacter with 70 commands and 91 net fields (it adds none of its own)");
    CHECK(npc && npc->command("ReplicatedMovement") && npc->command("ReplicatedMovement")->handle == 7 && npc->command("Controller")->handle == 22 &&
              npc->command("ReplicatedServerLastTransformUpdateTimeStamp")->handle == 31 && npc->command("ReplicatedMovementMode")->handle == 33 &&
              npc->command("CurrentMovementInput")->handle == 67 && same(npc->command("CurrentMovementInput")->struct_name, "FPlayerMovementInput"),
          "ANPCCharacter: ReplicatedMovement 7, Controller 22, the timestamp 31, ReplicatedMovementMode 33, CurrentMovementInput 67 (FPlayerMovementInput)");
    CHECK(aoc::protocol::repl::is_known_struct("FPlayerMovementInput"), "FPlayerMovementInput has a codec");
    const ClassSchema* info = find_class_schema("UNPCInfo");
    CHECK(info && !info->is_actor && same(info->super_name, "UCharacterInfo") && info->commands.size() == 22 && info->command("CreatureOwnerName")->handle == 9 &&
              info->command("Level")->handle == 13 && info->command("AssociatedNode")->handle == 21 && info->command("NPCDefinitionGuid")->handle == 22 &&
              info->command("CharacterGuid.D")->handle == 6,
          "UNPCInfo: 22 commands; CharacterGuid 3-6, CreatureOwnerName 9, Level 13, AssociatedNode 21, NPCDefinitionGuid 22");
    const ClassSchema* combat = find_class_schema("UNPCCombatInfo");
    CHECK(combat && same(combat->super_name, "UCharacterCombatInfo") && combat->commands.size() == 25 && combat->command("bIsDead")->handle == 5 &&
              combat->command("EliteRank")->handle == 23 && combat->command("StateTreeBehaviorState")->handle == 24 && combat->command("bInCombat")->handle == 16,
          "UNPCCombatInfo: 25 commands; bIsDead 5, bInCombat 16, EliteRank 23, StateTreeBehaviorState 24");
    const ClassSchema* look = find_class_schema("UNPCAppearanceComponent");
    CHECK(look && same(look->super_name, "UCharacterAppearanceComponent") && look->commands.size() == 12 && look->command("CharacterCustomization")->handle == 7 &&
              look->command("NPCAppearanceId.Guid")->handle == 9 && look->command("NPCDefinitionGuid")->handle == 12,
          "UNPCAppearanceComponent: 12 commands; CharacterCustomization 7, NPCAppearanceId 9-11, NPCDefinitionGuid 12");
    const ClassSchema* second = find_class_schema("UNPCSecondaryInfo");
    CHECK(second && same(second->super_name, "UCharacterSecondaryInfo") && second->commands.size() == 35 && second->command("PetSpawnerEffectId")->handle == 11 &&
              second->command("CreatureOwnerGuid.A")->handle == 22 && second->command("PopInstanceId")->handle == 34 && second->command("PopAssetId")->handle == 35,
          "UNPCSecondaryInfo (the subobject every NPC calls InteractInfo): 35 commands; PetSpawnerEffectId 11, CreatureOwnerGuid 22-25, PopInstanceId 34, PopAssetId 35");
    CHECK(find_component_schema("InteractInfo") == second && find_component_schema("NPCAppearanceComponent") == look,
          "InteractInfo and NPCAppearanceComponent resolve by their export names");
    const ClassSchema* inter = find_class_schema("UInteractableComponent");
    CHECK(inter && same(inter->super_name, "UIntrepidRepKeyComponent") && inter->commands.size() == 17 && inter->num_fields() == 19 && inter->command("ComponentDefId.Guid")->handle == 5 &&
              inter->command("InteractVisibleStateMap.Items") && inter->rpc("ServerInteractWithInteractableComponent") == nullptr,
          "UInteractableComponent: 17 commands, 19 net fields; ComponentDefId 5-7 (the vendor, mount and gatherable interaction)");
    CHECK(find_component_schema("InteractableComponent") == inter, "InteractableComponent resolves by its export name");
    const ClassSchema* plot = find_class_schema("UAoCNodePlotComponent");
    CHECK(plot && plot->commands.size() == 9 && plot->command("NodeRecordGuid")->handle == 3 && plot->command("BuildingRecordGuid")->handle == 5 &&
              plot->command("ConstructionProgress.EndProgress")->handle == 9 && find_component_schema("AoCNodePlot") == plot,
          "UAoCNodePlotComponent (a vendor's AoCNodePlot): 9 commands; NodeRecordGuid 3, PlotRecordGuid 4, BuildingRecordGuid 5, ConstructionProgress 6-9");
    const ClassSchema* gather = find_class_schema("AGatherableActor");
    CHECK(gather && gather->is_actor && same(gather->super_name, "AActor") && gather->commands.size() == 28 && gather->command("GatherableActorDef.Guid")->handle == 20 &&
              gather->command("GatherableGuid.A")->handle == 23 && gather->command("bGathered")->handle == 27 && gather->command("bSuppressed")->handle == 28,
          "AGatherableActor: 28 commands; GatherableActorDef 20-22, GatherableGuid 23-26, bGathered 27, bSuppressed 28");
    // Nodes 1, stage 3a: a town's replicator (96 retail opens; the payload is protocol/repl/node_layout.h)
    const ClassSchema* nl = find_class_schema("ANodeLayoutReplicator");
    const NetField* nlf = nl ? nl->field("LayoutAssetSetGuids") : nullptr;
    CHECK(nl && nl->is_actor && same(nl->super_name, "AActor") && nl->commands.size() == 25 && nl->num_fields() == 15 && nl->first_own_handle == 19 && nl->first_own_field == 13 &&
              nl->command("NodeGuid") && nl->command("NodeGuid")->handle == 25 && nl->command("NodeGuid")->type == PropType::Int64 && nl->command("LayoutAssetSetGuids.PlotChangeVersion")->handle == 24,
          "ANodeLayoutReplicator: 25 commands, 15 net fields; LayoutAssetSetGuids from handle 19 (Items, NodeLevel, NodeCulture, NodeSeason, NodeGameplayTags, PlotChangeVersion), NodeGuid 25");
    CHECK(nlf && (nlf->flags & kRepCustomDelta) && (nlf->flags & kRepFastArray) && (nlf->flags & kRepNotify) && nlf->handle == 19 && nl->elements(*nl->command(19)).size() == 7 &&
              same(nl->elements(*nl->command(19))[4].struct_name, "FQuat") && nl->elements(*nl->command(19))[0].width_bits == 64,
          "LayoutAssetSetGuids is a custom-delta fast array with a repnotify; its item is OwnerGuid, OwnerSecondaryId, OwnerArrayIndex, AssetSetRecordGuid and a transform (FQuat, FVector, FVector)");
}

/// Mounts, stage 1: the MountComponent of a summoned mount.  The retail opens of a ground, a flying and an aquatic mount (captures 911, 871, 898) export it
/// as GroundMountComponent, FlyingMountComponent and AquaticMountComponent, all of the Blueprint class MountComponent_C, which adds no replicated variable.
static void test_mount_component() {
    std::printf("UMountComponent (the MountComponent of a summoned mount, mounts stage 1)\n");
    const ClassSchema& m = kUMountComponent;
    CHECK(same(m.super_name, "UIntrepidRepKeyComponent") && !m.is_actor && m.commands.size() == 12 && m.num_fields() == 11 && m.field_max() == 12,
          "UMountComponent: 12 commands, 11 net fields -> SerializeInt max 12");
    CHECK(cmd_is(m, 3, "bIsPerformingSpecialAction", PropType::Bool, 1) && cmd_is(m, 4, "OriginalMountClass", PropType::Object, 128) &&
              cmd_is(m, 5, "ItemInstanceGuid.A", PropType::Int32, 32) && cmd_is(m, 8, "ItemInstanceGuid.D", PropType::Int32, 32) &&
              cmd_is(m, 9, "bIsDesummoning", PropType::Bool, 1) && cmd_is(m, 10, "bIsSummoning", PropType::Bool, 1) &&
              cmd_is(m, 11, "DissolveTimer", PropType::Float, 32) && cmd_is(m, 12, "MuleStorageState", PropType::Enum, 2),
          "handles: bIsPerformingSpecialAction 3, OriginalMountClass 4, ItemInstanceGuid 5-8, bIsDesummoning 9, bIsSummoning 10, DissolveTimer 11, MuleStorageState 12 (2 bits)");
    CHECK(field_is(m, 0, "bReplicates", FieldKind::Property) && field_is(m, 1, "bIsActive", FieldKind::Property) && field_is(m, 4, "ItemInstanceGuid", FieldKind::Property) &&
              field_is(m, 6, "bIsSummoning", FieldKind::Property) && field_is(m, 9, "InterServerDesummon", FieldKind::Function) &&
              field_is(m, 10, "ServerMountSpecialAction", FieldKind::Function),
          "fields: ItemInstanceGuid 4, bIsSummoning 6, then the two RPCs, InterServerDesummon 9 and ServerMountSpecialAction 10");
    const RpcSchema* desummon = m.rpc("InterServerDesummon");
    const RpcSchema* special = m.rpc("ServerMountSpecialAction");
    CHECK(desummon && desummon->field_index == 9 && (desummon->function_flags & kFuncNet) && desummon->params.size() == 1 && same(desummon->params[0].name, "bForced") &&
              desummon->params[0].type == PropType::Bool && special && special->field_index == 10 && (special->function_flags & kFuncNetServer) && special->params.empty(),
          "InterServerDesummon(bForced) and the client's ServerMountSpecialAction()");
    CHECK(find_component_schema("GroundMountComponent") == &m && find_component_schema("FlyingMountComponent") == &m && find_component_schema("AquaticMountComponent") == &m,
          "the three export names of a mount's component resolve to it");
    CHECK(find_class_schema("UMountComponent") == &m && find_class_schema("MountComponent_C") == &m,
          "the Blueprint class MountComponent_C is an alias of the native class (no header of its own)");
    bool listed = false;
    for (const ClassSchema* s : all_class_schemas()) listed = listed || s == &m;
    CHECK(listed, "UMountComponent is among all_class_schemas()");
}
// (C-O-A | C.Y.B)

static void test_invariants() {
    std::printf("table invariants over every generated schema\n");
    bool handles = true, fields = true, by_path = true, by_name = true, rpcs = true, elems = true, supers = true;
    for (const ClassSchema* s : all_class_schemas()) {
        for (size_t i = 0; i < s->commands.size(); ++i) {
            const RepCommand& c = s->commands[i];
            handles = handles && c.handle == i + 1 && c.path && c.owner;
            if (c.type == PropType::DynamicArray)
                elems = elems && c.elem_begin <= c.elem_end && c.elem_end <= s->element_commands.size();
            else
                elems = elems && c.elem_begin == 0 && c.elem_end == 0;
        }
        for (size_t i = 0; i < s->fields.size(); ++i) {
            const NetField& f = s->fields[i];
            fields = fields && f.index == i && f.name && f.owner;
            if (f.kind == FieldKind::Function) {
                rpcs = rpcs && f.rpc < s->rpcs.size() && same(s->rpcs[f.rpc].name, f.name) && s->rpcs[f.rpc].field_index == i &&
                       (f.function_flags & kFuncNet);
            } else {
                rpcs = rpcs && f.rpc == kNoRpc && f.handle >= 1 && f.handle <= s->commands.size() && f.function_flags == 0;
            }
        }
        by_path = by_path && s->commands_by_path.size() == s->commands.size();
        for (size_t i = 0; i + 1 < s->commands_by_path.size(); ++i)
            by_path = by_path && std::strcmp(s->commands[s->commands_by_path[i] - 1].path,
                                             s->commands[s->commands_by_path[i + 1] - 1].path) <= 0;
        for (const RepCommand& c : s->commands)
            by_path = by_path && s->command(c.path) && same(s->command(c.path)->path, c.path);
        by_name = by_name && s->fields_by_name.size() == s->fields.size();
        for (size_t i = 0; i + 1 < s->fields_by_name.size(); ++i)
            by_name = by_name && std::strcmp(s->fields[s->fields_by_name[i]].name, s->fields[s->fields_by_name[i + 1]].name) < 0;
        for (const NetField& f : s->fields)
            by_name = by_name && s->field(f.name) == &f;
        for (const RpcSchema& r : s->rpcs)
            for (const RpcParam& p : r.params)
                rpcs = rpcs && p.cmd_begin <= p.cmd_end && p.cmd_end <= r.commands.size() && (p.send_bit == (p.type != PropType::Bool));
        supers = supers && (same(s->super_name, "UObject") || find_class_schema(s->super_name) != nullptr);
    }
    CHECK(handles, "commands[i].handle == i + 1 with a path and owner");
    CHECK(fields, "fields[i].index == i with a name and owner");
    CHECK(by_path, "commands_by_path is sorted and finds every command");
    CHECK(by_name, "fields_by_name is sorted (names unique) and finds every field");
    CHECK(rpcs, "function fields <-> RPC schemas agree; property fields point at their handles");
    CHECK(elems, "element command ranges are in bounds");
    CHECK(supers, "every super class has a schema (UObject excepted)");
}

static void test_index() {
    std::printf("index: native, Blueprint and component names\n");
    CHECK(find_class_schema("AAoCPlayerController") == &kAAoCPlayerController, "AAoCPlayerController by native name");
    CHECK(find_class_schema("AoCPlayerControllerBP_C") && same(find_class_schema("AoCPlayerControllerBP_C")->super_name, "AAoCPlayerController"),
          "AoCPlayerControllerBP_C derives from AAoCPlayerController");
    const ClassSchema* gs = find_class_schema("AoCGameStateBP_C");
    CHECK(gs && same(gs->name, "AoCGameStateBP_C") && same(gs->super_name, "AAoCGameStateBase") &&
          gs->num_fields() == find_class_schema("AAoCGameStateBase")->num_fields() &&
          gs->commands.size() == find_class_schema("AAoCGameStateBase")->commands.size(),
          "AoCGameStateBP_C adds nothing to AAoCGameStateBase");
    const ClassSchema* ps = find_class_schema("AoCPlayerStateBP_C");
    CHECK(ps && same(ps->super_name, "AAoCPlayerState") && ps->num_fields() == find_class_schema("AAoCPlayerState")->num_fields(),
          "AoCPlayerStateBP_C adds nothing to AAoCPlayerState");
    CHECK(find_class_schema("NoSuchClass") == nullptr, "unknown name -> nullptr");
    CHECK(find_component_schema("BaseCharacterInfo") == &kUPlayerInfo, "BaseCharacterInfo -> UPlayerInfo");
    CHECK(find_component_schema("StatsComponent") == &kUAoCStatsComponent, "StatsComponent -> UAoCStatsComponent");
    CHECK(find_component_schema("AbilityComponent") == &kUAoCAbilityComponent, "AbilityComponent -> UAoCAbilityComponent");
    CHECK(find_component_schema("Character Appearance") && find_component_schema("OwnerInfo") &&
          find_component_schema("AlignmentComponent") && find_component_schema("CombatInfo"),
          "the other pawn components resolve");
    CHECK(find_component_schema("BackpackComponent") == find_component_schema("EquipmentComponent") &&
          find_component_schema("QuestStorageComponent") == find_component_schema("BackpackComponent") &&
          find_component_schema("RewardStorageComponent") &&
          same(find_component_schema("RewardStorageComponent")->super_name, "UItemStorageComponent"),
          "storage components are UItemStorageComponent (RewardStorage its subclass)");
    bool sorted = true;
    auto all = all_class_schemas();
    for (size_t i = 0; i + 1 < all.size(); ++i) sorted = sorted && std::strcmp(all[i]->name, all[i + 1]->name) < 0;
    CHECK(all.size() == 52 && sorted, "52 schemas (48 native, 4 Blueprint), sorted by name (an alias is not one of them); AIntrepidStateIFA and UIntrepidFoliageStateComponent joined with Gathering stage 3");
    const ClassSchema* res = find_class_schema("UResourceStorageComponent");   // the pawn's dynamic subobject (pkt#107)
    CHECK(res && !res->is_actor && same(res->super_name, "UItemStorageComponent") && res->command("ItemLocationType") &&
              res->command("ItemLocationType")->type == PropType::Enum && res->command("ItemLocationType")->width_bits == 5,
          "UResourceStorageComponent: a UItemStorageComponent with ItemLocationType, a 5-bit enum");
    CHECK(!kUAoCStatsComponent.is_actor && kAAoCPlayerController.is_actor, "is_actor distinguishes components from actors");
}

int main() {
    test_actor_and_pawn_handles();
    test_player_controller();
    test_pawn();
    test_components();
    test_leveling_component();
    test_npc_and_gatherable_schemas();
    test_mount_component();
    test_invariants();
    test_index();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

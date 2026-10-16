// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/constants.h
//
//  What worldd's bootstrap copies from the original server, each with where it
//  was seen (fixtures/replay_data.bin packet numbers, docs/world-bootstrap-
//  transcript.md sections, tools/ and the export table of the transcript's
//  scratch folder).  Same for every player and server: class and asset hashes
//  with their paths and checksums, class defaults that the client needs sent
//  (the GameState's combat settings), and values the capture shows and nothing
//  computes yet (the load tracker's flags, the clock formula).
//
//  The big tables are generated, not here: captured_stats.h, captured_abilities.h,
//  captured_level_list.h, captured_gamestate.h, captured_character.h and
//  captured_replies.h (python tools/capture/gen_all.py).
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/rep_state.h"
#include "protocol/repl/subobject.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace aoc::worldd::bootstrap {

namespace emit = ::aoc::protocol::emit;
namespace repl = ::aoc::protocol::repl;

// ---- export chains ---------------------------------------------------------------------------------------------------
// An export chain is an object, its path name and checksum, then its package, each with the flags the original wrote:
// 0x5 path + checksum for an asset or a class, 0x7 with no-load for a level and a component (transcript section 1.3,
// export_table.md). Static objects have ServerId 0 and Randomizer 0.

/// Default__AoCPlayerControllerBP_C in /Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP (pkt#22, ch 3).
repl::ExportChain pc_archetype_export();
/// Default__AoCPlayerStateBP_C in /Game/ThirdPersonCPP/Blueprints/AoCPlayerStateBP (pkt#434, ch 193).
repl::ExportChain player_state_archetype_export();
/// Default__PlayerPawn_C in /Game/ThirdPersonCPP/Blueprints/PlayerPawn (pkt#78, ch 19).
repl::ExportChain pawn_archetype_export();
/// Default__AoCGameStateBP_C in /Game/GameBlueprints/AoCGameStateBP (pkt#28, ch 6).
repl::ExportChain game_state_archetype_export();
/// PersistentLevel, Verra_World_Master and /Game/Levels/Verra_World_Master/Verra_World_Master, flags 7 each
/// (pkt#22, #28, #78).
repl::ExportChain persistent_level_export();
/// /Script/GameSystemsPlugin.GlobalGMCommands, the PlayerController's dynamic subobject class (pkt#22).
repl::ExportChain global_gm_commands_class_export();
/// Default__AoCAbilityLingeringEffect in /Script/GameSystemsPlugin: the archetype of a lingering zone's actor (plan D2, review; retail capture 44, the
/// actor opens at packet 302: id 11890361721422188155, checksum 3159250838).
repl::ExportChain lingering_effect_archetype_export();
/// Default__NodeLayoutReplicator in /Script/GameSystemsPlugin: the archetype of a town's scenery replicator (Nodes 1, stage 3a; 96 opens in 15 retail captures, all the same:
/// id 1074022900247277291, checksum 4242000812; tools/capture/gen_node_layout.py reads it from them).
repl::ExportChain node_layout_archetype_export();
/// /Script/GameSystemsPlugin.ResourceStorageComponent, the pawn's dynamic subobject class (pkt#107).
repl::ExportChain resource_storage_class_export();
/// Default__IntrepidStateIFA in /Script/Foliage: the archetype of a foliage cell's state actor (Gathering stage 3, "Foliage gathering"; capture 1198's export at packet 244:
/// id 18292086725290287385, checksum 3015814482, the package 12703675336461722793).
repl::ExportChain foliage_state_actor_archetype_export();
/// /Script/Foliage.IntrepidFoliageStateComponent, the state actor's dynamic subobject class (capture 1198, packet 244: id 8959184318458680037, checksum 480712344, outer the same package).
repl::ExportChain foliage_state_component_class_export();
/// BP_AOCHUD_C in /Game/UI/Widgets/BP_AOCHUD, ClientSetHUD's parameter (pkt#22 chSeq 956).
repl::ExportChain hud_class_export();
/// AoCGameModeBaseBP_C in /Game/GameBlueprints/AoCGameModeBaseBP, the GameState's GameModeClass (pkt#28).
repl::ExportChain game_mode_class_export();
// ~ C_Y_B & CoA ~

/// The GUID of /Game/GameBlueprints/AoCGameModeBaseBP.AoCGameModeBaseBP_C: a property value, with the export above.
emit::FIntrepidNetworkGUID game_mode_class_guid();
emit::FIntrepidNetworkGUID hud_class_guid();

/// A montage worldd can name in a ReplicatedActiveAbilities item: its GUID (a static asset's, ServerId 0) and the export
/// chain that gives the client its path. Static GUIDs are not computed from the path (a hash no one has found), so only the
/// montages a capture shows are known: the General_Rest montage (replay_data.bin, pkt#8175), and, from the retail captures
/// of 2026-02 (research 2026-10-02: the same ids in every session and on every server), the Sprint montage, the four Dodge
/// montages the client asks for by AnimationIndex and the Rogue's dash.
struct MontageRef {
    emit::FIntrepidNetworkGUID guid;
    repl::ExportChain          chain;
};

/// The Rest montage /Game/Characters/Animations/00_ABLT/CM_M_ABLT_General_Rest_Montage (pkt#8175: the export rides in the
/// first partial bunch of the first Rest update, flags 5 path + checksum for the montage and its package).
inline constexpr std::string_view kRestMontagePath =
    "/Game/Characters/Animations/00_ABLT/CM_M_ABLT_General_Rest_Montage.CM_M_ABLT_General_Rest_Montage";

/// The Sprint's montage, CM_M_ABLT_Sprint_Montage (id 9309136633382094689), and the MontageGroup of its item.  The group is
/// "Sprint"; every other ability's item says "DefaultGroup" (retail: 100 of 100 Sprint replies).
inline constexpr std::string_view kSprintMontagePath =
    "/Game/Characters/Animations/00_ABLT/CM_M_ABLT_Sprint_Montage.CM_M_ABLT_Sprint_Montage";
inline constexpr std::string_view kSprintMontageGroup = "Sprint";

/// The montage with soft path `path` ("/Game/.../X.X", an Ability row's AbilityAnimationSoftPaths), when its GUID is known.
std::optional<MontageRef> montage_for(std::string_view path);

/// Plan D2: a montage whose object id a retail capture names (ab_montage.net_guid, the same in every session and on every server) but whose
/// package export no capture shows (the captures start mid-session): the object keeps the retail id, the package gets an id of its own
/// made from it (a client takes any unused id for the package: the server mints them), and the export is the same chain as the table's.
/// nullopt for a path that is no "<package>.<name>" or an id of 0.
std::optional<MontageRef> montage_for(std::string_view path, uint64_t net_guid);

/// The Dodge montage the retail server named for a ServerTriggerAbility's AnimationIndex (a Dodge reply's item carries the
/// montage of the index the client chose, whatever the class): 5 forward, 7 right, 8 back, 9 no input, and 2, the Rogue's
/// dash to the right (31 Dodges in 16 captures; the indexes 5, 7, 8, 9 and 2 are all the captures show, so any other index
/// has no montage here: Montage unset).  `axis_degrees` is the direction the montage moves in, as the angle of an input vector
/// (atan2(y, x): forward 0, right 90, back 180); `fixed_yaw` the MeshYaw it always has (the no-input montage: 32768 = 180
/// degrees), when it has one.
struct DodgeMontage {
    std::string_view        path;
    double                  axis_degrees = 0;
    std::optional<uint16_t> fixed_yaw;
};
std::optional<DodgeMontage> dodge_montage_for(int32_t animation_index);

/// The MeshYaw of a Dodge item: the montage's `fixed_yaw`, else the angle of the input from the montage's axis as a 16-bit
/// angle (retail: (1, 0) forward 0, (0, 1) right 0, (-1, 0) back 0, and the diagonal (1, -1) on the forward montage 57344 =
/// 315 degrees; the only two diagonals the captures show).  0 for no input or an input that is not a finite number.
uint16_t dodge_mesh_yaw(const DodgeMontage& montage, double input_x, double input_y);

/// The checksums of the pawn's components' exports (flags 7, the pawn as outer; pkt#78, #107). The four storage
/// components share one, CollisionCylinder's is also TransformComponent0's.
inline constexpr uint32_t kChecksumBaseCharacterInfo = 0xaff9746f;
inline constexpr uint32_t kChecksumCombatInfo        = 0xb9f1a7bc;
inline constexpr uint32_t kChecksumOwnerInfo         = 0xf05c630d;
inline constexpr uint32_t kChecksumItemStorage       = 0x24e4d927;   // Backpack, Equipment, QuestStorage, RewardStorage
inline constexpr uint32_t kChecksumAppearance        = 0x864dde37;
inline constexpr uint32_t kChecksumAlignment         = 0x3fbd829c;
inline constexpr uint32_t kChecksumAbility           = 0x9e63ae5c;
inline constexpr uint32_t kChecksumStats             = 0xf04938d2;
inline constexpr uint32_t kChecksumCollisionCylinder = 0xb876932f;
/// A lingering zone's actor [R: capture 44 packet 302]: its stably named subobjects and the pawn's mesh it attaches to.
inline constexpr uint32_t kChecksumLingeringMovement = 1600984696u;   // LingeringEffectMovementComp
inline constexpr uint32_t kChecksumLingeringAbility  = 0x9e63ae5c;    // AbilityComp (the AbilityComponent's checksum)
inline constexpr uint32_t kChecksumLingeringStats    = 0xf04938d2;    // StatsComp
inline constexpr uint32_t kChecksumPawnMesh          = 3094778671u;   // CharacterMesh0 (the CollisionCylinder's checksum)
/// The pawn's InteractInfo (a UCharacterSecondaryInfo with MountPtr: the player's own default component) [R: capture 1198 packet 75: flags 7, the pawn as outer], and the
/// AttachmentState.InteractableZoneID of a rider [R: 910 packet 310].
inline constexpr uint32_t kChecksumInteractInfo      = 3257521646u;
inline constexpr uint64_t kMountInteractableZone     = 2;
inline constexpr uint32_t kChecksumXPTracker         = 0x037669d1;   // plan D1: the PlayerController's ULevelingComponent (transcript section 4.2)

// ---- the PlayerController and the pawn ---------------------------------------------------------------------------------

/// PlayerController.CharacterLoadTracker's flags (25, one 32-bit word each on the wire; transcript sections 1.3, 4.2):
/// four values in the original's order. Their meaning is not decoded: flag 0 flips at possession, flag 1 is clear before it.
inline constexpr std::string_view kLoadTrackerFirst        = "1000000000000000000000000";   // pkt#46, unreliable
inline constexpr std::string_view kLoadTrackerSecond       = "1011111110000110000001110";   // pkt#50, unreliable
inline constexpr std::string_view kLoadTrackerBeforePossession = "1011111111111111111111111";   // pkt#72, reliable
inline constexpr std::string_view kLoadTrackerAfterPossession  = "0111111111111111111111111";   // pkt#103, reliable
repl::LoadTracker load_tracker(std::string_view flags);

/// ClientCapBandwidth(Cap) (pkt#23).
inline constexpr int64_t kBandwidthCap = 200000;
/// ClientSetCameraMode(NewCamMode): the hardcoded FName EName(204) (pkt#97).
inline constexpr uint32_t kCameraModeEName = 204;
/// UPlayerInfo.CurrentBreathingTime at the pawn's open (pkt#78).
inline constexpr float kBreathingTime = 30.0f;
/// AttachmentReplication.RelativeScale3D of an attached actor: (1, 1, 1) as FVector_NetQuantize100's integers (pkt#103).
inline constexpr int64_t kRelativeScale100 = 100;
/// The class default the original server set on every actor: AuthServerIDReplicated 0 (pkt#22, #28, #78, #434).
inline constexpr uint64_t kAuthServerIdReplicated = 0;

/// The AlignmentComponent's four instances (pkt#107): three constant alignment keys with a zero Guid set, and the one
/// whose Guid set is the character's own Guid.
inline constexpr uint64_t kAlignmentKeyA = 6064629444695359488ull;
inline constexpr uint64_t kAlignmentKeyB = 6064629443712188425ull;
inline constexpr uint64_t kAlignmentKeyCharacter = 6064630243140894720ull;
inline constexpr uint64_t kAlignmentKeyC = 6064631980646400000ull;

// ---- the clock --------------------------------------------------------------------------------------------------------

/// ClientSetTime(NewGameTime, NewSynchronizedTime) from the server's world time. Derived from the eight samples of the
/// capture (pkt#23 and every 120 s after) and explained by nothing: NewSynchronizedTime is the world time plus a constant,
/// NewGameTime nine times that plus another (both hold to within the sampling jitter of +-0.5 s; transcript section 4.2).
inline constexpr double kSynchronizedTimeOffset = 13468184.6;
/// The clock of an effect's StartTime and of a cooldown's start and end (plan A7): the same synchronized clock, so
/// `StartTime = world time + kSynchronizedTimeOffset`.  Evidence: in pkt#8228 and #8301 the StartTime of the two Rest
/// effects (15,061,867.7 and 15,061,870.1) is 13,468,184.5 s ahead of the GameState's ReplicatedWorldTimeSecondsDouble at
/// the packet, both times, while ClientSetTime's NewSynchronizedTime is 13,468,184.6 ahead of it (the eight samples of
/// pkt#23 and every 120 s after: two estimates of one constant, within the +-0.5 s sampling jitter).  The clock of the
/// client's own prediction is not asked for: an infinite effect's StartTime is only its age.
inline double effect_clock(double world_time) { return world_time + kSynchronizedTimeOffset; }
inline constexpr double kGameTimeFactor = 9.0;
inline constexpr double kGameTimeOffset = 39389245.0;

struct ClientTimes {
    double game_time = 0;
    double synchronized_time = 0;
};
ClientTimes client_times(double world_time);

// ---- the storages ------------------------------------------------------------------------------------------------------

/// Gathering stage 3: the resource storage is three (Resource0 to Resource2, the dynamic UResourceStorageComponents of the resource bags worn in Equipment slots 23 to 25).
enum class StorageKind : uint8_t { Backpack, Equipment, QuestStorage, Resource0, Resource1, Resource2 };
inline constexpr bool is_resource_kind(StorageKind k) { return k >= StorageKind::Resource0; }
/// 0, 1 or 2 for Resource0 to Resource2.
inline constexpr int resource_kind_index(StorageKind k) { return static_cast<int>(k) - static_cast<int>(StorageKind::Resource0); }
inline constexpr StorageKind resource_kind(int index) { return static_cast<StorageKind>(static_cast<int>(StorageKind::Resource0) + index); }

/// ClientReceiveStoragePayload(TargetStorage, NewStorageData) of an empty storage: what differs between the four
/// (MaximumItems and the Dimension of the resource storage; pkt#82-97, transcript section 1.3).  A resource container's size is its bag's
/// (PlayerSetup::resource_bags); the entry here is the starter's, the Sanctus bag's 6 x 4.
struct StorageInfo {
    StorageKind kind;
    const char* component_name;   // the export name; the resource storage has none (a dynamic subobject)
    int32_t     slots;            // NewStorageData.MaximumItems, and the Storage array's count
    double      dimension_x, dimension_y;   // NewStorageData.Dimension
};
const StorageInfo& storage_info(StorageKind kind);

// ---- the GameState --------------------------------------------------------------------------------------------------------

/// The GameState's handles Tier 2 adds to Tier 1: GameTimeScalar, the 25 CombatSettings values the original server
/// sent and ServiceConnectionFlags (pkt#28), as typed values by RepLayout handle, read from the captured bits
/// (captured_gamestate.h). nullopt when they do not read: a library or capture mismatch.
std::optional<repl::RepState> game_state_tier2_values();

}  // namespace aoc::worldd::bootstrap

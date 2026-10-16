// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/constants.cpp
//
//  See constants.h. Every id, path and checksum below is the original server's,
//  from its export bunches in fixtures/replay_data.bin (packet numbers in
//  constants.h); the test_worldd_bootstrap golden tests compare each chain's
//  bits with the capture.
// ============================================================================
#include "bootstrap/constants.h"

#include "bootstrap/captured_gamestate.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"

#include <cmath>

namespace aoc::worldd::bootstrap {

using protocol::emit::FIntrepidNetworkGUID;
using protocol::repl::ExportChain;
using protocol::repl::ExportLink;

namespace {

/// A static object: ServerId 0, Randomizer 0. Every captured path is flagged with a checksum (a package's is 0).
ExportLink link(uint64_t id, const char* name, bool no_load, uint32_t checksum) {
    ExportLink l;
    l.guid.ObjectId = id;
    l.name = name;
    l.no_load = no_load;
    l.has_checksum = true;
    l.checksum = checksum;
    return l;
}

ExportChain asset(uint64_t id, const char* name, uint32_t checksum, uint64_t package_id, const char* package) {
    return {link(id, name, false, checksum), link(package_id, package, false, 0)};
}

constexpr uint64_t kGameSystemsPackageId = 158953490572197689ull;   // /Script/GameSystemsPlugin

ExportChain gameplay_class(uint64_t id, const char* name, uint32_t checksum) {
    return {link(id, name, false, checksum), link(kGameSystemsPackageId, "/Script/GameSystemsPlugin", false, 0)};
}

}  // namespace

ExportChain pc_archetype_export() {
    return asset(3503756484819958835ull, "Default__AoCPlayerControllerBP_C", 0x6b62891c, 4074085207143396457ull,
                 "/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP");
}

ExportChain player_state_archetype_export() {
    return asset(16037722909354717791ull, "Default__AoCPlayerStateBP_C", 0xf4f44182, 1938806816082851941ull,
                 "/Game/ThirdPersonCPP/Blueprints/AoCPlayerStateBP");
}

ExportChain pawn_archetype_export() {
    return asset(9327572450991073355ull, "Default__PlayerPawn_C", 0x2cdc5b6, 9839620482321143831ull,
                 "/Game/ThirdPersonCPP/Blueprints/PlayerPawn");
}

ExportChain game_state_archetype_export() {
    return asset(8237483582406389987ull, "Default__AoCGameStateBP_C", 0x1442053, 14291167106959361475ull,
                 "/Game/GameBlueprints/AoCGameStateBP");
}

ExportChain persistent_level_export() {
    return {link(16442478405498561049ull, "PersistentLevel", true, 0), link(16604466839667550161ull, "Verra_World_Master", true, 0),
            link(12923414834320654503ull, "/Game/Levels/Verra_World_Master/Verra_World_Master", true, 0)};
}

ExportChain global_gm_commands_class_export() { return gameplay_class(485698175673737329ull, "GlobalGMCommands", 0xcaaaee3e); }

ExportChain lingering_effect_archetype_export() {
    return gameplay_class(11890361721422188155ull, "Default__AoCAbilityLingeringEffect", 3159250838u);
}

ExportChain node_layout_archetype_export() {
    return gameplay_class(1074022900247277291ull, "Default__NodeLayoutReplicator", 4242000812u);
}
// C-O-A with C_Y_B

ExportChain resource_storage_class_export() {
    return gameplay_class(11144499416456600109ull, "ResourceStorageComponent", 0xf3927ccd);
}

namespace {
constexpr uint64_t kFoliagePackageId = 12703675336461722793ull;   // /Script/Foliage
}  // namespace

ExportChain foliage_state_actor_archetype_export() {
    return {link(18292086725290287385ull, "Default__IntrepidStateIFA", false, 3015814482u), link(kFoliagePackageId, "/Script/Foliage", false, 0)};
}

ExportChain foliage_state_component_class_export() {
    return {link(8959184318458680037ull, "IntrepidFoliageStateComponent", false, 480712344u), link(kFoliagePackageId, "/Script/Foliage", false, 0)};
}

ExportChain hud_class_export() {
    return asset(8463636446363390223ull, "BP_AOCHUD_C", 0xac6f1251, 7805116325763694055ull, "/Game/UI/Widgets/BP_AOCHUD");
}

ExportChain game_mode_class_export() {
    return asset(17588721769397045729ull, "AoCGameModeBaseBP_C", 0xac6f1251, 8496360648866442349ull,
                 "/Game/GameBlueprints/AoCGameModeBaseBP");
}

namespace {

/// A montage with its export: the object {id, ServerId 0, flags 5, name, checksum 0} and its package {id, flags 5, the path
/// without the object's name}, as the retail server exports them in the bunch of the first item that names the montage.
struct MontageDef {
    std::string_view path;       // the soft path, "<package>.<name>"
    const char*      name;
    uint64_t         id;
    uint64_t         package_id;
    const char*      package;
};

constexpr MontageDef kMontages[] = {
    {kRestMontagePath, "CM_M_ABLT_General_Rest_Montage", 3687208092518343773ull, 5464769470103251023ull,   // pkt#8175
     "/Game/Characters/Animations/00_ABLT/CM_M_ABLT_General_Rest_Montage"},
    {kSprintMontagePath, "CM_M_ABLT_Sprint_Montage", 9309136633382094689ull, 16661626241804687149ull,   // retail capture 1030
     "/Game/Characters/Animations/00_ABLT/CM_M_ABLT_Sprint_Montage"},
    {"/Game/Characters/Animations/00_ABLT/CM_M_BASE_DodgeFwd_v2_Notify_Montage_ALS.CM_M_BASE_DodgeFwd_v2_Notify_Montage_ALS",
     "CM_M_BASE_DodgeFwd_v2_Notify_Montage_ALS", 3289872098355792151ull, 2636644977518272439ull,   // capture 1001
     "/Game/Characters/Animations/00_ABLT/CM_M_BASE_DodgeFwd_v2_Notify_Montage_ALS"},
    {"/Game/Characters/Animations/00_ABLT/CM_M_BASE_DodgeRight_v2_Notify_Montage_ALS.CM_M_BASE_DodgeRight_v2_Notify_Montage_ALS",
     "CM_M_BASE_DodgeRight_v2_Notify_Montage_ALS", 1215243220362629129ull, 16591392575501622541ull,   // capture 1198
     "/Game/Characters/Animations/00_ABLT/CM_M_BASE_DodgeRight_v2_Notify_Montage_ALS"},
    {"/Game/Characters/Animations/00_ABLT/CM_M_BASE_DodgeBack_v2_Notify_Montage_ALS.CM_M_BASE_DodgeBack_v2_Notify_Montage_ALS",
     "CM_M_BASE_DodgeBack_v2_Notify_Montage_ALS", 858903896054330709ull, 16498051188094664897ull,   // capture 1195
     "/Game/Characters/Animations/00_ABLT/CM_M_BASE_DodgeBack_v2_Notify_Montage_ALS"},
    {"/Game/Characters/Animations/00_ABLT/CM_M_BASE_DodgeBack_v2_NoInput_Notify_Montage_ALS.CM_M_BASE_DodgeBack_v2_NoInput_Notify_Montage_ALS",
     "CM_M_BASE_DodgeBack_v2_NoInput_Notify_Montage_ALS", 11479915692035936417ull, 5290943011317832207ull,   // captures 741, 217
     "/Game/Characters/Animations/00_ABLT/CM_M_BASE_DodgeBack_v2_NoInput_Notify_Montage_ALS"},
    {"/Game/Characters/Animations/00_ABLT/Rogue/CM_M_ABLT_RogueDash_Right_Montage.CM_M_ABLT_RogueDash_Right_Montage",
     "CM_M_ABLT_RogueDash_Right_Montage", 10081100002258589133ull, 16102130323274345795ull,   // capture 742
     "/Game/Characters/Animations/00_ABLT/Rogue/CM_M_ABLT_RogueDash_Right_Montage"},
};

/// AnimationIndex -> the Dodge montage (an entry of kMontages), its axis and fixed yaw.
struct DodgeIndex {
    int32_t                 index;
    std::string_view        path;
    double                  axis_degrees;
    std::optional<uint16_t> fixed_yaw;
};

constexpr DodgeIndex kDodgeIndexes[] = {
    {5, kMontages[2].path, 0.0, std::nullopt},
    {7, kMontages[3].path, 90.0, std::nullopt},
    {8, kMontages[4].path, 180.0, std::nullopt},
    {9, kMontages[5].path, 180.0, uint16_t{32768}},
    {2, kMontages[6].path, 90.0, std::nullopt},
};

}  // namespace

std::optional<MontageRef> montage_for(std::string_view path) {
    for (const MontageDef& d : kMontages) {
        if (d.path != path) continue;
        MontageRef m;
        m.guid.ObjectId = d.id;
        m.chain = asset(d.id, d.name, 0, d.package_id, d.package);
        return m;
    }
    return std::nullopt;
}

std::optional<MontageRef> montage_for(std::string_view path, uint64_t net_guid) {
    if (net_guid == 0) return std::nullopt;
    if (const auto known = montage_for(path)) return known;   // a montage a capture exported keeps its own ids
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= path.size()) return std::nullopt;
    const std::string package(path.substr(0, dot)), name(path.substr(dot + 1));
    // the package's id: the object's, mixed so that it is another id and never 0 (the same montage always gets the same one)
    uint64_t package_id = (net_guid ^ 0x5041434B41474500ull) * 0x9E3779B97F4A7C15ull;
    if (package_id == 0 || package_id == net_guid) package_id = ~net_guid;
    MontageRef m;
    m.guid.ObjectId = net_guid;
    m.chain = asset(net_guid, name.c_str(), 0, package_id, package.c_str());
    return m;
}

std::optional<DodgeMontage> dodge_montage_for(int32_t animation_index) {
    for (const DodgeIndex& d : kDodgeIndexes) {
        if (d.index == animation_index) return DodgeMontage{d.path, d.axis_degrees, d.fixed_yaw};
    }
    return std::nullopt;
}

uint16_t dodge_mesh_yaw(const DodgeMontage& montage, double input_x, double input_y) {
    if (montage.fixed_yaw) return *montage.fixed_yaw;
    if (!std::isfinite(input_x) || !std::isfinite(input_y) || (input_x == 0 && input_y == 0)) return 0;
    constexpr double kPi = 3.14159265358979323846;
    double degrees = std::atan2(input_y, input_x) * 180.0 / kPi - montage.axis_degrees;
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0) degrees += 360.0;
    return static_cast<uint16_t>(std::llround(degrees / 360.0 * 65536.0) & 0xFFFF);
}

FIntrepidNetworkGUID game_mode_class_guid() {
    FIntrepidNetworkGUID g;
    g.ObjectId = 17588721769397045729ull;
    return g;
}

FIntrepidNetworkGUID hud_class_guid() {
    FIntrepidNetworkGUID g;
    g.ObjectId = 8463636446363390223ull;
    return g;
}
// C | Y | B.C.O.A

repl::LoadTracker load_tracker(std::string_view flags) {
    repl::LoadTracker t;
    for (const char c : flags) t.flags.push_back(c == '1');
    return t;
}

ClientTimes client_times(double world_time) {
    ClientTimes t;
    t.synchronized_time = world_time + kSynchronizedTimeOffset;
    t.game_time = kGameTimeFactor * t.synchronized_time + kGameTimeOffset;
    return t;
}

const StorageInfo& storage_info(StorageKind kind) {
    // MaximumItems of the four payloads (pkt#82, #89, #93, #97); the resource storage is a 6 x 4 grid, the others
    // have no Dimension (all zero bits).
    static const StorageInfo kInfo[] = {
        {StorageKind::Backpack, "BackpackComponent", 70, 0, 0},
        {StorageKind::Equipment, "EquipmentComponent", 83, 0, 0},
        {StorageKind::QuestStorage, "QuestStorageComponent", 100, 0, 0},
        {StorageKind::Resource0, "", 24, 6, 4},
        {StorageKind::Resource1, "", 24, 6, 4},
        {StorageKind::Resource2, "", 24, 6, 4},
    };
    return kInfo[static_cast<size_t>(kind)];
}

std::optional<repl::RepState> game_state_tier2_values() {
    const auto* schema = protocol::schema::gen::find_class_schema("AoCGameStateBP_C");
    if (!schema) return std::nullopt;
    const captured::CapturedBits& b = captured::kGameStateTier2Block;
    const auto bytes = captured::bytes_of(b);
    size_t pos = 0;
    auto st = repl::read_rep_block(*schema, bytes.data(), b.bits, pos);
    if (!st || pos != b.bits) return std::nullopt;
    return st;
}

}  // namespace aoc::worldd::bootstrap

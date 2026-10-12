// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/node_rpc.h  (Nodes 1, stage 2)
//
//  What the server says about city nodes, in a typed form over the library's fast arrays and RPC parameter sets.
//
//    the GameState's node maps (fast arrays of AAoCGameStateBase, retail's login order: capture 1198 line 47, a server hop 623 lines 3911 and 15639)
//        field 25  EstateIDToOwnerGuid               Key = the estate record (FreeholdEstateRecordBase), Value = the owner's FGuid (zero: no owner)
//        field 26  NodeIDToClientNodeData            Key = the node's CityNodeRecordBase guid; Value = FClientNodeData: CurrentLevel, CurrentCulture, Mayor
//                                                    (FGuid + name), ParentNodeGuid, PlayerCommodityOnBounty, NodeGameplayTags, VehicleDeployRadius
//        field 28  NodePercentLevelCompleteFastMap   Key = the node's RANK (uint8), Value = the percent of the current level (uint8)
//        field 29  NodeNumberOfCitizensFastMap       Key = the node's RANK (uint32), Value = the citizens (uint8, saturating)
//      The RANK is the index of the node's guid in the sorted guids of all CityNodeRecordBase records (81), not a guid: the client builds the same order from its
//      own table (docs: research/evidence.md section 4.2).  The percent map holds the nodes at level >= 1, the citizens map the nodes at level >= 3.
//      sync_node_maps brings the four FastArrayStates to a NodeStateMaps (adds what is new, changes what moved, removes what went), so the same call builds the
//      join's arrays and the live update after a level change: the writer sends only the difference to what the connection has.
//
//    the PlayerController's RPCs (AAoCPlayerController)
//        ClientReceiveNodeSummary(Summary: FAoCNodeSummary)            the answer to ServerRequestNodeSummary(Node)
//        ClientReceiveSiegeSummary(SiegeData: FSiegeDisplayData)       the answer to ServerRequestCurrentSiegeSummary(NodeId); an idle node's is the default
//        ClientReceiveAllCurrentNodeSiegeCooldowns(Cooldowns[])        the answer to ServerRequestAllNodeSiegeCooldowns; no cooldown: an empty array, one 0 send bit
//      The parameter sets are built whole (every member of a flattened struct has a value; rpc.h) from small plain structs; a retail call read back with
//      read_rpc_params has the same shape, so the tests compare the two member by member.
// ============================================================================
#pragma once

#include "protocol/repl/fast_array.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/value.h"
#include "protocol/schema/generated/schema_types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

// ---- the GameState's maps --------------------------------------------------------------------------------------------------------------------------

/// The tag every retail node carries in the GameState and in the replicator: no siege is on (War.Siege.States.SiegeInactive).
inline constexpr const char* kSiegeInactiveTag = "War.Siege.States.SiegeInactive";

/// One node as the client's GameState holds it (FClientNodeData).
struct NodeDataEntry {
    int64_t                  guid = 0;           // the key
    uint8_t                  level = 0;          // ENodeLevel: Wilderness 0 ... Metropolis 6
    uint8_t                  culture = 0;        // ECharacterRace (Kaelar 0)
    std::array<uint32_t, 4>  mayor_guid{};       // the mayor's character (zero: no mayor)
    std::string              mayor_name;
    int64_t                  parent = 0;         // the lord node (0: none)
    int64_t                  bounty = 0;         // PlayerCommodityOnBounty: an item record
    std::vector<std::string> tags;               // NodeGameplayTags
    float                    deploy_radius = 0;  // VehicleDeployRadius, cm
    bool operator==(const NodeDataEntry&) const = default;
};

/// The four maps, in the order of their arrays.  Keys of the two small maps are ranks.
struct NodeStateMaps {
    std::vector<NodeDataEntry>                                  nodes;
    std::vector<std::pair<uint32_t, uint8_t>>                   percent;    // (rank, percent of the current level)
    std::vector<std::pair<uint32_t, uint8_t>>                   citizens;   // (rank, citizens)
    std::vector<std::pair<int64_t, std::array<uint32_t, 4>>>    estates;    // (estate record, owner)
    bool operator==(const NodeStateMaps&) const = default;
    bool empty() const { return nodes.empty() && percent.empty() && citizens.empty() && estates.empty(); }
};

/// The GameState schema's four arrays are fast arrays of the layouts above (false otherwise: the schema is not the game's).
bool node_maps_layout_ok(const schema::ClassSchema& gamestate);
// C.Y.B : C O A

/// Brings the four node arrays of `arrays` (the GameState's) to `maps`: an entry whose key is new is added (the next ReplicationID), an entry whose values
/// moved is changed, a key `maps` no longer lists is removed; nothing is touched when nothing differs.  `changed` (when given) gets the number of items added,
/// changed and removed.  False, with the arrays as they were, when the schema has no such arrays or a value does not fit.
bool sync_node_maps(const schema::ClassSchema& gamestate, FastArrays& arrays, const NodeStateMaps& maps, size_t* changed = nullptr);

/// What the four arrays hold now (a test reads the join's arrays back with it).
NodeStateMaps read_node_maps(const schema::ClassSchema& gamestate, const FastArrays& arrays);

/// The maps of fast-array fields read from a payload (the delta's changed items, in order).  Fields other than the four are ignored.
NodeStateMaps node_maps_of_fields(const std::vector<FastArrayField>& fields);

// ---- the PlayerController's RPCs -------------------------------------------------------------------------------------------------------------------

/// One plot of the summary (FAoCNodePlotOverview): what stands on a service building plot.  EBuildingState: Empty 0, UnderConstruction 1, Operational 2 ...
struct PlotEntry {
    int64_t plot = 0;
    int64_t building = 0;
    uint8_t state = 0;
    int64_t building_time_end = 0;
    bool operator==(const PlotEntry&) const = default;
};

/// FAoCNodeSummary as a node with no mayor and no war sends it.
struct NodeSummary {
    int64_t                node = 0;
    int64_t                layout = 0;                      // LayoutRecordBase (0: the record is no real node)
    int32_t                level = 0;
    uint8_t                culture = 0;                     // ECharacterRace; retail sends 9 (none) for a record that is no node
    std::vector<PlotEntry> plots;
    int64_t                time_until_next_maintenance = 0; // an absolute unix time
    std::string            mayor_message;
    int64_t                next_mayor_message_edit_time = 60;   // unix; 60 for a node with no mayor, INT64_MAX for a record that is no node
    uint8_t                current_node_essence = 0;
    uint8_t                max_node_essence = 0;
    uint8_t                active_relic_slots = 0;
    bool operator==(const NodeSummary&) const = default;
};

/// FSiegeDisplayData of a node no war is declared on (retail's idle default): WarRecordID 0, CurrentPhaseIndex 255, every date the maximum, the target organisation the node.
struct SiegeSummary {
    std::string             target_org_name;            // the node's display name
    std::array<uint32_t, 4> target_org_guid{};          // (0, 0, high 32 bits, low 32 bits) of the node's guid
    bool operator==(const SiegeSummary&) const = default;
};

/// The idle siege summary of a node: the name and the guid as retail writes them.
SiegeSummary idle_siege_summary(int64_t node_guid, const std::string& display_name);

/// ClientReceiveNodeSummary's, ClientReceiveSiegeSummary's and ClientReceiveAllCurrentNodeSiegeCooldowns's parameters; nullopt when `rpc` is not that RPC or
/// the value does not fit.
std::optional<RpcParams> node_summary_params(const schema::RpcSchema& rpc, const NodeSummary& summary);
std::optional<RpcParams> siege_summary_params(const schema::RpcSchema& rpc, const SiegeSummary& summary);
std::optional<RpcParams> empty_siege_cooldowns_params(const schema::RpcSchema& rpc);

/// The node a ServerRequestNodeSummary (Node) or a ServerRequestCurrentSiegeSummary (NodeId) names; nullopt for another RPC or an undecoded call, 0 for the default.
std::optional<int64_t> parse_node_summary_request(const RpcCall& call);
std::optional<int64_t> parse_siege_summary_request(const RpcCall& call);

/// The same values read back from a call (a retail call or one of ours), for the comparisons of the tests; nullopt for another RPC or an undecoded call.
std::optional<NodeSummary>  parse_node_summary(const RpcCall& call);
std::optional<SiegeSummary> parse_siege_summary(const RpcCall& call);

/// The node's rank-free identity in a siege organisation guid: (0, 0, high, low).
std::array<uint32_t, 4> org_guid_of_node(int64_t node_guid);

// ---- the GM console RPCs --------------------------------------------------------------------------------------------------------------------------

/// ServerSetNodeLevel(InNode: string, InLevel, InCulture, bDisableVassalshipSystem) / ServerAddNodeExperience(InNode, Experience, InCulture, bUseRawXP) / ServerGMGetNodeData(NodeName):
/// the parameters of a decoded call.  nullopt for another RPC or an undecoded call.
struct GmSetNodeLevel {
    std::string node;
    int32_t     level = 0;
    int32_t     culture = 0;
    bool        disable_vassalship = false;
};
struct GmAddNodeExperience {
    std::string node;
    int32_t     experience = 0;
    int32_t     culture = 0;
    bool        raw_xp = false;
};
std::optional<GmSetNodeLevel>      parse_gm_set_node_level(const RpcCall& call);
std::optional<GmAddNodeExperience> parse_gm_add_node_experience(const RpcCall& call);
std::optional<std::string>         parse_gm_get_node_data(const RpcCall& call);

}}}  // namespace aoc::protocol::repl

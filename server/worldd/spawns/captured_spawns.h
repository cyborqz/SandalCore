// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/captured_spawns.h  (plan E1, stage 1)
//
//  What the retail captures give worldd of the other actors, parsed from the generated spawns/area_spawns.h (tools/capture/gen_area_spawns.py):
//
//    templates   one open per NPC archetype of the area's population chain: the archetype's export, the subobjects' names and checksums, and
//                every content block's payload read into the ObjectState the replicator writes from (libaocnet block_reader.h);
//    spawns      the town and node NPCs and the gatherable nodes seen within the area, each the open the retail server sent, with its position.
//
//  An OpenTemplate is a retail open as a value: `instantiate` makes a repl::Actor from it with NetGUIDs worldd mints (the actor, the AI
//  controller object its Owner and Controller name, then the components in the order the open sent them: retail's pattern, research/actors
//  evidence 1.1) and the values that must be this actor's own:
//
//    NPC          position and rotation (ReplicatedMovement and SerializeNewActor), Owner and Controller (the controller object), the timestamp,
//                 no Instigator, no attachment or based movement, no direction held; CharacterGuid (a new one, and the alignments that name the
//                 old one); Level; Health at its maximum; not dead; PopInstanceId and PopAssetId when the data says them
//    gatherable   GatherableGuid (a new one), AuthServerIDReplicated; its position is the capture's
//
//  Everything else is the captured block as it was: the abilities, the other stats, the appearance, the interact component.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/schema_types.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace aoc::worldd::spawns {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

/// A content block of a retail open, read.
struct ParsedBlock {
    int                          kind = 0;      // 0 the actor's, 1 a stably named subobject, 2 a dynamic one
    const schema::ClassSchema*   schema = nullptr;
    std::string                  cls;
    std::string                  name;          // kind 1
    uint32_t                     checksum = 0;  // kind 1
    repl::ExportChain            class_chain;   // kind 2
    repl::ObjectState            state;
};

/// A retail open: what a client is sent for an actor, but for its NetGUIDs and the values that are an instance's.
struct OpenTemplate {
    std::string               category;        // "npc" or "gatherable"
    std::string               archetype_path;
    repl::ExportChain         archetype;
    repl::SpawnInfo           spawn;           // as captured: the location, rotation and scale it had
    std::vector<ParsedBlock>  blocks;          // [0] is the actor's; the rest the subobjects in the order the open sent them
    int                       capture = 0, channel = 0, packet = 0;
    double                    x = 0, y = 0, z = 0;   // the captured location, cm
    const ParsedBlock&        actor_block() const { return blocks.front(); }
    /// The index in `blocks` of the first subobject whose class is `cls` (-1: none).
    int find(const std::string& cls) const;
};

/// A spawn the captures gave: an open and where it stood.
struct CapturedSpawn {
    const OpenTemplate* open = nullptr;
    double              x = 0, y = 0, z = 0;
};

/// The generated tables, parsed once at start.
class CapturedData {
public:
    /// Reads the generated header. nullopt, with `error`, when a payload does not read to its last bit or a class has no schema.
    static std::optional<CapturedData> load(std::string& error);
// ~ Cyb & C O A ~

    const std::vector<OpenTemplate>& opens() const { return opens_; }
    /// The template of an archetype path; nullptr when no capture opened it.
    const OpenTemplate* template_for(const std::string& archetype_path) const;
    /// Nodes 1, stage 3b: the class names ("Node_Villager_F", "Guard_Wizard") of the NPC Blueprints a retail capture opened: their opens carry the NPCAppearanceComponent a humanoid's body is
    /// merged from, so they show (spawns/node_population.h).
    std::set<std::string> npc_leaves() const;
    const std::vector<CapturedSpawn>& spawns() const { return spawns_; }
    size_t template_count() const { return by_path_.size(); }
    /// Every NetGUID of an export link of the tables (archetypes, packages, component classes): a NetGUID worldd mints for an object of its own
    /// (spawns/generic_npc.h) must not be one of them.
    bool static_id_in_use(uint64_t object_id) const { return static_ids_.count(object_id) != 0; }

    /// The area the header was cut for (spawns/area_spawns.h).
    double area_center_x() const;
    double area_center_y() const;
    double area_radius_cm() const;

private:
    std::vector<OpenTemplate>                opens_;
    std::map<std::string, size_t>            by_path_;
    std::vector<CapturedSpawn>               spawns_;
    std::unordered_set<uint64_t>             static_ids_;
};

/// The mount opens of captures 911 (ground), 898 (aquatic) and 871 (flying), parsed once at start from the generated spawns/mount_donors.h
/// (tools/capture/gen_mount.py): one OpenTemplate per kind of mount, which spawns/mount_spawn.h makes a summoned mount from.
class MountDonors {
public:
    /// Reads the generated header. nullopt, with `error`, when a payload does not read to its last bit or a class has no schema.
    static std::optional<MountDonors> load(std::string& error);

    /// The donor of a kind ("ground", "flying", "aquatic"); nullptr when the captures have none.
    const OpenTemplate* donor(const std::string& kind) const;
    const std::vector<OpenTemplate>& opens() const { return opens_; }
    /// Every NetGUID of an export link of the table (the three archetypes and their packages): a NetGUID worldd mints for a Blueprint of its own must not be one.
    bool static_id_in_use(uint64_t object_id) const { return static_ids_.count(object_id) != 0; }

private:
    std::vector<OpenTemplate>            opens_;
    std::map<std::string, size_t>        by_kind_;
    std::unordered_set<uint64_t>         static_ids_;
};

/// What an instance of an open needs of the world.
struct InstanceParams {
    double      x = 0, y = 0, z = 0;       // cm
    double      yaw_degrees = 0;
    bool        keep_captured_position = false;   // a captured spawn stands where its capture saw it (the open's own location and movement)
    int         level = 0;                 // 0: the template's
    uint32_t    server_id = 0;             // AuthServerIDReplicated
    double      world_time = 0;            // ReplicatedServerLastTransformUpdateTimeStamp
    int64_t     pop_instance = 0, pop_asset = 0;
    /// PopInstanceId and PopAssetId are set to 0: the client applies the population records they name to the NPC, and an instance record
    /// carries a DisplayNameOverrides that wins over the replicated CharacterName (the retail vendor's record says "Siege Vendor")
    bool        clear_population = false;
    std::string name;                      // "": the template's CreatureOwnerName
    uint64_t    seed = 0;                  // the new CharacterGuid / GatherableGuid words
    bool        full_health = true;
};

/// Mints the NetGUIDs of an actor: next() returns a fresh FIntrepidNetworkGUID of worldd's allocator.
using Minter = std::function<protocol::emit::FIntrepidNetworkGUID()>;

/// Makes a world actor from an open: new NetGUIDs, the instance's own values (above). nullopt, with `error`, when a value is refused.
std::optional<world::WorldActor> instantiate(const OpenTemplate& open, const Minter& mint, const InstanceParams& params, std::string& error);

}  // namespace aoc::worldd::spawns

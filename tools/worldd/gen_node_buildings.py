#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
"""Writes server/worldd/world/node_buildings_table.h from dist/content/content.db.

A node's service buildings (the mayor's Building Manager): what a building costs the node and its players, how long its phases run, what it gives the node
(its NodeModifications: skill points, effects) and its expansions (a skill tree whose ranks the mayor buys with node skill points, commodities and copper).
contentc keeps the buildings' own columns (`building`) but not the costs, the expansions or the trees' costs, so this reads the raw records:

  building     BuildingRecordBase: ServiceBuildingType, BuildingContributionPhasesId, CompletionNodeExperience, CanConstructionTimeOut, the phase durations
               (Construction, Demolition, DemolitionVote, Destroyed, Repair), NodeConstructionCostId, PlayerConstructionCostId, MaintenanceCostId,
               DisarrayRepairCostId, DamagedRepairCostId, ExpansionSkillTreeId, bDisplayInBuildingManager, NodeModificationsIds
  cost         ResourceCostRecordBase: CopperCost and CostEntries (Quantity of the entry's first ItemIds; an entry naming more items takes any of them: the others
               are listed as alternatives)
  phases       ContributionThresholdsRecordBase: ContributionStages (InterpolationTime, Weight; the InterpolationTimeMultiplier expression is not read)
  mod          NodeModificationRecordBase: SkillPoints (SkillPointTypeId, Quantity) and ServiceBuildingMaintenanceCostReduction (its effects are content.db's
               node_modification_ref, world/node_rewards.h)
  expansion    ExpansionRecordBase: the phase durations (PlayerConstructionPhase, Construction, Vote, RemovalVote), PostInitiationPlayerConstructionCostId,
               AddedMaintenanceId, NodeModificationsIds, the first ExpansionAssetSets (bReplaceBuilding) and ExpansionPopSpawns (bReplacement)
  tree node    SkillTreeRecordBase.Skills -> SkillTreeNodeRecordBase: SkillId, LevelRequirement (the node's level), PointRequirement, bAllAreRequired,
               SkillsRequired (SkillRequirementRecordBase: SkillTreeNodeRecordId, PointRequirement), MutuallyExclusiveSkillsIds;
               SkillRecordBase.SkillRanksIds -> SkillRankRecordBase: ExpansionId and SkillCost (CommodityCosts as the commodity's item, CurrencyCosts' copper,
               SkillPointCosts)

Run from the repository root; `--out FILE` writes elsewhere.
"""
import gzip
import json
import sqlite3
import sys

DB = "dist/content/content.db"
OUT = "server/worldd/world/node_buildings_table.h"
COPPER = 6064629132706054144
# EServiceBuildingType (the usmap)
TYPES = {"Empty": 0, "Artisan": 1, "Business": 2, "Content": 3, "Political": 4, "Vocational": 5, "Fabrication": 6, "Scholarly": 7, "Cultural": 8,
         "Civic": 9, "Magic": 10, "PlayerHousing": 11}
SCHEMAS = ("BuildingRecordBase", "ResourceCostRecordBase", "ContributionThresholdsRecordBase", "NodeModificationRecordBase", "ExpansionRecordBase",
           "SkillTreeRecordBase", "SkillTreeNodeRecordBase", "SkillRecordBase", "SkillRankRecordBase", "SkillRequirementRecordBase", "SkillPointTypeRecordBase",
           "CommodityDefRecordBase")


def g(ref):
    return (ref or {}).get("Guid", 0) or 0


def dur(v):
    return int((v or {}).get("Duration", 0) or 0)


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    out_path = OUT
    if "--out" in sys.argv:
        out_path = sys.argv[sys.argv.index("--out") + 1]
    db = sqlite3.connect("file:" + DB + "?mode=ro", uri=True)
    recs = {}
    q = "select guid, name, schema, data from record where schema in (%s)" % ",".join("?" * len(SCHEMAS))
    for guid, name, schema, data in db.execute(q, SCHEMAS):
        recs[guid] = (name, schema, json.loads(gzip.decompress(data)))

    def of(schema):
        return sorted((k, v[0], v[2]) for k, v in recs.items() if v[1] == schema)

    commodity_item = {k: g(r.get("CommodityProducedId")) for k, _, r in of("CommodityDefRecordBase")}

    # ---- the costs, phases and modifications the buildings and expansions name (only those)
    costs, cost_items, cost_index = [], [], {}

    def cost(guid):
        if not guid or guid not in recs or recs[guid][1] != "ResourceCostRecordBase":
            return 0
        if guid not in cost_index:
            name, _, r = recs[guid]
            first = len(cost_items)
            for e in r.get("CostEntries", []):
                ids = [g(i) for i in e.get("ItemIds", []) if g(i)]
                if not ids:
                    continue
                cost_items.append((ids[0], int(e.get("Quantity", 0) or 0), ids[1:]))
            cost_index[guid] = len(costs)
            costs.append((guid, name, int(r.get("CopperCost", 0) or 0), first, len(cost_items) - first))
        return guid

    phases, stages, phase_index = [], [], {}

    def phase(guid):
        if not guid or guid not in recs:
            return 0
        if guid not in phase_index:
            name, _, r = recs[guid]
            first = len(stages)
            for s in r.get("ContributionStages", []):
                stages.append((dur(s.get("InterpolationTime")), float(s.get("Weight", 1) or 0)))
            phase_index[guid] = len(phases)
            phases.append((guid, name, first, len(stages) - first))
        return guid

    mods, mod_points, mod_index = [], [], {}

    def mod(guid):
        if not guid or guid not in recs or recs[guid][1] != "NodeModificationRecordBase":
            return 0
        if guid not in mod_index:
            name, _, r = recs[guid]
            first = len(mod_points)
            for sp in r.get("SkillPoints", []):
                t = g(sp.get("SkillPointTypeId"))
                if t:
                    mod_points.append((t, int(sp.get("Quantity", 0) or 0)))
            mod_index[guid] = len(mods)
            mods.append((guid, name, float(r.get("ServiceBuildingMaintenanceCostReduction", 0) or 0), first, len(mod_points) - first))
        return guid

    # ---- the buildings
    buildings, building_mods = [], []
    for guid, name, r in of("BuildingRecordBase"):
        first = len(building_mods)
        for m in r.get("NodeModificationsIds", []):
            if mod(g(m)):
                building_mods.append(g(m))
        btype = TYPES.get(str(r.get("ServiceBuildingType", "")).split("::")[-1], 0)
        buildings.append((guid, name, btype, phase(g(r.get("BuildingContributionPhasesId"))), float(r.get("CompletionNodeExperience", 0) or 0),
                          bool(r.get("CanConstructionTimeOut")), dur(r.get("ConstructionPhaseDuration")), dur(r.get("DemolitionPhaseDuration")),
                          dur(r.get("DemolitionVoteDuration")), dur(r.get("DestroyedPhaseDuration")), dur(r.get("RepairPhaseDuration")),
                          cost(g(r.get("NodeConstructionCostId"))), cost(g(r.get("PlayerConstructionCostId"))), cost(g(r.get("MaintenanceCostId"))),
                          cost(g(r.get("DisarrayRepairCostId"))), cost(g(r.get("DamagedRepairCostId"))), g(r.get("ExpansionSkillTreeId")),
                          bool(r.get("bDisplayInBuildingManager")), first, len(building_mods) - first))

    # ---- the expansions
    expansions, expansion_mods = [], []
    for guid, name, r in of("ExpansionRecordBase"):
        first = len(expansion_mods)
        for m in r.get("NodeModificationsIds", []):
            if mod(g(m)):
                expansion_mods.append(g(m))
        assets = (r.get("ExpansionAssetSets") or [{}])[0]
        pops = (r.get("ExpansionPopSpawns") or [{}])[0]
        expansions.append((guid, name, dur(r.get("PlayerConstructionPhaseDuration")), dur(r.get("ConstructionDuration")), dur(r.get("VoteDuration")),
                           dur(r.get("RemovalVoteDuration")), cost(g(r.get("PostInitiationPlayerConstructionCostId"))), cost(g(r.get("AddedMaintenanceId"))),
                           g(assets.get("BuildingNodeAssetSetId")), bool(assets.get("bReplaceBuilding")), g(pops.get("BuildingSpawns")),
                           bool(pops.get("bReplacement")), first, len(expansion_mods) - first))

    # ---- the expansion trees (only the trees a building names)
    trees = sorted({b[16] for b in buildings if b[16]})
    tree_nodes, requires, exclusive, rank_items, rank_points = [], [], [], [], []
    unknown_commodities = 0
    for tree in trees:
        if tree not in recs:
            continue
        for slot in recs[tree][2].get("Skills", []):
            ng = g(slot.get("TreeNodeId"))
            if ng not in recs:
                continue
            nname, _, n = recs[ng]
            skill = g(n.get("SkillId"))
            if skill not in recs:
                continue
            req_first = len(requires)
            for sr in n.get("SkillsRequired", []):
                rg = g(sr.get("SkillRequirementId"))
                if rg in recs:
                    rr = recs[rg][2]
                    requires.append((g(rr.get("SkillTreeNodeRecordId")), int(rr.get("PointRequirement", 1) or 1)))
            ex_first = len(exclusive)
            for e in n.get("MutuallyExclusiveSkillsIds", []):
                if g(e):
                    exclusive.append(g(e))
            for rref in recs[skill][2].get("SkillRanksIds", []):
                rank = g(rref)
                if rank not in recs:
                    continue
                rk = recs[rank][2]
                sc = rk.get("SkillCost") or {}
                items_first = len(rank_items)
                for c in sc.get("CommodityCosts", []):
                    item = commodity_item.get(g(c.get("CommodityId")), 0)
                    if not item:
                        unknown_commodities += 1
                        continue
                    rank_items.append((item, int(c.get("Quantity", 0) or 0)))
                copper = sum(int(c.get("Quantity", 0) or 0) for c in sc.get("CurrencyCosts", []) if g(c.get("CurrencyId")) == COPPER)
                points_first = len(rank_points)
                for p in sc.get("SkillPointCosts", []):
                    t = g(p.get("SkillPointTypeId"))
                    if t:
                        rank_points.append((t, int(p.get("Quantity", 0) or 0)))
                tree_nodes.append((tree, ng, skill, rank, g(rk.get("ExpansionId")), nname, int(n.get("LevelRequirement", 0) or 0),
                                   int(n.get("PointRequirement", 0) or 0), bool(n.get("bAllAreRequired", True)), req_first, len(requires) - req_first,
                                   ex_first, len(exclusive) - ex_first, copper, items_first, len(rank_items) - items_first, points_first,
                                   len(rank_points) - points_first))

    point_types = [(guid, name) for guid, name, _ in of("SkillPointTypeRecordBase")]

    w = []
    w.append("// SPDX-License-Identifier: AGPL-3.0-only")
    w.append("// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).")
    w.append("// Generated by tools/worldd/gen_node_buildings.py from content.db.  Do not edit.")
    w.append("#pragma once")
    w.append("")
    w.append("#include <cstddef>")
    w.append("#include <cstdint>")
    w.append("")
    w.append("namespace aoc::worldd::world::node_buildings {")
    w.append("")
    w.append("/// A ResourceCostRecordBase: copper and items (kCostItems[first .. first + count)).")
    w.append("struct Cost { int64_t guid; const char* name; int64_t copper; size_t first, count; };")
    w.append("/// One entry of a cost: the item and how many (an entry that names more items takes any of them: `alt`, the second, 0 for none).")
    w.append("struct CostItem { int64_t item; int32_t quantity; int64_t alt; };")
    w.append("/// A ContributionThresholdsRecordBase: its stages (kStages[first .. first + count)).")
    w.append("struct Phases { int64_t guid; const char* name; size_t first, count; };")
    w.append("/// A contribution stage: how long the progress takes to reach it once its share is given (ms), and its share (Weight).")
    w.append("struct Stage { int64_t interpolation_ms; float weight; };")
    w.append("/// A NodeModificationRecordBase's skill points (kModPoints[first .. first + count)) and its maintenance reduction (a fraction).")
    w.append("struct Mod { int64_t guid; const char* name; float maintenance_reduction; size_t first, count; };")
    w.append("struct Points { int64_t type; int32_t quantity; };")
    w.append("/// A BuildingRecordBase.  `type` is EServiceBuildingType; the costs are Cost guids (0: none); the phases' lengths in ms; its modifications are")
    w.append("/// kBuildingMods[mods_first .. mods_first + mods_count).")
    w.append("struct Building {")
    w.append("    int64_t     guid;")
    w.append("    const char* name;")
    w.append("    uint8_t     type;")
    w.append("    int64_t     phases;")
    w.append("    double      completion_xp;")
    w.append("    bool        can_time_out;")
    w.append("    int64_t     construction_ms, demolition_ms, demolition_vote_ms, destroyed_ms, repair_ms;")
    w.append("    int64_t     node_cost, player_cost, maintenance_cost, disarray_repair_cost, damaged_repair_cost;")
    w.append("    int64_t     tree;")
    w.append("    bool        in_manager;")
    w.append("    size_t      mods_first, mods_count;")
    w.append("};")
    w.append("/// An ExpansionRecordBase: the phases' lengths (ms), the players' cost after it starts, the upkeep it adds, the asset sets and the building spawn it")
    w.append("/// puts on the plot (and whether they replace the building's), its modifications kExpansionMods[mods_first .. mods_first + mods_count).")
    w.append("struct Expansion {")
    w.append("    int64_t     guid;")
    w.append("    const char* name;")
    w.append("    int64_t     player_phase_ms, construction_ms, vote_ms, removal_vote_ms;")
    w.append("    int64_t     player_cost, added_maintenance;")
    w.append("    int64_t     asset_sets;")
    w.append("    bool        replace_building;")
    w.append("    int64_t     pop_spawn;")
    w.append("    bool        replace_spawn;")
    w.append("    size_t      mods_first, mods_count;")
    w.append("};")
    w.append("/// A node of an expansion tree with its (one) rank: the expansion it builds, the node level it needs, the points of its prerequisites")
    w.append("/// (kRequires[req_first ..): a tree node and its points; all of them or any one, `all_required`), the skills it excludes (kExclusive[ex_first ..)) and")
    w.append("/// what it costs: copper, items from the node's stores (kRankItems[items_first ..)) and node skill points (kRankPoints[points_first ..)).")
    w.append("struct TreeNode {")
    w.append("    int64_t     tree, node, skill, rank, expansion;")
    w.append("    const char* name;")
    w.append("    int32_t     level_requirement, point_requirement;")
    w.append("    bool        all_required;")
    w.append("    size_t      req_first, req_count, ex_first, ex_count;")
    w.append("    int64_t     copper;")
    w.append("    size_t      items_first, items_count, points_first, points_count;")
    w.append("};")
    w.append("struct Require { int64_t node; int32_t points; };")
    w.append("struct RankItem { int64_t item; int32_t quantity; };")
    w.append("struct PointType { int64_t guid; const char* name; };")
    w.append("")
    w.append("/// The TypeIds of FAoCRecordId (the summary's PurchasedExpansions and NodeSkillPoints name records with them).")
    w.append("inline constexpr int64_t kExpansionTypeId = 8187534410104834233ll;")
    w.append("inline constexpr int64_t kSkillPointTypeTypeId = 7337271797676719493ll;")
    w.append("")

    def arr(ctype, name, rows, fmt):
        w.append("inline constexpr %s %s[] = {" % (ctype, name))
        if not rows:
            w.append("    {},")
        for r in rows:
            w.append("    " + fmt(r) + ",")
        w.append("};")
        w.append("inline constexpr size_t %s_count = %d;" % (name, len(rows)))
        w.append("")

    def b(v):
        return "true" if v else "false"

    def ll(v):
        return "%dll" % v

    arr("Cost", "kCosts", costs, lambda c: "{%s, %s, %s, %d, %d}" % (ll(c[0]), cstr(c[1]), ll(c[2]), c[3], c[4]))
    arr("CostItem", "kCostItems", cost_items, lambda c: "{%s, %d, %s}" % (ll(c[0]), c[1], ll(c[2][0] if c[2] else 0)))
    arr("Phases", "kPhases", phases, lambda p: "{%s, %s, %d, %d}" % (ll(p[0]), cstr(p[1]), p[2], p[3]))
    arr("Stage", "kStages", stages, lambda s: "{%s, %sf}" % (ll(s[0]), repr(float(s[1]))))
    arr("Mod", "kMods", mods, lambda m: "{%s, %s, %sf, %d, %d}" % (ll(m[0]), cstr(m[1]), repr(float(m[2])), m[3], m[4]))
    arr("Points", "kModPoints", mod_points, lambda p: "{%s, %d}" % (ll(p[0]), p[1]))
    arr("Building", "kBuildings", buildings, lambda x: "{%s, %s, %d, %s, %s, %s, %d, %d, %d, %d, %d, %s, %s, %s, %s, %s, %s, %s, %d, %d}" % (
        ll(x[0]), cstr(x[1]), x[2], ll(x[3]), repr(float(x[4])), b(x[5]), x[6], x[7], x[8], x[9], x[10], ll(x[11]), ll(x[12]), ll(x[13]), ll(x[14]),
        ll(x[15]), ll(x[16]), b(x[17]), x[18], x[19]))
    arr("int64_t", "kBuildingMods", building_mods, ll)
    arr("Expansion", "kExpansions", expansions, lambda x: "{%s, %s, %d, %d, %d, %d, %s, %s, %s, %s, %s, %s, %d, %d}" % (
        ll(x[0]), cstr(x[1]), x[2], x[3], x[4], x[5], ll(x[6]), ll(x[7]), ll(x[8]), b(x[9]), ll(x[10]), b(x[11]), x[12], x[13]))
    arr("int64_t", "kExpansionMods", expansion_mods, ll)
    arr("TreeNode", "kTreeNodes", tree_nodes, lambda t: "{%s, %s, %s, %s, %s, %s, %d, %d, %s, %d, %d, %d, %d, %s, %d, %d, %d, %d}" % (
        ll(t[0]), ll(t[1]), ll(t[2]), ll(t[3]), ll(t[4]), cstr(t[5]), t[6], t[7], b(t[8]), t[9], t[10], t[11], t[12], ll(t[13]), t[14], t[15], t[16], t[17]))
    arr("Require", "kRequires", requires, lambda r: "{%s, %d}" % (ll(r[0]), r[1]))
    arr("int64_t", "kExclusive", exclusive, ll)
    arr("RankItem", "kRankItems", rank_items, lambda r: "{%s, %d}" % (ll(r[0]), r[1]))
    arr("Points", "kRankPoints", rank_points, lambda p: "{%s, %d}" % (ll(p[0]), p[1]))
    arr("PointType", "kPointTypes", point_types, lambda p: "{%s, %s}" % (ll(p[0]), cstr(p[1])))
    w.append("}  // namespace aoc::worldd::world::node_buildings")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(w) + "\n")
    print("%s: %d buildings, %d costs (%d items), %d phases, %d modifications (%d skill point grants), %d expansions, %d tree nodes in %d trees "
          "(%d prerequisites, %d exclusions, %d item costs, %d point costs), %d point types; %d commodity costs with no item" % (
              out_path, len(buildings), len(costs), len(cost_items), len(phases), len(mods), len(mod_points), len(expansions), len(tree_nodes), len(trees),
              len(requires), len(exclusive), len(rank_items), len(rank_points), len(point_types), unknown_commodities))


if __name__ == "__main__":
    main()

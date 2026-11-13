#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
"""Writes server/worldd/world/buy_order_table.h from dist/content/content.db.

A node's buy orders (the Buy Order Manager): its mayor asks for a commodity made by one of its recipes, and players bring the recipe's materials.  contentc keeps
none of it, so this reads the raw records:

  commodity    CommodityDefRecordBase: its Category (ECommodityCategory: IndustrialGoods 0, Luxuries 1, WarMaterials 2), the item it makes
               (CommodityProducedId) and its recipes (CommodityRecipesIds)
  recipe       CommodityRecipeDefRecordBase: where it may be ordered (AvailabilityPredicate: GetNodeBiome() == EBiomeType::<Biome>, ||-joined; "false": never),
               the mayor's ranges (Min/MaxCost copper, Min/MaxLimitContribution, Min/MaxQuantityProduced, Min/MaxReward, Min/MaxTimeDurationDays), the reward
               table each contribution rolls (RewardTypeId) and its material requirements (CommodityRecipeMaterialRequirementIds)
  requirement  CommodityRecipeMaterialRequirementDefRecordBase: a material tier (CommodityRecipeMaterialTierId) and how many of it one unit takes (Quantity)
  tier         CommodityRecipeMaterialTierDefRecordBase: its options (MaterialOptionsIds -> CommodityRecipeMaterialTierEntryRecordBase: MaterialId, the item, when its
               AvailabilityPredicate is "true"; the others are left out)
  constants    BuyOrderConstantsRecordBase: BuyOrderContributionLimitFrequencySeconds, BuyOrderMaterialCooldownDuration

Run from the repository root; `--out FILE` writes elsewhere.
"""
import gzip
import json
import re
import sqlite3
import sys

DB = "dist/content/content.db"
OUT = "server/worldd/world/buy_order_table.h"
CATEGORIES = {"IndustrialGoods": 0, "Luxuries": 1, "WarMaterials": 2}
# EBiomeType (the usmap), as gen_commissions.py has it
BIOMES = {"AlpineForest": 1, "AncientForest": 2, "ArcticTundra": 3, "Asian": 4, "Badlands": 5, "Desert": 6, "Fjords": 7, "Glacier": 8, "Grasslands": 9,
          "Iceland": 10, "Jungle": 11, "MagicalForest": 12, "RedwoodForest": 13, "Riverlands": 14, "Swamp": 15, "Taiga": 16, "TabletopMountains": 17,
          "Tropics": 18, "Volcano": 19, "UnderRealm": 20, "Highlands": 21}


def main():
    out_path = OUT
    if "--out" in sys.argv:
        out_path = sys.argv[sys.argv.index("--out") + 1]
    db = sqlite3.connect(DB)
    recs = {}
    for guid, name, schema, data in db.execute("select guid, name, schema, data from record where schema in (?, ?, ?, ?, ?, ?)",
                                               ("CommodityDefRecordBase", "CommodityRecipeDefRecordBase", "CommodityRecipeMaterialRequirementDefRecordBase",
                                                "CommodityRecipeMaterialTierDefRecordBase", "CommodityRecipeMaterialTierEntryRecordBase", "BuyOrderConstantsRecordBase")):
        recs[guid] = (name, schema, json.loads(gzip.decompress(data)))

    def g(ref):
        return (ref or {}).get("Guid", 0)

    def num(v, default=0):
        try:
            return int(float(v))
        except (TypeError, ValueError):
            return default

    options, requirements, recipes, commodities = [], [], [], []
    skipped = {"never": 0, "unknown predicate": 0}
    for cguid, (cname, cschema, c) in sorted((k, v) for k, v in recs.items() if v[1] == "CommodityDefRecordBase"):
        item = g(c.get("CommodityProducedId"))
        if not item:
            continue
        cat = CATEGORIES.get(str(c.get("Category", "")).split("::")[-1], 0)
        first_recipe = len(recipes)
        for rref in c.get("CommodityRecipesIds", []):
            rg = g(rref)
            if rg not in recs:
                continue
            rname, _, r = recs[rg]
            pred = str(r.get("AvailabilityPredicate", "")).strip()
            if pred == "false":
                skipped["never"] += 1
                continue
            mask = 0
            if pred and pred != "true":
                names = re.findall(r"EBiomeType::(\w+)", pred)
                if not names or not all(n in BIOMES for n in names):
                    skipped["unknown predicate"] += 1
                    continue
                for n in names:
                    mask |= 1 << BIOMES[n]
            first_req = len(requirements)
            ok = True
            for qref in r.get("CommodityRecipeMaterialRequirementIds", []):
                qg = g(qref)
                if qg not in recs:
                    ok = False
                    break
                _, _, q = recs[qg]
                tier = g(q.get("CommodityRecipeMaterialTierId"))
                first_opt = len(options)
                if tier in recs:
                    for oref in recs[tier][2].get("MaterialOptionsIds", []):
                        og = g(oref)
                        if og in recs and str(recs[og][2].get("AvailabilityPredicate", "true")).strip() == "true" and g(recs[og][2].get("MaterialId")):
                            options.append(g(recs[og][2].get("MaterialId")))
                requirements.append((tier, num(q.get("Quantity"), 1), first_opt, len(options) - first_opt))
            if not ok:
                del requirements[first_req:]
                continue
            recipes.append((rg, cguid, rname, mask, num(r.get("MinCost")), num(r.get("MaxCost")), num(r.get("MinLimitContribution")), num(r.get("MaxLimitContribution")),
                            num(r.get("MinQuantityProduced")), num(r.get("MaxQuantityProduced")), num(r.get("MinReward")), num(r.get("MaxReward")),
                            num(r.get("MinTimeDurationDays")), num(r.get("MaxTimeDurationDays")), g(r.get("RewardTypeId")), first_req, len(requirements) - first_req))
        commodities.append((cguid, cname, item, cat, first_recipe, len(recipes) - first_recipe))
    period, cooldown = 86400, 50
    for name, schema, d in recs.values():
        if schema == "BuyOrderConstantsRecordBase":
            period = num(d.get("BuyOrderContributionLimitFrequencySeconds"), period)
            cooldown = num(d.get("BuyOrderMaterialCooldownDuration"), cooldown)

    lines = [
        "// SPDX-License-Identifier: AGPL-3.0-only",
        "// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).",
        "// Generated by tools/worldd/gen_buy_orders.py from content.db.  Do not edit.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace aoc::worldd::world::buy_orders {",
        "",
        "/// A material requirement of a recipe: its tier, how many of it one unit takes, and the items it takes (kOptions[first .. first + count)).",
        "struct Requirement {",
        "    int64_t tier;",
        "    int32_t quantity;",
        "    size_t  first, count;",
        "};",
        "/// A recipe of a commodity: the biomes it may be ordered in (bit n: EBiomeType n; 0: any), the mayor's ranges, the reward table a contribution rolls, and",
        "/// its requirements (kRequirements[first .. first + count)).",
        "struct Recipe {",
        "    int64_t     recipe, commodity;",
        "    const char* name;",
        "    uint32_t    biomes;",
        "    int32_t     min_cost, max_cost;",
        "    int32_t     min_limit, max_limit;",
        "    int32_t     min_quantity, max_quantity;",
        "    int32_t     min_reward, max_reward;",
        "    int32_t     min_days, max_days;",
        "    int64_t     reward_table;",
        "    size_t      first, count;",
        "};",
        "/// A commodity a node may order: its def, the item it makes, its ECommodityCategory and its recipes (kRecipes[first .. first + count)).",
        "struct Commodity {",
        "    int64_t     def;",
        "    const char* name;",
        "    int64_t     item;",
        "    uint8_t     category;",
        "    size_t      first, count;",
        "};",
        f"/// BuyOrderConstants: the period of a player's contribution limit, and BuyOrderMaterialCooldownDuration.",
        f"inline constexpr int64_t kContributionPeriodSeconds = {period};",
        f"inline constexpr int32_t kMaterialCooldown = {cooldown};",
        "",
        "inline constexpr int64_t kOptions[] = {",
    ]
    lines += [f"    {o}ll," for o in options] or ["    0,"]
    lines.append("};")
    lines.append("inline constexpr Requirement kRequirements[] = {")
    lines += [f"    {{{t}ll, {q}, {f}, {n}}}," for t, q, f, n in requirements] or ["    {0, 0, 0, 0},"]
    lines.append("};")
    lines.append("inline constexpr Recipe kRecipes[] = {")
    for r in recipes:
        lines.append(f'    {{{r[0]}ll, {r[1]}ll, "{r[2]}", {r[3]}u, {r[4]}, {r[5]}, {r[6]}, {r[7]}, {r[8]}, {r[9]}, {r[10]}, {r[11]}, {r[12]}, {r[13]}, {r[14]}ll, {r[15]}, {r[16]}}},')
    lines.append("};")
    lines.append("inline constexpr Commodity kCommodities[] = {")
    for cg, cn, item, cat, f, n in commodities:
        lines.append(f'    {{{cg}ll, "{cn}", {item}ll, {cat}, {f}, {n}}},')
    lines.append("};")
    lines += ["", "}  // namespace aoc::worldd::world::buy_orders", ""]
    open(out_path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print(f"{out_path}: {len(commodities)} commodities, {len(recipes)} recipes ({skipped['never']} never ordered, {skipped['unknown predicate']} with a rule this reads not), "
          f"{len(requirements)} requirements, {len(options)} material options; contribution period {period} s, material cooldown {cooldown}")


if __name__ == "__main__":
    main()

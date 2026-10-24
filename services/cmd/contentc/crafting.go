// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/designdb"
)

// Crafting and processing (the crafting milestone, step 0; contentc version 20): craft_station_def, craft_station_asset, craft_recipe_set, craft_recipe_set_list,
// craft_recipe_list, craft_recipe_list_member, craft_pred, craft_recipe, craft_recipe_input, craft_cost and craft_reward (internal/content/crafting.go says what each
// holds). The records are copied as they are: a recipe's formulas stay text for the stat engine to run, and the reward tables stay the JSON of their containers. Nothing
// here is evaluated.

var craftingRecordTypes = []string{"CraftingStationDefRecordBase", "CraftingRecipeSetRecordBase", "CraftingRecipeListRecordBase", "CraftingRecipeDefRecordBase",
	"CraftingCurrencyCostRecordBase", "CertificationLevelRecordBase", "RewardTableRecordBase", "PopulationAssetRecordBase"}

// rewardKeyDrop are the keys of a reward table the JSON leaves out: a name for the editor, an icon, and the weight curves the data keeps as a placeholder.
var rewardKeyDrop = map[string]bool{"DisplayName": true, "RewardContainerIcon": true, "RewardsWeightCurve": true, "SubTablesWeightCurve": true, "Callout": true}

// anyOf turns a decoded value into what encoding/json writes: structs as objects, lists as arrays, an Id struct as its Guid (0 stays 0), text as its source.
func anyOf(v designdb.Value, drop map[string]bool) any {
	switch x := v.(type) {
	case designdb.Struct:
		if g, ok := x["Guid"].(int64); ok && len(x) <= 3 {
			if _, hasType := x["TypeId"]; hasType {
				return g
			}
		}
		out := make(map[string]any, len(x))
		for k, e := range x {
			if drop[k] {
				continue
			}
			out[k] = anyOf(e, drop)
		}
		return out
	case []designdb.Value:
		out := make([]any, 0, len(x))
		for _, e := range x {
			out = append(out, anyOf(e, drop))
		}
		return out
	case designdb.Text:
		if x.Source != "" {
			return x.Source
		}
		return x.Key
	case float32:
		return decimal32(x)
	case designdb.Pairs:
		out := make([]any, 0, len(x))
		for _, p := range x {
			out = append(out, []any{anyOf(p.Key, drop), anyOf(p.Value, drop)})
		}
		return out
	}
	return v
}

func commaInts(ids []int64) string {
	parts := make([]string, 0, len(ids))
	for _, g := range ids {
		if g != 0 {
			parts = append(parts, strconv.FormatInt(g, 10))
		}
	}
	return strings.Join(parts, ",")
}

// predicatesOf reads an AvailabilityPredicates list into craft_pred rows.
func writeCraftPreds(w *writer, kind string, guid int64, where string, list []designdb.Value) error {
	for i, pv := range list {
		ps, ok := pv.(designdb.Struct)
		if !ok {
			return fmt.Errorf("%s: AvailabilityPredicates[%d] is %T", where, i, pv)
		}
		pf := newFields(fmt.Sprintf("%s: AvailabilityPredicates[%d]", where, i), ps)
		typ := enumValue(pf.str("Type"))
		level := enumValue(pf.str("NodeLevel"))
		var biomes []string
		for _, b := range pf.list("Biomes") {
			s, _ := b.(string)
			biomes = append(biomes, enumValue(s))
		}
		mods := pf.ids("Modifications")
		nodes := pf.ids("Nodes")
		hidden, folio := pf.boolean("bHidden"), pf.boolean("bHiddenInFolio")
		if pf.err != nil {
			return pf.err
		}
		levelN, err := nodeLevelNumber(level)
		if err != nil {
			levelN = -1
		}
		if err := w.exec(`INSERT INTO craft_pred (owner_kind, owner_guid, ord, type, node_level, node_level_n, biomes, modifications, nodes, hidden, hidden_in_folio)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, kind, guid, i, typ, level, levelN, strings.Join(biomes, ","), commaInts(mods), commaInts(nodes), b2i(hidden), b2i(folio)); err != nil {
			return err
		}
	}
	return nil
}

func buildCrafting(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range craftingRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	// the certification tiers by guid
	tier := map[int64]int{}
	for _, r := range src.rows["CertificationLevelRecordBase"] {
		f := newFields(fmt.Sprintf("CertificationLevel %s (%d)", r.Name, r.Guid), r.Fields)
		tier[r.Guid] = int(f.int("CertificationTier"))
		if f.err != nil {
			return f.err
		}
	}
	tierOf := func(guid int64) int {
		if t, ok := tier[guid]; ok {
			return t
		}
		return -1
	}

	// the stations
	defs := map[string]int64{}
	for _, r := range src.rows["CraftingStationDefRecordBase"] {
		f := newFields(fmt.Sprintf("CraftingStationDef %s (%d)", r.Name, r.Guid), r.Fields)
		set, _ := f.id("CraftingRecipeSetsId")
		interaction, _ := f.id("InteractionTypeId")
		maxTier, _ := f.id("MaxCertificationTierId")
		tag := newFields(f.where+": TaxTag", f.strukt("TaxTag"))
		tagName := ""
		if _, has := tag.get("TagName"); has {
			tagName = tag.str("TagName")
		}
		if tagName == "None" {
			tagName = ""
		}
		display := f.text("DisplayName")
		kind, ptype := enumValue(f.str("Type")), enumValue(f.str("ProcessingStationType"))
		queue, simul := f.int("QueueSize"), f.int("SimulateneousJobNum")
		individual, black := f.boolean("bIndividual"), f.boolean("Blacklisted")
		minQ, maxQ, icon := f.str("MinimumBatchQuantityFormula"), f.str("MaximumBatchQuantityFormula"), f.str("WindowIcon")
		if err := firstErr(f, tag); err != nil {
			return err
		}
		if err := w.exec(`INSERT INTO craft_station_def (guid, name, display_name, kind, processing_type, max_tier, max_tier_guid, queue_size, simultaneous, individual, blacklisted,
			tax_tag, interaction_type, recipe_set_guid, min_quantity, max_quantity, window_icon) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, display, kind, ptype, tierOf(maxTier), maxTier, queue, simul, b2i(individual), b2i(black), tagName, interaction, set, minQ, maxQ, icon); err != nil {
			return err
		}
		defs[r.Name] = r.Guid
	}

	// which station definition a population asset is the station of: the leaf of its asset reference is the definition's name; a few Blueprints are named after the
	// profession differently (the Scribing stations' definitions say Scribe)
	linked := 0
	for _, r := range src.rows["PopulationAssetRecordBase"] {
		f := newFields(fmt.Sprintf("PopulationAsset %s (%d)", r.Name, r.Guid), r.Fields)
		ref := ""
		if _, has := f.get("AssetRef"); has {
			ref = f.str("AssetRef")
		}
		if f.err != nil {
			return f.err
		}
		leaf := ref
		if i := strings.LastIndex(ref, "."); i >= 0 {
			leaf = ref[i+1:]
		}
		if !strings.HasPrefix(leaf, "CraftingStation_") && !strings.HasPrefix(leaf, "ProcessingStation_") {
			continue
		}
		def, ok := defs[leaf]
		if !ok {
			def, ok = defs[strings.Replace(leaf, "_Scribing_", "_Scribe_", 1)]
		}
		if !ok {
			continue
		}
		if err := w.exec(`INSERT INTO craft_station_asset (asset_guid, def_guid) VALUES (?, ?)`, r.Guid, def); err != nil {
			return err
		}
		linked++
	}

	// the recipe sets and lists
	for _, r := range src.rows["CraftingRecipeSetRecordBase"] {
		f := newFields(fmt.Sprintf("CraftingRecipeSet %s (%d)", r.Name, r.Guid), r.Fields)
		prof, _ := f.id("ProfessionId")
		tg, _ := f.id("CertificationTierId")
		lists := f.ids("RecipeListsIds")
		black := f.boolean("Blacklisted")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO craft_recipe_set (guid, name, profession_guid, tier_guid, blacklisted) VALUES (?, ?, ?, ?, ?)`, r.Guid, r.Name, prof, tg, b2i(black)); err != nil {
			return err
		}
		for i, l := range lists {
			if err := w.exec(`INSERT INTO craft_recipe_set_list (set_guid, ord, list_guid) VALUES (?, ?, ?)`, r.Guid, i, l); err != nil {
				return err
			}
		}
	}
	for _, r := range src.rows["CraftingRecipeListRecordBase"] {
		f := newFields(fmt.Sprintf("CraftingRecipeList %s (%d)", r.Name, r.Guid), r.Fields)
		recipes := f.ids("RecipesIds")
		preds := f.list("AvailabilityPredicates")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO craft_recipe_list (guid, name) VALUES (?, ?)`, r.Guid, r.Name); err != nil {
			return err
		}
		for i, rec := range recipes {
			if err := w.exec(`INSERT INTO craft_recipe_list_member (list_guid, ord, recipe_guid) VALUES (?, ?, ?)`, r.Guid, i, rec); err != nil {
				return err
			}
		}
		if err := writeCraftPreds(w, "list", r.Guid, f.where, preds); err != nil {
			return err
		}
	}

	// the costs
	for _, r := range src.rows["CraftingCurrencyCostRecordBase"] {
		f := newFields(fmt.Sprintf("CraftingCurrencyCost %s (%d)", r.Name, r.Guid), r.Fields)
		tg, _ := f.id("CurrencyCostTierId")
		value, base := f.str("CurrencyCostValue"), f.int("BaseCurrencyCost")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO craft_cost (guid, name, tier_guid, value, base) VALUES (?, ?, ?, ?, ?)`, r.Guid, r.Name, tg, value, base); err != nil {
			return err
		}
	}

	// the recipes
	rewardWanted := map[int64]bool{}
	nInputs, nFuel := 0, 0
	for _, r := range src.rows["CraftingRecipeDefRecordBase"] {
		f := newFields(fmt.Sprintf("CraftingRecipe %s (%d)", r.Name, r.Guid), r.Fields)
		prof, _ := f.id("ProfessionId")
		cert, _ := f.id("CertificationLevelId")
		cost, _ := f.id("CraftingCurrencyCostId")
		reward, _ := f.id("RewardId")
		dist, _ := f.id("DistributionRewardId")
		leveling, _ := f.id("LevelingTemplateId")
		category := newFields(f.where+": CategoryTag", f.strukt("CategoryTag"))
		catName := ""
		if _, has := category.get("TagName"); has {
			catName = category.str("TagName")
		}
		duration := f.number("BaseDuration")
		stdCost := f.str("StandardCurrencyCost")
		dest := enumValue(f.str("ItemDestination"))
		fuelTags := f.tags("FuelTags")
		general, primary := f.list("GeneralResourceCost"), f.list("PrimaryResourceCosts")
		preds := f.list("AvailabilityPredicates")
		overrides := f.boolean("bUseOverrides")
		minQ, maxQ := "", ""
		if _, has := f.get("MinimumQuantityFormula"); has {
			minQ = f.str("MinimumQuantityFormula")
		}
		if _, has := f.get("MaximumQuantityFormula"); has {
			maxQ = f.str("MaximumQuantityFormula")
		}
		if err := firstErr(f, category); err != nil {
			return err
		}
		if err := w.exec(`INSERT INTO craft_recipe (guid, name, profession_guid, tier_guid, tier, category_tag, base_duration, duration_formula, quantity_formula, quality_formula,
			min_quantity, max_quantity, std_cost, cost_guid, reward_guid, distribution_guid, leveling_guid, destination, default_quality, difficulty, fuel, fuel_tags, learnable,
			can_fast_craft, can_gameplay, use_overrides, blacklisted) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, prof, cert, tierOf(cert), catName, duration, f.str("DurationFormula"), f.str("QuantityFormula"), f.str("QualityFormula"), minQ, maxQ, stdCost, cost, reward, dist,
			leveling, dest, f.int("DefaultQuality"), f.int("Difficulty"), f.int("Fuel"), strings.Join(fuelTags, ","), b2i(f.boolean("Learnable")), b2i(f.boolean("bCanFastCraft")),
			b2i(f.boolean("bCanGameplayLayer")), b2i(overrides), b2i(f.boolean("Blacklisted"))); err != nil {
			return err
		}
		if f.err != nil {
			return f.err
		}
		if reward != 0 {
			rewardWanted[reward] = true
		}
		if f.int("Fuel") > 0 || len(fuelTags) > 0 {
			nFuel++
		}
		for i, gv := range general {
			gs, ok := gv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: GeneralResourceCost[%d] is %T", f.where, i, gv)
			}
			gf := newFields(fmt.Sprintf("%s: GeneralResourceCost[%d]", f.where, i), gs)
			item, _ := gf.id("Item")
			filter, _ := gf.id("CostGatherItemFilter")
			tags := gf.tags("MaterialTags")
			qty, minRarity, consume := gf.int("Quantity"), rarityIndex(enumValue(gf.str("MinRarity"))), gf.boolean("bConsumeInCrafting")
			if gf.err != nil {
				return gf.err
			}
			if err := w.exec(`INSERT INTO craft_recipe_input (recipe_guid, kind, ord, item_guid, filter_guid, tags, quantity, min_rarity, consume) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, "general", i, item, filter, strings.Join(tags, ","), qty, minRarity, b2i(consume)); err != nil {
				return err
			}
			nInputs++
		}
		for i, pv := range primary {
			ps, ok := pv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: PrimaryResourceCosts[%d] is %T", f.where, i, pv)
			}
			pf := newFields(fmt.Sprintf("%s: PrimaryResourceCosts[%d]", f.where, i), ps)
			item, _ := pf.id("Item")
			qty, rarity, consume := pf.int("Quantity"), rarityIndex(enumValue(pf.str("Rarity"))), pf.boolean("bConsumeInCrafting")
			if pf.err != nil {
				return pf.err
			}
			if err := w.exec(`INSERT INTO craft_recipe_input (recipe_guid, kind, ord, item_guid, filter_guid, tags, quantity, min_rarity, consume) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, "primary", i, item, 0, "", qty, rarity, b2i(consume)); err != nil {
				return err
			}
			nInputs++
		}
		if err := writeCraftPreds(w, "recipe", r.Guid, f.where, preds); err != nil {
			return err
		}
	}

	// the reward tables the recipes name, and the sub-tables they reach
	tables := byGuid(src.rows["RewardTableRecordBase"])
	written := map[int64]bool{}
	queue := sortedKeys(rewardWanted)
	missing := 0
	for len(queue) > 0 {
		g := queue[0]
		queue = queue[1:]
		if written[g] {
			continue
		}
		r, ok := tables[g]
		if !ok {
			missing++
			opt.logf("warning: a recipe names the reward table %d, which is no row: skipped", g)
			written[g] = true
			continue
		}
		written[g] = true
		body, err := json.Marshal(anyOf(designdb.Struct(r.Fields), rewardKeyDrop))
		if err != nil {
			return fmt.Errorf("RewardTable %s (%d): %w", r.Name, r.Guid, err)
		}
		if err := w.exec(`INSERT INTO craft_reward (guid, name, body) VALUES (?, ?, ?)`, r.Guid, r.Name, string(body)); err != nil {
			return err
		}
		f := newFields(fmt.Sprintf("RewardTable %s (%d)", r.Name, r.Guid), r.Fields)
		subs := f.ids("SubTablesIds")
		if f.err != nil {
			return f.err
		}
		for _, s := range subs {
			if s != 0 && !written[s] {
				queue = append(queue, s)
			}
		}
		sort.Slice(queue, func(i, j int) bool { return queue[i] < queue[j] })
	}

	opt.logf("crafting: %d station definitions, %d assets linked to a station, %d recipes with %d inputs (%d need fuel), %d reward tables (%d missing)", len(defs), linked,
		len(src.rows["CraftingRecipeDefRecordBase"]), nInputs, nFuel, len(written)-missing, missing)
	meta["crafting.stations"] = strconv.Itoa(len(defs))
	meta["crafting.recipes"] = strconv.Itoa(len(src.rows["CraftingRecipeDefRecordBase"]))
	return nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// The city nodes (milestone "Nodes 1", stage 1; contentc version 12): see package content, nodes.go, for what each table holds. "Node" is the
// settlement (CityNodeRecordBase), not a resource node or a talent node.
//
// The tables are typed now, all of them, so that the version bumps once: stage 1 reads city_node and city_zoi (the rank, the type, the biome, the zone
// of influence), stage 2 the tax, citizenship and election tables, stage 3 the layouts, plots, buildings and their populations.

// nodeSchemas are the record structs the node tables read (build.go's keep names them); the first two must be present.
var nodeSchemas = []string{
	"CityNodeRecordBase", "ZOIRecordBase", "NodeLevelDetailsRecordBase", "NodeModificationRecordBase", "NodeSizeRecordBase", "ChildNodeRulesRecordBase",
	"ChildNodeDesiredLevelRulesRecordBase", "VassalshipDefRecordBase", "TaxDetailsRecordBase", "NodeGlobalTaxTableRecordBase",
	"NodeToPlayerReputationTierRecordBase", "CitizenshipNodeRecordBase", "CitizenshipGenericRecordBase", "QuantityScalarDefRecordBase",
	"CitizenshipDuesDefRecordBase", "PropertyTaxDuesDefRecordBase", "CityNodeElectionDefRecordBase", "ElectionConstantsRecordBase",
	"CityNodeConstantsRecordBase", "NodeAssetSetRecordBase", "NodeAssetDefinitionRecordBase", "ServiceBuildingPlotRecordBase", "BuildingRecordBase",
	"BuildingNodeAssetSetsRecordBase",
}

// looseNumber reads a number the data types as a float, an integer or a decimal string ("0.1"): the reputation tiers and the vassal tax are strings.
func (f *fields) looseNumber(name string) float64 {
	v, ok := f.get(name)
	if s, isStr := v.(string); ok && isStr {
		n, err := strconv.ParseFloat(strings.TrimSpace(s), 64)
		if err != nil {
			f.fail(name, "a number (as text)", v, true)
			return 0
		}
		return n
	}
	return f.number(name)
}

// enumOrd reads an enum field "Type::Value" of the nodes' enums as its ordinal and its value without the prefix.
func (f *fields) enumOrd(name, typ string) (ord int, value string) {
	s := f.str(name)
	if f.err != nil {
		return 0, ""
	}
	t, o, ok := aocexpr.EnumOrdinalOf(s)
	if !ok || t != typ {
		f.err = fmt.Errorf("%s: field %s is %q, not a value of %s", f.where, name, s, typ)
		return 0, ""
	}
	return o, s[strings.Index(s, "::")+2:]
}

// built with C | O | A and C-Y-B

// enumMask reads an array of enum strings of type typ as a bit mask of their ordinals.
func (f *fields) enumMask(name, typ string) int64 {
	var mask int64
	for _, v := range f.list(name) {
		s, ok := v.(string)
		if !ok {
			f.fail(name, "array of enum strings", v, true)
			return 0
		}
		t, o, known := aocexpr.EnumOrdinalOf(s)
		if !known || t != typ || o >= 62 {
			if f.err == nil {
				f.err = fmt.Errorf("%s: %s has %q, not a value of %s", f.where, name, s, typ)
			}
			return 0
		}
		mask |= 1 << uint(o)
	}
	return mask
}

// duration reads a Timespan struct {Duration} (milliseconds).
func (f *fields) duration(name string) int64 {
	s := f.strukt(name)
	if f.err != nil {
		return 0
	}
	return newFields(f.where+": "+name, s).int("Duration")
}

// xform is an FTransform of the node layouts and asset sets.
type xform struct {
	q [4]float64 // rotation X, Y, Z, W
	t [3]float64 // translation
	s [3]float64 // scale
}

func readVec3(f *fields, name string, keys [3]string) [3]float64 {
	s := f.strukt(name)
	if f.err != nil {
		return [3]float64{}
	}
	vf := newFields(f.where+": "+name, s)
	out := [3]float64{vf.number(keys[0]), vf.number(keys[1]), vf.number(keys[2])}
	f.err = vf.err
	return out
}

// transform reads a Transform struct {Rotation{X,Y,Z,W}, Translation, Scale3D}.
func (f *fields) transform(name string) xform {
	s := f.strukt(name)
	if f.err != nil {
		return xform{}
	}
	tf := newFields(f.where+": "+name, s)
	rot := tf.strukt("Rotation")
	var x xform
	if tf.err == nil {
		rf := newFields(tf.where+": Rotation", rot)
		x.q = [4]float64{rf.number("X"), rf.number("Y"), rf.number("Z"), rf.number("W")}
		tf.err = rf.err
	}
	x.t = readVec3(tf, "Translation", [3]string{"X", "Y", "Z"})
	x.s = readVec3(tf, "Scale3D", [3]string{"X", "Y", "Z"})
	f.err = tf.err
	return x
}

// structs reads an array of structs.
func (f *fields) structs(name string) []designdb.Struct {
	var out []designdb.Struct
	for i, v := range f.list(name) {
		s, ok := v.(designdb.Struct)
		if !ok {
			f.fail(name, fmt.Sprintf("array of structs (element %d)", i), v, true)
			return nil
		}
		out = append(out, s)
	}
	return out
}

// idMap reads a map whose values are Id structs into the keys and the Guids.
func (f *fields) idMap(name string) (keys []designdb.Value, guids []int64) {
	for i, p := range f.pairs(name) {
		s, ok := p.Value.(designdb.Struct)
		if !ok {
			f.fail(name, "map of Id structs", p, true)
			return nil, nil
		}
		g, _ := idOf(f, fmt.Sprintf("%s: %s[%d]", f.where, name, i), s)
		keys = append(keys, p.Key)
		guids = append(guids, g)
	}
	return keys, guids
}

func nodeEnumKey(k designdb.Value, typ string) (int, error) {
	s, ok := k.(string)
	if !ok {
		return 0, fmt.Errorf("the key %v is %T, want an %s string", k, k, typ)
	}
	t, o, known := aocexpr.EnumOrdinalOf(s)
	if !known || t != typ {
		return 0, fmt.Errorf("the key %q is not a value of %s", s, typ)
	}
	return o, nil
}

// stateLists are the BuildingNodeAssetSets and BuildingLevelSpawn lists of each EBuildingState (Empty has none).
var stateAssetLists = []string{"", "ConstructionAssetSetIds", "OperationalAssetSetIds", "DisarrayAssetSetIds", "DamagedAssetSetIds", "RepairingAssetSetIds",
	"DestroyedAssetSetIds", "DemolitionAssetSetIds"}
var statePopLists = []string{"", "ConstructionPopulationIds", "OperationalPopulationIds", "DisarrayPopulationIds", "DamagedPopulationIds", "RepairingPopulationIds",
	"DestroyedPopulationIds", "DemolitionPopulationIds"}

// C_Y_B captures, C.O.A code

// buildNodes fills the city node tables.
func buildNodes(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range nodeSchemas[:2] {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	rows := func(schema string) []designdb.Row { return src.rows[schema] }
	counts := map[string]int{}
	insert := func(table, query string, args ...any) error {
		if err := w.exec(query, args...); err != nil {
			return err
		}
		counts[table]++
		return nil
	}

	// --- the zones of influence ---------------------------------------------------------------------------------------------------------------
	zoiOfNode := map[int64]int64{}
	zois := append([]designdb.Row(nil), rows("ZOIRecordBase")...)
	sort.Slice(zois, func(i, j int) bool { return zois[i].Guid < zois[j].Guid })
	zoiType := map[int64]string{}
	for _, r := range zois {
		f := newFields(fmt.Sprintf("ZOI %s (%d)", r.Name, r.Guid), r.Fields)
		typeOrd, typeName := f.enumOrd("ZoiType", "EZoiType")
		biomeOrd, biomeName := f.enumOrd("Biome", "EBiomeType")
		node, _ := f.id("CityNodeId")
		env := enumValue(f.str("ZoiEnvironmentType"))
		ext := f.strukt("Extents")
		adj := f.ids("AdjacentZonesOfInfluenceIds")
		if f.err != nil {
			return f.err
		}
		ef := newFields(f.where+": Extents", ext)
		valid := ef.boolean("IsValid")
		lo := readVec3(ef, "Min", [3]string{"X", "Y", "Z"})
		hi := readVec3(ef, "Max", [3]string{"X", "Y", "Z"})
		if ef.err != nil {
			return ef.err
		}
		if node != 0 {
			if prev, dup := zoiOfNode[node]; dup && prev != r.Guid {
				return fmt.Errorf("%s: node %d has the zones %d and %d (a node has one)", f.where, node, prev, r.Guid)
			}
			zoiOfNode[node] = r.Guid
		}
		zoiType[r.Guid] = typeName
		if err := insert("city_zoi", `INSERT INTO city_zoi (guid, name, zoi_type, zoi_type_ord, biome, biome_ord, node_guid, environment, has_extents,
			min_x, min_y, min_z, max_x, max_y, max_z) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, typeName, typeOrd, biomeName, biomeOrd, node, env, b2i(valid), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]); err != nil {
			return err
		}
		for i, a := range adj {
			if err := insert("city_zoi_adjacent", `INSERT INTO city_zoi_adjacent (zoi_guid, ord, adjacent_guid) VALUES (?, ?, ?)`, r.Guid, i, a); err != nil {
				return err
			}
		}
	}

	// --- the nodes ----------------------------------------------------------------------------------------------------------------------------
	nodes := append([]designdb.Row(nil), rows("CityNodeRecordBase")...)
	sort.Slice(nodes, func(i, j int) bool { return nodes[i].Guid < nodes[j].Guid })
	levelable := 0
	for rank, r := range nodes {
		f := newFields(fmt.Sprintf("CityNode %s (%d)", r.Name, r.Guid), r.Fields)
		nodeName := f.str("nodeName")
		typeOrd, typeName := f.enumOrd("NodeType", "ENodeType")
		mapName := f.str("mapName")
		loc := readVec3(f, "location", [3]string{"X", "Y", "Z"})
		rot := readVec3(f, "rotation", [3]string{"Pitch", "Yaw", "Roll"})
		radius := f.number("Radius")
		size, _ := f.id("NodeSizeId")
		layout, _ := f.id("LayoutId")
		election, _ := f.id("ElectionDefinitionId")
		taxTable, _ := f.id("TaxTableId")
		dues, _ := f.id("CitizenshipDuesDefinitionId")
		propTax, _ := f.id("PropertyTaxDefinitionId")
		vendor, _ := f.id("VendorDefId")
		reqProp := f.boolean("DoesCitizenshipRequireProperty")
		mayorMin := f.int("mayorVoteMinimumCitizens")
		repMin := f.number("MinimumNodeToPlayerReputation")
		repMax := f.number("MaximumNodeToPlayerReputation")
		caravan, _ := f.enumOrd("MinimumLevelToTurnInCaravan", "ENodeLevel")
		atRisk := f.number("CitizenshipAtRiskTimeLimitSeconds")
		pops := f.ids("PopulationSetsIds")
		var details [7]int64
		for l := 0; l < 7; l++ {
			details[l], _ = f.id(fmt.Sprintf("L%d_DetailsId", l))
		}
		if f.err != nil {
			return f.err
		}
		zoi := zoiOfNode[r.Guid]
		// a node that grows: a type, the world map, and a zone of influence that has a node
		grows := typeOrd != 0 && mapName == "Verra_World_Master" && zoi != 0 && zoiType[zoi] == "HasNode"
		if grows {
			levelable++
		}
		if err := insert("city_node", `INSERT INTO city_node (rank, guid, name, node_name, node_type, node_type_ord, map_name, x, y, z, yaw, radius, size_guid,
			layout_guid, zoi_guid, levelable, election_def_guid, tax_table_guid, dues_def_guid, property_tax_def_guid, vendor_def_guid, requires_property,
			mayor_min_citizens, min_reputation, max_reputation, caravan_min_level, citizenship_at_risk_secs)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			rank, r.Guid, r.Name, nodeName, typeName, typeOrd, mapName, loc[0], loc[1], loc[2], rot[1], radius, size, layout, zoi, b2i(grows), election, taxTable,
			dues, propTax, vendor, b2i(reqProp), mayorMin, int64(repMin), int64(repMax), caravan, int64(atRisk)); err != nil {
			return err
		}
		for l, d := range details {
			if d == 0 {
				continue
			}
			if err := insert("city_node_level", `INSERT INTO city_node_level (node_guid, level, details_guid) VALUES (?, ?, ?)`, r.Guid, l, d); err != nil {
				return err
			}
		}
		for i, p := range pops {
			if err := insert("city_node_population", `INSERT INTO city_node_population (node_guid, ord, population_set_guid) VALUES (?, ?, ?)`, r.Guid, i, p); err != nil {
				return err
			}
		}
	}

	// --- level details, modifications, size, the tree's rules ---------------------------------------------------------------------------------
	for _, r := range rows("NodeLevelDetailsRecordBase") {
		f := newFields(fmt.Sprintf("NodeLevelDetails %s (%d)", r.Name, r.Guid), r.Fields)
		mods := f.ids("LevelModificationsIds")
		if f.err != nil {
			return f.err
		}
		if err := insert("node_level_details", `INSERT INTO node_level_details (guid, name) VALUES (?, ?)`, r.Guid, r.Name); err != nil {
			return err
		}
		for i, m := range mods {
			if err := insert("node_level_details_mod", `INSERT INTO node_level_details_mod (details_guid, ord, mod_guid) VALUES (?, ?, ?)`, r.Guid, i, m); err != nil {
				return err
			}
		}
	}
	for _, r := range rows("NodeModificationRecordBase") {
		f := newFields(fmt.Sprintf("NodeModification %s (%d)", r.Name, r.Guid), r.Fields)
		relic, hand, slots, essence := f.int("ActiveRelicSlots"), f.int("CommissionHandSlots"), f.int("CommissionSlots"), f.int("NodeEssence")
		radius := f.number("VehicleDeployRadius")
		reduction := f.looseNumber("ServiceBuildingMaintenanceCostReduction")
		unlocks := f.boolean("UnlocksCraftingRecipes")
		refs := []struct {
			kind, field string
		}{{"effect", "EffectsIds"}, {"mayoral_ability", "MayoralAbilitiesIds"}, {"commission", "NCSCommissionIds"}, {"recipe_set", "CraftingRecipeSetsIds"}}
		var lists [4][]int64
		for i, rf := range refs {
			lists[i] = f.ids(rf.field)
		}
		if f.err != nil {
			return f.err
		}
		if err := insert("node_modification", `INSERT INTO node_modification (guid, name, relic_slots, commission_hand_slots, commission_slots, node_essence,
			vehicle_deploy_radius, maintenance_cost_reduction, unlocks_recipes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, relic, hand, slots, essence, radius, reduction, b2i(unlocks)); err != nil {
			return err
		}
		for i, rf := range refs {
			for ord, g := range lists[i] {
				if err := insert("node_modification_ref", `INSERT INTO node_modification_ref (mod_guid, kind, ord, ref_guid) VALUES (?, ?, ?, ?)`, r.Guid, rf.kind, ord, g); err != nil {
					return err
				}
			}
		}
	}
	for _, r := range rows("NodeSizeRecordBase") {
		f := newFields(fmt.Sprintf("NodeSize %s (%d)", r.Name, r.Guid), r.Fields)
		radii := f.pairs("NodeLevelRadii")
		if f.err != nil {
			return f.err
		}
		if err := insert("node_size", `INSERT INTO node_size (guid, name) VALUES (?, ?)`, r.Guid, r.Name); err != nil {
			return err
		}
		for _, p := range radii {
			level, ok := p.Key.(int64)
			rf := newFields(f.where+": NodeLevelRadii", designdb.Struct{"v": p.Value})
			radius := rf.number("v")
			if !ok || rf.err != nil {
				return fmt.Errorf("%s: NodeLevelRadii entry %v", f.where, p)
			}
			if err := insert("node_size_radius", `INSERT INTO node_size_radius (size_guid, level, radius) VALUES (?, ?, ?)`, r.Guid, level, radius); err != nil {
				return err
			}
		}
	}
	desired := byGuid(rows("ChildNodeDesiredLevelRulesRecordBase"))
	for _, r := range rows("ChildNodeRulesRecordBase") {
		f := newFields(fmt.Sprintf("ChildNodeRules %s (%d)", r.Name, r.Guid), r.Fields)
		keys, guids := f.idMap("ChildNodeDesiredLevelRulesIds")
		if f.err != nil {
			return f.err
		}
		for i, k := range keys {
			parent, err := nodeEnumKey(k, "ENodeLevel")
			if err != nil {
				return fmt.Errorf("%s: %w", f.where, err)
			}
			d, ok := desired[guids[i]]
			if !ok {
				return fmt.Errorf("%s: ChildNodeDesiredLevelRules %d is no row", f.where, guids[i])
			}
			df := newFields(fmt.Sprintf("ChildNodeDesiredLevelRules %s (%d)", d.Name, d.Guid), d.Fields)
			for _, p := range df.pairs("MaxChildNodesOfThisLevel") {
				child, err := nodeEnumKey(p.Key, "ENodeLevel")
				if err != nil {
					return fmt.Errorf("%s: %w", df.where, err)
				}
				cf := newFields(df.where, designdb.Struct{"n": p.Value})
				max := cf.int("n")
				if cf.err != nil {
					return cf.err
				}
				if err := insert("child_node_rule", `INSERT INTO child_node_rule (rules_guid, rules_name, parent_level, child_level, max_children) VALUES (?, ?, ?, ?, ?)`,
					r.Guid, r.Name, parent, child, max); err != nil {
					return err
				}
			}
			if df.err != nil {
				return df.err
			}
		}
	}
	for _, r := range rows("VassalshipDefRecordBase") {
		f := newFields(fmt.Sprintf("VassalshipDef %s (%d)", r.Name, r.Guid), r.Fields)
		rules, _ := f.id("ChildNodeRulesId")
		maxLevel, vassals := f.int("MaxNumOfMaxLevelNodes"), f.int("MaxNumOfVassals")
		minLevel, _ := f.enumOrd("MinimumLevelToAcquireVassals", "ENodeLevel")
		tax := f.looseNumber("VassalTaxPercentage")
		every := f.int("VassalTaxCollectionFrequencySeconds")
		sizes := f.list("RequiredRegionSizeAtLevel")
		if f.err != nil {
			return f.err
		}
		if err := insert("vassalship_def", `INSERT INTO vassalship_def (guid, name, rules_guid, max_max_level_nodes, max_vassals, min_level_to_acquire, tax_percentage,
			tax_frequency_secs) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, rules, maxLevel, vassals, minLevel, tax, every); err != nil {
			return err
		}
		for level, v := range sizes {
			n, ok := v.(int64)
			if !ok {
				return fmt.Errorf("%s: RequiredRegionSizeAtLevel[%d] is %T", f.where, level, v)
			}
			if err := insert("vassalship_region_size", `INSERT INTO vassalship_region_size (def_guid, level, size) VALUES (?, ?, ?)`, r.Guid, level, n); err != nil {
				return err
			}
		}
	}

	// --- taxes, reputation, citizenship, elections --------------------------------------------------------------------------------------------
	for _, r := range rows("TaxDetailsRecordBase") {
		f := newFields(fmt.Sprintf("TaxDetails %s (%d)", r.Name, r.Guid), r.Fields)
		vals := [7]float64{f.looseNumber("DefaultTaxPercent"), f.looseNumber("MinimumTaxPercentage"), f.looseNumber("MaximumTaxPercentage"),
			f.looseNumber("DefaultFlatTaxAmount"), f.looseNumber("MinimumFlatTaxAmount"), f.looseNumber("MaximumFlatTaxAmount"), f.looseNumber("OwningNodeTaxPercentage")}
		if f.err != nil {
			return f.err
		}
		if err := insert("node_tax_details", `INSERT INTO node_tax_details (guid, name, default_percent, min_percent, max_percent, default_flat, min_flat, max_flat,
			owning_node_percent) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6]); err != nil {
			return err
		}
	}
	for _, r := range rows("NodeGlobalTaxTableRecordBase") {
		f := newFields(fmt.Sprintf("NodeGlobalTaxTable %s (%d)", r.Name, r.Guid), r.Fields)
		entries := f.pairs("GlobalTaxTable")
		if f.err != nil {
			return f.err
		}
		if err := insert("node_tax_table", `INSERT INTO node_tax_table (guid, name) VALUES (?, ?)`, r.Guid, r.Name); err != nil {
			return err
		}
		for ord, p := range entries {
			k, kok := p.Key.(designdb.Struct)
			v, vok := p.Value.(designdb.Struct)
			if !kok || !vok {
				return fmt.Errorf("%s: GlobalTaxTable entry %d is %T -> %T", f.where, ord, p.Key, p.Value)
			}
			tag := newFields(f.where, k).str("TagName")
			vf := newFields(fmt.Sprintf("%s: %s", f.where, tag), v)
			avail := vf.str("AvailabilityPredicate")
			details, _ := vf.id("TaxDetails")
			if vf.err != nil {
				return vf.err
			}
			if err := insert("node_tax_table_tag", `INSERT INTO node_tax_table_tag (table_guid, ord, tag, availability, details_guid) VALUES (?, ?, ?, ?, ?)`,
				r.Guid, ord, tag, avail, details); err != nil {
				return err
			}
		}
	}
	for _, r := range rows("NodeToPlayerReputationTierRecordBase") {
		f := newFields(fmt.Sprintf("NodeToPlayerReputationTier %s (%d)", r.Name, r.Guid), r.Fields)
		rankOrd, rankName := f.enumOrd("TierRank", "ENodeReputationRank")
		mod := f.looseNumber("NodeTaxModifier")
		mayoral, building, demolition := f.boolean("CanVoteMayoral"), f.boolean("CanVoteBuildingAndExpansion"), f.boolean("CanVoteDemolition")
		if f.err != nil {
			return f.err
		}
		if err := insert("node_reputation_tier", `INSERT INTO node_reputation_tier (guid, name, rank_ord, rank_name, tax_modifier, can_vote_mayoral, can_vote_building,
			can_vote_demolition) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, rankOrd, rankName, mod, b2i(mayoral), b2i(building), b2i(demolition)); err != nil {
			return err
		}
	}
	for _, r := range rows("CitizenshipNodeRecordBase") {
		f := newFields(fmt.Sprintf("CitizenshipNode %s (%d)", r.Name, r.Guid), r.Fields)
		level, cooldown := f.int("Level"), f.int("CitizenshipCooldownTimeSeconds")
		duesScalar, taxScalar := f.looseNumber("DuesNodeLevelScalar"), f.looseNumber("PropertyTaxNodeLevelScalar")
		minSlots, maxSlots := f.int("MinimumBuyOrderSlots"), f.int("MaximumBuyOrderSlots")
		missedDues, missedTax := f.looseNumber("MissedDuesPercentIncrease"), f.looseNumber("MissedTaxPercentIncrease")
		if f.err != nil {
			return f.err
		}
		if err := insert("citizenship_node", `INSERT INTO citizenship_node (guid, name, level, cooldown_secs, dues_level_scalar, property_tax_scalar, min_buy_order_slots,
			max_buy_order_slots, missed_dues_increase, missed_tax_increase) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, level, cooldown, duesScalar, taxScalar, minSlots, maxSlots, missedDues, missedTax); err != nil {
			return err
		}
	}
	scalars := byGuid(rows("QuantityScalarDefRecordBase"))
	for _, r := range rows("CitizenshipGenericRecordBase") {
		f := newFields(fmt.Sprintf("CitizenshipGeneric %s (%d)", r.Name, r.Guid), r.Fields)
		due, fee := f.int("InitialDueCost"), f.int("InitialFeeCost")
		missedDues, missedTax := f.int("MissedDuesPaymentPeriod"), f.int("MissedTaxPaymentPeriod")
		missableDues, missableTax := f.int("NumberOfMissableDuesPayments"), f.int("NumberOfMissableTaxPayments")
		dues, tax := f.ids("DuesPopulationCountScalarsIds"), f.ids("PropertyTaxCountScalarsIds")
		if f.err != nil {
			return f.err
		}
		if err := insert("citizenship_generic", `INSERT INTO citizenship_generic (guid, name, initial_due_cost, initial_fee_cost, missed_dues_period, missed_tax_period,
			missable_dues_payments, missable_tax_payments) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, due, fee, missedDues, missedTax, missableDues, missableTax); err != nil {
			return err
		}
		for _, kl := range []struct {
			kind string
			ids  []int64
		}{{"dues", dues}, {"property_tax", tax}} {
			for ord, g := range kl.ids {
				s, ok := scalars[g]
				if !ok {
					return fmt.Errorf("%s: QuantityScalarDef %d is no row", f.where, g)
				}
				sf := newFields(fmt.Sprintf("QuantityScalarDef %s (%d)", s.Name, s.Guid), s.Fields)
				threshold, scalar := sf.looseNumber("Threshold"), sf.looseNumber("Scalar")
				if sf.err != nil {
					return sf.err
				}
				if err := insert("citizenship_scalar", `INSERT INTO citizenship_scalar (generic_guid, kind, ord, guid, name, threshold, scalar) VALUES (?, ?, ?, ?, ?, ?, ?)`,
					r.Guid, kl.kind, ord, g, s.Name, threshold, scalar); err != nil {
					return err
				}
			}
		}
	}
	for _, r := range rows("CitizenshipDuesDefRecordBase") {
		f := newFields(fmt.Sprintf("CitizenshipDuesDef %s (%d)", r.Name, r.Guid), r.Fields)
		base, _ := f.id("BaseDuesRequiredId")
		rank, level, nonOwner := f.str("CitizenshipRankImpact"), f.str("LevelImpact"), f.str("NonPropertyOwnerImpact")
		day := enumValue(f.str("DueDayOfTheWeek"))
		weeks, late, overdue := f.int("FrequencyDuesPaidWeeks"), f.looseNumber("LateFee"), f.int("OverduePeriodDays")
		if f.err != nil {
			return f.err
		}
		if err := insert("citizenship_dues_def", `INSERT INTO citizenship_dues_def (guid, name, base_dues_guid, rank_impact, level_impact, non_property_owner, due_day_of_week,
			weeks, late_fee, overdue_days) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, base, rank, level, nonOwner, day, weeks, late, overdue); err != nil {
			return err
		}
	}
	for _, r := range rows("PropertyTaxDuesDefRecordBase") {
		f := newFields(fmt.Sprintf("PropertyTaxDuesDef %s (%d)", r.Name, r.Guid), r.Fields)
		day := enumValue(f.str("DueDayOfTheWeek"))
		weeks, overdue := f.int("FrequencyDuesPaidWeeks"), f.int("OverduePeriodDays")
		item, _ := f.id("ItemUsedToPayId")
		if f.err != nil {
			return f.err
		}
		if err := insert("property_tax_def", `INSERT INTO property_tax_def (guid, name, item_guid, due_day_of_week, weeks, overdue_days) VALUES (?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, item, day, weeks, overdue); err != nil {
			return err
		}
	}
	for _, r := range rows("CityNodeElectionDefRecordBase") {
		f := newFields(fmt.Sprintf("CityNodeElectionDef %s (%d)", r.Name, r.Guid), r.Fields)
		kind := enumValue(f.str("ElectionType"))
		regDays, elDays := f.int("RegistrationPeriodDurationDays"), f.int("ElectionPeriodDurationDays")
		startDay, startHour, endHour, resultsHour := f.int("ElectionPeriodStartDay"), f.int("ElectionPeriodStartHour"), f.int("ElectionPeriodEndHour"), f.int("ElectionResultsHour")
		term := f.int("MinimumLengthOfTermDays")
		firstReg, firstEl, firstWait := f.int("FirstTimeRegistrationPeriodDurationSeconds"), f.int("FirstTimeElectionPeriodDurationSeconds"), f.int("FirstTimeWaitForResultsPeriodDurationSeconds")
		quest, _ := f.id("RequiredQuestId")
		if f.err != nil {
			return f.err
		}
		if err := insert("election_def", `INSERT INTO election_def (guid, name, election_type, registration_days, election_days, start_day, start_hour, end_hour, results_hour,
			min_term_days, first_registration_secs, first_election_secs, first_wait_for_results_secs, required_quest_guid) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, kind, regDays, elDays, startDay, startHour, endHour, resultsHour, term, firstReg, firstEl, firstWait, quest); err != nil {
			return err
		}
	}
	for _, c := range []struct{ schema, field string }{{"ElectionConstantsRecordBase", "RankedChoiceVoteCountLimit"}, {"CityNodeConstantsRecordBase", "NodeToPlayerReputationDecayPerHour"}} {
		for _, r := range rows(c.schema) {
			f := newFields(fmt.Sprintf("%s %s (%d)", c.schema, r.Name, r.Guid), r.Fields)
			v := f.looseNumber(c.field)
			if f.err != nil {
				return f.err
			}
			if err := insert("node_constant", `INSERT INTO node_constant (name, value) VALUES (?, ?)`, c.field, strconv.FormatFloat(v, 'g', -1, 64)); err != nil {
				return err
			}
		}
	}

	// --- layouts, asset sets, asset definitions -----------------------------------------------------------------------------------------------
	putTransform := func(table, query string, key []any, x xform) error {
		args := append(append([]any(nil), key...), x.q[0], x.q[1], x.q[2], x.q[3], x.t[0], x.t[1], x.t[2], x.s[0], x.s[1], x.s[2])
		return insert(table, query, args...)
	}
	for _, r := range rows("LayoutRecordBase") {
		f := newFields(fmt.Sprintf("Layout %s (%d)", r.Name, r.Guid), r.Fields)
		level := f.int("level")
		props, plots := f.structs("Props"), f.pairs("PlotsPerNodeLevel")
		pops := f.ids("PopulationSetsIds")
		if f.err != nil {
			return f.err
		}
		if err := insert("node_layout", `INSERT INTO node_layout (guid, name, level) VALUES (?, ?, ?)`, r.Guid, r.Name, level); err != nil {
			return err
		}
		for i, p := range props {
			pf := newFields(fmt.Sprintf("%s: Props[%d]", f.where, i), p)
			set, _ := pf.id("AssetSet")
			x := pf.transform("Transform")
			if pf.err != nil {
				return pf.err
			}
			if err := putTransform("node_layout_prop", `INSERT INTO node_layout_prop (layout_guid, idx, asset_set_guid, qx, qy, qz, qw, tx, ty, tz, sx, sy, sz)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, []any{r.Guid, i, set}, x); err != nil {
				return err
			}
		}
		for _, lp := range plots {
			lvl, ok := lp.Key.(int64)
			val, vok := lp.Value.(designdb.Struct)
			if !ok || !vok {
				return fmt.Errorf("%s: PlotsPerNodeLevel entry %T -> %T", f.where, lp.Key, lp.Value)
			}
			vf := newFields(fmt.Sprintf("%s: PlotsPerNodeLevel[%d]", f.where, lvl), val)
			list := vf.structs("Plots")
			if vf.err != nil {
				return vf.err
			}
			for i, pl := range list {
				pf := newFields(fmt.Sprintf("%s: Plots[%d]", vf.where, i), pl)
				plot, _ := pf.id("Plot")
				x := pf.transform("Transform")
				if pf.err != nil {
					return pf.err
				}
				if err := putTransform("node_layout_plot", `INSERT INTO node_layout_plot (layout_guid, level, idx, plot_guid, qx, qy, qz, qw, tx, ty, tz, sx, sy, sz)
					VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, []any{r.Guid, lvl, i, plot}, x); err != nil {
					return err
				}
			}
		}
		for i, p := range pops {
			if err := insert("node_layout_population", `INSERT INTO node_layout_population (layout_guid, ord, population_set_guid) VALUES (?, ?, ?)`, r.Guid, i, p); err != nil {
				return err
			}
		}
	}
	for _, r := range rows("NodeAssetSetRecordBase") {
		f := newFields(fmt.Sprintf("NodeAssetSet %s (%d)", r.Name, r.Guid), r.Fields)
		members := f.structs("ContainedAssetDefinitions")
		if f.err != nil {
			return f.err
		}
		if err := insert("node_asset_set", `INSERT INTO node_asset_set (guid, name) VALUES (?, ?)`, r.Guid, r.Name); err != nil {
			return err
		}
		for i, m := range members {
			mf := newFields(fmt.Sprintf("%s: ContainedAssetDefinitions[%d]", f.where, i), m)
			def, _ := mf.id("Asset")
			x := mf.transform("Transform")
			if mf.err != nil {
				return mf.err
			}
			if err := putTransform("node_asset_set_member", `INSERT INTO node_asset_set_member (set_guid, ord, def_guid, qx, qy, qz, qw, tx, ty, tz, sx, sy, sz)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, []any{r.Guid, i, def}, x); err != nil {
				return err
			}
		}
	}
	for _, r := range rows("NodeAssetDefinitionRecordBase") {
		f := newFields(fmt.Sprintf("NodeAssetDefinition %s (%d)", r.Name, r.Guid), r.Fields)
		lo, hi := f.int("NodeLevelMinimum"), f.int("NodeLevelMaximum")
		weight := f.looseNumber("Weight")
		allBiomes := f.boolean("bAllBiomes")
		cultures := f.enumMask("ValidCultures", "ECharacterRace")
		types := f.enumMask("ValidNodeTypes", "ENodeType")
		seasons := f.enumMask("ValidSeasons", "ESeason")
		biomes := f.enumMask("ValidBiomes", "EBiomeType")
		tags := f.tags("ValidGameplayTags")
		if f.err != nil {
			return f.err
		}
		if err := insert("node_asset_def", `INSERT INTO node_asset_def (guid, name, level_min, level_max, weight, all_biomes, cultures, node_types, seasons, biomes)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, lo, hi, weight, b2i(allBiomes), cultures, types, seasons, biomes); err != nil {
			return err
		}
		for i, t := range tags {
			if err := insert("node_asset_def_tag", `INSERT INTO node_asset_def_tag (def_guid, ord, tag) VALUES (?, ?, ?)`, r.Guid, i, t); err != nil {
				return err
			}
		}
	}

	// --- plots and buildings ------------------------------------------------------------------------------------------------------------------
	for _, r := range rows("ServiceBuildingPlotRecordBase") {
		f := newFields(fmt.Sprintf("ServiceBuildingPlot %s (%d)", r.Name, r.Guid), r.Fields)
		buildings := f.ids("BuildingsIds")
		empty, _ := f.id("EmptyBuildingId")
		pre := f.boolean("PreConstruct")
		state, _ := f.enumOrd("StartingState", "EBuildingState")
		if f.err != nil {
			return f.err
		}
		if err := insert("service_plot", `INSERT INTO service_plot (guid, name, pre_construct, starting_state, empty_building_guid) VALUES (?, ?, ?, ?, ?)`,
			r.Guid, r.Name, b2i(pre), state, empty); err != nil {
			return err
		}
		for i, b := range buildings {
			if err := insert("service_plot_building", `INSERT INTO service_plot_building (plot_guid, ord, building_guid) VALUES (?, ?, ?)`, r.Guid, i, b); err != nil {
				return err
			}
		}
	}
	for _, r := range rows("BuildingRecordBase") {
		f := newFields(fmt.Sprintf("Building %s (%d)", r.Name, r.Guid), r.Fields)
		kind := enumValue(f.str("ServiceBuildingType"))
		sets, _ := f.id("BuildingAssetSetId")
		phases, _ := f.id("BuildingContributionPhasesId")
		xp := f.looseNumber("CompletionNodeExperience")
		timeout := f.boolean("CanConstructionTimeOut")
		construction, demolition, vote, destroyed, repair := f.duration("ConstructionPhaseDuration"), f.duration("DemolitionPhaseDuration"),
			f.duration("DemolitionVoteDuration"), f.duration("DestroyedPhaseDuration"), f.duration("RepairPhaseDuration")
		nodeCost, _ := f.id("NodeConstructionCostId")
		playerCost, _ := f.id("PlayerConstructionCostId")
		maintenance, _ := f.id("MaintenanceCostId")
		disarray, _ := f.id("DisarrayRepairCostId")
		damaged, _ := f.id("DamagedRepairCostId")
		tree, _ := f.id("ExpansionSkillTreeId")
		manager := f.boolean("bDisplayInBuildingManager")
		spawnKeys, spawnGuids := f.idMap("BuildingPopSpawnMapIds")
		if f.err != nil {
			return f.err
		}
		if err := insert("building", `INSERT INTO building (guid, name, building_type, asset_sets_guid, contribution_phases_guid, completion_node_xp, can_time_out, construction_ms,
			demolition_ms, demolition_vote_ms, destroyed_ms, repair_ms, node_cost_guid, player_cost_guid, maintenance_cost_guid, disarray_repair_guid, damaged_repair_guid,
			expansion_tree_guid, in_building_manager) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, kind, sets, phases, xp, b2i(timeout), construction, demolition, vote, destroyed, repair, nodeCost, playerCost, maintenance, disarray, damaged,
			tree, b2i(manager)); err != nil {
			return err
		}
		for i, k := range spawnKeys {
			level, err := nodeEnumKey(k, "ENodeLevel")
			if err != nil {
				return fmt.Errorf("%s: %w", f.where, err)
			}
			if err := insert("building_pop_spawn", `INSERT INTO building_pop_spawn (building_guid, level, spawn_guid) VALUES (?, ?, ?)`, r.Guid, level, spawnGuids[i]); err != nil {
				return err
			}
		}
	}
	for _, r := range rows("BuildingNodeAssetSetsRecordBase") {
		f := newFields(fmt.Sprintf("BuildingNodeAssetSets %s (%d)", r.Name, r.Guid), r.Fields)
		if err := insert("building_asset_sets", `INSERT INTO building_asset_sets (guid, name) VALUES (?, ?)`, r.Guid, r.Name); err != nil {
			return err
		}
		for state := 1; state < len(stateAssetLists); state++ {
			for ord, g := range f.ids(stateAssetLists[state]) {
				if err := insert("building_asset_state", `INSERT INTO building_asset_state (sets_guid, state, ord, asset_set_guid) VALUES (?, ?, ?, ?)`, r.Guid, state, ord, g); err != nil {
					return err
				}
			}
		}
		if f.err != nil {
			return f.err
		}
	}
	for _, r := range rows("BuildingLevelSpawnRecordBase") {
		f := newFields(fmt.Sprintf("BuildingLevelSpawn %s (%d)", r.Name, r.Guid), r.Fields)
		if err := insert("building_level_spawn", `INSERT INTO building_level_spawn (guid, name) VALUES (?, ?)`, r.Guid, r.Name); err != nil {
			return err
		}
		for state := 1; state < len(statePopLists); state++ {
			for ord, g := range f.ids(statePopLists[state]) {
				if err := insert("building_level_spawn_pop", `INSERT INTO building_level_spawn_pop (spawn_guid, state, ord, population_set_guid) VALUES (?, ?, ?, ?)`, r.Guid, state, ord, g); err != nil {
					return err
				}
			}
		}
		if f.err != nil {
			return f.err
		}
	}

	opt.logf("city_node: %d nodes (%d levelable), %d zones, %d layouts with %d props and %d plots, %d asset sets and %d definitions, %d buildings with %d plots",
		counts["city_node"], levelable, counts["city_zoi"], counts["node_layout"], counts["node_layout_prop"], counts["node_layout_plot"], counts["node_asset_set"],
		counts["node_asset_def"], counts["building"], counts["service_plot"])
	meta["typed.city_node.levelable"] = strconv.Itoa(levelable)
	for _, t := range content.NodeTables {
		meta["typed."+t] = strconv.Itoa(counts[t])
	}
	return nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// The city node tables on the real design database (the shared build of real_test.go), against what the Nodes 1 research found in the data and in the
// retail captures.

import (
	"testing"

	"aocrop/services/internal/content"
)

// Cyb - C | O | A

func TestRealNodes(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	count := func(q string, args ...any) int64 {
		t.Helper()
		return scalarInt(t, db, q, args...)
	}
	// the research's counts of the records
	for table, want := range map[string]int64{
		"city_node": 81, "city_zoi": 78, "node_level_details": 28, "node_modification": 424, "node_size": 2, "vassalship_def": 2, "vassalship_region_size": 14,
		"node_tax_details": 4, "node_tax_table": 3, "node_reputation_tier": 5, "citizenship_node": 2, "citizenship_generic": 1, "citizenship_scalar": 6,
		"citizenship_dues_def": 1, "property_tax_def": 1, "election_def": 2, "node_constant": 2, "node_layout": 70, "node_asset_set": 424, "node_asset_def": 634,
		"service_plot": 192, "building": 71, "building_asset_sets": 60, "building_level_spawn": 333,
	} {
		if got := count(`SELECT COUNT(*) FROM ` + table); got != want {
			t.Errorf("%s has %d rows, want %d", table, got, want)
		}
		if meta["typed."+table] != i64s(want) {
			t.Errorf("meta typed.%s = %q, want %d", table, meta["typed."+table], want)
		}
	}
	if got := count(`SELECT COUNT(*) FROM node_tax_table_tag WHERE table_guid = 6064632161568489473`); got != 78 {
		t.Errorf("the default global tax table has %d tags, want 78", got)
	}
	// 9 child rules: Metropolis 3, City 3, Town 2, Village 1
	if got := count(`SELECT COUNT(*) FROM child_node_rule`); got != 9 {
		t.Errorf("child_node_rule has %d rows, want 9", got)
	}
	if got := count(`SELECT max_children FROM child_node_rule WHERE parent_level = 4 AND child_level = 3`); got != 2 {
		t.Errorf("a Town may have %d Villages, want 2", got)
	}

	nodes, err := db.CityNodes()
	if err != nil || len(nodes) != 81 {
		t.Fatalf("CityNodes = %d, %v", len(nodes), err)
	}
	// the rank keys of the client's two small GameState maps, as captures 1198 and 930 show them
	ranks := map[string]int{"Verra_RVR_Winstead": 5, "Verra_RVR_Joeva": 6, "Verra_RVR_NewAela": 7, "Verra_RVR_Halcyon": 8, "Verra_RVR_Miraleth": 9, "Verra_JUN_Wildport": 43, "Verra_ANV_KalTorhum": 63}
	byName := map[string]content.CityNode{}
	grow := 0
	for i, n := range nodes {
		byName[n.Name] = n
		if n.Rank != i || (i > 0 && n.Guid <= nodes[i-1].Guid) {
			t.Fatalf("node %d: rank %d guid %d after %d", i, n.Rank, n.Guid, nodes[max(i-1, 0)].Guid)
		}
		if n.Levelable {
			grow++
		}
	}
	for name, want := range ranks {
		if byName[name].Rank != want {
			t.Errorf("%s has rank %d, want %d", name, byName[name].Rank, want)
		}
	}
	if grow != 26 || meta["typed.city_node.levelable"] != "26" {
		t.Errorf("%d nodes grow, want 26", grow)
	}
	joeva := byName["Verra_RVR_Joeva"]
	if joeva.Guid != 108646121231993 || joeva.NodeName != "Joeva" || joeva.NodeType != "Scientific" || joeva.NodeTypeOrd != 1 || !joeva.Levelable || joeva.ZoiGuid != 6064629126905528345 ||
		joeva.MayorMinCitizens != 10 || joeva.X != -1094396.042325 || joeva.CaravanMinLevel != content.NodeLevelVillage || joeva.CitizenshipAtRiskSecs != 604800 {
		t.Errorf("Joeva = %+v", joeva)
	}
	// types of the 26: Divine 7, Scientific 7, Economic 6, Martial 6
	types := map[string]int{}
	for _, n := range nodes {
		if n.Levelable {
			types[n.NodeType]++
		}
	}
	if types["Divine"] != 7 || types["Scientific"] != 7 || types["Economic"] != 6 || types["Martial"] != 6 {
		t.Errorf("types of the nodes that grow: %v", types)
	}

	zois, err := db.CityZois()
	if err != nil || len(zois) != 78 {
		t.Fatalf("CityZois = %d, %v", len(zois), err)
	}
	byZoi := map[int64]content.CityZoi{}
	for _, z := range zois {
		byZoi[z.Guid] = z
	}
	// the five Riverlands nodes' zones are Riverlands (EBiomeType 14) and have a node; Ruins of Aela belongs to Lionhold
	for _, name := range []string{"Verra_RVR_Winstead", "Verra_RVR_Joeva", "Verra_RVR_NewAela", "Verra_RVR_Halcyon", "Verra_RVR_Miraleth"} {
		z := byZoi[byName[name].ZoiGuid]
		if z.Biome != "Riverlands" || z.BiomeOrd != 14 || z.ZoiType != "HasNode" || z.NodeGuid != byName[name].Guid || !z.HasExtents {
			t.Errorf("%s: zone %+v", name, z)
		}
	}
	if z := byZoi[6064629127135166465]; z.Name != "Verra_RVR_StartingArea_RuinsOfAela" || z.NodeGuid != byName["Verra_RVR_Lionhold"].Guid || z.ZoiType != "CannotOwn" || len(z.Adjacent) != 5 {
		t.Errorf("Ruins of Aela = %+v", z)
	}
	if z := byZoi[byName["Verra_TUR_Windansea"].ZoiGuid]; z.Biome != "Tropics" || z.BiomeOrd != 18 {
		t.Errorf("Windansea's zone = %+v", z)
	}
	// Joeva's zone is next to Halcyon's, Winstead's and New Aela's (research appendix 17)
	adj := map[int64]bool{}
	for _, a := range byZoi[joeva.ZoiGuid].Adjacent {
		adj[a] = true
	}
	for _, name := range []string{"Verra_RVR_Halcyon", "Verra_RVR_Winstead", "Verra_RVR_NewAela"} {
		if !adj[byName[name].ZoiGuid] {
			t.Errorf("Joeva's zone is not next to %s's", name)
		}
	}

	// the Village level of the 27 nodes that name one, and the base modification's numbers
	lv, err := db.CityNodeLevelDetails()
	if err != nil {
		t.Fatal(err)
	}
	if lv[joeva.Guid][3] == 0 || len(lv[joeva.Guid]) != 1 {
		t.Errorf("Joeva's level details: %v", lv[joeva.Guid])
	}
	mods, err := db.NodeModifications()
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, m := range mods {
		if m.Guid == 6064633379597385729 {
			found = true
			if m.Name != "Verra_NodeBase_L3" || m.RelicSlots != 3 || m.CommissionHandSlots != 10 || m.CommissionSlots != 6 || m.NodeEssence != 70 || m.VehicleDeployRadius != 40000 {
				t.Errorf("Verra_NodeBase_L3 = %+v", m)
			}
		}
	}
	if !found {
		t.Error("Verra_NodeBase_L3 is not a node modification")
	}
	vd, err := db.VassalshipDefs()
	if err != nil || len(vd) != 2 {
		t.Fatalf("VassalshipDefs = %v, %v", vd, err)
	}
	for _, v := range vd {
		if v.Guid == 6064631443227279360 && (v.MaxMaxLevelNodes != 5 || v.MaxVassals != 10 || v.MinLevelToAcquire != 3 || len(v.RequiredRegionSizeAt) != 7 || v.RequiredRegionSizeAt[4] != 5) {
			t.Errorf("MaxLevelVassalshipDef = %+v", v)
		}
	}
	// the reputation tiers' tax modifiers, the election, the dues and the decay
	if got := count(`SELECT COUNT(*) FROM node_reputation_tier WHERE (rank_ord, tax_modifier) IN ((0, 1.0), (1, 0.95), (2, 0.9), (3, 0.85), (4, 0.8))`); got != 5 {
		t.Errorf("%d reputation tiers have the research's tax modifier, want 5", got)
	}
	if got := count(`SELECT COUNT(*) FROM election_def WHERE name = 'Scientific_Election_Definition' AND registration_days = 5 AND election_days = 5 AND results_hour = 1
		AND first_registration_secs = 86400 AND first_election_secs = 86400`); got != 1 {
		t.Errorf("the live election definition is not as the research found it")
	}
	if got := count(`SELECT COUNT(*) FROM citizenship_dues_def WHERE due_day_of_week = 'Thursday' AND weeks = 2 AND overdue_days = 5`); got != 1 {
		t.Errorf("the dues definition is not as the research found it")
	}
	if got := count(`SELECT COUNT(*) FROM node_constant WHERE (name = 'RankedChoiceVoteCountLimit' AND value = '3') OR (name = 'NodeToPlayerReputationDecayPerHour' AND value = '1')`); got != 2 {
		t.Errorf("the node constants are not as the research found them")
	}
	// the layouts: Flat_D, River_C and Coastal_Flat_A serve the 26 nodes; River_C has 78 props and 23, 31, 31, 25, 4 and 8 plots at levels 3, 4, 5, 6, 1 and 2
	if got := count(`SELECT COUNT(*) FROM node_layout_prop WHERE layout_guid = 6064631477152448529`); got != 78 {
		t.Errorf("Layout_River_C has %d props, want 78", got)
	}
	for level, want := range map[int]int64{1: 4, 2: 8, 3: 23, 4: 31, 5: 31, 6: 25} {
		if got := count(`SELECT COUNT(*) FROM node_layout_plot WHERE layout_guid = 6064631477152448529 AND level = ?`, level); got != want {
			t.Errorf("Layout_River_C has %d plots at level %d, want %d", got, level, want)
		}
	}
	// the five NodeXP construction projects name their experience: 100,000 (three), 1,000,000 and 5,000,000
	if got := count(`SELECT COUNT(*) FROM building WHERE completion_node_xp > 0`); got != 5 {
		t.Errorf("%d buildings give node experience, want 5", got)
	}
	// the asset definitions' windows: 235 are Village only, 142 Town only
	if a, b := count(`SELECT COUNT(*) FROM node_asset_def WHERE level_min = 3 AND level_max = 3`), count(`SELECT COUNT(*) FROM node_asset_def WHERE level_min = 4 AND level_max = 4`); a != 235 || b != 142 {
		t.Errorf("asset definitions: %d Village only and %d Town only, want 235 and 142", a, b)
	}
}

func i64s(n int64) string { return mountItoa(n) }

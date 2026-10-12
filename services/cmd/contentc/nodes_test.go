// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// The city nodes (contentc version 12): the tables on a synthetic design database, and the real file against what the research found.

import (
	"context"
	"database/sql"
	"io"
	"path/filepath"
	"reflect"
	"testing"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

func nodeID(guid int64) st { return idv(guid, aocexpr.TypeCityNode, "None") }

func vecS(x, y, z float64) st { return st{"X": x, "Y": y, "Z": z} }

func identity() st {
	return st{"Rotation": st{"X": 0.0, "Y": 0.0, "Z": 0.0, "W": 1.0}, "Translation": vecS(0, 0, 0), "Scale3D": vecS(1, 1, 1)}
}

// syntheticNodes is a design database with three nodes (guids out of rank order), two zones of influence, a layout with a prop and a plot at level 3, one
// asset set and definition, one plot and one building, and the rules and definitions around them.
func syntheticNodes() *source {
	src := &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}
	node := func(guid int64, name, display, typ, mapName string, details int64) designdb.Row {
		f := st{
			"nodeName": display, "NodeType": "ENodeType::" + typ, "mapName": mapName, "location": vecS(100, 200, 300),
			"rotation": st{"Pitch": 0.0, "Yaw": -67.5, "Roll": 0.0}, "Radius": int64(10000), "NodeSizeId": idv(900, 0, "None"), "LayoutId": idv(800, 0, "None"),
			"ElectionDefinitionId": idv(700, 0, "None"), "TaxTableId": idv(701, 0, "None"), "CitizenshipDuesDefinitionId": idv(702, 0, "None"),
			"PropertyTaxDefinitionId": idv(0, 0, "None"), "VendorDefId": idv(703, 0, "None"), "DoesCitizenshipRequireProperty": false,
			"mayorVoteMinimumCitizens": int64(10), "MinimumNodeToPlayerReputation": int64(-700), "MaximumNodeToPlayerReputation": int64(100000),
			"MinimumLevelToTurnInCaravan": "ENodeLevel::Village", "CitizenshipAtRiskTimeLimitSeconds": int64(604800),
			"PopulationSetsIds": list{idv(5000, 0, "None")},
		}
		for l := 0; l < 7; l++ {
			g := int64(0)
			if l == 3 {
				g = details
			}
			f["L"+string(rune('0'+l))+"_DetailsId"] = idv(g, 0, "None")
		}
		return mkRow(guid, name, f)
	}
	src.rows["CityNodeRecordBase"] = []designdb.Row{
		node(300, "Verra_RVR_Joeva", "Joeva", "Scientific", "Verra_World_Master", 600),
		node(100, "Verra_RVR_Lionhold", "Lionhold", "Default", "Verra_World_Master", 0),
		node(200, "Node_Playground_Bucktown", "Bucktown", "Economic", "NODE_Playground", 0),
	}
	zoi := func(guid int64, name, typ, biome string, node int64, valid bool, adj ...int64) designdb.Row {
		var a list
		for _, g := range adj {
			a = append(a, idv(g, 0, "None"))
		}
		return mkRow(guid, name, st{"ZoiType": "EZoiType::" + typ, "Biome": "EBiomeType::" + biome, "CityNodeId": nodeID(node), "ZoiEnvironmentType": "EZoiEnvironmentType::None",
			"Extents": st{"IsValid": valid, "Min": vecS(-10, -20, -30), "Max": vecS(10, 20, 30)}, "AdjacentZonesOfInfluenceIds": a})
	}
	src.rows["ZOIRecordBase"] = []designdb.Row{
		zoi(1001, "Verra_RVR_Node_Joeva", "HasNode", "Riverlands", 300, true, 1002),
		zoi(1002, "Verra_RVR_StartingArea_RuinsOfAela", "CannotOwn", "Riverlands", 100, true, 1001),
		zoi(1003, "ZOI1", "HasNode", "Default", 0, false),
	}
	src.rows["NodeLevelDetailsRecordBase"] = []designdb.Row{mkRow(600, "NLD_Joeva_L3", st{"LevelModificationsIds": list{idv(610, 0, "None"), idv(611, 0, "None")}})}
	src.rows["NodeModificationRecordBase"] = []designdb.Row{
		mkRow(610, "Verra_NodeBase_L3", st{"ActiveRelicSlots": int64(3), "CommissionHandSlots": int64(10), "CommissionSlots": int64(6), "NodeEssence": int64(70),
			"VehicleDeployRadius": 40000.0, "ServiceBuildingMaintenanceCostReduction": 0.0, "UnlocksCraftingRecipes": false, "EffectsIds": list{},
			"MayoralAbilitiesIds": list{idv(620, 0, "None"), idv(621, 0, "None")}, "NCSCommissionIds": list{idv(630, 0, "None")}, "CraftingRecipeSetsIds": list{}}),
		mkRow(611, "Verra_RVR_L3", st{"ActiveRelicSlots": int64(0), "CommissionHandSlots": int64(0), "CommissionSlots": int64(0), "NodeEssence": int64(0),
			"VehicleDeployRadius": 0.0, "ServiceBuildingMaintenanceCostReduction": float32(0.05), "UnlocksCraftingRecipes": true, "EffectsIds": list{idv(640, 0, "None")},
			"MayoralAbilitiesIds": list{}, "NCSCommissionIds": list{}, "CraftingRecipeSetsIds": list{idv(650, 0, "None")}}),
	}
	src.rows["NodeSizeRecordBase"] = []designdb.Row{mkRow(900, "StandardUnassignedSize", st{"NodeLevelRadii": designdb.Pairs{{Key: int64(1), Value: int64(10000)}, {Key: int64(3), Value: 50000.0}}})}
	src.rows["ChildNodeRulesRecordBase"] = []designdb.Row{mkRow(910, "Max_Rules", st{"ChildNodeDesiredLevelRulesIds": designdb.Pairs{
		{Key: "ENodeLevel::Town", Value: idv(911, 0, "None")}, {Key: "ENodeLevel::Village", Value: idv(912, 0, "None")}}})}
	src.rows["ChildNodeDesiredLevelRulesRecordBase"] = []designdb.Row{
		mkRow(911, "Town_Rules", st{"MaxChildNodesOfThisLevel": designdb.Pairs{{Key: "ENodeLevel::Village", Value: int64(2)}, {Key: "ENodeLevel::Encampment", Value: int64(2)}}}),
		mkRow(912, "Village_Rules", st{"MaxChildNodesOfThisLevel": designdb.Pairs{{Key: "ENodeLevel::Encampment", Value: int64(1)}}}),
	}
	src.rows["VassalshipDefRecordBase"] = []designdb.Row{mkRow(920, "MaxLevelVassalshipDef", st{"ChildNodeRulesId": idv(910, 0, "None"), "MaxNumOfMaxLevelNodes": int64(5),
		"MaxNumOfVassals": int64(10), "MinimumLevelToAcquireVassals": "ENodeLevel::Village", "VassalTaxPercentage": "0.1", "VassalTaxCollectionFrequencySeconds": int64(2628288),
		"RequiredRegionSizeAtLevel": list{int64(1), int64(1), int64(1), int64(2), int64(5), int64(11), int64(255)}})}
	src.rows["TaxDetailsRecordBase"] = []designdb.Row{mkRow(930, "TaxDefaultValue", st{"DefaultTaxPercent": float32(0.5), "MinimumTaxPercentage": float32(0.1), "MaximumTaxPercentage": float32(1),
		"DefaultFlatTaxAmount": float32(0), "MinimumFlatTaxAmount": float32(0), "MaximumFlatTaxAmount": float32(0), "OwningNodeTaxPercentage": float32(1)})}
	src.rows["NodeGlobalTaxTableRecordBase"] = []designdb.Row{mkRow(701, "DefaultGlobalTaxTable", st{"GlobalTaxTable": designdb.Pairs{
		{Key: st{"TagName": "NodeTax"}, Value: st{"AvailabilityPredicate": "true", "TaxDetails": idv(930, 0, "None")}},
		{Key: st{"TagName": "NodeTax.GoodsTax.Marketplace"}, Value: st{"AvailabilityPredicate": "Node_NumOfServiceBuilding(GetNodeRecord(), $#1:2$) > 0", "TaxDetails": idv(930, 0, "None")}}}})}
	src.rows["NodeToPlayerReputationTierRecordBase"] = []designdb.Row{mkRow(940, "NodeToPlayerReputation_Tier2", st{"TierRank": "ENodeReputationRank::Commoner", "NodeTaxModifier": "0.95",
		"CanVoteMayoral": true, "CanVoteBuildingAndExpansion": true, "CanVoteDemolition": false})}
	src.rows["CitizenshipNodeRecordBase"] = []designdb.Row{mkRow(950, "Node_3", st{"Level": int64(3), "CitizenshipCooldownTimeSeconds": int64(0), "DuesNodeLevelScalar": int64(1),
		"PropertyTaxNodeLevelScalar": int64(1), "MinimumBuyOrderSlots": int64(15), "MaximumBuyOrderSlots": int64(20), "MissedDuesPercentIncrease": float32(0.5), "MissedTaxPercentIncrease": float32(0.5)})}
	src.rows["QuantityScalarDefRecordBase"] = []designdb.Row{
		mkRow(961, "Dues_Low", st{"Scalar": int64(1), "Threshold": int64(50)}), mkRow(962, "Tax_Low", st{"Scalar": float32(1.5), "Threshold": int64(10)})}
	src.rows["CitizenshipGenericRecordBase"] = []designdb.Row{mkRow(960, "NewRecord_8580602100177045749", st{"InitialDueCost": int64(20), "InitialFeeCost": int64(100),
		"MissedDuesPaymentPeriod": int64(1), "MissedTaxPaymentPeriod": int64(1), "NumberOfMissableDuesPayments": int64(1), "NumberOfMissableTaxPayments": int64(1),
		"DuesPopulationCountScalarsIds": list{idv(961, 0, "Dues_Low")}, "PropertyTaxCountScalarsIds": list{idv(962, 0, "Tax_Low")}})}
	src.rows["CitizenshipDuesDefRecordBase"] = []designdb.Row{mkRow(702, "CitizenshipDuesDef_Test", st{"BaseDuesRequiredId": idv(970, 0, "None"), "CitizenshipRankImpact": "var CitizenMod = 1.0;",
		"LevelImpact": "var m = 1.0;", "NonPropertyOwnerImpact": "1", "DueDayOfTheWeek": "ENodeDayOfWeek::Thursday", "FrequencyDuesPaidWeeks": int64(2), "LateFee": int64(5), "OverduePeriodDays": int64(5)})}
	src.rows["PropertyTaxDuesDefRecordBase"] = []designdb.Row{mkRow(980, "Base_PropertyTax_Node", st{"DueDayOfTheWeek": "ENodeDayOfWeek::Monday", "FrequencyDuesPaidWeeks": int64(2),
		"ItemUsedToPayId": idv(981, 0, "None"), "OverduePeriodDays": int64(5)})}
	src.rows["CityNodeElectionDefRecordBase"] = []designdb.Row{mkRow(700, "Scientific_Election_Definition", st{"ElectionType": "EMayorElectionType::Scientific", "RegistrationPeriodDurationDays": int64(5),
		"ElectionPeriodDurationDays": int64(5), "ElectionPeriodStartDay": int64(1), "ElectionPeriodStartHour": int64(0), "ElectionPeriodEndHour": int64(23), "ElectionResultsHour": int64(1),
		"MinimumLengthOfTermDays": int64(0), "FirstTimeRegistrationPeriodDurationSeconds": int64(86400), "FirstTimeElectionPeriodDurationSeconds": int64(86400),
		"FirstTimeWaitForResultsPeriodDurationSeconds": int64(10), "RequiredQuestId": idv(0, 0, "None")})}
	src.rows["ElectionConstantsRecordBase"] = []designdb.Row{mkRow(990, "ElectionConstants", st{"RankedChoiceVoteCountLimit": int64(3)})}
	src.rows["CityNodeConstantsRecordBase"] = []designdb.Row{mkRow(991, "ConstantsDoNotAddMultiple", st{"NodeToPlayerReputationDecayPerHour": "1"})}

	src.rows["LayoutRecordBase"] = []designdb.Row{mkRow(800, "Layout_River_C", st{"level": int64(0), "PopulationSetsIds": list{idv(5001, 0, "None"), idv(5002, 0, "None")},
		"Props": list{st{"AssetSet": idv(1100, 0, "None"), "Transform": identity()}},
		"PlotsPerNodeLevel": designdb.Pairs{{Key: int64(3), Value: st{"Plots": list{st{"Plot": idv(1300, 0, "None"), "Transform": st{
			"Rotation": st{"X": 0.0, "Y": 0.0, "Z": 0.0307, "W": 0.9995}, "Translation": vecS(-6538.5, -23962.9, -23.07), "Scale3D": vecS(1, 1, 1)}}}}}}})}
	src.rows["NodeAssetSetRecordBase"] = []designdb.Row{mkRow(1100, "NS_N3_Woodshop", st{"ContainedAssetDefinitions": list{st{"Asset": idv(1200, 0, "None"), "Transform": identity()}}})}
	src.rows["NodeAssetDefinitionRecordBase"] = []designdb.Row{mkRow(1200, "NA_Hu_Kae_N3_Woodshop", st{"NodeLevelMinimum": int64(3), "NodeLevelMaximum": int64(4), "Weight": int64(1),
		"bAllBiomes": false, "ValidCultures": list{"ECharacterRace::Kaelar", "ECharacterRace::Tulnar"}, "ValidNodeTypes": list{"ENodeType::Divine", "ENodeType::Scientific"},
		"ValidSeasons": list{"ESeasons::Spring", "ESeasons::Winter"}, "ValidBiomes": list{"EBiomeType::Riverlands", "EBiomeType::Highlands"}, "ValidGameplayTags": list{}})}
	src.rows["ServiceBuildingPlotRecordBase"] = []designdb.Row{mkRow(1300, "Plot_00_NodeType", st{"BuildingsIds": list{idv(1400, 0, "None")}, "EmptyBuildingId": idv(0, 0, "None"),
		"PreConstruct": true, "StartingState": "EBuildingState::Operational"})}
	dur := func(ms int64) st { return st{"Duration": ms} }
	src.rows["BuildingRecordBase"] = []designdb.Row{mkRow(1400, "Default_Townhall", st{"ServiceBuildingType": "EServiceBuildingType::Civic", "BuildingAssetSetId": idv(1500, 0, "None"),
		"BuildingContributionPhasesId": idv(0, 0, "None"), "CompletionNodeExperience": int64(100000), "CanConstructionTimeOut": true, "ConstructionPhaseDuration": dur(604800000),
		"DemolitionPhaseDuration": dur(86400000), "DemolitionVoteDuration": dur(0), "DestroyedPhaseDuration": dur(72000), "RepairPhaseDuration": dur(86400000),
		"NodeConstructionCostId": idv(1, 0, "None"), "PlayerConstructionCostId": idv(2, 0, "None"), "MaintenanceCostId": idv(3, 0, "None"), "DisarrayRepairCostId": idv(4, 0, "None"),
		"DamagedRepairCostId": idv(5, 0, "None"), "ExpansionSkillTreeId": idv(6, 0, "None"), "bDisplayInBuildingManager": true,
		"BuildingPopSpawnMapIds": designdb.Pairs{{Key: "ENodeLevel::Village", Value: idv(1600, 0, "None")}, {Key: "ENodeLevel::Town", Value: idv(1601, 0, "None")}}})}
	src.rows["BuildingNodeAssetSetsRecordBase"] = []designdb.Row{mkRow(1500, "BNAS_Townhall", st{
		"ConstructionAssetSetIds": list{idv(1100, 0, "None")}, "OperationalAssetSetIds": list{idv(1100, 0, "None"), idv(1101, 0, "None")}, "DisarrayAssetSetIds": list{},
		"DamagedAssetSetIds": list{}, "RepairingAssetSetIds": list{}, "DestroyedAssetSetIds": list{}, "DemolitionAssetSetIds": list{}})}
	src.rows["BuildingLevelSpawnRecordBase"] = []designdb.Row{mkRow(1600, "L3_Townhall_Spawn", st{
		"ConstructionPopulationIds": list{}, "OperationalPopulationIds": list{idv(5003, 0, "None"), idv(5004, 0, "None")}, "DisarrayPopulationIds": list{idv(5005, 0, "None")},
		"DamagedPopulationIds": list{}, "RepairingPopulationIds": list{}, "DestroyedPopulationIds": list{}, "DemolitionPopulationIds": list{}})}
	return src
}

func buildNodesOnly(t *testing.T, src *source) (*content.DB, map[string]string, error) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "content.db")
	sdb, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer sdb.Close()
	sdb.SetMaxOpenConns(1)
	if err := content.Create(sdb); err != nil {
		t.Fatal(err)
	}
	tx, err := sdb.BeginTx(context.Background(), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	w := &writer{tx: tx, stmts: map[string]*sql.Stmt{}}
	meta := map[string]string{}
	if err := buildNodes(w, src, &Options{Log: io.Discard}, meta); err != nil {
		return nil, nil, err
	}
	if err := w.close(); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(`INSERT INTO meta VALUES (?, ?), (?, ?)`, content.MetaSchemaVersion, "1", content.MetaRecordEncoding, content.EncodingJSON); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	sdb.Close()
	db, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db, meta, nil
}

// Cyb captures, C | O | A code

func TestSyntheticNodes(t *testing.T) {
	db, meta, err := buildNodesOnly(t, syntheticNodes())
	if err != nil {
		t.Fatal(err)
	}
	nodes, err := db.CityNodes()
	if err != nil || len(nodes) != 3 {
		t.Fatalf("CityNodes = %v, %v", nodes, err)
	}
	// the rank is the index in the guid-sorted list
	for i, want := range []int64{100, 200, 300} {
		if nodes[i].Rank != i || nodes[i].Guid != want {
			t.Errorf("node %d: rank %d guid %d, want guid %d", i, nodes[i].Rank, nodes[i].Guid, want)
		}
	}
	joeva := nodes[2]
	if joeva.Name != "Verra_RVR_Joeva" || joeva.NodeName != "Joeva" || joeva.NodeType != "Scientific" || joeva.NodeTypeOrd != 1 || joeva.X != 100 || joeva.Yaw != -67.5 ||
		joeva.ZoiGuid != 1001 || !joeva.Levelable || joeva.ElectionDefGuid != 700 || joeva.MayorMinCitizens != 10 || joeva.MinReputation != -700 || joeva.MaxReputation != 100000 ||
		joeva.CaravanMinLevel != content.NodeLevelVillage || joeva.CitizenshipAtRiskSecs != 604800 || joeva.SizeGuid != 900 || joeva.LayoutGuid != 800 {
		t.Errorf("Joeva = %+v", joeva)
	}
	// a starting area (type Default) and a test node (another map) do not grow
	if nodes[0].Levelable || nodes[1].Levelable || nodes[0].ZoiGuid != 1002 || nodes[1].ZoiGuid != 0 {
		t.Errorf("Lionhold %+v, Bucktown %+v", nodes[0], nodes[1])
	}
	if meta["typed.city_node.levelable"] != "1" || meta["typed.city_node"] != "3" || meta["typed.city_zoi"] != "3" || meta["typed.city_zoi_adjacent"] != "2" {
		t.Errorf("meta = %v", meta)
	}

	zois, err := db.CityZois()
	if err != nil || len(zois) != 3 {
		t.Fatalf("CityZois = %v, %v", zois, err)
	}
	z := zois[0]
	if z.Guid != 1001 || z.ZoiType != "HasNode" || z.ZoiTypeOrd != 0 || z.Biome != "Riverlands" || z.BiomeOrd != 14 || z.NodeGuid != 300 || !z.HasExtents || z.Min != [3]float64{-10, -20, -30} ||
		!reflect.DeepEqual(z.Adjacent, []int64{1002}) || z.Environment != "None" {
		t.Errorf("zone = %+v", z)
	}
	if zois[2].HasExtents || zois[2].BiomeOrd != 0 || zois[2].NodeGuid != 0 {
		t.Errorf("test zone = %+v", zois[2])
	}

	lv, err := db.CityNodeLevelDetails()
	if err != nil || !reflect.DeepEqual(lv, map[int64]map[int]int64{300: {3: 600}}) {
		t.Errorf("CityNodeLevelDetails = %v, %v", lv, err)
	}
	nld, err := db.NodeLevelDetailsList()
	if err != nil || len(nld) != 1 || !reflect.DeepEqual(nld[0].Modifications, []int64{610, 611}) {
		t.Errorf("NodeLevelDetailsList = %v, %v", nld, err)
	}
	mods, err := db.NodeModifications()
	if err != nil || len(mods) != 2 || mods[0].CommissionSlots != 6 || mods[0].NodeEssence != 70 || mods[0].VehicleDeployRadius != 40000 || mods[1].MaintenanceCostReduction != 0.05 || !mods[1].UnlocksRecipes {
		t.Errorf("NodeModifications = %+v, %v", mods, err)
	}
	rules, err := db.ChildNodeRules()
	want := []content.ChildNodeRule{
		{RulesGuid: 910, RulesName: "Max_Rules", ParentLevel: 3, ChildLevel: 2, MaxChildren: 1},
		{RulesGuid: 910, RulesName: "Max_Rules", ParentLevel: 4, ChildLevel: 2, MaxChildren: 2},
		{RulesGuid: 910, RulesName: "Max_Rules", ParentLevel: 4, ChildLevel: 3, MaxChildren: 2},
	}
	if err != nil || !reflect.DeepEqual(rules, want) {
		t.Errorf("ChildNodeRules = %v, %v", rules, err)
	}
	vd, err := db.VassalshipDefs()
	if err != nil || len(vd) != 1 || vd[0].MaxMaxLevelNodes != 5 || vd[0].MinLevelToAcquire != 3 || vd[0].TaxPercentage != 0.1 || !reflect.DeepEqual(vd[0].RequiredRegionSizeAt, []int{1, 1, 1, 2, 5, 11, 255}) {
		t.Errorf("VassalshipDefs = %+v, %v", vd, err)
	}

	// the rest by SQL: the tax table, the reputation tier, the citizenship and election definitions, the layouts and the buildings
	count := func(q string, args ...any) int64 {
		t.Helper()
		var n int64
		if err := db.SQL().QueryRow(q, args...).Scan(&n); err != nil {
			t.Fatalf("%s: %v", q, err)
		}
		return n
	}
	for q, want := range map[string]int64{
		`SELECT COUNT(*) FROM node_tax_table_tag WHERE table_guid = 701`:                                                                              2,
		`SELECT COUNT(*) FROM node_tax_table_tag WHERE tag = 'NodeTax.GoodsTax.Marketplace' AND availability LIKE 'Node_NumOfServiceBuilding%'`:       1,
		`SELECT COUNT(*) FROM node_reputation_tier WHERE rank_ord = 1 AND rank_name = 'Commoner' AND tax_modifier = 0.95 AND can_vote_demolition = 0`: 1,
		`SELECT COUNT(*) FROM citizenship_scalar WHERE kind = 'dues' AND name = 'Dues_Low' AND threshold = 50`:                                        1,
		`SELECT COUNT(*) FROM citizenship_scalar WHERE kind = 'property_tax' AND scalar = 1.5`:                                                        1,
		`SELECT COUNT(*) FROM citizenship_dues_def WHERE due_day_of_week = 'Thursday' AND weeks = 2`:                                                  1,
		`SELECT COUNT(*) FROM property_tax_def WHERE due_day_of_week = 'Monday'`:                                                                      1,
		`SELECT COUNT(*) FROM election_def WHERE election_type = 'Scientific' AND first_election_secs = 86400`:                                        1,
		`SELECT COUNT(*) FROM node_constant WHERE name = 'RankedChoiceVoteCountLimit' AND value = '3'`:                                                1,
		`SELECT COUNT(*) FROM node_constant WHERE name = 'NodeToPlayerReputationDecayPerHour' AND value = '1'`:                                        1,
		`SELECT COUNT(*) FROM node_size_radius WHERE size_guid = 900`:                                                                                 2,
		`SELECT COUNT(*) FROM node_modification_ref WHERE kind = 'mayoral_ability'`:                                                                   2,
		`SELECT COUNT(*) FROM node_modification_ref WHERE kind IN ('effect', 'recipe_set', 'commission')`:                                             3,
		`SELECT COUNT(*) FROM node_layout_prop WHERE layout_guid = 800 AND idx = 0 AND asset_set_guid = 1100 AND qw = 1 AND sx = 1`:                   1,
		`SELECT COUNT(*) FROM node_layout_plot WHERE layout_guid = 800 AND level = 3 AND plot_guid = 1300 AND qz = 0.0307 AND tx = -6538.5`:           1,
		`SELECT COUNT(*) FROM node_layout_population WHERE layout_guid = 800`:                                                                         2,
		`SELECT COUNT(*) FROM city_node_population WHERE node_guid = 300 AND population_set_guid = 5000`:                                              1,
		`SELECT COUNT(*) FROM node_asset_set_member WHERE set_guid = 1100 AND def_guid = 1200`:                                                        1,
		// Kaelar 0 and Tulnar 8; Scientific 1 and Divine 2; Winter 0 and Spring 1; Riverlands 14 and Highlands 21
		`SELECT COUNT(*) FROM node_asset_def WHERE level_min = 3 AND level_max = 4 AND cultures = 257 AND node_types = 6 AND seasons = 3 AND biomes = 2113536 AND all_biomes = 0`: 1,
		`SELECT COUNT(*) FROM service_plot WHERE guid = 1300 AND pre_construct = 1 AND starting_state = 2`:                                                                        1,
		`SELECT COUNT(*) FROM building WHERE building_type = 'Civic' AND completion_node_xp = 100000 AND construction_ms = 604800000 AND destroyed_ms = 72000`:                    1,
		`SELECT COUNT(*) FROM building_pop_spawn WHERE building_guid = 1400 AND level IN (3, 4)`:                                                                                  2,
		`SELECT COUNT(*) FROM building_asset_state WHERE sets_guid = 1500 AND state = 2`:                                                                                          2,
		`SELECT COUNT(*) FROM building_asset_state WHERE sets_guid = 1500 AND state = 1`:                                                                                          1,
		`SELECT COUNT(*) FROM building_level_spawn_pop WHERE spawn_guid = 1600 AND state = 2`:                                                                                     2,
		`SELECT COUNT(*) FROM building_level_spawn_pop WHERE spawn_guid = 1600 AND state = 3`:                                                                                     1,
	} {
		if got := count(q); got != want {
			t.Errorf("%s = %d, want %d", q, got, want)
		}
	}
}

func TestSyntheticNodesRefuse(t *testing.T) {
	// an enum that is not one of the nodes' values stops the build with the record
	src := syntheticNodes()
	src.rows["ZOIRecordBase"][0].Fields["Biome"] = "EBiomeType::Atlantis"
	if _, _, err := buildNodesOnly(t, src); err == nil {
		t.Error("a biome that is not in EBiomeType was accepted")
	}
	// a node with no location is a data error
	src = syntheticNodes()
	delete(src.rows["CityNodeRecordBase"][0].Fields, "location")
	if _, _, err := buildNodesOnly(t, src); err == nil {
		t.Error("a node with no location was accepted")
	}
	// two zones for one node
	src = syntheticNodes()
	src.rows["ZOIRecordBase"][2].Fields["CityNodeId"] = nodeID(300)
	if _, _, err := buildNodesOnly(t, src); err == nil {
		t.Error("two zones of one node were accepted")
	}
	// the zone and node tables are required
	if _, _, err := buildNodesOnly(t, &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}); err == nil {
		t.Error("a database with no CityNodeRecordBase was accepted")
	}
}

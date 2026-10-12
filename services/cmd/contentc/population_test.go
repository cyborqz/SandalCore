// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Plan E1, stage 1 (contentc version 7; stage 2, version 8: the routes' waypoints and the instances' patrol settings): the NPC population chain and the vendors, on synthetic rows (TestSyntheticPopulation) and on the real design
// database (TestRealPopulation, with AOC_DESIGN_DB).

import (
	"context"
	"database/sql"
	"encoding/json"
	"io"
	"math"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

const (
	pAssetWolf  = 8300000000000000001
	pAssetNoBP  = 8300000000000000002
	pAssetGuard = 8300000000000000003
	pSetWolves  = 8300000000000000010
	pSetEmpty   = 8300000000000000011
	pInstWolf   = 8300000000000000020
	pInstGuard  = 8300000000000000021
	pPopSet     = 8300000000000000030
	pPopSetArea = 8300000000000000031
	pWorld      = 8300000000000000040
	pArea       = 8300000000000000050
	pRoute      = 8300000000000000060
	pVendor     = 8300000000000000070
	pInventory  = 8300000000000000071
	pItemA      = 8300000000000000080
	pItemB      = 8300000000000000081
	pOffer      = 8300000000000000082
	pLootTable  = 8300000000000000090
	pRespawn    = 8300000000000000091
	pFilter     = 8300000000000000095
)

// [Cyb x C-O-A]

func rid(guid int64) st { return idv(guid, 7000000000000000000, "None") }

func vec3(x, y, z float64) st { return st{"X": x, "Y": y, "Z": z} }

func rot(yaw float64) st { return st{"Pitch": float64(0), "Roll": float64(0), "Yaw": yaw} }

// popFilterRow is a PopulationFilter as the design database holds it.
func popFilterRow(levelMin, levelMax string, cultures list) st {
	return st{"Biomes": list{}, "Cultures": cultures, "DespawningStrategy": st{"DespawnStrategy": "EDespawnStrategy::Immediate"}, "NodeIds": list{}, "NodeLevelMax": levelMax, "NodeLevelMin": levelMin,
		"NodeTypes": list{}, "RequiredGameplayTags": list{"npc.stature.rareSpawn"}, "Seasons": list{"ESeason::Winter"}, "SpawningStrategy": st{"SpawnStrategy": "ESpawnStrategy::Immediate"},
		"TimeOfDayEnd": int64(-1), "TimeOfDayStart": float32(10), "WeatherTypes": list{}, "ZoneOfInfluenceIds": list{rid(9)}}
}

// the records that spawn a set when their turn comes: one of each kind gateRules knows, an event trigger and a quest objective naming pPopSet
func gateSources(src *source) {
	for _, schema := range gateSchemas() {
		src.rows[schema] = nil
	}
	src.rows["EventTriggerDefRecordBase"] = []designdb.Row{mkRow(8300000000000000100, "Trigger", st{"PopulationSetsIds": list{rid(pPopSet), rid(pPopSetArea)}, "OtherSet": rid(pPopSet)})}
	src.rows["NarrativeObjectiveV2"] = []designdb.Row{mkRow(8300000000000000101, "Objective", st{
		"OnStartActions": list{st{"Action": st{"SpawnIDs": list{st{"PopulationSet": rid(pPopSet)}}}}},
		// a kill objective's targets are the world's own NPCs: not a gate
		"KillObjectiveData": st{"PopulationSets": list{rid(pPopSetArea)}}})}
}

func populationSource() *source {
	src := &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}
	// radii are ints in one record and floats in another, as the design data has them
	src.rows["PopulationAssetRecordBase"] = []designdb.Row{
		mkRow(pAssetWolf, "Wolf_Asset", st{"AssetRef": "/Game/DesignAssets/Population/NPC/Beast/Canine/Wolf.Wolf", "FactionId": int64(7), "AggroRadius": int64(1500), "LeashRadius": float32(4000.5),
			"bOverrideAggroRadius": true, "bOverrideLeashRadius": false, "ScaleMin": float32(0.9), "ScaleMax": float32(1.1), "GameplayTags": list{st{"TagName": "npc.disposition.enemy"}, st{"TagName": "character.beast"}},
			"DialogueRoot": rid(0), "InteractComponentDefId": rid(0), "InteractionDefId": rid(0), "AppearanceOverrideId": rid(0), "LootTablesIds": list{rid(pLootTable), rid(0)},
			"PopulationFilter": popFilterRow("ENodeLevel::Village", "ENodeLevel::Town", list{"ECharacterRace::Kaelar"})}),
		mkRow(pAssetNoBP, "Marker_Asset", st{"AssetRef": "", "FactionId": int64(0), "AggroRadius": int64(0), "LeashRadius": int64(0), "bOverrideAggroRadius": false, "bOverrideLeashRadius": false,
			"ScaleMin": int64(1), "ScaleMax": int64(1), "GameplayTags": list{}, "DialogueRoot": rid(0), "InteractComponentDefId": rid(0), "InteractionDefId": rid(0), "AppearanceOverrideId": rid(0), "LootTablesIds": list{},
			"PopulationFilter": popFilterRow("ENodeLevel::Wilderness", "ENodeLevel::Metropolis", list{})}),
		mkRow(pAssetGuard, "Guard_Asset", st{"AssetRef": "/Game/DesignAssets/Population/NPC/PCRaces/Guards/Guard_Soldier.Guard_Soldier_C", "FactionId": int64(2), "AggroRadius": float32(600), "LeashRadius": int64(2000),
			"bOverrideAggroRadius": false, "bOverrideLeashRadius": true, "ScaleMin": float32(1), "ScaleMax": float32(1), "GameplayTags": list{}, "DialogueRoot": rid(5), "InteractComponentDefId": rid(6),
			"InteractionDefId": rid(7), "AppearanceOverrideId": rid(8), "LootTablesIds": list{},
			"PopulationFilter": popFilterRow("ENodeLevel::Wilderness", "ENodeLevel::Metropolis", list{})}),
	}
	member := func(asset int64, weight float64) designdb.Pair {
		return designdb.Pair{Key: rid(asset), Value: st{"DisplayNameOverrides": list{}, "Weight": weight}}
	}
	src.rows["AssetSetRecordBase"] = []designdb.Row{
		mkRow(pSetWolves, "Wolves", st{"AssetDefMap": designdb.Pairs{member(pAssetWolf, 3), member(pAssetNoBP, 1), member(8399999999999999999, 1)}, "GameplayTags": list{"Quest.EventTarget"}}),
		mkRow(pSetEmpty, "Empty", st{"AssetDefMap": designdb.Pairs{}, "GameplayTags": list{}}),
	}
	inst := func(guid int64, name string, sets []int64, lmin, lmax int64, ambient string) designdb.Row {
		var s list
		for _, g := range sets {
			s = append(s, rid(g))
		}
		return mkRow(guid, name, st{"NPCLevelMin": lmin, "NPCLevelMax": lmax, "bDynamicallyLeveled": false, "RespawnTime": float32(150), "RespawnVariance": int64(10), "RespawnRule": "ERespawnRule::Refill",
			"PatrolRouteId": rid(pRoute), "NpcAmbientBehavior": ambient, "MinRadius": int64(100), "MaxRadius": float32(1000), "ArrivalDistance": int64(300), "WanderSpeedMultiplier": float32(0.5),
			"WanderDelay": float32(2), "WanderDelayDeviation": float32(1), "SpeedMultiplierExpression": "", "SnappingRule": "ESnappingRules::SnapToGround", "OrientationType": "ENpcAIOrientationType::FaceMovementDirection",
			"AggroRadius": int64(0), "bOverrideAggroRadius": false, "PopulationAreaWeight": float32(1), "VendorTableId": rid(pVendor), "InteractComponentDefId": rid(0), "InteractResponseId": rid(0),
			"DefaultDialogueId": rid(0), "GroupId": int64(4), "SpawnLocationType": "ESpawnLocationCheckType::Land", "OverrideActivationRange": int64(0), "Blacklisted": false,
			"NumberOfWaypoints": int64(6), "MaxAngle": float32(45), "FollowDistance": int64(100), "Formation": "ENpcAIPatrolRouteFormation::None", "IdleAnim": "ECharacterIdle::None", "AlignToSlope": false,
			"FilteringOverrideId": rid(pFilter), "SpawnerPriority": "ESpawnerPriority::High", "GameplayTags": list{st{"TagName": "Event.RVR.Test"}},
			"AssetSetsIds": s, "LootTablesIds": list{rid(pLootTable)}})
	}
	src.rows["PopulationInstanceRecordBase"] = []designdb.Row{
		inst(pInstWolf, "Wolf_Wander", []int64{pSetWolves, pSetEmpty}, 13, 14, "ENpcAmbientBehavior::Wander"),
		inst(pInstGuard, "Guard_Stand", nil, 25, 25, "ENpcAmbientBehavior::None"),
	}
	src.rows["PopulationSetRecordBase"] = []designdb.Row{
		mkRow(pPopSet, "Camp", st{"PopulationAreaId": rid(0), "OverrideActivationRange": int64(0), "Blacklisted": false, "FilterOverrideId": rid(0), "GameplayTags": list{"Event.Camp"}, "PopulationInstancesMap": designdb.Pairs{
			{Key: int64(1), Value: st{"InstanceId": rid(pInstWolf), "Location": vec3(100, 200, 300), "Rotation": rot(90), "bRandomRotation": false}},
			{Key: int64(2), Value: st{"InstanceId": rid(pInstGuard), "Location": vec3(-100.5, 0, 12), "Rotation": rot(-45.5), "bRandomRotation": true}}}}),
		mkRow(pPopSetArea, "Wilderness", st{"PopulationAreaId": rid(pArea), "OverrideActivationRange": float32(5000), "Blacklisted": true, "FilterOverrideId": rid(pFilter), "GameplayTags": list{}, "PopulationInstancesMap": designdb.Pairs{
			{Key: int64(1), Value: st{"InstanceId": rid(pInstWolf), "Location": vec3(0, 0, 0), "Rotation": rot(0), "bRandomRotation": false}}}}),
	}
	src.rows["WorldSpawnRecordBase"] = []designdb.Row{
		mkRow(pWorld, "World_A", st{"Culture": "ECharacterRace::cCount", "MapName": "Verra_World_Master", "FilterOverrideId": rid(0), "PopulationSetsMap": designdb.Pairs{
			{Key: int64(1), Value: st{"SetId": rid(pPopSet), "Location": vec3(-777000, 616000, 15000), "Rotation": rot(180), "MinLevelOverride": int64(0), "MaxLevelOverride": int64(7)}},
			{Key: int64(2), Value: st{"SetId": rid(pPopSetArea), "Location": vec3(10, 20, 30), "Rotation": rot(0), "MinLevelOverride": int64(0), "MaxLevelOverride": int64(0)}}}}),
	}
	src.rows["PopulationAreaRecordBase"] = []designdb.Row{
		mkRow(pArea, "Area_A", st{"MinNumSpawns": int64(1), "MaxNumSpawns": int64(6), "PartitionLength": float32(100), "AreaDefinitions": list{
			st{"Center": vec3(-35130, -52430, 3730), "Extent": int64(1500), "MaxNumSpawns": int64(-1), "Weight": float32(1)},
			st{"Center": vec3(1, 2, 3), "Extent": float32(250.5), "MaxNumSpawns": int64(2), "Weight": int64(3)}}}),
	}
	pt := func(x, y, z float64) st {
		return st{"ArriveTangent": vec3(0, 0, 0), "InVal": float32(0), "InterpMode": "CIM_Linear", "LeaveTangent": vec3(0, 0, 0), "OutVal": vec3(x, y, z)}
	}
	src.rows["PathingRouteRecordBase"] = []designdb.Row{
		mkRow(pRoute, "Route_A", st{"Looping": true, "PathActorTransform": st{"Rotation": st{"W": float64(1), "X": float64(0), "Y": float64(0), "Z": float64(0)}, "Scale3D": vec3(1, 1, 1), "Translation": vec3(-778716.5, 456071.75, 16393)},
			"SplineCurves":      st{"Position": st{"LoopKeyOffset": float32(0), "Points": list{pt(0, 0, 0), pt(870, -2790, 0), pt(3210, -4400, 5)}, "bIsLooped": false}},
			"WaypointLocations": list{vec3(0, 0, 0), vec3(870, -2790, 0), vec3(3210, -4400, 5)}, "WaypointPauseLengths": list{float32(0), float32(7.5), int64(3)}, "StartRouteClosestPoint": true,
			"RelativeToCityNode": false, "RelativeToEncounter": false, "PathingSet": "Set_A"}),
	}
	listing := func(item int64, offers ...int64) st {
		var o list
		for _, g := range offers {
			o = append(o, rid(g))
		}
		return st{"AvailabilityPredicates": list{}, "ItemId": rid(item), "ItemVariation": rid(0), "Offers": o, "bHiddenIfUnavailable": false}
	}
	src.rows["VendorInventoryDefRecordBase"] = []designdb.Row{
		mkRow(pInventory, "Inv_Gear", st{"AvailabilityPredicates": list{}, "bHiddenIfUnavailable": true, "Listings": list{listing(pItemA), listing(pItemB, pOffer)}}),
	}
	src.rows["VendorDefRecordBase"] = []designdb.Row{
		mkRow(pVendor, "Vendor_Gear", st{"Blacklisted": false, "SupportedOperations": int64(231), "InventoryIds": list{rid(pInventory), rid(8399999999999999998)}, "PriceModifiersIds": list{}, "SellFiltersIds": list{}}),
	}
	src.rows["FilteringOverrideRecordBase"] = []designdb.Row{
		mkRow(pFilter, "Academic_Filter", st{"PopulationFilterOverride": st{"Biomes": list{"ENodeBiome::Riverlands"}, "Cultures": list{"ECharacterRace::Kaelar"}, "NodeIds": list{rid(41), rid(42)}, "NodeLevelMax": "ENodeLevel::Metropolis",
			"NodeLevelMin": "ENodeLevel::Wilderness", "NodeTypes": list{"ENodeType::Scientific"}, "RequiredGameplayTags": list{}, "Seasons": list{}, "TimeOfDayEnd": int64(23), "TimeOfDayStart": int64(10),
			"WeatherTypes": list{"EWeather::Rain"}, "ZoneOfInfluenceIds": list{}}}),
	}
	gateSources(src)
	return src
}

// Cyb captures, C O A code

func buildPopulationSynthetic(t *testing.T, src *source) (*sql.DB, map[string]string, error) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "content.db")
	sdb, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { sdb.Close() })
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
	if err := buildPopulation(w, src, &Options{Log: io.Discard}, meta); err != nil {
		return nil, nil, err
	}
	if err := w.close(); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	return sdb, meta, nil
}

func one[T any](t *testing.T, db *sql.DB, query string, args ...any) T {
	t.Helper()
	var v T
	if err := db.QueryRow(query, args...).Scan(&v); err != nil {
		t.Fatalf("%s: %v", query, err)
	}
	return v
}

func TestArchetypePath(t *testing.T) {
	for ref, want := range map[string]string{
		"/Game/DesignAssets/Population/NPC/Beast/Canine/Wolf.Wolf":                       "/Game/DesignAssets/Population/NPC/Beast/Canine/Wolf.Default__Wolf_C",
		"/Game/DesignAssets/Population/NPC/PCRaces/Guards/Guard_Soldier.Guard_Soldier_C": "/Game/DesignAssets/Population/NPC/PCRaces/Guards/Guard_Soldier.Default__Guard_Soldier_C",
		"": "", "Wolf": "", "/Game/X/Wolf": "", "/Game.X/Wolf": "", "/Game/X/.": "", "/Game/X.Y.Z": "/Game/X.Y.Default__Z_C", "  /Game/A/B.B  ": "/Game/A/B.Default__B_C",
	} {
		if got := archetypePath(ref); got != want {
			t.Errorf("archetypePath(%q) = %q, want %q", ref, got, want)
		}
	}
}

func TestSyntheticPopulation(t *testing.T) {
	db, meta, err := buildPopulationSynthetic(t, populationSource())
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.npc_asset"] != "3" || meta["typed.npc_instance"] != "2" || meta["typed.npc_set"] != "2" || meta["typed.npc_placement"] != "2" || meta["typed.npc_area_circle"] != "2" ||
		meta["typed.npc_route"] != "1" || meta["typed.vendor_def"] != "1" || meta["typed.vendor_listing"] != "2" {
		t.Errorf("meta = %v", meta)
	}

	// assets: ints and floats alike become numbers; the archetype's path is the class default object's; a ref that is no package path has none
	type asset struct {
		path                     string
		faction                  int64
		aggro, leash, smin, smax float64
		overAggro, overLeash     int64
		tags                     string
		dialogue, icd, pid, look int64
	}
	var a asset
	if err := db.QueryRow(`SELECT archetype_path, faction_guid, aggro_radius, leash_radius, scale_min, scale_max, override_aggro, override_leash, tags, dialogue_root_guid,
		interact_component_guid, interaction_def_guid, appearance_guid FROM npc_asset WHERE guid = ?`, pAssetWolf).Scan(&a.path, &a.faction, &a.aggro, &a.leash, &a.smin, &a.smax, &a.overAggro, &a.overLeash, &a.tags,
		&a.dialogue, &a.icd, &a.pid, &a.look); err != nil {
		t.Fatal(err)
	}
	if a.path != "/Game/DesignAssets/Population/NPC/Beast/Canine/Wolf.Default__Wolf_C" || a.faction != 7 || a.aggro != 1500 || a.leash != 4000.5 || math.Abs(a.smin-0.9) > 1e-9 || math.Abs(a.smax-1.1) > 1e-9 ||
		a.overAggro != 1 || a.overLeash != 0 || a.tags != `["npc.disposition.enemy","character.beast"]` {
		t.Errorf("the wolf asset = %+v", a)
	}
	if got := one[string](t, db, `SELECT archetype_path FROM npc_asset WHERE guid = ?`, pAssetNoBP); got != "" {
		t.Errorf("an asset with no ref has archetype path %q", got)
	}
	if got := one[string](t, db, `SELECT archetype_path FROM npc_asset WHERE guid = ?`, pAssetGuard); got != "/Game/DesignAssets/Population/NPC/PCRaces/Guards/Guard_Soldier.Default__Guard_Soldier_C" {
		t.Errorf("the _C of a ref is dropped: %q", got)
	}
	if n := one[int](t, db, `SELECT COUNT(*) FROM npc_asset_loot WHERE asset_guid = ?`, pAssetWolf); n != 1 {
		t.Errorf("the wolf's loot links = %d (the null table is not one)", n)
	}
	if got := one[int64](t, db, `SELECT table_guid FROM npc_asset_loot WHERE asset_guid = ? AND ord = 0`, pAssetWolf); got != pLootTable {
		t.Errorf("the loot table = %d", got)
	}

	// asset sets keep their order and weights, including a member that is no asset row
	if n := one[int](t, db, `SELECT COUNT(*) FROM npc_asset_set_member WHERE set_guid = ?`, pSetWolves); n != 3 {
		t.Errorf("the wolves set has %d members", n)
	}
	if w := one[float64](t, db, `SELECT weight FROM npc_asset_set_member WHERE set_guid = ? AND ord = 0`, pSetWolves); w != 3 {
		t.Errorf("first weight = %v", w)
	}
	if n := one[int](t, db, `SELECT COUNT(*) FROM npc_asset_set WHERE guid IN (?, ?)`, pSetWolves, pSetEmpty); n != 2 {
		t.Errorf("sets = %d", n)
	}

	// instances
	var lmin, lmax int64
	var respawn, variance, minR, maxR, arrive, mult float64
	var rule, ambient, snap string
	var route, vendor int64
	if err := db.QueryRow(`SELECT level_min, level_max, respawn_time, respawn_variance, respawn_rule, patrol_route_guid, ambient_behavior, min_radius, max_radius, arrival_distance, wander_speed_mult,
		snapping_rule, vendor_guid FROM npc_instance WHERE guid = ?`, pInstWolf).Scan(&lmin, &lmax, &respawn, &variance, &rule, &route, &ambient, &minR, &maxR, &arrive, &mult, &snap, &vendor); err != nil {
		t.Fatal(err)
	}
	if lmin != 13 || lmax != 14 || respawn != 150 || variance != 10 || rule != "Refill" || route != pRoute || ambient != "Wander" || minR != 100 || maxR != 1000 || arrive != 300 || mult != 0.5 ||
		snap != "SnapToGround" || vendor != pVendor {
		t.Errorf("the wolf instance: %d-%d respawn %v +- %v %s route %d %s radius %v-%v arrival %v mult %v %s vendor %d", lmin, lmax, respawn, variance, rule, route, ambient, minR, maxR, arrive, mult, snap, vendor)
	}
	if n := one[int](t, db, `SELECT COUNT(*) FROM npc_instance_asset_set WHERE instance_guid = ?`, pInstWolf); n != 2 {
		t.Errorf("the wolf instance draws from %d sets", n)
	}
	if n := one[int](t, db, `SELECT COUNT(*) FROM npc_instance_loot WHERE instance_guid = ?`, pInstWolf); n != 1 {
		t.Errorf("instance loot links = %d", n)
	}

	// sets and placements: positions in cm, yaw in degrees, the area of a set, the level overrides of a placement
	var x, y, z, yaw float64
	var random int64
	if err := db.QueryRow(`SELECT x, y, z, yaw, random_rotation FROM npc_set_member WHERE set_guid = ? AND key = 2`, pPopSet).Scan(&x, &y, &z, &yaw, &random); err != nil {
		t.Fatal(err)
	}
	if x != -100.5 || y != 0 || z != 12 || yaw != -45.5 || random != 1 {
		t.Errorf("set member 2 = (%v, %v, %v) yaw %v random %d", x, y, z, yaw, random)
	}
	if area := one[int64](t, db, `SELECT area_guid FROM npc_set WHERE guid = ?`, pPopSetArea); area != pArea {
		t.Errorf("the area of the wilderness set = %d", area)
	}
	if b, act := one[int](t, db, `SELECT blacklisted FROM npc_set WHERE guid = ?`, pPopSetArea), one[float64](t, db, `SELECT activation_range FROM npc_set WHERE guid = ?`, pPopSetArea); b != 1 || act != 5000 {
		t.Errorf("blacklisted %d activation range %v", b, act)
	}
	if err := db.QueryRow(`SELECT x, y, z, yaw, max_level FROM npc_placement WHERE spawn_guid = ? AND key = 1`, pWorld).Scan(&x, &y, &z, &yaw, &lmax); err != nil {
		t.Fatal(err)
	}
	if x != -777000 || y != 616000 || z != 15000 || yaw != 180 || lmax != 7 {
		t.Errorf("placement 1 = (%v, %v, %v) yaw %v max level %d", x, y, z, yaw, lmax)
	}
	if got := one[string](t, db, `SELECT culture FROM npc_world_spawn WHERE guid = ?`, pWorld); got != "cCount" {
		t.Errorf("culture = %q", got)
	}

	// version 9: the filters, the tags, the gates, bDevOnly
	var filter string
	var devOnly int
	if err := db.QueryRow(`SELECT filter, dev_only FROM npc_asset WHERE guid = ?`, pAssetWolf).Scan(&filter, &devOnly); err != nil {
		t.Fatal(err)
	}
	if want := `{"biomes":[],"cultures":["Kaelar"],"node_ids":[],"node_level_min":"Village","node_level_max":"Town","node_types":[],"required_tags":["npc.stature.rareSpawn"],"seasons":["Winter"],` +
		`"time_of_day_start":10,"time_of_day_end":-1,"weather":[],"zones_of_influence":[9]}`; filter != want || devOnly != 0 {
		t.Errorf("the wolf asset's filter = %s (dev only %d), want %s", filter, devOnly, want)
	}
	var priority, tags string
	var override int64
	if err := db.QueryRow(`SELECT spawner_priority, tags, filter_override_guid FROM npc_instance WHERE guid = ?`, pInstWolf).Scan(&priority, &tags, &override); err != nil {
		t.Fatal(err)
	}
	if priority != "High" || tags != `["Event.RVR.Test"]` || override != pFilter {
		t.Errorf("the wolf instance: priority %q tags %s override %d", priority, tags, override)
	}
	if got := one[string](t, db, `SELECT tags FROM npc_asset_set WHERE guid = ?`, pSetWolves); got != `["Quest.EventTarget"]` {
		t.Errorf("asset set tags = %s", got)
	}
	if got := one[string](t, db, `SELECT tags FROM npc_set WHERE guid = ?`, pPopSet); got != `["Event.Camp"]` {
		t.Errorf("set tags = %s", got)
	}
	if got := one[int64](t, db, `SELECT filter_override_guid FROM npc_set WHERE guid = ?`, pPopSetArea); got != pFilter {
		t.Errorf("set override = %d", got)
	}
	if got := one[string](t, db, `SELECT map_name FROM npc_world_spawn WHERE guid = ?`, pWorld); got != "Verra_World_Master" {
		t.Errorf("map = %q", got)
	}
	if got := one[string](t, db, `SELECT filter FROM npc_filter WHERE guid = ?`, pFilter); got != `{"biomes":["Riverlands"],"cultures":["Kaelar"],"node_ids":[41,42],"node_level_min":"Wilderness","node_level_max":"Metropolis",`+
		`"node_types":["Scientific"],"required_tags":[],"seasons":[],"time_of_day_start":10,"time_of_day_end":23,"weather":["Rain"],"zones_of_influence":[]}` {
		t.Errorf("override filter = %s", got)
	}
	// the gates: a set the event trigger and the objective's start spawn, one the trigger spawns, and none for a kill objective's target or a field that is no gate
	rows, err := db.Query(`SELECT set_guid, source FROM npc_gate ORDER BY set_guid, source`)
	if err != nil {
		t.Fatal(err)
	}
	var gates []string
	for rows.Next() {
		var g int64
		var s string
		if err := rows.Scan(&g, &s); err != nil {
			t.Fatal(err)
		}
		gates = append(gates, strings.Join([]string{strings.TrimPrefix(jsonText(g), "83000000000000000"), s}, ":"))
	}
	rows.Close()
	if strings.Join(gates, ",") != "30:EventTriggerDef,30:NarrativeObjective,31:EventTriggerDef" {
		t.Errorf("gates = %v", gates)
	}
	if meta["typed.npc_gate"] != "3" || meta["typed.npc_filter"] != "1" {
		t.Errorf("meta gates %q filters %q", meta["typed.npc_gate"], meta["typed.npc_filter"])
	}

	// areas and routes
	if n, ext := one[int](t, db, `SELECT COUNT(*) FROM npc_area_circle WHERE area_guid = ?`, pArea), one[float64](t, db, `SELECT extent FROM npc_area_circle WHERE area_guid = ? AND ord = 1`, pArea); n != 2 || ext != 250.5 {
		t.Errorf("area circles = %d, second extent %v", n, ext)
	}
	var points string
	var looping int64
	var tx float64
	if err := db.QueryRow(`SELECT looping, tx, points FROM npc_route WHERE guid = ?`, pRoute).Scan(&looping, &tx, &points); err != nil {
		t.Fatal(err)
	}
	var pts [][3]float64
	if err := json.Unmarshal([]byte(points), &pts); err != nil || len(pts) != 3 || pts[2] != [3]float64{3210, -4400, 5} || looping != 1 || tx != -778716.5 {
		t.Errorf("route: looping %d tx %v points %s (%v)", looping, tx, points, err)
	}
	// version 8: the waypoints and the pause at each, the flags and the set, the instance's patrol settings
	var waypoints, pauses, pset string
	var startClosest, relCity, relEnc int
	if err := db.QueryRow(`SELECT waypoints, pauses, start_closest, relative_city_node, relative_encounter, pathing_set FROM npc_route WHERE guid = ?`, pRoute).Scan(&waypoints, &pauses, &startClosest, &relCity, &relEnc, &pset); err != nil {
		t.Fatal(err)
	}
	var wps [][3]float64
	var ps []float64
	if err := json.Unmarshal([]byte(waypoints), &wps); err != nil || len(wps) != 3 || wps[1] != [3]float64{870, -2790, 0} {
		t.Errorf("waypoints %s (%v)", waypoints, err)
	}
	if err := json.Unmarshal([]byte(pauses), &ps); err != nil || len(ps) != 3 || ps[1] != 7.5 || ps[2] != 3 || startClosest != 1 || relCity != 0 || relEnc != 0 || pset != "Set_A" {
		t.Errorf("pauses %s start_closest %d city %d encounter %d set %q (%v)", pauses, startClosest, relCity, relEnc, pset, err)
	}
	if n, f, form := one[int](t, db, `SELECT num_waypoints FROM npc_instance WHERE guid = ?`, pInstWolf), one[float64](t, db, `SELECT follow_distance FROM npc_instance WHERE guid = ?`, pInstWolf),
		one[string](t, db, `SELECT formation FROM npc_instance WHERE guid = ?`, pInstWolf); n != 6 || f != 100 || form != "None" {
		t.Errorf("instance patrol settings: waypoints %d follow %v formation %q", n, f, form)
	}

	// vendors: the listings with their offers, the inventories in order (one that is no row stays listed)
	if n := one[int](t, db, `SELECT COUNT(*) FROM vendor_def_inventory WHERE vendor_guid = ?`, pVendor); n != 2 {
		t.Errorf("vendor inventories = %d", n)
	}
	if got := one[string](t, db, `SELECT offers FROM vendor_listing WHERE inventory_guid = ? AND ord = 1`, pInventory); got != "["+strings.TrimSpace(jsonText(pOffer))+"]" {
		t.Errorf("offers = %s", got)
	}
	if got := one[string](t, db, `SELECT offers FROM vendor_listing WHERE inventory_guid = ? AND ord = 0`, pInventory); got != "[]" {
		t.Errorf("a free listing's offers = %s", got)
	}
	if got := one[int64](t, db, `SELECT item_guid FROM vendor_listing WHERE inventory_guid = ? AND ord = 1`, pInventory); got != pItemB {
		t.Errorf("item = %d", got)
	}
	if got := one[int](t, db, `SELECT hidden FROM vendor_inventory WHERE guid = ?`, pInventory); got != 1 {
		t.Errorf("hidden = %d", got)
	}
}

// A record of the wrong shape stops the build with a message that names the row and the field.
func TestSyntheticPopulationRefuses(t *testing.T) {
	src := populationSource()
	src.rows["PopulationAssetRecordBase"][0].Fields["ScaleMin"] = "oops"
	if _, _, err := buildPopulationSynthetic(t, src); err == nil || !strings.Contains(err.Error(), "Wolf_Asset") || !strings.Contains(err.Error(), "ScaleMin") {
		t.Errorf("a string scale: %v", err)
	}
	src = populationSource()
	delete(src.rows, "WorldSpawnRecordBase")
	if _, _, err := buildPopulationSynthetic(t, src); err == nil || !strings.Contains(err.Error(), "WorldSpawnRecordBase") {
		t.Errorf("a missing table: %v", err)
	}
	src = populationSource()
	src.rows["PopulationSetRecordBase"][0].Fields["PopulationInstancesMap"] = designdb.Pairs{{Key: "one", Value: st{}}}
	if _, _, err := buildPopulationSynthetic(t, src); err == nil || !strings.Contains(err.Error(), "PopulationInstancesMap") {
		t.Errorf("a map with a string key: %v", err)
	}
}

// The real design database: the counts the survey found, the chain's links and the PTR vendors.
func TestRealPopulation(t *testing.T) {
	db := builtDB(t)
	counts := map[string]int64{
		"npc_asset": 4576, "npc_asset_set": 4290, "npc_instance": 16902, "npc_set": 4728, "npc_set_member": 45714, "npc_world_spawn": 1089, "npc_placement": 15179,
		"npc_area": 608, "npc_area_circle": 4126, "npc_route": 1346, "vendor_def": 95, "vendor_inventory": 278,
	}
	for table, want := range counts {
		if got := scalarInt(t, db, `SELECT COUNT(*) FROM `+table); got != want {
			t.Errorf("%s has %d rows, want %d", table, got, want)
		}
	}
	// the links of the chain hold but for the design data's own strays (a row that names a deleted record)
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_asset_set_member m LEFT JOIN npc_asset a ON a.guid = m.asset_guid WHERE a.guid IS NULL`); n != 19 {
		t.Errorf("%d asset set members name no asset, want the 19 of the survey", n)
	}
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_set_member m LEFT JOIN npc_instance i ON i.guid = m.instance_guid WHERE i.guid IS NULL`); n != 12 {
		t.Errorf("%d set members name no instance, want 12", n)
	}
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_placement p LEFT JOIN npc_set s ON s.guid = p.set_guid WHERE s.guid IS NULL`); n != 10 {
		t.Errorf("%d placements name no set, want 10", n)
	}
	// a blueprint's archetype path as the retail server exports it: a wolf (nine assets name that blueprint)
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_asset WHERE archetype_path = '/Game/DesignAssets/Population/NPC/Beast/Canine/Wolf.Default__Wolf_C'`); n < 1 {
		t.Errorf("no asset has the wolf's archetype path")
	}
	// the points around the login spot (the spec's area): 3,929 instance points within 1 km
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_placement p JOIN npc_set_member m ON m.set_guid = p.set_guid
		WHERE (p.x + m.x * cos(radians(p.yaw)) - m.y * sin(radians(p.yaw)) + 777664.5) * (p.x + m.x * cos(radians(p.yaw)) - m.y * sin(radians(p.yaw)) + 777664.5)
		    + (p.y + m.x * sin(radians(p.yaw)) + m.y * cos(radians(p.yaw)) - 616578.6875) * (p.y + m.x * sin(radians(p.yaw)) + m.y * cos(radians(p.yaw)) - 616578.6875) <= 100000.0 * 100000.0`); n != 0 && n < 3000 {
		t.Errorf("only %d points within 1 km of the login spot", n)
	}
	// the PTR vendors list GM and PTR items for free
	var vend, inv int64
	if err := db.SQL().QueryRow(`SELECT guid FROM vendor_def WHERE name = 'PTR_ONLY_Vendor_Gear_20-29'`).Scan(&vend); err != nil {
		t.Fatal(err)
	}
	if inv = scalarInt(t, db, `SELECT COUNT(*) FROM vendor_def_inventory WHERE vendor_guid = ?`, vend); inv < 1 {
		t.Errorf("the PTR gear vendor has %d inventories", inv)
	}
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM vendor_listing l JOIN vendor_def_inventory d ON d.inventory_guid = l.inventory_guid WHERE d.vendor_guid = ? AND l.offers != '[]'`, vend); n != 0 {
		t.Errorf("%d listings of the PTR gear vendor have offers (a price)", n)
	}
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM vendor_listing l JOIN item i ON i.guid = l.item_guid JOIN vendor_def_inventory d ON d.inventory_guid = l.inventory_guid
		WHERE d.vendor_guid = ? AND (i.name LIKE 'GM%' OR i.name LIKE '%PTR%')`, vend); n < 60 {
		t.Errorf("the PTR gear vendor lists %d GM or PTR items", n)
	}
	// version 8: 1,332 of the 1,346 routes have waypoints (14 have neither those nor a spline); 924 also have the spline of the same points
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_route WHERE waypoints != '[]'`); n != 1332 {
		t.Errorf("%d routes have waypoints, want 1332", n)
	}
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_route WHERE waypoints != '[]' AND points != '[]'`); n != 924 {
		t.Errorf("%d routes have both waypoints and a spline, want 924", n)
	}
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_instance WHERE patrol_route_guid != 0`); n != 1619 {
		t.Errorf("%d instances name a route, want 1619", n)
	}
	// the NPC stat initializer lists were in stat_init already
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM stat_init WHERE list_name = 'NPC_Base'`); n != 40 {
		t.Errorf("NPC_Base has %d stats", n)
	}
	// version 9: the filters and the gates (the survey of the real data: 247 overrides; 1,761 sets that some quest, event, phase, building or layout spawns)
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_filter`); n != 247 {
		t.Errorf("npc_filter has %d rows, want 247", n)
	}
	if n := scalarInt(t, db, `SELECT COUNT(DISTINCT set_guid) FROM npc_gate`); n != 1761 {
		t.Errorf("%d sets are gated, want 1761", n)
	}
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_asset WHERE filter LIKE '%"node_level_min":"cCount"%'`); n != 44 {
		t.Errorf("%d assets can never pass their node level, want 44", n)
	}
	if n := scalarInt(t, db, `SELECT COUNT(*) FROM npc_world_spawn WHERE map_name = 'Verra_World_Master'`); n != 543 {
		t.Errorf("%d world spawns are of the world map, want 543", n)
	}
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	if meta["contentc.version"] != "12" || meta["typed.npc_placement"] != "15179" {
		t.Errorf("meta contentc.version %q, typed.npc_placement %q", meta["contentc.version"], meta["typed.npc_placement"])
	}
}

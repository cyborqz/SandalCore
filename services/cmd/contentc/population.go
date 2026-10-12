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

// Plan E1, stage 1 (contentc version 7): the NPC population chain and the vendors (internal/content/population.go has the tables).  Stage 2
// (version 8) adds what walking needs: the waypoints and pauses of a PathingRoute (a route has its waypoints, and most also a spline of the same
// points) and a PopulationInstance's patrol settings.
//
// A PopulationAsset names an NPC's blueprint; an AssetSet is a weighted list of assets; a PopulationInstance draws from AssetSets and says how
// the NPC lives (level range, respawn, wander, patrol); a PopulationSet places instances at local offsets; a WorldSpawn places sets in the world;
// a PopulationArea is a set of circles the instances of its set spawn inside. Everything is copied as the design data holds it: nothing here
// decides which asset a spawner picks or where in a circle (worldd does, from these rows and a seed).

// populationRecordTypes are the record structs the builder reads.
var populationRecordTypes = []string{
	"PopulationAssetRecordBase", "AssetSetRecordBase", "PopulationInstanceRecordBase", "PopulationSetRecordBase", "WorldSpawnRecordBase",
	"PopulationAreaRecordBase", "PathingRouteRecordBase", "VendorDefRecordBase", "VendorInventoryDefRecordBase", "FilteringOverrideRecordBase",
}

type populationSummary struct {
	assets, assetLoot, sets, setMembers, instances, instanceSets, instanceLoot, popSets, popMembers, worldSpawns, placements int
	areas, circles, routes, routePoints, waypoints, vendors, vendorInventories, listings, noArchetype, badRefs               int
	filters, gates, gatedSets, devOnly                                                                                       int
}

// archetypePath is the path of a Blueprint's class default object from an AssetRef ("/Game/X/Wolf.Wolf" -> "/Game/X/Wolf.Default__Wolf_C"), the
// name the retail server exports an NPC's archetype by; "" for an asset that names no Blueprint (an empty ref, or one that is not a package path).
func archetypePath(ref string) string {
	ref = strings.TrimSpace(ref)
	if !strings.HasPrefix(ref, "/") {
		return ""
	}
	dot := strings.LastIndex(ref, ".")
	slash := strings.LastIndex(ref, "/")
	if dot < 0 || dot < slash {
		return ""
	}
	pkg, leaf := ref[:dot], ref[dot+1:]
	if pkg == "" || leaf == "" {
		return ""
	}
	leaf = strings.TrimSuffix(leaf, "_C")
	return pkg + ".Default__" + leaf + "_C"
}

// vec reads an {X, Y, Z} struct of a field.
func vec(f *fields, name string) (x, y, z float64) {
	s := f.strukt(name)
	if f.err != nil {
		return 0, 0, 0
	}
	vf := newFields(f.where+": "+name, s)
	x, y, z = vf.number("X"), vf.number("Y"), vf.number("Z")
	if vf.err != nil {
		f.err = vf.err
	}
	return x, y, z
}

// [Cyb x C.O.A]

// yawOf reads a Rotator's Yaw.
func yawOf(f *fields, name string) float64 {
	s := f.strukt(name)
	if f.err != nil {
		return 0
	}
	rf := newFields(f.where+": "+name, s)
	yaw := rf.number("Yaw")
	if rf.err != nil {
		f.err = rf.err
	}
	return yaw
}

func jsonText(v any) string {
	b, err := json.Marshal(v)
	if err != nil {
		return "[]"
	}
	return string(b)
}

// popFilter is a PopulationFilter as npc_asset.filter and npc_filter.filter hold it (JSON: the field order is the struct's, so a build is the same
// bytes every time). The two strategies (how an NPC phases in and out) are not kept: nothing here spawns by time.
type popFilter struct {
	Biomes    []string `json:"biomes"`
	Cultures  []string `json:"cultures"`
	NodeIDs   []int64  `json:"node_ids"`
	LevelMin  string   `json:"node_level_min"`
	LevelMax  string   `json:"node_level_max"`
	NodeTypes []string `json:"node_types"`
	Tags      []string `json:"required_tags"`
	Seasons   []string `json:"seasons"`
	TodStart  float64  `json:"time_of_day_start"`
	TodEnd    float64  `json:"time_of_day_end"`
	Weather   []string `json:"weather"`
	Zones     []int64  `json:"zones_of_influence"`
}

// enumList reads an array of enum values ("ENodeBiome::Riverlands") as their names without the type.
func enumList(f *fields, name string) []string {
	out := []string{}
	for _, v := range f.list(name) {
		s, ok := v.(string)
		if !ok {
			f.fail(name, "array of enum values", v, true)
			return out
		}
		out = append(out, enumValue(s))
	}
	return out
}

// readPopFilter reads the PopulationFilter struct held in field `name` of f.
func readPopFilter(f *fields, name string) popFilter {
	pf := popFilter{Biomes: []string{}, Cultures: []string{}, NodeIDs: []int64{}, NodeTypes: []string{}, Tags: []string{}, Seasons: []string{}, Weather: []string{}, Zones: []int64{}}
	st := f.strukt(name)
	if f.err != nil {
		return pf
	}
	ff := newFields(f.where+": "+name, st)
	pf.Biomes = enumList(ff, "Biomes")
	pf.Cultures = enumList(ff, "Cultures")
	pf.NodeIDs = ff.ids("NodeIds")
	pf.LevelMin = enumValue(ff.str("NodeLevelMin"))
	pf.LevelMax = enumValue(ff.str("NodeLevelMax"))
	pf.NodeTypes = enumList(ff, "NodeTypes")
	pf.Tags = ff.tags("RequiredGameplayTags")
	pf.Seasons = enumList(ff, "Seasons")
	pf.TodStart = ff.number("TimeOfDayStart")
	pf.TodEnd = ff.number("TimeOfDayEnd")
	pf.Weather = enumList(ff, "WeatherTypes")
	pf.Zones = ff.ids("ZoneOfInfluenceIds")
	if pf.NodeIDs == nil {
		pf.NodeIDs = []int64{}
	}
	if pf.Zones == nil {
		pf.Zones = []int64{}
	}
	if pf.Tags == nil {
		pf.Tags = []string{}
	}
	if ff.err != nil {
		f.err = ff.err
	}
	return pf
}

// gateRule names a field of a record that spawns a PopulationSet when the record's turn comes: a quest objective's start, success or tick actions,
// an event's trigger or stage, a story arc phase, a building's level, a node layout, an action response. A field that only counts or talks to the
// set's NPCs (a kill objective's targets, a dialogue source, a tracker, an interact volume's filter) is not one: those NPCs are the world's.
type gateRule struct {
	schema string
	path   string
	source string
}

// [C_Y_B :: C.O.A]

var gateRules = []gateRule{
	{"NarrativeObjectiveV2", "OnStartActions.[].Action.SpawnIDs.[].PopulationSet", "NarrativeObjective"},
	{"NarrativeObjectiveV2", "OnSuccessActions.[].Action.SpawnIDs.[].PopulationSet", "NarrativeObjective"},
	{"NarrativeObjectiveV2", "OnTickActions.[].Action.SpawnIDs.[].PopulationSet", "NarrativeObjective"},
	{"NarrativeObjectiveV2", "TreasureMapData.OnAnyItemRefUse.[].Action.SpawnIDs.[].PopulationSet", "NarrativeObjective"},
	{"NarrativeObjectiveV2", "UseItemData.OnAnyItemRefUse.[].Action.SpawnIDs.[].PopulationSet", "NarrativeObjective"},
	{"NarrativeQuestV2", "OnStartActions.[].Action.SpawnIDs.[].PopulationSet", "NarrativeQuest"},
	{"EventTriggerDefRecordBase", "PopulationSetsIds.[]", "EventTriggerDef"},
	{"ActionResponseRecordBase", "SpawnPopulationSets.[].PopulationSet", "ActionResponse"},
	{"ActionResponseRecordBase", "SetSpawnersAreaStates.[].SpecificSpawners.SpawnerSets.[]", "ActionResponse"},
	{"StoryArcPhaseRecordBase", "ActivePopulationSetsIds.[]", "StoryArcPhase"},
	{"LayoutRecordBase", "PopulationSetsIds.[]", "Layout"},
	{"CannonizationStateRecordBase", "PopulationSetsIds.[]", "CannonizationState"},
	{"EventObjectiveDefRecordBase", "PopulationSetId", "EventObjectiveDef"},
	{"EventStageDefRecordBase", "RewardPopulationSetId", "EventStageDef"},
	{"EventDefRecordBase", "RewardPopulationSetId", "EventDef"},
	{"BuildingLevelSpawnRecordBase", "ConstructionPopulationIds.[]", "BuildingLevelSpawn"},
	{"BuildingLevelSpawnRecordBase", "DisarrayPopulationIds.[]", "BuildingLevelSpawn"},
	{"BuildingLevelSpawnRecordBase", "DamagedPopulationIds.[]", "BuildingLevelSpawn"},
	{"BuildingLevelSpawnRecordBase", "DemolitionPopulationIds.[]", "BuildingLevelSpawn"},
	{"BuildingLevelSpawnRecordBase", "DestroyedPopulationIds.[]", "BuildingLevelSpawn"},
	{"BuildingLevelSpawnRecordBase", "OperationalPopulationIds.[]", "BuildingLevelSpawn"},
	{"BuildingLevelSpawnRecordBase", "RepairingPopulationIds.[]", "BuildingLevelSpawn"},
}

// gateSchemas lists the record structs gateRules reads.
func gateSchemas() []string {
	seen := map[string]bool{}
	var out []string
	for _, r := range gateRules {
		if !seen[r.schema] {
			seen[r.schema] = true
			out = append(out, r.schema)
		}
	}
	sort.Strings(out)
	return out
}

// walkRefs visits every Id struct (Guid and TypeId) under v with the path of the field names (an array level is "[]") that leads to it.
func walkRefs(v designdb.Value, path string, visit func(path string, guid int64)) {
	join := func(elem string) string {
		if path == "" {
			return elem
		}
		return path + "." + elem
	}
	switch t := v.(type) {
	case designdb.Struct:
		if g, ok := t["Guid"].(int64); ok {
			if _, has := t["TypeId"]; has {
				visit(path, g)
			}
		}
		for k, c := range t {
			walkRefs(c, join(k), visit)
		}
	case []designdb.Value:
		for _, c := range t {
			walkRefs(c, join("[]"), visit)
		}
	case designdb.Pairs:
		for _, pr := range t {
			walkRefs(pr.Key, join("[key]"), visit)
			walkRefs(pr.Value, join("[]"), visit)
		}
	}
}

// populationGates finds the PopulationSets that some record spawns when its turn comes (gateRules): set guid -> the sources that name it.
func populationGates(src *source, sets map[int64]bool) (map[int64]map[string]bool, error) {
	out := map[int64]map[string]bool{}
	bySchema := map[string][]gateRule{}
	for _, r := range gateRules {
		bySchema[r.schema] = append(bySchema[r.schema], r)
	}
	for schema, rules := range bySchema {
		rows, err := src.need(schema)
		if err != nil {
			return nil, err
		}
		want := map[string]string{}
		for _, r := range rules {
			want[r.path] = r.source
		}
		for _, row := range rows {
			walkRefs(row.Fields, "", func(path string, guid int64) {
				source, ok := want[path]
				if !ok || !sets[guid] {
					return
				}
				if out[guid] == nil {
					out[guid] = map[string]bool{}
				}
				out[guid][source] = true
			})
		}
	}
	return out, nil
}

// C | O | A / Cyb

func buildPopulation(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range populationRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	for _, schema := range gateSchemas() {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	var sum populationSummary
	known := map[int64]bool{}

	// assets
	for _, r := range src.rows["PopulationAssetRecordBase"] {
		f := newFields(fmt.Sprintf("PopulationAsset %s (%d)", r.Name, r.Guid), r.Fields)
		ref := f.str("AssetRef")
		faction := f.int("FactionId")
		aggro := f.number("AggroRadius")
		leash := f.number("LeashRadius")
		overAggro := f.boolean("bOverrideAggroRadius")
		overLeash := f.boolean("bOverrideLeashRadius")
		smin, smax := f.number("ScaleMin"), f.number("ScaleMax")
		tags := f.tags("GameplayTags")
		dialogue, _ := f.id("DialogueRoot")
		icd, _ := f.id("InteractComponentDefId")
		pid, _ := f.id("InteractionDefId")
		look, _ := f.id("AppearanceOverrideId")
		loot := f.ids("LootTablesIds")
		filter := readPopFilter(f, "PopulationFilter")
		if f.err != nil {
			return f.err
		}
		path := archetypePath(ref)
		if path == "" {
			sum.noArchetype++
		}
		if err := w.exec(`INSERT INTO npc_asset (guid, name, asset_ref, archetype_path, faction_guid, aggro_radius, leash_radius, override_aggro, override_leash,
			scale_min, scale_max, tags, dialogue_root_guid, interact_component_guid, interaction_def_guid, appearance_guid, dev_only, filter)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, ref, path, faction, aggro, leash, b2i(overAggro), b2i(overLeash), smin, smax, jsonText(tags), dialogue, icd, pid, look, b2i(r.DevOnly),
			jsonText(filter)); err != nil {
			return err
		}
		if r.DevOnly {
			sum.devOnly++
		}
		known[r.Guid] = true
		sum.assets++
		for i, t := range loot {
			if t == 0 {
				continue
			}
			if err := w.exec(`INSERT INTO npc_asset_loot (asset_guid, ord, table_guid) VALUES (?, ?, ?)`, r.Guid, i, t); err != nil {
				return err
			}
			sum.assetLoot++
		}
	}

	// asset sets
	for _, r := range src.rows["AssetSetRecordBase"] {
		f := newFields(fmt.Sprintf("AssetSet %s (%d)", r.Name, r.Guid), r.Fields)
		defs := f.pairs("AssetDefMap")
		setTags := f.tags("GameplayTags")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO npc_asset_set (guid, name, tags) VALUES (?, ?, ?)`, r.Guid, r.Name, jsonText(setTags)); err != nil {
			return err
		}
		sum.sets++
		for i, p := range defs {
			ks, kok := p.Key.(designdb.Struct)
			vs, vok := p.Value.(designdb.Struct)
			if !kok || !vok {
				return fmt.Errorf("%s: AssetDefMap[%d] is not an Id and a struct", f.where, i)
			}
			kf := newFields(f.where, nil)
			asset, _ := idOf(kf, fmt.Sprintf("%s: AssetDefMap[%d]", f.where, i), ks)
			vf := newFields(fmt.Sprintf("%s: AssetDefMap[%d]", f.where, i), vs)
			weight := vf.number("Weight")
			if err := firstErr(kf, vf); err != nil {
				return err
			}
			if err := w.exec(`INSERT INTO npc_asset_set_member (set_guid, ord, asset_guid, weight) VALUES (?, ?, ?, ?)`, r.Guid, i, asset, weight); err != nil {
				return err
			}
			if !known[asset] {
				sum.badRefs++
			}
			sum.setMembers++
		}
	}

	// instances
	for _, r := range src.rows["PopulationInstanceRecordBase"] {
		f := newFields(fmt.Sprintf("PopulationInstance %s (%d)", r.Name, r.Guid), r.Fields)
		levelMin, levelMax := f.int("NPCLevelMin"), f.int("NPCLevelMax")
		dyn := f.boolean("bDynamicallyLeveled")
		respawn, variance := f.number("RespawnTime"), f.number("RespawnVariance")
		rule := f.str("RespawnRule")
		route, _ := f.id("PatrolRouteId")
		ambient := f.str("NpcAmbientBehavior")
		minR, maxR := f.number("MinRadius"), f.number("MaxRadius")
		arrival := f.number("ArrivalDistance")
		wanderMult := f.number("WanderSpeedMultiplier")
		wanderDelay, wanderDev := f.number("WanderDelay"), f.number("WanderDelayDeviation")
		speed := f.str("SpeedMultiplierExpression")
		snap := f.str("SnappingRule")
		orient := f.str("OrientationType")
		aggro := f.number("AggroRadius")
		overAggro := f.boolean("bOverrideAggroRadius")
		areaWeight := f.number("PopulationAreaWeight")
		vendor, _ := f.id("VendorTableId")
		icd, _ := f.id("InteractComponentDefId")
		resp, _ := f.id("InteractResponseId")
		dialogue, _ := f.id("DefaultDialogueId")
		group := f.int("GroupId")
		locType := f.str("SpawnLocationType")
		activation := f.number("OverrideActivationRange")
		black := f.boolean("Blacklisted")
		numWaypoints := f.int("NumberOfWaypoints")
		maxAngle := f.number("MaxAngle")
		follow := f.number("FollowDistance")
		formation := f.str("Formation")
		idleAnim := f.str("IdleAnim")
		alignSlope := f.boolean("AlignToSlope")
		override, _ := f.id("FilteringOverrideId")
		priority := f.str("SpawnerPriority")
		instTags := f.tags("GameplayTags")
		sets := f.ids("AssetSetsIds")
		loot := f.ids("LootTablesIds")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO npc_instance (guid, name, level_min, level_max, dynamic_leveling, respawn_time, respawn_variance, respawn_rule, patrol_route_guid,
			ambient_behavior, min_radius, max_radius, arrival_distance, wander_speed_mult, wander_delay, wander_delay_deviation, speed_expr, snapping_rule, orientation_type,
			aggro_radius, override_aggro, area_weight, vendor_guid, interact_component_guid, interact_response_guid, dialogue_guid, group_id, spawn_location_type,
			activation_range, blacklisted, num_waypoints, max_angle, follow_distance, formation, idle_anim, align_to_slope, dev_only, filter_override_guid, spawner_priority, tags)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, levelMin, levelMax, b2i(dyn), respawn, variance, enumValue(rule), route, enumValue(ambient), minR, maxR, arrival, wanderMult, wanderDelay, wanderDev,
			speed, enumValue(snap), enumValue(orient), aggro, b2i(overAggro), areaWeight, vendor, icd, resp, dialogue, group, enumValue(locType), activation, b2i(black),
			numWaypoints, maxAngle, follow, enumValue(formation), enumValue(idleAnim), b2i(alignSlope), b2i(r.DevOnly), override, enumValue(priority), jsonText(instTags)); err != nil {
			return err
		}
		if r.DevOnly {
			sum.devOnly++
		}
		sum.instances++
		for i, s := range sets {
			if s == 0 {
				continue
			}
			if err := w.exec(`INSERT INTO npc_instance_asset_set (instance_guid, ord, set_guid) VALUES (?, ?, ?)`, r.Guid, i, s); err != nil {
				return err
			}
			sum.instanceSets++
		}
		for i, t := range loot {
			if t == 0 {
				continue
			}
			if err := w.exec(`INSERT INTO npc_instance_loot (instance_guid, ord, table_guid) VALUES (?, ?, ?)`, r.Guid, i, t); err != nil {
				return err
			}
			sum.instanceLoot++
		}
	}

	// sets
	for _, r := range src.rows["PopulationSetRecordBase"] {
		f := newFields(fmt.Sprintf("PopulationSet %s (%d)", r.Name, r.Guid), r.Fields)
		area, _ := f.id("PopulationAreaId")
		activation := f.number("OverrideActivationRange")
		black := f.boolean("Blacklisted")
		override, _ := f.id("FilterOverrideId")
		setTags := f.tags("GameplayTags")
		members := f.pairs("PopulationInstancesMap")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO npc_set (guid, name, area_guid, activation_range, blacklisted, dev_only, filter_override_guid, tags) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, area, activation, b2i(black), b2i(r.DevOnly), override, jsonText(setTags)); err != nil {
			return err
		}
		if r.DevOnly {
			sum.devOnly++
		}
		sum.popSets++
		for i, p := range members {
			key, kok := p.Key.(int64)
			vs, vok := p.Value.(designdb.Struct)
			if !kok || !vok {
				return fmt.Errorf("%s: PopulationInstancesMap[%d] is not an int and a struct", f.where, i)
			}
			vf := newFields(fmt.Sprintf("%s: PopulationInstancesMap[%d]", f.where, i), vs)
			inst, _ := vf.id("InstanceId")
			x, y, z := vec(vf, "Location")
			yaw := yawOf(vf, "Rotation")
			random := vf.boolean("bRandomRotation")
			if vf.err != nil {
				return vf.err
			}
			if err := w.exec(`INSERT INTO npc_set_member (set_guid, key, instance_guid, x, y, z, yaw, random_rotation) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, key, inst, x, y, z, yaw, b2i(random)); err != nil {
				return err
			}
			sum.popMembers++
		}
	}

	// world spawns
	for _, r := range src.rows["WorldSpawnRecordBase"] {
		f := newFields(fmt.Sprintf("WorldSpawn %s (%d)", r.Name, r.Guid), r.Fields)
		culture := f.str("Culture")
		mapName := f.str("MapName")
		override, _ := f.id("FilterOverrideId")
		places := f.pairs("PopulationSetsMap")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO npc_world_spawn (guid, name, culture, map_name, dev_only, filter_override_guid) VALUES (?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, enumValue(culture), mapName, b2i(r.DevOnly), override); err != nil {
			return err
		}
		if r.DevOnly {
			sum.devOnly++
		}
		sum.worldSpawns++
		for i, p := range places {
			key, kok := p.Key.(int64)
			vs, vok := p.Value.(designdb.Struct)
			if !kok || !vok {
				return fmt.Errorf("%s: PopulationSetsMap[%d] is not an int and a struct", f.where, i)
			}
			vf := newFields(fmt.Sprintf("%s: PopulationSetsMap[%d]", f.where, i), vs)
			set, _ := vf.id("SetId")
			x, y, z := vec(vf, "Location")
			yaw := yawOf(vf, "Rotation")
			minL, maxL := vf.int("MinLevelOverride"), vf.int("MaxLevelOverride")
			if vf.err != nil {
				return vf.err
			}
			if err := w.exec(`INSERT INTO npc_placement (spawn_guid, key, set_guid, x, y, z, yaw, min_level, max_level) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, key, set, x, y, z, yaw, minL, maxL); err != nil {
				return err
			}
			sum.placements++
		}
	}

	// areas
	for _, r := range src.rows["PopulationAreaRecordBase"] {
		f := newFields(fmt.Sprintf("PopulationArea %s (%d)", r.Name, r.Guid), r.Fields)
		minN, maxN := f.int("MinNumSpawns"), f.int("MaxNumSpawns")
		part := f.number("PartitionLength")
		defs := f.list("AreaDefinitions")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO npc_area (guid, name, min_spawns, max_spawns, partition_length) VALUES (?, ?, ?, ?, ?)`, r.Guid, r.Name, minN, maxN, part); err != nil {
			return err
		}
		sum.areas++
		for i, dv := range defs {
			ds, ok := dv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: AreaDefinitions[%d] is %T", f.where, i, dv)
			}
			df := newFields(fmt.Sprintf("%s: AreaDefinitions[%d]", f.where, i), ds)
			x, y, z := vec(df, "Center")
			extent := df.number("Extent")
			weight := df.number("Weight")
			maxSpawns := df.int("MaxNumSpawns")
			if df.err != nil {
				return df.err
			}
			if err := w.exec(`INSERT INTO npc_area_circle (area_guid, ord, x, y, z, extent, weight, max_spawns) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, i, x, y, z, extent, weight, maxSpawns); err != nil {
				return err
			}
			sum.circles++
		}
	}

	// patrol routes
	for _, r := range src.rows["PathingRouteRecordBase"] {
		f := newFields(fmt.Sprintf("PathingRoute %s (%d)", r.Name, r.Guid), r.Fields)
		looping := f.boolean("Looping")
		tr := f.strukt("PathActorTransform")
		curves := f.strukt("SplineCurves")
		wps := f.list("WaypointLocations")
		pauseVals := f.list("WaypointPauseLengths")
		startClosest := f.boolean("StartRouteClosestPoint")
		relCity := f.boolean("RelativeToCityNode")
		relEnc := f.boolean("RelativeToEncounter")
		pset := f.str("PathingSet")
		if f.err != nil {
			return f.err
		}
		var waypoints [][3]float64
		for i, wv := range wps {
			ws, ok := wv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: WaypointLocations[%d] is %T", f.where, i, wv)
			}
			wf := newFields(fmt.Sprintf("%s: WaypointLocations[%d]", f.where, i), ws)
			x, y, z := wf.number("X"), wf.number("Y"), wf.number("Z")
			if wf.err != nil {
				return wf.err
			}
			waypoints = append(waypoints, [3]float64{x, y, z})
		}
		if waypoints == nil {
			waypoints = [][3]float64{}
		}
		pauses := []float64{}
		for i, pv := range pauseVals {
			switch n := pv.(type) {
			case float64:
				pauses = append(pauses, n)
			case float32:
				pauses = append(pauses, decimal32(n))
			case int64:
				pauses = append(pauses, float64(n))
			default:
				return fmt.Errorf("%s: WaypointPauseLengths[%d] is %T", f.where, i, pv)
			}
		}
		tf := newFields(f.where+": PathActorTransform", tr)
		tx, ty, tz := vec(tf, "Translation")
		sx, sy, sz := vec(tf, "Scale3D")
		rot := tf.strukt("Rotation")
		if tf.err != nil {
			return tf.err
		}
		rf := newFields(tf.where+": Rotation", rot)
		qx, qy, qz, qw := rf.number("X"), rf.number("Y"), rf.number("Z"), rf.number("W")
		cf := newFields(f.where+": SplineCurves", curves)
		pos := cf.strukt("Position")
		if err := firstErr(rf, cf); err != nil {
			return err
		}
		pf := newFields(cf.where+": Position", pos)
		pts := pf.list("Points")
		if pf.err != nil {
			return pf.err
		}
		var points [][3]float64
		for i, pv := range pts {
			ps, ok := pv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: Points[%d] is %T", pf.where, i, pv)
			}
			ptf := newFields(fmt.Sprintf("%s: Points[%d]", pf.where, i), ps)
			x, y, z := vec(ptf, "OutVal")
			if ptf.err != nil {
				return ptf.err
			}
			points = append(points, [3]float64{x, y, z})
		}
		if points == nil {
			points = [][3]float64{}
		}
		if err := w.exec(`INSERT INTO npc_route (guid, name, looping, tx, ty, tz, qx, qy, qz, qw, sx, sy, sz, points, waypoints, pauses, start_closest, relative_city_node, relative_encounter,
			pathing_set) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, b2i(looping), tx, ty, tz, qx, qy, qz, qw, sx, sy, sz, jsonText(points), jsonText(waypoints), jsonText(pauses), b2i(startClosest), b2i(relCity), b2i(relEnc),
			pset); err != nil {
			return err
		}
		sum.routes++
		sum.routePoints += len(points)
		sum.waypoints += len(waypoints)
	}

	// the filtering overrides
	for _, r := range src.rows["FilteringOverrideRecordBase"] {
		f := newFields(fmt.Sprintf("FilteringOverride %s (%d)", r.Name, r.Guid), r.Fields)
		filter := readPopFilter(f, "PopulationFilterOverride")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO npc_filter (guid, name, filter) VALUES (?, ?, ?)`, r.Guid, r.Name, jsonText(filter)); err != nil {
			return err
		}
		sum.filters++
	}

	// the sets some record spawns when its turn comes
	setGuids := map[int64]bool{}
	for _, r := range src.rows["PopulationSetRecordBase"] {
		setGuids[r.Guid] = true
	}
	gates, err := populationGates(src, setGuids)
	if err != nil {
		return err
	}
	var gatedGuids []int64
	for g := range gates {
		gatedGuids = append(gatedGuids, g)
	}
	sort.Slice(gatedGuids, func(i, j int) bool { return gatedGuids[i] < gatedGuids[j] })
	for _, g := range gatedGuids {
		var names []string
		for n := range gates[g] {
			names = append(names, n)
		}
		sort.Strings(names)
		for _, n := range names {
			if err := w.exec(`INSERT INTO npc_gate (set_guid, source) VALUES (?, ?)`, g, n); err != nil {
				return err
			}
			sum.gates++
		}
		sum.gatedSets++
	}

	// vendors
	for _, r := range src.rows["VendorInventoryDefRecordBase"] {
		f := newFields(fmt.Sprintf("VendorInventoryDef %s (%d)", r.Name, r.Guid), r.Fields)
		hidden := f.boolean("bHiddenIfUnavailable")
		listings := f.list("Listings")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO vendor_inventory (guid, name, hidden) VALUES (?, ?, ?)`, r.Guid, r.Name, b2i(hidden)); err != nil {
			return err
		}
		sum.vendorInventories++
		for i, lv := range listings {
			ls, ok := lv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: Listings[%d] is %T", f.where, i, lv)
			}
			lf := newFields(fmt.Sprintf("%s: Listings[%d]", f.where, i), ls)
			item, _ := lf.id("ItemId")
			offers := lf.ids("Offers")
			lhidden := lf.boolean("bHiddenIfUnavailable")
			if lf.err != nil {
				return lf.err
			}
			if offers == nil {
				offers = []int64{}
			}
			if err := w.exec(`INSERT INTO vendor_listing (inventory_guid, ord, item_guid, offers, hidden) VALUES (?, ?, ?, ?, ?)`, r.Guid, i, item, jsonText(offers), b2i(lhidden)); err != nil {
				return err
			}
			sum.listings++
		}
	}
	for _, r := range src.rows["VendorDefRecordBase"] {
		f := newFields(fmt.Sprintf("VendorDef %s (%d)", r.Name, r.Guid), r.Fields)
		ops := f.int("SupportedOperations")
		invs := f.ids("InventoryIds")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO vendor_def (guid, name, operations) VALUES (?, ?, ?)`, r.Guid, r.Name, ops); err != nil {
			return err
		}
		sum.vendors++
		for i, inv := range invs {
			if err := w.exec(`INSERT INTO vendor_def_inventory (vendor_guid, ord, inventory_guid) VALUES (?, ?, ?)`, r.Guid, i, inv); err != nil {
				return err
			}
		}
	}

	opt.logf("population: %d assets (%d name no Blueprint), %d loot links, %d asset sets with %d members (%d name an asset that is no row), %d instances (%d asset set links, %d loot links), "+
		"%d sets with %d members, %d world spawns with %d placements, %d areas with %d circles, %d routes with %d spline points and %d waypoints; %d vendors, %d inventories, %d listings; "+
		"%d filtering overrides, %d sets spawned by a quest, event, phase, building or layout (%d links), %d records bDevOnly",
		sum.assets, sum.noArchetype, sum.assetLoot, sum.sets, sum.setMembers, sum.badRefs, sum.instances, sum.instanceSets, sum.instanceLoot, sum.popSets, sum.popMembers,
		sum.worldSpawns, sum.placements, sum.areas, sum.circles, sum.routes, sum.routePoints, sum.waypoints, sum.vendors, sum.vendorInventories, sum.listings,
		sum.filters, sum.gatedSets, sum.gates, sum.devOnly)
	meta["typed.npc_asset"] = strconv.Itoa(sum.assets)
	meta["typed.npc_instance"] = strconv.Itoa(sum.instances)
	meta["typed.npc_set"] = strconv.Itoa(sum.popSets)
	meta["typed.npc_placement"] = strconv.Itoa(sum.placements)
	meta["typed.npc_area_circle"] = strconv.Itoa(sum.circles)
	meta["typed.npc_route"] = strconv.Itoa(sum.routes)
	meta["typed.npc_route_waypoint"] = strconv.Itoa(sum.waypoints)
	meta["typed.npc_filter"] = strconv.Itoa(sum.filters)
	meta["typed.npc_gate"] = strconv.Itoa(sum.gates)
	meta["typed.vendor_def"] = strconv.Itoa(sum.vendors)
	meta["typed.vendor_listing"] = strconv.Itoa(sum.listings)
	return nil
}

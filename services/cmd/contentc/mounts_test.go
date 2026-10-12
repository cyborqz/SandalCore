// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// The mounts (contentc version 11): the text of an item, the tables on a synthetic design database, and the real file.

import (
	"context"
	"database/sql"
	"io"
	"path/filepath"
	"strconv"
	"testing"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

func TestParseMountText(t *testing.T) {
	for _, tc := range []struct {
		name, text string
		want       mountText
	}{
		{"a terror bird", "Ground Mount\r\nLevel 20\r\n\r\n165% Ground Speed", mountText{level: 20, ground: 165, health: 100}},
		{"a dawnbreaker", "Flying Mount\r\nLevel 25\r\n\r\n165% Ground Speed\r\n215% Flying Speed\r\n500% Maximum Health", mountText{level: 25, ground: 165, flying: 215, health: 500}},
		{"the drake", "Flying Mount\r\nLevel 25\r\n\r\n165% Ground Speed\r\n215% Flying Speed\r\n1000% Maximum Health", mountText{level: 25, ground: 165, flying: 215, health: 1000}},
		{"a hippo", "Ground Mount\r\nLevel 15\r\n\r\n145% Ground Speed\r\n120% Water Speed", mountText{level: 15, ground: 145, water: 120, health: 100}},
		{"an aquatic mount", "Aquatic Mount\r\nLevel 11\r\n\r\n120% Ground Speed\r\n160% Water Speed\r\nUnderwater Breathing", mountText{level: 11, ground: 120, water: 160, health: 100}},
		{"a weak scorpion", "Ground Mount\r\nLevel 9\r\n\r\n165% Ground Speed\r\n50% Maximum Health", mountText{level: 9, ground: 165, health: 50}},
		{"a mule", "Ground Mule\r\nLevel 15\r\n\r\n100% Ground Speed\r\nCrate Carrying Capacity: 2\r\n\r\nSummoning a mule will end PvP protection.",
			mountText{level: 15, ground: 100, health: 100, crate: 2, mule: true}},
		{"no numbers at all", "Ground Mount", mountText{health: 100}},
		{"an empty text", "", mountText{health: 100}},
	} {
		got, err := parseMountText(tc.text)
		if err != nil || got != tc.want {
			t.Errorf("%s: parseMountText = %+v, %v; want %+v", tc.name, got, err, tc.want)
		}
	}
}

// (C | O | A : C | Y | B)

// syntheticMounts is a design database with two mount items (a ground one with no speed text and a flyer cast through Summon_Mount), an item that is no
// mount, the Mount_Base list over a linear HP curve and the equations its ratings read, the abilities that matter to a rider and the effects of riding.
func syntheticMounts() *source {
	const (
		statMaxHealth, statRegen, statMitigation, statType, statMoveMult = 91, 92, 93, 94, content.GuidMoveSpeedMult
		curveHP                                                          = 7000
		eqHPPerCon, eqRating, eqHPPerRating, eqRegen, eqMit              = 8001, 8002, 8003, 8004, 8005
		archSummon, archCast                                             = 9001, 9002
	)
	src := &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}
	curve, err := aocexpr.NewCurve([]aocexpr.Key{{Time: 1, Value: 500, Interp: aocexpr.InterpLinear}, {Time: 25, Value: 4900, Interp: aocexpr.InterpLinear}})
	if err != nil {
		panic(err)
	}
	src.curves = &aocexpr.CurveMap{TypeID: content.TypeStatCurve, Curves: map[int64]*aocexpr.Curve{curveHP: curve}}
	src.statNames = map[int64]string{statMaxHealth: "MaxHealth", statRegen: "HealthRegenerationRating", statMitigation: "MitigationRating", statType: "CharacterType", statMoveMult: "MoveSpeedMult"}

	eq := func(guid int64) string { return "$#7180619218012719533:" + mountItoa(guid) + "$" }
	src.rows["StatEquationTypeRecordBase"] = []designdb.Row{
		mkRow(eqRating, "Coeff_HPRatingPerCON", st{"Equation": "// CON provides 2 \"hp rating\"\n2"}),
		mkRow(eqHPPerRating, "Coeff_HPPerRating", st{"Equation": "1.5"}),
		mkRow(eqHPPerCon, "Coeff_HPPerCon", st{"Equation": "EvalE(" + eq(eqRating) + ") * EvalE(" + eq(eqHPPerRating) + ")"}),
		mkRow(eqRegen, "Coeff_RegenPA", st{"Equation": "1"}),
		mkRow(eqMit, "Coeff_MitigationPA", st{"Equation": "2"}),
	}
	hp := "const v = EvalCurve($#7037248826340794574:" + mountItoa(curveHP) + "$, Float(GetCharacterLevel(GetOwner())));\nGetStatInitRunningTotal() + v"
	entry := func(stat int64, expr string) st { return st{"StatId": statID(stat), "ValueExpression": expr} }
	src.rows["StatInitializerListRecordBase"] = []designdb.Row{mkRow(content.GuidMountBase, "Mount_Base", st{"StatIDs": list{
		entry(statMaxHealth, hp),
		entry(statRegen, "const v = EvalCurve($#7037248826340794574:"+mountItoa(curveHP)+"$, Float(GetCharacterLevel(GetOwner())));\nconst CON = v/EvalE("+eq(eqHPPerCon)+");\nCON * EvalE("+eq(eqRegen)+") * 1.5"),
		entry(statMitigation, "const v = EvalCurve($#7037248826340794574:"+mountItoa(curveHP)+"$, Float(GetCharacterLevel(GetOwner())));\nconst CON = v/EvalE("+eq(eqHPPerCon)+");\nCON * EvalE("+eq(eqMit)+") * 2.0"),
		entry(statType, "1.1"),
		entry(statMoveMult, "1"),
		entry(statMaxHealth, "GetStatInitRunningTotal() * 2"), // a second entry of a stat continues from the first
		entry(0, ""),                                          // an entry with no stat and no text is skipped
	}})}

	arch := func(guid int64, name, action string) designdb.Row {
		return mkRow(guid, name, st{"ActivationAction": "EItemActivationAction::" + action, "ConsumeOnUse": false, "Cooldown": int64(0), "DisabledInCombat": false})
	}
	src.rows["ItemActivationArchetypeRecordBase"] = []designdb.Row{arch(archSummon, "Mount", "SummonMount"), arch(archCast, "MountCast", "CastAbility")}
	item := func(guid int64, name, tag, path, text string, archetype int64, tradeable bool) designdb.Row {
		return mkRow(guid, name, st{"GameplayTags": list{tag}, "SummonNotifyClassPath": path, "Description": designdb.Text{Source: text}, "Tradeable": tradeable,
			"ActivationArchetypeId": idv(archetype, 1, "None"), "ActivationCastId": idv(content.GuidSummonMount, 2, "None")})
	}
	src.rows["ItemRecordBase"] = []designdb.Row{
		item(501, "Mount_Grounded_Horse_StartingKaelar", "Item.Mount.Ground", "/Game/GameBlueprints/Mounts/Mount_Grounded_Horse_0.Mount_Grounded_Horse_0_C", "", archSummon, false),
		item(502, "Mount_Flying_Dawnbreaker_1_Alpha", "Item.Mount.Flying", "/Game/GameBlueprints/Mounts/Mount_Flying_Dawnbreaker_1.Mount_Flying_Dawnbreaker_1_C",
			"Flying Mount\r\nLevel 25\r\n\r\n165% Ground Speed\r\n215% Flying Speed\r\n500% Maximum Health", archCast, true),
		item(503, "RETIRED_Mount_Grounded_Daystrider_0", "Item.Mount.Ground", "/Game/GameBlueprints/Mounts/Mount_Grounded_Daystrider_0.Mount_Grounded_Daystrider_0_C", "", archSummon, false),
		mkRow(504, "Mount_Banner", st{"GameplayTags": list{"Item.Cosmetic"}, "SummonNotifyClassPath": "", "Description": designdb.Text{}, "Tradeable": true,
			"ActivationArchetypeId": idv(0, 1, "None"), "ActivationCastId": idv(0, 2, "None")}),
	}

	flags := func(guid int64, name string, usable, auto bool) designdb.Row {
		return mkRow(guid, name, st{"bUsableWhileMounted": usable, "bAutoDismount": auto})
	}
	src.rows["AoCAbilityRecordBase"] = []designdb.Row{
		flags(content.GuidSummonMount, "Summon_Mount", true, false), flags(content.GuidMountAbilitySprint, "MountAbility_Sprint", false, false),
		flags(content.GuidDismountElevated, "General_Dismount_Elevated", true, true), flags(1, "Mage_Blink", true, true), flags(2, "Fighter_Slash", false, false),
		flags(3, "Cleric_Heal", true, false),
	}
	src.rows["StatModRecordBase"] = []designdb.Row{
		mkRow(601, "Road_T3_Speed_Mount", st{"StatRefId": statID(statMoveMult), "Value": "0.25", "ValueInputTerms": list{}}),
		mkRow(602, "MountAbility_Sprint", st{"StatRefId": statID(statMoveMult), "Value": "GetTerm($#6137747221710071900:5$) * GetStat(GetSource(), $#5365104778484440215:109228544644729$)",
			"ValueInputTerms": list{st{"Value": "0.35"}}}),
	}
	src.rows["EffectRecordBase"] = []designdb.Row{
		mkRow(701, "Road_T3_Speed_Mount", st{"bInfiniteDuration": true, "EffectDuration": "None", "StatModsIds": list{modID(601)}}),
		mkRow(702, "MountAbility_Sprint", st{"EffectDuration": "5", "StatModsIds": list{modID(602)}}),
		mkRow(703, "Rogue_Caltrops_Spooked", st{"EffectDuration": "15", "bForceDismount": true}),
		mkRow(704, "General_Sprint", st{"bInfiniteDuration": true, "StatModsIds": list{}}),
	}
	return src
}

func mountItoa(n int64) string { return strconv.FormatInt(n, 10) }

func buildMountsOnly(t *testing.T, src *source) (*content.DB, map[string]string, error) {
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
	if err := buildMounts(w, src, &Options{Log: io.Discard}, meta); err != nil {
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

// -- Cyb . C.O.A --

func TestSyntheticMounts(t *testing.T) {
	db, meta, err := buildMountsOnly(t, syntheticMounts())
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.mount"] != "2" || meta["typed.mount_ability"] != "5" || meta["typed.mount_effect"] != "3" || meta["typed.mount_stat"] != "250" {
		t.Errorf("meta = %v", meta)
	}
	ms, err := db.Mounts()
	if err != nil || len(ms) != 2 {
		t.Fatalf("Mounts = %v, %v (a retired item, one with no mount tag: not mounts)", ms, err)
	}
	flying, horse := ms[0], ms[1]
	if flying.ItemName != "Mount_Flying_Dawnbreaker_1_Alpha" || flying.Kind != content.MountFlying || flying.Blueprint != "Mount_Flying_Dawnbreaker_1" || flying.Activation != "CastAbility" ||
		flying.CastAbilityGuid != content.GuidSummonMount || flying.Level != 25 || flying.GroundPct != 165 || flying.FlyingPct != 215 || flying.HealthFactor() != 5 || !flying.Tradeable {
		t.Errorf("flying mount = %+v", flying)
	}
	if horse.Kind != content.MountGround || horse.Blueprint != "Mount_Grounded_Horse_0" || horse.Activation != "SummonMount" || horse.GroundPct != 0 || horse.HealthPct != 100 || horse.Tradeable {
		t.Errorf("a ground mount with no text = %+v", horse)
	}

	// the stats at the owner's level: MaxHealth is the curve (the second entry doubles it), the ratings the curve over 3 hp per CON
	st25, err := db.MountStats(25)
	if err != nil {
		t.Fatal(err)
	}
	byName := map[string]float64{}
	for _, s := range st25 {
		byName[s.StatName] = s.Value
	}
	if byName["MaxHealth"] != 2*4900 || byName["HealthRegenerationRating"] != 4900/3.0*1*1.5 || byName["MitigationRating"] != 4900/3.0*2*2.0 || byName["CharacterType"] != 1.1 ||
		byName["MoveSpeedMult"] != 1 || len(st25) != 5 {
		t.Errorf("Mount_Base at owner level 25 = %v", byName)
	}
	if st1, _ := db.MountStats(1); len(st1) != 5 || st1[0].Value == 0 {
		t.Errorf("Mount_Base at owner level 1 = %v", st1)
	}
	if st51, _ := db.MountStats(51); len(st51) != 0 {
		t.Errorf("a level past the maximum has stats: %v", st51)
	}

	ab, err := db.MountAbilities()
	if err != nil || len(ab) != 5 {
		t.Fatalf("MountAbilities = %v, %v", ab, err)
	}
	roles := map[string]content.MountAbility{}
	for _, a := range ab {
		roles[a.Name] = a
	}
	if roles["Summon_Mount"].Role != content.MountRoleSummon || !roles["Summon_Mount"].UsableWhileMounted || roles["Summon_Mount"].AutoDismount ||
		roles["MountAbility_Sprint"].Role != content.MountRoleDash || roles["MountAbility_Sprint"].UsableWhileMounted ||
		roles["General_Dismount_Elevated"].Role != content.MountRoleDismountE || !roles["General_Dismount_Elevated"].AutoDismount ||
		!roles["Mage_Blink"].AutoDismount || roles["Mage_Blink"].Role != "" {
		t.Errorf("mount abilities = %+v", roles)
	}
	if _, ok := roles["Fighter_Slash"]; ok {
		t.Error("an ability with neither flag is a mount ability")
	}

	ef, err := db.MountEffects()
	if err != nil || len(ef) != 3 {
		t.Fatalf("MountEffects = %v, %v", ef, err)
	}
	effects := map[string]content.MountEffect{}
	for _, e := range ef {
		effects[e.Name] = e
	}
	road, dash, force := effects["Road_T3_Speed_Mount"], effects["MountAbility_Sprint"], effects["Rogue_Caltrops_Spooked"]
	if road.Kind != content.MountEffectRoad || road.Tier != 3 || road.Value != 0.25 || road.Relative || !road.Infinite || road.StatGuid != content.GuidMoveSpeedMult || road.Duration != 0 {
		t.Errorf("road effect = %+v", road)
	}
	if dash.Kind != content.MountEffectDash || dash.Value != 0.35 || !dash.Relative || dash.Duration != 5 || dash.Infinite || dash.Tier != -1 {
		t.Errorf("dash effect = %+v", dash)
	}
	if force.Kind != content.MountEffectForceDismount || !force.ForceDismount || force.Duration != 15 || force.StatGuid != 0 {
		t.Errorf("force-dismount effect = %+v", force)
	}
}

func TestSyntheticMountsRefuse(t *testing.T) {
	// a mount whose Blueprint is not in the mounts folder is a data error, not a row with a guessed path
	src := syntheticMounts()
	src.rows["ItemRecordBase"][0].Fields["SummonNotifyClassPath"] = "/Game/Elsewhere/Horse.Horse_C"
	if _, _, err := buildMountsOnly(t, src); err == nil {
		t.Error("a Blueprint outside GameBlueprints/Mounts was accepted")
	}
	// an equation that is not there stops the build with its Guid
	src = syntheticMounts()
	src.rows["StatEquationTypeRecordBase"] = src.rows["StatEquationTypeRecordBase"][:1]
	if _, _, err := buildMountsOnly(t, src); err == nil {
		t.Error("a list that reads a missing equation was accepted")
	}
	// a database with no mounts at all (the synthetic one of the other tests) writes empty tables
	empty := &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}
	db, meta, err := buildMountsOnly(t, empty)
	if err != nil || meta["typed.mount"] != "0" {
		t.Fatalf("no mounts: %v %v", meta, err)
	}
	if ms, _ := db.Mounts(); len(ms) != 0 {
		t.Errorf("mounts of an empty database: %v", ms)
	}
}

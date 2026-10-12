// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// The ability tables (plan D2) over a synthetic design database: the rows of syntheticSource (Sprint, Dodge, Rest and what they reach),
// completed with the fields the ability tables read, and a Bard tree with one rank that grants an ability whose closure has a hit, a
// projectile, a lingering zone, effects, stat mods with terms, an effect cost and montages.

const (
	abBase       = int64(5000000000000000000)
	abTree       = abBase + 1
	abNode       = abBase + 2
	abSkill      = abBase + 3
	abRank       = abBase + 4
	abNimble     = abBase + 10 // Bard_Dance_Nimble: its montage NetGUID is in montage_ids.tsv
	abShortbow   = abBase + 11 // WeaponCombo_Ranged_Shortbow: two ids were seen for index 1, so no id is written
	abHitSelf    = abBase + 20
	abHitProj    = abBase + 21
	abHitZone    = abBase + 22
	abHitTick    = abBase + 23
	abHitLoose   = abBase + 24 // not reached
	abEffPep     = abBase + 30
	abEffTick    = abBase + 31
	abEffLoose   = abBase + 32 // not reached
	abEffPassive = abBase + 33 // granted by a rank
	abProj       = abBase + 40
	abZone       = abBase + 41
	abTarget     = abBase + 50
	abModPep     = abBase + 60
	abModPassive = abBase + 61
	abResonance  = int64(6064632468947286527)
)

// -- Cyb . C O A --

// completeFields gives the rows of the kinds the ability tables read every field they read (the synthetic rows of the other tests
// carry only what the older tables need), without touching a value the row has.
func completeFields(src *source) {
	missing := func(rows []designdb.Row, defaults st) {
		for i := range rows {
			if rows[i].Fields == nil {
				rows[i].Fields = st{}
			}
			for k, v := range defaults {
				if _, ok := rows[i].Fields[k]; !ok {
					rows[i].Fields[k] = v
				}
			}
		}
	}
	missing(src.rows["AoCAbilityRecordBase"], st{
		"TargetActivationRequirement": "EAbilityActivationTargetRequirement::Never", "DefaultFallbackTargetType": "EDefaultFallbackTargetType::SelfOnly",
		"ApplicableTypes": int64(0), "WeaponRequirement": "EAbilityWeaponRequirement::NoRequirement", "bUsableFromHotbar": true,
		"AreaTargetRange": float32(0), "AreaTargetRadius": float32(0), "CooldownCharges": "", "DynamicCastDuration": "", "CanBreakEffects": "", "HasProc": "",
		"ModifyCooldownsIds": list{}, "ReactionsIds": list{}, "TeleportRecordsIds": list{},
		"ProjectilesIds": designdb.Pairs{}, "LingeringEffectsIds": designdb.Pairs{}, "TargetingRecordsIds": designdb.Pairs{},
	})
	for _, r := range src.rows["AoCAbilityRecordBase"] { // the montages the other tests wrote carry the soft paths only
		if ms, ok := r.Fields["AnimMontages"].(list); ok {
			for _, m := range ms {
				ms := m.(st)
				for k, v := range (st{"AbilityDirection": "EAbilityDirection::NONE", "AnimExpression": "", "HitKeys": list{}, "StartSectionName": "None", "TargetingKey": int64(0)}) {
					if _, ok := ms[k]; !ok {
						ms[k] = v
					}
				}
			}
		}
	}
	missing(src.rows["AbilityHitRecordBase"], st{
		"ApplicableTargets": int64(1), "ApplicableTypes": int64(0), "NetworkType": "EAttackNetworkType::NET_ServerVerify",
		"DefaultFallbackTargetType": "EDefaultFallbackTargetType::SelfOnly", "HitDelay": "", "ThreatToTarget": "", "MaxTargetsPerHit": "",
		"ProjectilesToFire": list{}, "ProjectilesToSpawn": list{}, "LingeringZonesIds": list{},
		"TeleportId": idv(0, 1, "None"), "ResurrectId": idv(0, 2, "None"),
		"ModifyCooldownsIds": list{}, "ModifyEffectsIds": list{}, "EffectsToRemoveIds": list{}, "ReactionsIds": list{},
		// plan E1, stage 3: what the damage rules read
		"EventTags": list{}, "ApplicableLifeStates": int64(1), "CritRate": "", "BlockRate": "", "MissRate": "",
		"bForceCantCrit": false, "bForceCantBlock": false, "bForceCantMiss": false, "bForceRollCrit": false, "bForceRollBlock": false, "bForceRollMiss": false,
		"bShouldUseItemTags": false,
	})
	missing(src.rows["EffectRecordBase"], st{
		"InPlaceStackLimit": "", "CrowdControlId": idv(0, 3, "None"), "ReactionsIds": list{}, "HitModsIds": list{}, "FragilityDefinitionsIds": list{},
	})
	missing(src.rows["StatModRecordBase"], st{"bUseShieldAbsorb": false, "ValueInputTerms": list{}})
}

func projectileID(g int64) st { return idv(g, 979581256175327151, "None") }
func zoneID(g int64) st       { return idv(g, 7793593157135267051, "None") }
func targetID(g int64) st     { return idv(g, 3999912890578843546, "None") }

// abilitySource is the synthetic source with the Bard's tree and its ability.
func abilitySource() *source {
	src := syntheticSource()
	for _, schema := range []string{"ProjectileRecordBase", "LingeringEffectRecordBase", "SkillTreeRecordBase", "SkillTreeNodeRecordBase", "SkillRecordBase",
		"SkillRankRecordBase", "AbilityTargetingRecordBase"} {
		src.typeIDs[schema] = 1
		src.rows[schema] = nil
	}
	// the Bard's class row names the tree
	for i := range src.rows["CharacterClassesRecordBase"] {
		if r := &src.rows["CharacterClassesRecordBase"][i]; r.Name == "Bard" {
			r.Fields["ActiveSkillTreeId"] = idv(abTree, 297887639323616934, "Test_SkillTable")
		}
	}
	src.rows["SkillTreeRecordBase"] = []designdb.Row{mkRow(abTree, "Test_SkillTable", st{"Skills": list{
		st{"TreeNodeId": idv(abNode, 1084214640716601260, "None"), "X": int64(0), "Y": int64(0)},
		st{"TreeNodeId": idv(0, 1084214640716601260, "None"), "X": int64(1), "Y": int64(1)}, // a null node is skipped
	}})}
	src.rows["SkillTreeNodeRecordBase"] = []designdb.Row{mkRow(abNode, "Bard_Nimble", st{
		"SkillId": idv(abSkill, 4146438674247114715, "None"), "LevelRequirement": int64(3), "PointRequirement": int64(2),
		"bAutoGranted": true, "bAutoAddToHotbar": false,
	})}
	src.rows["SkillRecordBase"] = []designdb.Row{mkRow(abSkill, "Bard_Nimble_Skill", st{"SkillRanksIds": list{
		idv(abRank, 4847957796014348780, "None"), idv(abRank+100, 4847957796014348780, "None"), // the second is no row: skipped
	}})}
	src.rows["SkillRankRecordBase"] = []designdb.Row{mkRow(abRank, "Bard_Nimble_Skill", st{
		"AbilityIdId": idv(abNimble, content.TypeAbility, "None"), "EffectIdId": idv(abEffPassive, content.TypeEffect, "None"),
	})}
	src.rows["AbilityTargetingRecordBase"] = []designdb.Row{mkRow(abTarget, "Bard_Dance_Nimble_Targeting", st{})}

	src.rows["StatModRecordBase"] = append(src.rows["StatModRecordBase"],
		mkRow(abModPep, "Bard_Pep_Mod", st{"StatRefId": statID(statMoveSp), "Value": "GetTerm($#6137747221710071900:11$) * 2.0", "ValueInputTerms": list{
			st{"TermId": idv(11, 6137747221710071900, "None"), "Value": "0.25 + Lerp(0.0, 1.0, GetAbilityProgressRatio(\"SongGroup\"))"},
			st{"TermId": idv(5, 6137747221710071900, "None"), "Value": "1.5"},
		}}),
		mkRow(abModPassive, "Bard_Passive_Mod", st{"StatRefId": statID(content.GuidMaxStamina), "Value": "10", "bUseShieldAbsorb": true}))
	src.rows["AbilityHitRecordBase"] = append(src.rows["AbilityHitRecordBase"],
		hitRow(abHitSelf, "Nimble_Self", st{"ApplicableTargets": int64(1), "ApplicationRequirements": "GetSkillPurchased(GetInstigator(), $#297887639323616934:1$, $#4146438674247114715:2$) // a comment",
			"ApplyEffects": list{applyEffect(abEffPep, "1", "", "4.0")}, "ModifyCooldownsIds": list{idv(abBase+99, 459033338675683531, "None")}}),
		hitRow(abHitProj, "Nimble_Proj_Hit", st{"ApplicableTargets": int64(2), "EffectsAppliedToSelf": list{st{"Effect": effectID(abEffTick), "MaxNumApplications": int64(2), "OnHitBehavior": "EApplyOnHitBehavior::ApplyIfHit", "StackCount": "1"}}}),
		hitRow(abHitZone, "Nimble_Zone_Hit", st{"ProjectilesToFire": list{st{"Projectile": projectileID(abProj), "ProjectileTag": st{"TagName": "ability.projectile.transient"}}}, "LingeringZonesIds": list{zoneID(abZone)}}),
		hitRow(abHitTick, "Nimble_Tick_Hit", st{"StatModsIds": list{modID(abModPep)}}),
		hitRow(abHitLoose, "Not_Reached_Hit", nil))
	src.rows["EffectRecordBase"] = append(src.rows["EffectRecordBase"],
		effectRow(abEffPep, "Bard_Pep", st{"EffectDuration": "4.0", "StatModsIds": list{modID(abModPep)}, "ApplyHitsIds": list{hitID(abHitSelf)}, "TickHitsIds": list{hitID(abHitTick)}, "TickTimer": float32(2)}),
		effectRow(abEffTick, "Bard_Tick", nil),
		effectRow(abEffLoose, "Not_Reached_Effect", nil),
		effectRow(abEffPassive, "Bard_Passive", st{"bInfiniteDuration": true, "StatModsIds": list{modID(abModPassive)}}))
	src.rows["ProjectileRecordBase"] = []designdb.Row{mkRow(abProj, "Nimble_Projectile", st{
		"ProjectileType": "EProjectileType::PROJECTILE_Homing", "Speed": float32(8000), "Range": float32(5000), "LifeTime": float32(10),
		"MaxProjectileHits": int64(1), "ChainHitCount": int64(0), "ChainRadius": float32(0), "SphereCollisionRadius": float32(16),
		"HitsIds": list{hitID(abHitProj)},
	})}
	src.rows["LingeringEffectRecordBase"] = []designdb.Row{mkRow(abZone, "Nimble_Zone", st{
		"LifeTime": float32(30), "TickRate": float32(1.5), "bUseGlobalTickRate": true, "NumberOfCharges": int64(1), "DelayTime": float32(0),
		"DelayType": "ELingeringZoneTriggerDelay::DELAY_Time", "Speed": float32(0), "ExpireAfterAppliedHits": "", "ExpireIfExpressionTrue": "",
		"LingeringHitTargetingInfoId": targetID(abTarget),
		"InitialHitsIds":              list{}, "LingeringHitsIds": list{hitID(abHitTick)}, "OnBeginOverlapHitsIds": list{hitID(abHitZone)},
		"OnEndOverlapHitsIds": list{}, "EndHitsIds": list{},
	})}
	animMontage := func(path, expr string) st {
		return st{"AbilityAnimationSoftPaths": list{path}, "AbilityDirection": "EAbilityDirection::NONE", "AnimExpression": expr, "HitKeys": list{int64(1)},
			"StartSectionName": "None", "TargetingKey": int64(0)}
	}
	nimble := abilityRow(abNimble, "Bard_Dance_Nimble", st{
		"AbilityName": txt("Nimble Dance"), "AbilityTags": list{st{"TagName": "ability.Archetype"}}, "Cooldown": "25", "CooldownCharges": "SelectInt(true, 2, 1)",
		"ApplicableTargets": int64(5), "TargetActivationRequirement": "EAbilityActivationTargetRequirement::Always",
		"Costs": list{
			st{"Enabled": "", "ChargeCost": "1", "bTriggersCooldown": true, "StatCosts": list{st{"Stat": statID(content.GuidStamina), "Value": "20 * 5.6875"}}, "EffectCosts": list{
				st{"EffectFilter": fmt.Sprintf("GetEffect() == $#3377240744094973029:%d$", abResonance), "Num": "5"},
				st{"EffectFilter": "GetEffect() != GetEffect()", "Num": ""},
			}},
			st{"Enabled": "// nothing", "ChargeCost": "", "bTriggersCooldown": false, "StatCosts": list{}, "EffectCosts": list{}},
		},
		"EffectsIds":          designdb.Pairs{{Key: int64(1), Value: effectID(abEffPep)}},
		"HitsIds":             designdb.Pairs{{Key: int64(1), Value: hitID(abHitSelf)}, {Key: int64(2), Value: hitID(abHitZone)}},
		"ProjectilesIds":      designdb.Pairs{{Key: int64(1), Value: projectileID(abProj)}},
		"LingeringEffectsIds": designdb.Pairs{{Key: int64(1), Value: zoneID(abZone)}},
		"TargetingRecordsIds": designdb.Pairs{{Key: int64(1), Value: targetID(abTarget)}},
		"ModifyCooldownsIds":  designdb.Pairs{{Key: int64(1), Value: idv(abBase+99, 459033338675683531, "None")}},
		"AnimMontages": list{animMontage("/Game/A.A", "IsApplicableTarget(GetTarget(), EAffectedTarget::TARGET_Enemy)"),
			animMontage("/Game/B.B", "")},
	})
	shortbow := abilityRow(abShortbow, "WeaponCombo_Ranged_Shortbow", st{"AnimMontages": list{animMontage("/Game/C.C", ""), animMontage("/Game/D.D", "")}})
	src.rows["AoCAbilityRecordBase"] = append(src.rows["AoCAbilityRecordBase"], nimble, shortbow)
	src.rows["SkillRankRecordBase"] = append(src.rows["SkillRankRecordBase"], mkRow(abRank+1, "Shortbow_Rank", st{
		"AbilityIdId": idv(abShortbow, content.TypeAbility, "None"), "EffectIdId": idv(0, content.TypeEffect, "None")}))
	src.rows["SkillRecordBase"][0].Fields["SkillRanksIds"] = list{idv(abRank, 1, "None"), idv(abRank+1, 1, "None"), idv(abRank+100, 1, "None")}
	completeFields(src)
	return src
}

// buildAbilitySynthetic runs buildAbilities over src into a content.db.
func buildAbilitySynthetic(t *testing.T, src *source) (*content.DB, map[string]string, error) {
	t.Helper()
	// the synthetic source has none of the captures' abilities that ability_names.tsv lists
	saved := abilityNames
	abilityNames = ""
	defer func() { abilityNames = saved }()
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
	// plan D1's buildTalents has written the ranks by now: the rows of the ranks the synthetic skill lists
	for ri, rg := range src.rows["SkillRecordBase"][0].Fields["SkillRanksIds"].(list) {
		id := rg.(st)["Guid"].(int64)
		for _, r := range src.rows["SkillRankRecordBase"] {
			if r.Guid != id {
				continue
			}
			ag, _ := newFields("rank", r.Fields).id("AbilityIdId")
			eg, _ := newFields("rank", r.Fields).id("EffectIdId")
			if _, err := tx.Exec(`INSERT INTO skill_rank (skill_guid, ord, rank_guid, name, ability_guid, ability_name, effect_guid, effect_name, effect_infinite, effect_duration)
				VALUES (?, ?, ?, ?, ?, '', ?, '', 0, 0)`, abSkill, ri, id, r.Name, ag, eg); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := buildAbilities(w, src, &Options{Log: io.Discard}, meta); err != nil {
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

// C.O.A : C | Y | B

func guids(t *testing.T, db *content.DB, q string, args ...any) []int64 {
	t.Helper()
	rows, err := db.SQL().Query(q, args...)
	if err != nil {
		t.Fatalf("%s: %v", q, err)
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var g int64
		if err := rows.Scan(&g); err != nil {
			t.Fatal(err)
		}
		out = append(out, g)
	}
	return out
}

func TestAbilityTablesOfASyntheticBard(t *testing.T) {
	db, meta, err := buildAbilitySynthetic(t, abilitySource())
	if err != nil {
		t.Fatal(err)
	}

	// the ranks are plan D1's skill_rank: the closure starts from the abilities and effects they name (the unknown rank is skipped)
	if n := scalarInt(t, db, `SELECT count(*) FROM skill_rank`); n != 2 {
		t.Errorf("skill_rank holds %d ranks, want the 2 the test wrote", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM sqlite_master WHERE name = 'ab_rank'`); n != 0 {
		t.Error("there is an ab_rank table: the ranks are plan D1's")
	}

	// the closure: the Bard's ability, the Shortbow's, and Sprint, Dodge and Rest, which no tree grants
	var names []string
	for _, g := range guids(t, db, `SELECT guid FROM ab_ability ORDER BY name`) {
		a, err := db.AbilityRow(g)
		if err != nil {
			t.Fatal(err)
		}
		names = append(names, a.Name)
	}
	if want := []string{"Bard_Dance_Nimble", "General_Dodge", "General_Rest", "General_Sprint", "WeaponCombo_Ranged_Shortbow"}; !reflect.DeepEqual(names, want) {
		t.Errorf("abilities = %v, want %v", names, want)
	}
	a, err := db.AbilityRowByName("Bard_Dance_Nimble")
	if err != nil || a.DisplayName != "Nimble Dance" || !reflect.DeepEqual(a.Tags, []string{"ability.Archetype"}) || a.ApplicableTargets != 5 || !strings.HasSuffix(a.TargetRequirement, "::Always") ||
		a.ChargesExpr == 0 || a.CooldownExpr == 0 || a.CastExpr != 0 || a.RequirementExpr != 0 || !a.UsableFromHotbar || !strings.Contains(a.Extra, fmt.Sprint(abBase+99)) {
		t.Errorf("Nimble = %+v, %v", a, err)
	}
	// the zone the ability itself lists (LingeringEffectsIds) is kept in its extra, the only link from an ability to a zone that has no hit of its own
	// (Bard_Dance_Mesmerizing's), and the zone a hit spawns in the hit's extra
	var abilityExtra struct {
		Zones []int64 `json:"LingeringEffectsIds"`
	}
	if err := json.Unmarshal([]byte(a.Extra), &abilityExtra); err != nil || !reflect.DeepEqual(abilityExtra.Zones, []int64{abZone}) {
		t.Errorf("Nimble's extra = %s (%v): its LingeringEffectsIds are the zone %d", a.Extra, err, abZone)
	}
	zoneHit, err := db.HitRow(abHitZone)
	var hitExtra struct {
		Zones []int64 `json:"LingeringZonesIds"`
	}
	if err != nil || json.Unmarshal([]byte(zoneHit.Extra), &hitExtra) != nil || !reflect.DeepEqual(hitExtra.Zones, []int64{abZone}) {
		t.Errorf("the zone hit's extra = %s (%v): it spawns the zone %d", zoneHit.Extra, err, abZone)
	}
	// the expressions: a text once however often it appears; none for an empty or comment-only text
	cd, _ := db.Expr(a.CooldownExpr)
	ch, _ := db.Expr(a.ChargesExpr)
	if cd.Text != "25" || ch.Text != "SelectInt(true, 2, 1)" || cd.Program != -1 || cd.Error != "" {
		t.Errorf("cooldown %+v, charges %+v", cd, ch)
	}
	costs, err := db.AbilityCosts(abNimble)
	if err != nil || len(costs) != 2 || costs[0].StatName != "Stamina" || !costs[0].TriggersCooldown || costs[0].EnabledExpr != 0 || costs[0].ChargeCostExpr == 0 ||
		costs[1].StatOrd != -1 || costs[1].EnabledExpr != 0 || costs[1].ChargeCostExpr != 0 || costs[1].AmountExpr != 0 {
		t.Fatalf("costs = %+v, %v", costs, err)
	}
	if amount, _ := db.Expr(costs[0].AmountExpr); amount.Text != "20 * 5.6875" {
		t.Errorf("cost amount = %+v", amount)
	}
	ecs, err := db.AbilityEffectCosts(abNimble)
	if err != nil || len(ecs) != 2 || ecs[0].EffectGuid != abResonance || ecs[0].NumExpr == 0 || ecs[1].EffectGuid != 0 || ecs[1].NumExpr != 0 {
		t.Errorf("effect costs = %+v, %v", ecs, err)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM ab_expr WHERE text = '25'`); n != 1 {
		t.Errorf("the text 25 is in %d rows", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM ab_expr WHERE trim(text) = '' OR text LIKE '%nothing%'`); n != 0 {
		t.Errorf("%d expressions of nothing", n)
	}

	// targeting, montages with the NetGUID of the capture where one id was seen
	var tn string
	if err := db.SQL().QueryRow(`SELECT targeting_name FROM ab_targeting WHERE ability_guid = ?`, abNimble).Scan(&tn); err != nil || tn != "Bard_Dance_Nimble_Targeting" {
		t.Errorf("targeting name = %q, %v", tn, err)
	}
	ms, err := db.AbilityMontages(abNimble)
	if err != nil || len(ms) != 2 {
		t.Fatalf("montages = %+v, %v", ms, err)
	}
	if ms[0].NetGuid == nil || *ms[0].NetGuid != 17062721381084758155 || ms[0].SoftPath != "/Game/A.A" || ms[0].SelectExpr == 0 || !reflect.DeepEqual(ms[0].HitKeys, []int64{1}) {
		t.Errorf("montage 0 = %+v", ms[0])
	}
	if ms[1].NetGuid != nil || ms[1].SelectExpr != 0 {
		t.Errorf("montage 1 = %+v (no capture names index 1)", ms[1])
	}
	sm, err := db.AbilityMontages(abShortbow)
	if err != nil || len(sm) != 2 || sm[0].NetGuid == nil || *sm[0].NetGuid != 2427103063386065711 || sm[1].NetGuid != nil {
		t.Errorf("Shortbow montages = %+v, %v (index 0 is named by a capture, the capture names two ids for index 1: none is written)", sm, err)
	}

	// hits: directly, through the projectile and through the zone; the one nothing reaches is not in
	ah, err := db.AbilityHits(abNimble)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, h := range ah {
		got = append(got, fmt.Sprintf("%s/%d/%d/%s/%d/%d", h.Source, h.Key, h.OwnerGuid-abBase, h.Role, h.Ord, h.HitGuid-abBase))
	}
	want := []string{"hit/1/-8000000000000000000//0/20", "hit/2/-8000000000000000000//0/22", "projectile/1/40/HitsIds/0/21", "zone/1/41/LingeringHitsIds/0/23", "zone/1/41/OnBeginOverlapHitsIds/0/22"}
	for i := range want {
		want[i] = strings.ReplaceAll(want[i], "-8000000000000000000", "-"+fmt.Sprint(abBase))
	}
	sort.Strings(got)
	sort.Strings(want)
	if !reflect.DeepEqual(got, want) {
		t.Errorf("ability hits = %v\nwant %v", got, want)
	}
	if hs := guids(t, db, `SELECT guid - ? FROM ab_hit WHERE guid BETWEEN ? AND ? + 1000 ORDER BY guid`, abBase, abBase, abBase); !reflect.DeepEqual(hs, []int64{20, 21, 22, 23}) {
		t.Errorf("hits = %v: Not_Reached_Hit must not be in", hs)
	}
	if es := guids(t, db, `SELECT guid - ? FROM ab_effect WHERE guid BETWEEN ? AND ? + 1000 ORDER BY guid`, abBase, abBase, abBase); !reflect.DeepEqual(es, []int64{30, 31, 33}) {
		t.Errorf("effects = %v: the passive the rank grants is in, Not_Reached_Effect is not", es)
	}
	h, err := db.HitRow(abHitSelf)
	if err != nil || h.ApplicableTargets != 1 || h.RequirementExpr == 0 || !strings.Contains(h.Extra, fmt.Sprint(abBase+99)) || h.NetType != "EAttackNetworkType::NET_ServerVerify" {
		t.Errorf("hit = %+v, %v", h, err)
	}
	if rq, _ := db.Expr(h.RequirementExpr); strings.Contains(rq.Text, "comment") == false || rq.Program != -1 {
		t.Errorf("hit requirement = %+v (the text is kept as written)", rq)
	}
	hes, err := db.HitEffects(abHitSelf)
	if err != nil || len(hes) != 1 || hes[0].Kind != "apply" || hes[0].EffectGuid != abEffPep || hes[0].StackCountExpr == 0 || hes[0].DurationExpr == 0 {
		t.Errorf("hit effects = %+v, %v", hes, err)
	}
	hes, err = db.HitEffects(abHitProj)
	if err != nil || len(hes) != 1 || hes[0].Kind != "self" || hes[0].MaxApplications != 2 || hes[0].OnHitBehavior != "EApplyOnHitBehavior::ApplyIfHit" {
		t.Errorf("self effects = %+v, %v", hes, err)
	}
	p, err := db.Projectile(abProj)
	if err != nil || p.Speed != 8000 || p.SphereRadius != 16 || !reflect.DeepEqual(p.Hits, []int64{abHitProj}) {
		t.Errorf("projectile = %+v, %v", p, err)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM ab_hit_projectile WHERE hit_guid = ? AND kind = 'fire' AND projectile_guid = ?`, abHitZone, abProj); n != 1 {
		t.Errorf("the hit fires the projectile %d times", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM ab_hit_zone WHERE hit_guid = ? AND zone_guid = ?`, abHitZone, abZone); n != 1 {
		t.Errorf("the hit spawns the zone %d times", n)
	}
	z, err := db.Zone(abZone)
	if err != nil || z.TickRate != 1.5 || !z.UseGlobalTickRate || z.TargetingGuid != abTarget ||
		!reflect.DeepEqual(z.Hits, map[string][]int64{"LingeringHitsIds": {abHitTick}, "OnBeginOverlapHitsIds": {abHitZone}}) {
		t.Errorf("zone = %+v, %v", z, err)
	}

	// effects and stat mods; the terms of a StatMod are in the value's expression
	e, err := db.EffectRowByName("Bard_Pep")
	if err != nil || e.DurationExpr == 0 || e.TickTimer != 2 || e.InfiniteDuration || e.StackType != "EEffectStackingType::STACK_OverrideAlways" {
		t.Errorf("Bard_Pep = %+v, %v", e, err)
	}
	eh, err := db.EffectHits(abEffPep)
	if err != nil || len(eh) != 2 || eh[0].Phase != "Apply" || eh[0].HitGuid != abHitSelf || eh[1].Phase != "Tick" {
		t.Errorf("effect hits = %+v, %v", eh, err)
	}
	mods, err := db.Mods("effect", abEffPep)
	if err != nil || len(mods) != 1 || mods[0].StatName != "MoveSpeedMult" || mods[0].ShieldAbsorb {
		t.Fatalf("mods = %+v, %v", mods, err)
	}
	ve, err := db.Expr(mods[0].ValueExpr)
	if err != nil || ve.Text != "GetTerm($#6137747221710071900:11$) * 2.0" || len(ve.Terms) != 2 || ve.Terms[5] != "1.5" || !strings.Contains(ve.Terms[11], "GetAbilityProgressRatio") {
		t.Errorf("mod value = %+v, %v", ve, err)
	}
	pm, err := db.Mods("effect", abEffPassive)
	if err != nil || len(pm) != 1 || pm[0].StatName != "MaxStamina" || !pm[0].ShieldAbsorb {
		t.Errorf("passive mods = %+v, %v", pm, err)
	}
	if hm, err := db.Mods("hit", abHitTick); err != nil || len(hm) != 1 || hm[0].ValueExpr != mods[0].ValueExpr {
		t.Errorf("hit mods = %+v, %v (the same text and terms are one expression)", hm, err)
	}

	// the abilities of plan A7 are in, and the Sprint's drain
	sprint, err := db.AbilityRow(content.GuidSprint)
	if err != nil || sprint.Name != "General_Sprint" || sprint.HeldBehavior != "EHeldAbilityPressBehavior::Smart" {
		t.Errorf("Sprint = %+v, %v", sprint, err)
	}
	if _, err := db.EffectRow(content.GuidSprintStaminaDrain); err != nil {
		t.Errorf("the Sprint's drain: %v", err)
	}

	// meta and counts
	if meta["ab.abilities"] != "5" || meta["ab.effects"] == "" || meta["ab.exprs"] != fmt.Sprint(scalarInt(t, db, `SELECT count(*) FROM ab_expr`)) {
		t.Errorf("meta = %v", meta)
	}
}

// (CoA ~ C | Y | B)

// Two builds of the same rows give the same tables, expression ids included.
func TestAbilityTablesAreDeterministic(t *testing.T) {
	dump := func() string {
		db, _, err := buildAbilitySynthetic(t, abilitySource())
		if err != nil {
			t.Fatal(err)
		}
		var sb strings.Builder
		for _, table := range []string{"ab_expr", "ab_ability", "ab_cost", "ab_hit", "ab_hit_effect", "ab_effect", "ab_mod", "ab_montage", "ab_ability_hit", "ab_zone_hit"} {
			rows, err := db.SQL().Query(`SELECT * FROM ` + table)
			if err != nil {
				t.Fatal(err)
			}
			cols, _ := rows.Columns()
			for rows.Next() {
				vals := make([]any, len(cols))
				ptrs := make([]any, len(cols))
				for i := range vals {
					ptrs[i] = &vals[i]
				}
				if err := rows.Scan(ptrs...); err != nil {
					t.Fatal(err)
				}
				fmt.Fprintln(&sb, table, vals)
			}
			rows.Close()
		}
		return sb.String()
	}
	a, b := dump(), dump()
	if a != b || len(a) < 1000 {
		t.Errorf("two builds differ (or are empty: %d bytes)", len(a))
	}
}

// What the walk refuses: a reference to a record that is no row.
func TestAbilityTablesRefuseDanglingReferences(t *testing.T) {
	for name, tc := range map[string]struct {
		edit func(src *source)
		want string
	}{
		"a hit applies an effect that is no row": {func(src *source) {
			src.rows["AbilityHitRecordBase"][len(src.rows["AbilityHitRecordBase"])-5].Fields["ApplyEffects"] = list{applyEffect(abBase+777, "", "", "")}
		}, "no Effect row"},
		"an ability fires a hit that is no row": {func(src *source) {
			for i := range src.rows["AoCAbilityRecordBase"] {
				if r := &src.rows["AoCAbilityRecordBase"][i]; r.Guid == abNimble {
					r.Fields["HitsIds"] = designdb.Pairs{{Key: int64(1), Value: hitID(abBase + 778)}}
				}
			}
		}, "no AbilityHit row"},
		"an ability fires a projectile that is no row": {func(src *source) {
			for i := range src.rows["AoCAbilityRecordBase"] {
				if r := &src.rows["AoCAbilityRecordBase"][i]; r.Guid == abNimble {
					r.Fields["ProjectilesIds"] = designdb.Pairs{{Key: int64(1), Value: projectileID(abBase + 779)}}
				}
			}
		}, "no Projectile row"},
		"an ability has a zone that is no row": {func(src *source) {
			for i := range src.rows["AoCAbilityRecordBase"] {
				if r := &src.rows["AoCAbilityRecordBase"][i]; r.Guid == abNimble {
					r.Fields["LingeringEffectsIds"] = designdb.Pairs{{Key: int64(1), Value: zoneID(abBase + 780)}}
				}
			}
		}, "no LingeringEffect row"},
		"an effect lists a stat mod that is no row": {func(src *source) {
			for i := range src.rows["EffectRecordBase"] {
				if r := &src.rows["EffectRecordBase"][i]; r.Guid == abEffTick {
					r.Fields["StatModsIds"] = list{modID(abBase + 781)}
				}
			}
		}, "which is not a row"},
		"a mod names a stat that is no row": {func(src *source) {
			for i := range src.rows["StatModRecordBase"] {
				if r := &src.rows["StatModRecordBase"][i]; r.Guid == abModPassive {
					r.Fields["StatRefId"] = statID(abBase + 782)
				}
			}
		}, "not a StatTypeDef row"},
		"a field is missing": {func(src *source) {
			for i := range src.rows["AoCAbilityRecordBase"] {
				if r := &src.rows["AoCAbilityRecordBase"][i]; r.Guid == abNimble {
					delete(r.Fields, "TargetActivationRequirement")
				}
			}
		}, "TargetActivationRequirement"},
		"the Sprint is missing": {func(src *source) {
			rows := src.rows["AoCAbilityRecordBase"][:0]
			for _, r := range src.rows["AoCAbilityRecordBase"] {
				if r.Guid != content.GuidSprint {
					rows = append(rows, r)
				}
			}
			src.rows["AoCAbilityRecordBase"] = rows
		}, "no Ability row"},
		"a table is missing": {func(src *source) { delete(src.rows, "ProjectileRecordBase") }, "ProjectileRecordBase"},
	} {
		src := abilitySource()
		tc.edit(src)
		_, _, err := buildAbilitySynthetic(t, src)
		if err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s: err = %v, want one with %q", name, err, tc.want)
		}
	}
}

func TestMontageNetGuidsOfTheCaptures(t *testing.T) {
	m, err := montageNetGuids()
	if err != nil {
		t.Fatal(err)
	}
	if m["Bard_Misc_QuickWit_Jest"][0] != 16448494777319543161 || m["Bard_Music_Epic"][2] != 8609607387380755675 || m["Bard_Music_Menacing"][2] != 8609607387380755675 {
		t.Errorf("Jest %v, Epic %v, Menacing %v", m["Bard_Misc_QuickWit_Jest"], m["Bard_Music_Epic"], m["Bard_Music_Menacing"])
	}
	if _, ok := m["WeaponCombo_Ranged_Shortbow"][1]; ok {
		t.Error("an index with two ids seen has one written")
	}
	if len(m) < 60 {
		t.Errorf("%d abilities in montage_ids.tsv", len(m))
	}
}

func TestSeedByName(t *testing.T) {
	saved := abilityNames
	defer func() { abilityNames = saved }()
	rows := map[int64]*designdb.Row{1: {Name: "General_Guard"}, 2: {Name: "General_Guard"}, 3: {Name: "Other"}}
	abilityNames = "# a comment\nGeneral_Guard\tgeneral\n"
	seed := map[int64]bool{}
	n, err := seedByName(rows, seed)
	if err != nil || n != 2 || !seed[1] || !seed[2] || seed[3] {
		t.Errorf("seedByName = %d, %v, seed %v: want both rows of the name", n, err, seed)
	}
	abilityNames = "Missing_Ability\tgeneral\n"
	if _, err := seedByName(rows, map[int64]bool{}); err == nil || !strings.Contains(err.Error(), "no Ability row named") {
		t.Errorf("a name no row has: err = %v, want the stale-list error", err)
	}
	abilityNames = "General_Guard\n"
	if _, err := seedByName(rows, map[int64]bool{}); err == nil || !strings.Contains(err.Error(), "columns") {
		t.Errorf("a line with one column: err = %v", err)
	}
}

// CoA + C_Y_B

func TestExprSetSharesTextAndTerms(t *testing.T) {
	db, _, err := buildAbilitySynthetic(t, abilitySource())
	if err != nil {
		t.Fatal(err)
	}
	// the same text with other terms is another expression: GetTerm of the same name in two mods
	if n := scalarInt(t, db, `SELECT count(*) FROM ab_expr WHERE text = 'GetTerm($#6137747221710071900:11$) * 2.0'`); n != 1 {
		t.Errorf("%d rows for the one StatMod text", n)
	}
	var terms string
	if err := db.SQL().QueryRow(`SELECT terms FROM ab_expr WHERE text LIKE 'GetTerm%'`).Scan(&terms); err != nil || !strings.HasPrefix(terms, `{"11":`) || !strings.Contains(terms, `"5":"1.5"`) {
		t.Errorf("terms = %s, %v (JSON, sorted by key as text)", terms, err)
	}
	// 0 for nothing
	e := &exprSet{ids: map[string]int64{}}
	for _, text := range []string{"", "  \r\n", "// only a comment", "/* block */ \n // and a line"} {
		id, err := e.id(text, nil)
		if err != nil || id != 0 {
			t.Errorf("id(%q) = %d, %v; want 0", text, id, err)
		}
	}
}

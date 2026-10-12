// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"database/sql"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// A synthetic design database: rows built by hand in the shape designdb
// decodes them to, with the real formulas of Sprint, Dodge and
// StaminaRegeneration, so the typed tables are tested without the 144 MB file.
// Where the real values differ (the curves) the numbers are the test's own.

type (
	st   = designdb.Struct
	list = []designdb.Value
)

func idv(guid, typeID int64, name string) st {
	return st{"Guid": guid, "TypeId": typeID, "Name": name}
}

func effectID(guid int64) st { return idv(guid, content.TypeEffect, "None") }
func hitID(guid int64) st    { return idv(guid, content.TypeAbilityHit, "None") }
func modID(guid int64) st    { return idv(guid, content.TypeStatMod, "None") }
func statID(guid int64) st   { return idv(guid, content.TypeStatDef, "None") }

func txt(source string) designdb.Text { return designdb.Text{Source: source} }

// crlf restores the CRLF line ends the design database keeps in formulas.
func crlf(s string) string { return strings.ReplaceAll(s, "\n", "\r\n") }

func mkRow(guid int64, name string, f st) designdb.Row {
	return designdb.Row{Guid: guid, Name: name, Fields: f}
}

// merge returns base with over's entries on top.
func merge(base, over st) st {
	out := st{}
	for k, v := range base {
		out[k] = v
	}
	for k, v := range over {
		out[k] = v
	}
	return out
}

func statRow(guid int64, name string, rep int64, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"GameplayType": "EStatGameplayType::Type_Combat", "StatType": "EStatRecordType::Type_Float", "Replication": rep,
		"base": "", "ValueMin": "0", "ValueMax": "", "RegenRate": "",
		"OnSpawn": "EStatResetBehavior::Reset_None", "OnDeath": "EStatResetBehavior::Reset_None", "OnRevive": "EStatResetBehavior::Reset_None",
		"bPersistValue": false, "bHidden": false, "bClampStatFinalValue": false, "bMultiplyStatMods": false,
		"bRegenWhileDead": false, "bRecalculateOnCombatState": false,
	}, f))
}

func effectRow(guid int64, name string, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"EffectName": txt(name), "EffectCategory": "EEffectCategory::EFFECT_Hidden", "Tags": list{}, "EffectTags": list{},
		"ApplicationRequirements": "", "bInfiniteDuration": false, "EffectDuration": "", "TickTimer": float32(0),
		"bIgnoreInitialTick": false, "EffectStackType": "EEffectStackingType::STACK_OverrideAlways", "GlobalStackLimit": int64(50),
		"StackDurationRefresh": "EEffectStackDurationType::STACK_AlwaysRefresh", "bPersistsThroughDeath": false,
		"bPersistsRelog": false, "bDispellable": false, "bSignificant": false,
		"PreApplyHitsIds": list{}, "ApplyHitsIds": list{}, "TickHitsIds": list{}, "ConsumeHitsIds": list{},
		"RemoveHitsIds": list{}, "ExpireHitsIds": list{}, "EndHitsIds": list{}, "StatModsIds": list{},
	}, f))
}

// == CoA | C:Y:B ==

func hitRow(guid int64, name string, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"ApplicationRequirements": "", "ApplyEffects": list{}, "EffectsAppliedToSelf": list{}, "StatModsIds": list{},
		"OnHitFXId": st{"Guid": int64(0), "Name": "None"}, "OnActivateFXId": st{"Guid": int64(0), "Name": "None"}, "MaxNumEffectsToRemove": int64(0),
	}, f))
}

func applyEffect(guid int64, stack, requirement, duration string) st {
	return st{"StackCount": stack, "EffectId": effectID(guid), "EffectApplicationRequirement": requirement, "EffectDuration": duration}
}

func abilityRow(guid int64, name string, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"AbilityTags": list{}, "AbilityName": txt(name), "ActivationRequirements": "", "Cooldown": "0",
		"bDisableDuringCooldown": false, "SharedCooldownTag": "None", "HeldAbilityBehavior": "EHeldAbilityPressBehavior::Naive",
		"bOverrideHeldBehavior": false, "bHoldAnimation": false, "MaximumHoldTime": float32(0),
		"ComboInputs": list{}, "AbilityActivationInput": "EAbilityActivationInput::UserConfigurable",
		"bUsableInAir": true, "bUsableWhileSwimming": false, "bUsableWhileWalking": true, "bUsableWhileMounted": false,
		"NetworkingMaxSpeed": float32(0), "Costs": list{}, "EffectsIds": designdb.Pairs{}, "HitsIds": designdb.Pairs{},
		"ApplicableTargets": int64(0), "CancelSectionName": "None", "AnimMontages": list{},
	}, f))
}

// Real formulas (see package aocexpr's tests for their provenance).
var (
	regenExpr = crlf(`const disableRegen = (GetEffectCount(GetSource(), $#3377240744094973029:6064631641826394197$) > 0 && IsMoving(GetSource())) ||
GetEffectCount(GetSource(), $#3377240744094973029:6064631667969556480$) > 0 ||
GetEffectCount(GetSource(), $#3377240744094973029:106223481149050$) > 0 ||
GetEffectCount(GetSource(), $#3377240744094973029:6064631799761208354$) > 0 ||
GetEffectCount(GetSource(), $#3377240744094973029:6064631668308443138$) > 0;

SelectFloat(disableRegen, 0.0, 5.0)`)
	sprintCostExpr = crlf(`var inCombat = IsInCombat(GetSource());
var Cost = 1.25;

if (inCombat)
{
set Cost = 2.5;
}

Cost`)
	dodgeCostExpr = crlf(`var snareStacks = GetEffectCount(GetInstigator(), $#3377240744094973029:6064629784692426711$);
var chilledStacks = GetEffectCount(GetInstigator(), $#3377240744094973029:6064628906043703296$);
var chilled = SelectInt(chilledStacks > 0, 2, 0);
var mult = 1 + (0.25 * Min((snareStacks + chilled),3));

SelectInt(GetEffectCount(GetInstigator(), $#3377240744094973029:6064632660379712165$) > 0 &&
GetSkillPurchased(GetOwner(),$#297887639323616934:6064630422842835238$,$#4146438674247114715:6064632892734570925$),
30,
50) * mult`)
	drainExpr = crlf(`var inCombat = IsInCombat(GetSource());
var isMoving = IsMoving(GetSource());
var Cost = 0.0;

if(isMoving)
{
set Cost = -1.25;

if (inCombat)
{
set Cost = -2.5;
}

}

Cost`)
	restHealthExpr = crlf(`const maxHp = GetStat(GetInstigator(), $#5365104778484440215:108986356618873$);
maxHp * 0.01 + 1`)
	restManaExpr = crlf(`const maxMp = GetStat(GetInstigator(), $#5365104778484440215:109183578756729$);
maxMp * 0.01 + 1`)
	dodgeCooldownExpr = crlf(`SelectFloat(GetEffectCount(GetInstigator(), $#3377240744094973029:6064632660379712165$) > 0 &&
GetSkillPurchased(GetOwner(),$#297887639323616934:6064630422842835238$,$#4146438674247114715:6064632892734570925$),
0.4,
0.65)`)
)

const (
	effSprint   = 6064631641826394197
	effGuard    = 6064631667969556480
	effEvasion  = 106223481149050
	effStun     = 6064631799761208354
	effExertion = 6064631668308443138
	effSkill    = 6064632660379712165 // Rogue_Advantage
	effTracker  = 6064634618775274433
	effTick     = 7000000000000000001 // applied by a hit of the Sprint effect
	effBeyond   = 7000000000000000002 // applied by a hit of effTick: one step too far
	effDodgeHit = 7000000000000000003 // applied by a hit the Dodge ability lists itself
	effRest     = 109394092054139     // General_Rest
	effDrain    = content.GuidSprintStaminaDrain

	hitTick       = 8000000000000000001
	hitEnd        = 8000000000000000002
	hitDodge      = 8000000000000000003
	hitExert      = 8000000000000000004
	hitDrain      = 8000000000000000005 // the drain effect's tick hit, with the stat mod
	hitSprintEnd  = 8000000000000000006 // the Sprint effect's End hit: applies the exertion effect
	hitEvasionEnd = 8000000000000000007

	modSprint     = 9000000000000000001
	modHit        = 9000000000000000002
	modDrain      = 9000000000000000003
	modRestHealth = 109394190095997
	modRestMana   = 109394393781881
	statMoveSp    = 109228544644729
	statMaxHealth = 108986356618873
	statMaxMana   = 109183578756729
	statHealthReg = 109343399171806
	statManaReg   = 109343399171796
)

// attributeCurve is the guid of the curve of attribute i in the synthetic data.
func attributeCurve(i int) int64 { return int64(5000 + i) }

func formula(curve int64, mult string) string {
	return fmt.Sprintf("const v = EvalCurve($#7037248826340794574:%d$, Float(GetCharacterLevel(GetOwner())));\r\nRound(v * %s)", curve, mult)
}

// ~ C | O | A :: Cyb ~

// syntheticSource returns the rows buildTyped needs.
func syntheticSource() *source {
	src := &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}
	for schema, id := range recordTypes {
		src.typeIDs[schema] = id
		src.rows[schema] = nil
	}

	stats := []designdb.Row{
		statRow(content.GuidStamina, "Stamina", 1, st{"ValueMax": "GetStat(GetInstigator(), $#5365104778484440215:6064631747701440948$)",
			"RegenRate": "GetStat(GetSource(), $#5365104778484440215:6064631663405498419$)", "OnSpawn": "EStatResetBehavior::Reset_SetMax"}),
		statRow(content.GuidMaxStamina, "MaxStamina", 1, st{"ValueMin": "1"}),
		statRow(content.GuidStaminaRegeneration, "StaminaRegeneration", 1, st{"base": regenExpr}),
		statRow(statMoveSp, "MoveSpeedMult", 7, nil),
	}
	for _, a := range content.PrimaryAttributes {
		stats = append(stats, statRow(a.Guid, a.Name, 1, nil))
	}
	stats = append(stats, statRow(statMaxHealth, "MaxHealth", 7, nil), statRow(statMaxMana, "MaxMana", 7, nil),
		statRow(statHealthReg, "HealthRegeneration", 1, nil), statRow(statManaReg, "ManaRegeneration", 1, nil))
	src.rows["StatTypeDefRecordBase"] = stats

	// One curve per attribute: 100 at every level except level 20, which is 335.
	var curves []designdb.Row
	for i := range content.PrimaryAttributes {
		var keys list
		for level := 1; level <= 50; level++ {
			v := float32(100)
			if level == 20 {
				v = 335
			}
			keys = append(keys, st{"Time": float32(level), "Value": v, "InterpMode": "ERichCurveInterpMode::RCIM_Linear"})
		}
		curves = append(curves, mkRow(attributeCurve(i), fmt.Sprintf("Attrib_%d", i),
			st{"Curve": st{"ExternalCurve": "", "EditorCurveData": st{"DefaultValue": float32(0), "Keys": keys}}}))
	}
	curves = append(curves, mkRow(5100, "Empty", st{"Curve": st{"ExternalCurve": "/Game/Curves/X.X",
		"EditorCurveData": st{"DefaultValue": float32(3.4028235e+38), "Keys": list{}}}}))
	src.rows["StatCurveRecordBase"] = curves

	entry := func(stat int64, expr string) st { return st{"StatId": statID(stat), "ValueExpression": expr} }
	inits := []designdb.Row{mkRow(content.GuidPlayerBase, "Player_Base", st{"StatIDs": list{
		entry(content.GuidMaxStamina, "100"), entry(content.GuidStamina, "100"), entry(content.GuidStaminaRegeneration, ""),
	}})}
	var classes []designdb.Row
	for ci, name := range content.PlayerClasses {
		var entries list
		for i, a := range content.PrimaryAttributes {
			entries = append(entries, entry(a.Guid, formula(attributeCurve(i), "1.3")))
		}
		inits = append(inits, mkRow(int64(6000+ci), "Player_Base_"+name, st{"StatIDs": entries}))
		for _, n := range []string{name, name + "Stage"} {
			classes = append(classes, mkRow(int64(100+len(classes)), n, st{
				"Class": "ECharacterClass::" + n, "LevelTypeId": idv(1, 2, "Adventuring"), "ActiveSkillTreeId": idv(3, 4, n+"_SkillTable"),
				"bPrefersMelee": ci%2 == 0,
			}))
		}
	}
	src.rows["StatInitializerListRecordBase"] = inits
	src.rows["CharacterClassesRecordBase"] = classes

	src.rows["StatModRecordBase"] = []designdb.Row{
		mkRow(modSprint, "General_Sprint", st{"StatRefId": statID(statMoveSp), "Value": "0.2"}),
		mkRow(modHit, "DisableRegen_ApplyHit", st{"StatRefId": statID(content.GuidStamina), "Value": "0.001"}),
		mkRow(modDrain, "General_Sprint_StaminaDrain", st{"StatRefId": statID(content.GuidStamina), "Value": drainExpr}),
		mkRow(modRestHealth, "General_Rest_HealthRegen", st{"StatRefId": statID(statHealthReg), "Value": restHealthExpr}),
		mkRow(modRestMana, "General_Rest_ManaRegen", st{"StatRefId": statID(statManaReg), "Value": restManaExpr}),
	}

	src.rows["AbilityHitRecordBase"] = []designdb.Row{
		hitRow(hitTick, "Sprint_Tick_Hit", st{"ApplicationRequirements": "IsMoving(GetInstigator())",
			"ApplyEffects": list{applyEffect(effTick, "", "", "")}}),
		hitRow(hitEnd, "Tick_End_Hit", st{"ApplyEffects": list{applyEffect(effBeyond, "1", "x > 1", "2")},
			"EffectsAppliedToSelf": list{st{"Effect": effectID(effExertion), "MaxNumApplications": int64(1), "OnHitBehavior": "EApplyOnHitBehavior::ApplyIfHit", "StackCount": "3"}},
			"StatModsIds":          list{modID(modHit)}}),
		hitRow(hitDodge, "Dodge_Hit", st{"ApplyEffects": list{applyEffect(effDodgeHit, "", "", "")}}),
		hitRow(hitExert, "Exertion_Hit", nil),
		hitRow(hitDrain, "General_Sprint_StaminaDrain", st{"StatModsIds": list{modID(modDrain)}}),
		hitRow(hitSprintEnd, "General_Stamina_Sprint_EndHit", st{"ApplyEffects": list{applyEffect(effExertion, "", "", "")}}),
		hitRow(hitEvasionEnd, "General_Stamina_Dodge_EndHit", st{"ApplyEffects": list{applyEffect(effExertion, "", "", "")}}),
	}

	src.rows["EffectRecordBase"] = []designdb.Row{
		effectRow(effSprint, "General_Sprint", st{"EffectName": txt("Sprint"), "EffectCategory": "EEffectCategory::EFFECT_Buff",
			"Tags": list{"Effect.DisableStamRegen"}, "EffectTags": list{st{"TagName": "Effect.DisableStamRegen"}},
			"bInfiniteDuration": true, "TickTimer": float32(1), "bIgnoreInitialTick": true,
			"TickHitsIds": list{hitID(hitTick)}, "EndHitsIds": list{hitID(hitSprintEnd)}, "StatModsIds": list{modID(modSprint)}}),
		effectRow(effGuard, "General_Guard", nil),
		effectRow(effEvasion, "General_Dodge_Evasion", st{"EffectDuration": "0.5", "EndHitsIds": list{hitID(hitEvasionEnd)}}),
		effectRow(effStun, "General_Guard_GuardBreak_Stun", nil),
		effectRow(effExertion, "General_Stamina_DisableRegen", st{"EffectDuration": "1", "ApplyHitsIds": list{hitID(hitExert)}}),
		effectRow(effSkill, "Rogue_Advantage", nil),
		effectRow(effTracker, "General_Stamina_Dodge_EvasiveManeuver_Tracker", st{"EffectDuration": "0.5"}),
		effectRow(effTick, "Sprint_Tick_Effect", st{"EndHitsIds": list{hitID(hitEnd)}}),
		effectRow(effBeyond, "Beyond_Effect", st{"TickHitsIds": list{hitID(hitExert)}}), // not in the set
		effectRow(effDodgeHit, "Dodge_Hit_Effect", nil),
		effectRow(7000000000000000099, "Unrelated_Effect", st{"TickHitsIds": list{hitID(hitExert)}}),
		effectRow(effRest, "General_Rest", st{"bInfiniteDuration": true, "EffectDuration": "360", "StatModsIds": list{modID(modRestHealth), modID(modRestMana)}}),
		effectRow(effDrain, "General_Sprint_StaminaDrain", st{"bInfiniteDuration": true, "TickTimer": float32(0.25), "bIgnoreInitialTick": true,
			"TickHitsIds": list{hitID(hitDrain)}}),
	}

	sprintCost := st{"Enabled": "IsMoving(GetOwner())", "ChargeCost": "0", "bTriggersCooldown": false,
		"StatCosts": list{st{"Stat": statID(content.GuidStamina), "Value": sprintCostExpr}}, "EffectCosts": list{}}
	dodgeCost := st{"Enabled": "", "ChargeCost": "0", "bTriggersCooldown": true,
		"StatCosts": list{st{"Stat": statID(content.GuidStamina), "Value": dodgeCostExpr}}, "EffectCosts": list{}}
	src.rows["AoCAbilityRecordBase"] = []designdb.Row{
		abilityRow(1, "Some_Other_Ability", nil),
		abilityRow(content.GuidSprint, "General_Sprint", st{
			"AbilityName": txt("Sprint"), "ActivationRequirements": "GetStat(GetSource(), $#5365104778484440215:6064629575339934476$) >= 1.0",
			"HeldAbilityBehavior": "EHeldAbilityPressBehavior::Smart", "bOverrideHeldBehavior": true, "bHoldAnimation": true,
			"ComboInputs": list{"EComboButtons::BUTTON_Sprint"}, "Costs": list{sprintCost, st{"Enabled": "", "ChargeCost": "1",
				"bTriggersCooldown": true, "StatCosts": list{}, "EffectCosts": list{}}},
			"EffectsIds": designdb.Pairs{{Key: int64(1), Value: effectID(effSprint)}}, "CancelSectionName": "End",
			"AnimMontages": list{st{"AbilityAnimationSoftPaths": list{"/Game/Characters/Animations/00_ABLT/CM_M_ABLT_Sprint_Montage.CM_M_ABLT_Sprint_Montage"}}},
		}),
		abilityRow(content.GuidDodge, "General_Dodge", st{
			"AbilityName": txt("Dodge"), "AbilityTags": list{st{"TagName": "ability.dodge"}}, "Cooldown": dodgeCooldownExpr,
			"bDisableDuringCooldown": true, "ComboInputs": list{"EComboButtons::BUTTON_Dodge"}, "NetworkingMaxSpeed": float32(2500),
			"Costs":      list{dodgeCost},
			"EffectsIds": designdb.Pairs{{Key: int64(1), Value: effectID(effEvasion)}, {Key: int64(2), Value: effectID(effTracker)}},
			"HitsIds":    designdb.Pairs{{Key: int64(1), Value: hitID(hitDodge)}},
		}),
		abilityRow(content.GuidRest, "General_Rest", st{
			"AbilityName": txt("Rest"), "ActivationRequirements": "!IsInCombat(GetInstigator())", "HeldAbilityBehavior": "EHeldAbilityPressBehavior::Toggle",
			"ApplicableTargets": int64(1), "CancelSectionName": "SitEnd", "Cooldown": "",
			"AnimMontages": list{st{"AbilityAnimationSoftPaths": list{"/Game/Characters/Animations/00_ABLT/CM_M_ABLT_General_Rest_Montage.CM_M_ABLT_General_Rest_Montage"}}},
			"EffectsIds":   designdb.Pairs{{Key: int64(1), Value: effectID(effRest)}},
		}),
	}
	return src
}

// == C O A | C | Y | B ==

// buildSynthetic runs the typed builders over src into a content.db.
func buildSynthetic(t *testing.T, src *source) (*content.DB, error) {
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
	if err := buildTyped(w, src, &Options{Log: io.Discard}, meta); err != nil {
		return nil, err
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
	if meta["typed.stat_def"] != "14" || meta["typed.class_stat_base"] != "2400" {
		t.Errorf("typed meta = %v", meta)
	}
	return db, nil
}

func TestSyntheticTypedTables(t *testing.T) {
	db, err := buildSynthetic(t, syntheticSource())
	if err != nil {
		t.Fatal(err)
	}

	// stat_def.
	s, err := db.StatDefByName("Stamina")
	if err != nil || s.Replication != 1 || s.RepField != "StatRepInt32Owner" || s.Audience != "Rep_Autonomous" ||
		s.OnSpawn != "EStatResetBehavior::Reset_SetMax" || !strings.Contains(s.MaxExpr, "6064631747701440948") {
		t.Errorf("Stamina = %+v, %v", s, err)
	}
	if s, _ := db.StatDefByName("MoveSpeedMult"); s.Replication != 7 || s.RepField != "StatRepInt32EveryoneProxy" {
		t.Errorf("MoveSpeedMult = %+v", s)
	}

	// stat_curve: the empty curve is a row with no keys.
	if n := scalarInt(t, db, `SELECT count(*) FROM stat_curve`); n != 6*50 {
		t.Errorf("stat_curve has %d keys, want %d", n, 6*50)
	}
	var keyCount int
	var external string
	if err := db.SQL().QueryRow(`SELECT key_count, external_curve FROM stat_curve_def WHERE guid = 5100`).Scan(&keyCount, &external); err != nil || keyCount != 0 || external != "/Game/Curves/X.X" {
		t.Errorf("empty curve = %d, %q, %v", keyCount, external, err)
	}
	curves, err := db.Curves()
	if err != nil || len(curves.Curves) != 6 {
		t.Errorf("Curves = %d curves, %v", len(curves.Curves), err)
	}

	// class and class_stat_base: 100 * 1.3 = 130 everywhere but level 20, where
	// 335 * 1.3 is 436 in float64 and 435 in float32 (rounding_ambiguous).
	classes, err := db.Classes()
	if err != nil || len(classes) != 8 {
		t.Fatalf("Classes = %d, %v", len(classes), err)
	}
	for i, c := range classes {
		if c.Name != content.PlayerClasses[i] || c.ArchetypeID != int64(17747+i) || c.StatInitListName != "Player_Base_"+c.Name ||
			c.StatInitListGuid != int64(6000+i) || c.SkillTreeName != c.Name+"_SkillTable" || c.PrefersMelee != (i%2 == 0) {
			t.Errorf("class %d = %+v", i, c)
		}
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM class_stat_base`); n != 2400 {
		t.Errorf("class_stat_base has %d rows", n)
	}
	for level, want := range map[int]float64{1: 130, 19: 130, 20: 436, 21: 130, 50: 130} {
		got, err := db.ClassStatBase("Mage", level)
		if err != nil || len(got) != 6 {
			t.Fatalf("Mage level %d = %v, %v", level, got, err)
		}
		for stat, v := range got {
			if v.Value != want || v.RoundingAmbiguous != (level == 20) {
				t.Errorf("Mage level %d %s = %+v, want %v (ambiguous at level 20 only)", level, stat, v, want)
			}
		}
	}
	amb, err := db.RoundingAmbiguous()
	if err != nil || len(amb) != 8*6 {
		t.Errorf("RoundingAmbiguous = %d values, %v; want 48 (every class and attribute at level 20)", len(amb), err)
	}
	for _, a := range amb {
		if a.Level != 20 {
			t.Errorf("ambiguous value %+v", a)
		}
	}

	// ability, ability_cost, ability_effect, ability_hit.
	sprint, err := db.Ability(content.GuidSprint)
	if err != nil {
		t.Fatal(err)
	}
	if len(sprint.Costs) != 2 || sprint.Costs[0].StatName != "Stamina" || sprint.Costs[0].EnabledExpr != "IsMoving(GetOwner())" ||
		sprint.Costs[1].StatOrd != -1 || !sprint.Costs[1].TriggersCooldown || sprint.Costs[1].ChargeCostExpr != "1" ||
		!sprint.OverrideHeldBehavior || len(sprint.Effects) != 1 || sprint.Effects[0].Name != "General_Sprint" ||
		sprint.DisplayName != "Sprint" || sprint.Tags == nil || len(sprint.Tags) != 0 {
		t.Errorf("Sprint = %+v", sprint)
	}
	dodge, err := db.AbilityByName("General_Dodge")
	if err != nil {
		t.Fatal(err)
	}
	if len(dodge.Effects) != 2 || dodge.Effects[1].Ord != 2 || len(dodge.Hits) != 1 || dodge.Hits[0].Name != "Dodge_Hit" ||
		fmt.Sprint(dodge.Tags) != "[ability.dodge]" || dodge.NetworkingMaxSpeed != 2500 || !dodge.Costs[0].TriggersCooldown {
		t.Errorf("Dodge = %+v", dodge)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM ability`); n != 3 {
		t.Errorf("%d abilities (Sprint, Dodge and Rest; Some_Other_Ability is not one of them)", n)
	}

	// The effect set: what the abilities list (Sprint, Evasion, Tracker), what
	// their hits apply (the Dodge ability's own hit: effDodgeHit), the effects the
	// StaminaRegeneration formula names (guard, stun, exertion, evasion, sprint)
	// and what the hits of those apply (effTick through the Sprint effect's tick
	// hit); the Beyond effect is one step too far, the Unrelated one is not reached.
	rows, err := db.SQL().Query(`SELECT name FROM effect ORDER BY name`)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for rows.Next() {
		var n string
		rows.Scan(&n)
		got = append(got, n)
	}
	rows.Close()
	want := "[Dodge_Hit_Effect General_Dodge_Evasion General_Guard General_Guard_GuardBreak_Stun General_Rest General_Sprint General_Sprint_StaminaDrain General_Stamina_DisableRegen General_Stamina_Dodge_EvasiveManeuver_Tracker Sprint_Tick_Effect]"
	if fmt.Sprint(got) != want {
		t.Errorf("effects = %v\nwant     %s", got, want)
	}
	// The hits of those effects: Sprint's tick hit, the Tick effect's end hit,
	// the Exertion effect's apply hit, and the Dodge ability's own.
	rows, err = db.SQL().Query(`SELECT name FROM hit ORDER BY name`)
	if err != nil {
		t.Fatal(err)
	}
	got = nil
	for rows.Next() {
		var n string
		rows.Scan(&n)
		got = append(got, n)
	}
	rows.Close()
	if want := "[Dodge_Hit Exertion_Hit General_Sprint_StaminaDrain General_Stamina_Dodge_EndHit General_Stamina_Sprint_EndHit Sprint_Tick_Hit Tick_End_Hit]"; fmt.Sprint(got) != want {
		t.Errorf("hits = %v, want %s", got, want)
	}

	sprintEffect, err := db.EffectByName("General_Sprint")
	if err != nil {
		t.Fatal(err)
	}
	if !sprintEffect.InfiniteDuration || sprintEffect.TickTimer != 1 || !sprintEffect.IgnoreInitialTick || fmt.Sprint(sprintEffect.Tags) != "[Effect.DisableStamRegen]" ||
		fmt.Sprint(sprintEffect.EffectTags) != "[Effect.DisableStamRegen]" || len(sprintEffect.Hits) != 2 || sprintEffect.Hits[0].Phase != "Tick" || sprintEffect.Hits[1].Phase != "End" ||
		len(sprintEffect.StatMods) != 1 || sprintEffect.StatMods[0].StatName != "MoveSpeedMult" || sprintEffect.StatMods[0].ValueExpr != "0.2" {
		t.Errorf("Sprint effect = %+v", sprintEffect)
	}
	// hit_effect: kinds, and the effect one step beyond the set is named.
	type he struct {
		kind, effect, req, stack, dur string
	}
	var hes []he
	hrows, err := db.SQL().Query(`SELECT kind, effect_name, requirement_expr, stack_count_expr, duration_expr FROM hit_effect
		WHERE hit_guid = ? ORDER BY kind, ord`, hitEnd)
	if err != nil {
		t.Fatal(err)
	}
	for hrows.Next() {
		var x he
		hrows.Scan(&x.kind, &x.effect, &x.req, &x.stack, &x.dur)
		hes = append(hes, x)
	}
	hrows.Close()
	if fmt.Sprint(hes) != "[{apply Beyond_Effect x > 1 1 2} {self General_Stamina_DisableRegen  3 }]" {
		t.Errorf("hit_effect of the end hit = %v", hes)
	}
	if mods, err := db.StatMods("hit", hitEnd); err != nil || len(mods) != 1 || mods[0].StatName != "Stamina" || mods[0].ValueExpr != "0.001" {
		t.Errorf("hit stat mods = %+v, %v", mods, err)
	}

	// stamina_rule and stamina_regen_disable: the real formulas, run.
	rules, err := db.StaminaRules()
	if err != nil {
		t.Fatal(err)
	}
	for key, want := range map[string]string{
		content.RuleStaminaInitial: "100", content.RuleMaxStaminaInitial: "100", content.RuleRegenRateEnabled: "5",
		content.RuleRegenRateDisabled: "0", content.RuleSprintCostOutOfCombat: "1.25", content.RuleSprintCostInCombat: "2.5",
		content.RuleDodgeCostBase: "50", content.RuleDodgeCostSkilled: "30", content.RuleDodgeCooldownBase: "0.65",
		content.RuleDodgeCooldownSkilled: "0.4", content.RuleSprintChargeInterval: "0.25",
		content.RuleStatRegenTime: "1", "sprint.effect_tick_timer": "1", "sprint.cooldown": "0", "dodge.skill_effect_name": "Rogue_Advantage",
		"dodge.triggers_cooldown": "1", "sprint.triggers_cooldown": "0", "stamina.rep_field": "StatRepInt32Owner",
	} {
		if rules[key].Value != want {
			t.Errorf("rule %s = %q, want %q", key, rules[key].Value, want)
		}
		if rules[key].Source == "" {
			t.Errorf("rule %s has no source", key)
		}
	}
	if !strings.Contains(rules[content.RuleStatRegenTime].Source, "no client Config directory") {
		t.Errorf("stat_regen_time source = %q", rules[content.RuleStatRegenTime].Source)
	}
	if src := rules[content.RuleSprintChargeInterval].Source; !strings.Contains(src, "General_Sprint_StaminaDrain") || !strings.Contains(src, "no record references that effect") {
		t.Errorf("charge interval source = %q", src)
	}
	dis, err := db.StaminaRegenDisables()
	if err != nil || len(dis) != 5 || dis[0].EffectName != "General_Sprint" || dis[0].DisablesStanding || !dis[0].DisablesMoving ||
		dis[4].EffectName != "General_Stamina_DisableRegen" || !dis[4].DisablesStanding {
		t.Errorf("regen disables = %+v, %v", dis, err)
	}
}

// failure cases: the build stops and says what is wrong.
func TestSyntheticErrors(t *testing.T) {
	for _, tt := range []struct {
		name   string
		mutate func(s *source)
		want   string
	}{
		{"a formula outside the subset", func(s *source) {
			l := &s.rows["StatInitializerListRecordBase"][1]
			l.Fields["StatIDs"].(list)[0] = st{"StatId": statID(content.GuidStrength), "ValueExpression": "GetStatInitRunningTotal() * 2"}
		}, `unsupported function "GetStatInitRunningTotal"`},
		{"an attribute missing from a class's list", func(s *source) {
			l := &s.rows["StatInitializerListRecordBase"][1]
			ids := l.Fields["StatIDs"].(list)
			l.Fields["StatIDs"] = ids[1:]
		}, "Player_Base_Bard has 0 entries for stat"},
		{"a class row missing", func(s *source) {
			var keep []designdb.Row
			for _, r := range s.rows["CharacterClassesRecordBase"] {
				if r.Name != "Tank" {
					keep = append(keep, r)
				}
			}
			s.rows["CharacterClassesRecordBase"] = keep
		}, "no CharacterClasses row named Tank"},
		{"a wrong class enum", func(s *source) {
			s.rows["CharacterClassesRecordBase"][0].Fields["Class"] = "ECharacterClass::Mage"
		}, "Class is ECharacterClass::Mage, want ECharacterClass::Bard"},
		{"a class list missing", func(s *source) {
			s.rows["StatInitializerListRecordBase"][8].Name = "Player_Base_Nobody"
		}, "no StatInitializerList Player_Base_Tank"},
		{"a stat of the wrong name", func(s *source) {
			s.rows["StatTypeDefRecordBase"][4].Name = "Power"
		}, "content names it Strength"},
		{"an unknown stat in a list", func(s *source) {
			l := &s.rows["StatInitializerListRecordBase"][0]
			l.Fields["StatIDs"] = append(l.Fields["StatIDs"].(list), st{"StatId": statID(42), "ValueExpression": ""})
		}, "names stat 42, which is not a StatTypeDef row"},
		{"a missing field", func(s *source) {
			delete(s.rows["StatTypeDefRecordBase"][0].Fields, "ValueMax")
		}, "StatTypeDef Stamina (6064629575339934476): no field ValueMax"},
		{"a field of the wrong kind", func(s *source) {
			s.rows["StatTypeDefRecordBase"][0].Fields["Replication"] = "seven"
		}, "field Replication is string, want int64"},
		{"a duplicate stat", func(s *source) {
			s.rows["StatTypeDefRecordBase"] = append(s.rows["StatTypeDefRecordBase"], s.rows["StatTypeDefRecordBase"][0])
		}, "two StatTypeDef rows have Guid"},
		{"no Sprint ability", func(s *source) {
			s.rows["AoCAbilityRecordBase"] = s.rows["AoCAbilityRecordBase"][:1]
		}, "no Ability row 6064631641648332884"},
		{"a hit that does not exist", func(s *source) {
			s.rows["AbilityHitRecordBase"] = s.rows["AbilityHitRecordBase"][1:]
		}, "no AbilityHit row 8000000000000000001"},
		{"a stat mod that does not exist", func(s *source) {
			s.rows["StatModRecordBase"] = s.rows["StatModRecordBase"][1:]
		}, "names StatMod 9000000000000000001"},
		{"a regeneration formula that names no effect the data has", func(s *source) {
			var keep []designdb.Row
			for _, r := range s.rows["EffectRecordBase"] {
				if r.Guid != effGuard {
					keep = append(keep, r)
				}
			}
			s.rows["EffectRecordBase"] = keep
		}, "no Effect row 6064631667969556480"},
		{"a record struct with another TypeId", func(s *source) {
			s.typeIDs["EffectRecordBase"]++
		}, "EffectRecordBase has TypeId"},
		{"a table that did not decode", func(s *source) {
			delete(s.rows, "StatModRecordBase")
		}, "no decoded StatModRecordBase table"},
		{"a drain effect that takes another amount than the Sprint cost", func(s *source) {
			for i := range s.rows["StatModRecordBase"] {
				if s.rows["StatModRecordBase"][i].Guid == modDrain {
					s.rows["StatModRecordBase"][i].Fields["Value"] = "-2.0"
				}
			}
		}, "the drain effect is not the Sprint's"},
		{"a drain effect that takes Stamina while standing still", func(s *source) {
			for i := range s.rows["StatModRecordBase"] {
				if s.rows["StatModRecordBase"][i].Guid == modDrain {
					s.rows["StatModRecordBase"][i].Fields["Value"] = "SelectFloat(IsInCombat(GetSource()), -2.5, -1.25)"
				}
			}
		}, "takes 1.25 Stamina per tick while the owner stands still"},
		{"a drain effect with no tick", func(s *source) {
			for i := range s.rows["EffectRecordBase"] {
				if s.rows["EffectRecordBase"][i].Guid == effDrain {
					s.rows["EffectRecordBase"][i].Fields["TickTimer"] = float32(0)
				}
			}
		}, "want an infinite effect with a tick"},
		{"no drain effect", func(s *source) {
			var keep []designdb.Row
			for _, r := range s.rows["EffectRecordBase"] {
				if r.Guid != effDrain {
					keep = append(keep, r)
				}
			}
			s.rows["EffectRecordBase"] = keep
		}, "no Effect row 6064632350237720599"},
		{"no Rest ability", func(s *source) {
			var keep []designdb.Row
			for _, r := range s.rows["AoCAbilityRecordBase"] {
				if r.Guid != content.GuidRest {
					keep = append(keep, r)
				}
			}
			s.rows["AoCAbilityRecordBase"] = keep
		}, "no Ability row 109393746155128"},
		{"a stat mod that is not linear in its stat", func(s *source) {
			for i := range s.rows["StatModRecordBase"] {
				if s.rows["StatModRecordBase"][i].Guid == modRestHealth {
					s.rows["StatModRecordBase"][i].Fields["Value"] = "Min(GetStat(GetInstigator(), $#5365104778484440215:108986356618873$), 10)"
				}
			}
		}, "is not linear in stat 108986356618873"},
		{"a stat mod that reads two stats", func(s *source) {
			for i := range s.rows["StatModRecordBase"] {
				if s.rows["StatModRecordBase"][i].Guid == modRestHealth {
					s.rows["StatModRecordBase"][i].Fields["Value"] = "GetStat(GetInstigator(), $#5365104778484440215:108986356618873$) + GetStat(GetInstigator(), $#5365104778484440215:109183578756729$)"
				}
			}
		}, "reads 2 stats, the runtime handles one"},
		{"an activation requirement that reads another stat", func(s *source) {
			for i := range s.rows["AoCAbilityRecordBase"] {
				if s.rows["AoCAbilityRecordBase"][i].Guid == content.GuidSprint {
					s.rows["AoCAbilityRecordBase"][i].Fields["ActivationRequirements"] = "GetStat(GetSource(), $#5365104778484440215:108986356618873$) >= 1.0"
				}
			}
		}, "stat 108986356618873 has no value in this environment"},
		{"an activation requirement that no character can meet", func(s *source) {
			for i := range s.rows["AoCAbilityRecordBase"] {
				if s.rows["AoCAbilityRecordBase"][i].Guid == content.GuidRest {
					s.rows["AoCAbilityRecordBase"][i].Fields["ActivationRequirements"] = "false"
				}
			}
		}, "no character can use the ability"},
		{"a cost charged only while moving that is not the Sprint's", func(s *source) {
			for i := range s.rows["AoCAbilityRecordBase"] {
				if s.rows["AoCAbilityRecordBase"][i].Guid == content.GuidDodge {
					costs := s.rows["AoCAbilityRecordBase"][i].Fields["Costs"].(list)
					costs[0].(st)["Enabled"] = "IsMoving(GetOwner())"
				}
			}
		}, "the runtime knows the cadence of the Sprint's alone"},
		{"a cost group enabled by something else", func(s *source) {
			for i := range s.rows["AoCAbilityRecordBase"] {
				if s.rows["AoCAbilityRecordBase"][i].Guid == content.GuidDodge {
					costs := s.rows["AoCAbilityRecordBase"][i].Fields["Costs"].(list)
					costs[0].(st)["Enabled"] = "IsInCombat(GetOwner())"
				}
			}
		}, "is neither always true nor true exactly while moving"},
		{"an End hit that applies two regeneration pauses", func(s *source) {
			for i := range s.rows["AbilityHitRecordBase"] {
				if s.rows["AbilityHitRecordBase"][i].Guid == hitEvasionEnd {
					s.rows["AbilityHitRecordBase"][i].Fields["ApplyEffects"] = list{applyEffect(effExertion, "", "", ""), applyEffect(effStun, "", "", "")}
				}
			}
		}, "its End hits apply 6064631668308443138 and 6064631799761208354"},
		{"a Player_Base without Stamina's value", func(s *source) {
			l := &s.rows["StatInitializerListRecordBase"][0]
			l.Fields["StatIDs"].(list)[1] = st{"StatId": statID(content.GuidStamina), "ValueExpression": "GetStat(GetOwner(), $#5365104778484440215:1$)"}
		}, `unsupported function "GetStat"`},
	} {
		src := syntheticSource()
		tt.mutate(src)
		_, err := buildSynthetic(t, src)
		if err == nil || !strings.Contains(err.Error(), tt.want) {
			t.Errorf("%s: error %v, want one containing %q", tt.name, err, tt.want)
		}
	}
}

// TestStatRegenTime reads DefaultCombat.ini (one very long line, with a byte
// order mark) or falls back to the documented default.
func TestStatRegenTime(t *testing.T) {
	dir := t.TempDir()
	ini := "\xef\xbb\xbf[/Script/GameSystemsPlugin.CombatConfig]\r\nGameSettings=(General=(InCombatTimer=6.000000),Stats=(StatRegenTime=2.500000,GlobalDamageMultiplier=1.000000))\r\n"
	if err := os.WriteFile(filepath.Join(dir, "DefaultCombat.ini"), []byte(ini), 0o644); err != nil {
		t.Fatal(err)
	}
	v, src, err := statRegenTime(dir)
	if err != nil || v != 2.5 || !strings.HasPrefix(src, "DefaultCombat.ini") {
		t.Errorf("statRegenTime(dir) = %v, %q, %v", v, src, err)
	}
	v, src, err = statRegenTime("")
	if err != nil || v != content.DefaultStatRegenTime || !strings.Contains(src, "no client Config directory") {
		t.Errorf("statRegenTime(\"\") = %v, %q, %v", v, src, err)
	}
	v, src, err = statRegenTime(filepath.Join(dir, "missing"))
	if err != nil || v != content.DefaultStatRegenTime || !strings.Contains(src, "does not exist") {
		t.Errorf("statRegenTime(missing) = %v, %q, %v", v, src, err)
	}
	other := t.TempDir()
	os.WriteFile(filepath.Join(other, "DefaultCombat.ini"), []byte("[x]\r\nStats=(Other=1.0)\r\n"), 0o644)
	if _, _, err := statRegenTime(other); err == nil || !strings.Contains(err.Error(), "has no StatRegenTime") {
		t.Errorf("an ini without StatRegenTime: %v", err)
	}
	// The source names the section and property the value was found in.
	_, src, _ = statRegenTime(dir)
	if want := "DefaultCombat.ini [/Script/GameSystemsPlugin.CombatConfig] GameSettings.Stats.StatRegenTime"; src != want {
		t.Errorf("source = %q, want %q", src, want)
	}
}

// (C:Y:B + C O A)

// TestStatRegenTimeChecksItsSection: StatRegenTime counts only in the
// GameSettings property of [/Script/GameSystemsPlugin.CombatConfig], not as the
// same text in another section or in another property.
func TestStatRegenTimeChecksItsSection(t *testing.T) {
	const section = "[/Script/GameSystemsPlugin.CombatConfig]\r\n"
	for _, tt := range []struct {
		name, ini string
		want      float64 // 0: an error
	}{
		{"the right section and property", section + "GameSettings=(Stats=(StatRegenTime=2.5))\r\n", 2.5},
		{"after other simple properties of Stats", section + "GameSettings=(General=(A=1.0),Stats=(Foo=1,Bar=x,StatRegenTime=4.0,Nested=(C=1)))\r\n", 4},
		{"after a nested property of Stats", section + "GameSettings=(Stats=(Nested=(C=1),StatRegenTime=4.0))\r\n", 0},
		{"in another section", "[/Script/Other.Config]\r\nGameSettings=(Stats=(StatRegenTime=3.0))\r\n", 0},
		{"in another property of the right section", section + "AccountSettings=(Stats=(StatRegenTime=9.0))\r\n", 0},
		{"in the right section after another section's", "[/Script/Other.Config]\r\nGameSettings=(Stats=(StatRegenTime=3.0))\r\n" + section + "GameSettings=(Stats=(StatRegenTime=6.0))\r\n", 6},
		{"before any section", "GameSettings=(Stats=(StatRegenTime=3.0))\r\n" + section, 0},
		{"in the value of another key", section + "Other=GameSettings=(Stats=(StatRegenTime=3.0))\r\n", 0},
	} {
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, "DefaultCombat.ini"), []byte("\xef\xbb\xbf"+tt.ini), 0o644); err != nil {
			t.Fatal(err)
		}
		v, src, err := statRegenTime(dir)
		if tt.want == 0 {
			if err == nil || !strings.Contains(err.Error(), "has no StatRegenTime in the GameSettings property of [/Script/GameSystemsPlugin.CombatConfig]") {
				t.Errorf("%s: statRegenTime = %v, %q, %v; want the no-StatRegenTime error", tt.name, v, src, err)
			}
			continue
		}
		if err != nil || v != tt.want || !strings.Contains(src, "[/Script/GameSystemsPlugin.CombatConfig] GameSettings.Stats.StatRegenTime") {
			t.Errorf("%s: statRegenTime = %v, %q, %v; want %v", tt.name, v, src, err, tt.want)
		}
	}
}

// TestRuleSetKeepsTheFirstProblem: a rule put twice is an error returned to the
// caller, not a panic, and the first value stays.
func TestRuleSetKeepsTheFirstProblem(t *testing.T) {
	rs := &ruleSet{rows: map[string][2]string{}}
	rs.put("a", "1", "first")
	rs.putFloat("b", 2.5, "src")
	if rs.err != nil {
		t.Fatalf("err = %v", rs.err)
	}
	rs.put("a", "2", "second")
	rs.putFloat("b", 9, "again")
	rs.put("c", "3", "ok")
	if rs.err == nil || !strings.Contains(rs.err.Error(), "stamina rule a is given twice") {
		t.Errorf("err = %v", rs.err)
	}
	if rs.rows["a"] != [2]string{"1", "first"} || rs.rows["b"][0] != "2.5" || rs.rows["c"][0] != "3" {
		t.Errorf("rows = %v", rs.rows)
	}
}

func TestFieldReader(t *testing.T) {
	f := newFields("Row X", st{
		"s": "text", "b": true, "i": int64(7), "f32": float32(0.001), "f64": 2.5, "l": list{"a", "b"},
		"id": idv(5, 6, "Name"), "txt": designdb.Text{Key: "K", Source: ""}, "txt2": designdb.Text{Key: "K", Source: "S"},
		"ids": list{idv(1, 2, "A"), idv(3, 4, "B")}, "tags": list{st{"TagName": "t.a"}, st{"TagName": "t.b"}},
		"tags2": list{"u.a"}, "map": designdb.Pairs{{Key: int64(1), Value: idv(9, 8, "M")}},
	})
	if f.str("s") != "text" || !f.boolean("b") || f.int("i") != 7 || f.float("f32") != 0.001 || f.float("f64") != 2.5 || len(f.list("l")) != 2 {
		t.Error("scalar reads")
	}
	if g, n := f.id("id"); g != 5 || n != "Name" {
		t.Errorf("id = %d, %q", g, n)
	}
	if f.text("txt") != "K" || f.text("txt2") != "S" {
		t.Error("text reads")
	}
	if ids := f.ids("ids"); len(ids) != 2 || ids[1] != 3 {
		t.Errorf("ids = %v", ids)
	}
	if k, g := f.keyedIds("map"); len(k) != 1 || k[0] != 1 || g[0] != 9 {
		t.Errorf("keyedIds = %v %v", k, g)
	}
	if fmt.Sprint(f.tags("tags")) != "[t.a t.b]" || fmt.Sprint(f.tags("tags2")) != "[u.a]" {
		t.Error("tags")
	}
	if f.err != nil {
		t.Fatalf("unexpected error %v", f.err)
	}

	// The first problem sticks and later reads return zero values.
	f.str("missing")
	first := f.err
	f.int("s")
	if f.err != first || !strings.Contains(first.Error(), "Row X: no field missing") {
		t.Errorf("error = %v", f.err)
	}
	g := newFields("Row Y", st{"s": 5})
	if g.str("s"); g.err == nil || !strings.Contains(g.err.Error(), "field s is int, want string") {
		t.Errorf("wrong kind: %v", g.err)
	}
	h := newFields("Row Z", st{"id": st{"Guid": "x"}, "tag": list{3}})
	if h.id("id"); h.err == nil || !strings.Contains(h.err.Error(), "not an Id struct") {
		t.Errorf("bad id: %v", h.err)
	}
	h2 := newFields("Row Z", st{"tag": list{3}})
	if h2.tags("tag"); h2.err == nil || !strings.Contains(h2.err.Error(), "array of tags") {
		t.Errorf("bad tags: %v", h2.err)
	}
}

func TestDecimal32(t *testing.T) {
	for in, want := range map[float32]float64{0.001: 0.001, 1028.9219: 1028.9219, 0.05: 0.05, 3.4028235e+38: 3.4028235e+38, 30: 30, 0: 0} {
		if got := decimal32(in); got != want {
			t.Errorf("decimal32(%v) = %v, want %v", in, got, want)
		}
	}
}

// (C | Y | B ~ C-O-A)

func TestRunFlags(t *testing.T) {
	var out, errb strings.Builder
	for _, tt := range []struct {
		name string
		args []string
		env  map[string]string
		code int
		want string
	}{
		{"no inputs", nil, map[string]string{"AOC_DESIGN_DB": "", "AOC_USMAP": ""}, 2, "--dbc and --usmap are required"},
		{"unknown flag", []string{"--bogus"}, nil, 2, "flag provided but not defined"},
		{"stray argument", []string{"--dbc", "a", "--usmap", "b", "extra"}, nil, 2, `unexpected argument "extra"`},
		{"help", []string{"-h"}, nil, 0, "-record-encoding"},
		{"a missing design database", []string{"--dbc", filepath.Join(t.TempDir(), "x.dbc"), "--usmap", "y", "--out", filepath.Join(t.TempDir(), "o.db")}, nil, 1, "design database"},
		{"a bad encoding", []string{"--dbc", "a", "--usmap", "b", "--record-encoding", "zip"}, nil, 1, `unknown record encoding "zip"`},
	} {
		for k, v := range tt.env {
			t.Setenv(k, v)
		}
		out.Reset()
		errb.Reset()
		code := run(tt.args, &out, &errb)
		if code != tt.code || !strings.Contains(errb.String(), tt.want) {
			t.Errorf("%s: exit %d, stderr %q; want exit %d containing %q", tt.name, code, errb.String(), tt.code, tt.want)
		}
	}
}

func TestRepoRootDefault(t *testing.T) {
	// From any directory of the checkout the default output is dist/content/content.db
	// at the root (the directory above services).
	wd, _ := os.Getwd()
	root := repoRoot()
	if _, err := os.Stat(filepath.Join(root, "services", "go.mod")); err != nil {
		t.Errorf("repoRoot() = %s from %s: no services/go.mod there", root, wd)
	}
}

// TestSyntheticAbilityRuntime: the three runtime tables from the synthetic rows, which carry the real formulas.
func TestSyntheticAbilityRuntime(t *testing.T) {
	db, err := buildSynthetic(t, syntheticSource())
	if err != nil {
		t.Fatal(err)
	}
	abilities, err := db.AbilityRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	sprint, dodge, rest := abilities[content.GuidSprint], abilities[content.GuidDodge], abilities[content.GuidRest]
	if sprint.HeldBehavior != "Smart" || sprint.MinStamina != 1 || sprint.CostAmount != 1.25 || !sprint.CostRequiresMoving ||
		sprint.ChargeInterval != 0.25 || sprint.RequiresOutOfCombat || sprint.CancelSection != "End" || sprint.EffectGuid != effSprint ||
		sprint.TriggersCooldown || !strings.Contains(sprint.MontagePath, "Sprint_Montage") {
		t.Errorf("Sprint = %+v", sprint)
	}
	if dodge.HeldBehavior != "Naive" || dodge.CostAmount != 50 || dodge.CostRequiresMoving || dodge.ChargeInterval != 0 || dodge.Cooldown != 0.65 ||
		!dodge.TriggersCooldown || !dodge.DisableDuringCooldown || dodge.EffectGuid != effEvasion || dodge.MinStamina != 0 || dodge.MontagePath != "" ||
		dodge.CancelSection != "" {
		t.Errorf("Dodge = %+v", dodge)
	}
	if rest.HeldBehavior != "Toggle" || !rest.RequiresOutOfCombat || rest.CostStatGuid != 0 || !rest.TargetsSelf || rest.EffectGuid != effRest ||
		rest.CancelSection != "SitEnd" || !strings.Contains(rest.MontagePath, "General_Rest_Montage") || rest.Cooldown != 0 {
		t.Errorf("Rest = %+v", rest)
	}
	effects, err := db.EffectRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	if len(effects) != 4 {
		t.Errorf("%d effects: %+v", len(effects), effects)
	}
	if e := effects[effSprint]; !e.Infinite || e.TickTimer != 1 || e.EndEffectGuid != effExertion || e.EndEffectDuration != 1 || e.StopsRegenStanding || !e.StopsRegenMoving {
		t.Errorf("Sprint effect = %+v", e)
	}
	if e := effects[effEvasion]; e.Infinite || e.Duration != 0.5 || e.EndEffectGuid != effExertion || !e.StopsRegenStanding {
		t.Errorf("Evasion = %+v", e)
	}
	if e := effects[effRest]; !e.Infinite || e.Duration != 0 || e.EndEffectGuid != 0 {
		t.Errorf("Rest effect = %+v", e)
	}
	if e := effects[effExertion]; e.Infinite || e.Duration != 1 {
		t.Errorf("exertion = %+v", e)
	}
	mods, err := db.EffectStatRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	rm := mods[effRest]
	if len(rm) != 2 || rm[0].StatGuid != statHealthReg || rm[0].SourceStatGuid != statMaxHealth || rm[0].Factor != 0.01 || rm[0].Addend != 1 ||
		rm[1].StatGuid != statManaReg || rm[1].SourceStatGuid != statMaxMana || rm[1].Factor != 0.01 || rm[1].Addend != 1 {
		t.Errorf("Rest mods = %+v", rm)
	}
	if sm := mods[effSprint]; len(sm) != 1 || sm[0].Addend != 0.2 || sm[0].SourceStatGuid != 0 || sm[0].Factor != 0 {
		t.Errorf("Sprint mods = %+v", sm)
	}
}

// TestProbeActivation: the requirement formulas are read by running them.
func TestProbeActivation(t *testing.T) {
	const stamina = "GetStat(GetSource(), $#5365104778484440215:6064629575339934476$)"
	for _, tt := range []struct {
		expr        string
		min         float64
		outOfCombat bool
		err         string
	}{
		{"", 0, false, ""},
		{stamina + " >= 1.0", 1, false, ""},
		{stamina + " > 2.5", 2.5, false, ""},
		{"!IsInCombat(GetInstigator())", 0, true, ""},
		{"!IsInCombat(GetInstigator()) && " + stamina + " >= 10", 10, true, ""},
		{"true", 0, false, ""},
		{"false", 0, false, "no character can use the ability"},
		{"1 + 2", 0, false, "not a condition"},
		{stamina + " < 3.0", 0, false, "no character can use the ability"},
	} {
		min, out, err := probeActivation(tt.expr)
		if tt.err != "" {
			if err == nil || !strings.Contains(err.Error(), tt.err) {
				t.Errorf("%q: error %v, want one containing %q", tt.expr, err, tt.err)
			}
			continue
		}
		if err != nil || min != tt.min || out != tt.outOfCombat {
			t.Errorf("%q = %v, %v, %v; want %v, %v", tt.expr, min, out, err, tt.min, tt.outOfCombat)
		}
	}
}

// C:Y:B captures, C | O | A code

func TestLinearMod(t *testing.T) {
	const x = "GetStat(GetInstigator(), $#5365104778484440215:7$)"
	const y = "GetStat(GetInstigator(), $#5365104778484440215:8$)"
	for _, tt := range []struct {
		expr           string
		source         int64
		factor, addend float64
		err            string
	}{
		{"0.2", 0, 0, 0.2, ""},
		{"1.0", 0, 0, 1, ""},
		{x + " * 0.01 + 1", 7, 0.01, 1, ""},
		{"const m = " + x + ";\r\nm * 0.5", 7, 0.5, 0, ""},
		{"-" + x + " * 2 + 3", 7, -2, 3, ""},
		{"Min(" + x + ", 10)", 0, 0, 0, "is not linear in stat 7"},
		{x + " * " + x, 0, 0, 0, "is not linear in stat 7"},
		{x + " + " + y, 0, 0, 0, "reads 2 stats"},
		{"GetDamage()", 0, 0, 0, "unsupported function"},
	} {
		m, err := linearMod(tt.expr)
		if tt.err != "" {
			if err == nil || !strings.Contains(err.Error(), tt.err) {
				t.Errorf("%q: error %v, want one containing %q", tt.expr, err, tt.err)
			}
			continue
		}
		if err != nil || m.source != tt.source || m.factor != tt.factor || m.addend != tt.addend {
			t.Errorf("%q = %+v, %v; want source %d factor %v addend %v", tt.expr, m, err, tt.source, tt.factor, tt.addend)
		}
	}
}

func TestCostNeedsMoving(t *testing.T) {
	for expr, want := range map[string]bool{"": false, "true": false, "IsMoving(GetOwner())": true, "IsMoving(GetSource()) == true": true} {
		got, err := costNeedsMoving(expr)
		if err != nil || got != want {
			t.Errorf("costNeedsMoving(%q) = %v, %v; want %v", expr, got, err, want)
		}
	}
	for _, expr := range []string{"false", "IsInCombat(GetOwner())", "!IsMoving(GetOwner())", "IsMoving(GetOwner()) && !IsInCombat(GetOwner())"} {
		if _, err := costNeedsMoving(expr); err == nil {
			t.Errorf("costNeedsMoving(%q) is not an error", expr)
		}
	}
}

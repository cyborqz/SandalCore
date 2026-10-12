// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Plan D1: the item uses, action responses, reward tables, level curves and auto-granted ranks, on synthetic rows (the
// shape of the GM items' records) and on the real design database (TestRealProgression, with AOC_DESIGN_DB).

import (
	"context"
	"database/sql"
	"io"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

const (
	synthLevelScroll    = 8100000000000000001 // GM_Scroll_MakeMeLevel3
	synthClassScroll    = 8100000000000000002 // GM_Scroll_PrimaryArchetypeBard
	synthGearScroll     = 8100000000000000003 // GM_Scroll_Gear 01
	synthArchConsume    = 8100000000000000010
	synthArchKeep       = 8100000000000000011
	synthActionLevel    = 8100000000000000020
	synthActionClass    = 8100000000000000021
	synthActionGear     = 8100000000000000022
	synthTableLevel     = 8100000000000000030
	synthTableGear      = 8100000000000000031
	synthTableRandom    = 8100000000000000032
	synthTableBadXP     = 8100000000000000033
	synthActionBadXP    = 8100000000000000023
	synthBadXPScroll    = 8100000000000000004
	synthLevelCurve     = 8100000000000000040 // a StatCurve: 0, 300, 4000, 11500 at levels 1-4
	synthUniversal      = 8100000000000000050
	synthNodeSprint     = 8100000000000000060
	synthNodeStam       = 8100000000000000061
	synthNodeBought     = 8100000000000000062
	synthSkillSprint    = 8100000000000000070
	synthSkillStam      = 8100000000000000071
	synthRankSprint     = 8100000000000000080
	synthRankStam       = 8100000000000000081
	synthEffectSprint   = 8100000000000000090
	synthEffectStam     = 8100000000000000091
	synthModStam        = 8100000000000000100
	synthModFormula     = 8100000000000000101
	synthAdventuring    = 8100000000000000110
	synthWeaponType     = 8100000000000000111
	synthMultStat       = 6064630809541214212
	synthPointArchetype = 8100000000000000120
	synthPointStamina   = 8100000000000000121
	synthPointWeapon    = 8100000000000000122
)

// Cyb : C | O | A

func pointContainer(pointType int64, qty int64) st {
	return st{"Quantity": qty, "SkillPointTypeId": idv(pointType, content.TypeSkillPointType, "None")}
}

func refText(typeID uint64, guid int64) string {
	return "$#" + itoa(uint64(typeID)) + ":" + itoa(uint64(guid)) + "$"
}

func itoa(v uint64) string {
	if v == 0 {
		return "0"
	}
	var b []byte
	for v > 0 {
		b = append([]byte{byte('0' + v%10)}, b...)
		v /= 10
	}
	return string(b)
}

func levelScrollExpr(target string) string {
	return crlf(`const guid = GetActorGuid(GetInstigator());
const curLevelXP = GetPlayerXPForCurrentLevel(guid);
const xpDebt = GetXPDebtRatio(guid) * curLevelXP;
const xp   = EvalCurve(` + refText(content.TypeStatCurve, synthLevelCurve) + `, ` + target + `) + xpDebt;
const cur  = EvalCurve(` + refText(content.TypeStatCurve, synthLevelCurve) + `, float(GetPlayerLevel()));
const mult = Max(0.001, 1.0 + GetStat(GetPlayer(), ` + refText(content.TypeStatDef, synthMultStat) + `));
(xp - cur)/mult + 10`)
}

func curveKeys(values ...float32) list {
	var keys list
	for i, v := range values {
		keys = append(keys, st{"Time": float32(i + 1), "Value": v, "InterpMode": "ERichCurveInterpMode::RCIM_Linear",
			"ArriveTangent": float32(0), "LeaveTangent": float32(0)})
	}
	return keys
}

func rewardItem(item int64, qty string) st {
	return st{"Item": st{"ItemId": idv(item, content.TypeItem, "None"), "VariationId": idv(0, 7640658097578260109, "None")}, "Quantity": qty, "bPrioritizeQuality": false}
}

func rewardContainer(grant, selection string, rewards ...st) st {
	var rs list
	for _, r := range rewards {
		rs = append(rs, r)
	}
	return st{"GrantMethod": "ERewardGrantMethod::" + grant, "SelectionAlgorithm": "ERewardSelectionAlgorithm::" + selection, "Rewards": rs,
		"NumberToSelect": int64(0), "GrantPredicate": ""}
}

func rewardOf(items list, xps list) st {
	return st{"ItemRewards": items, "ExperienceRewards": xps, "Currency": list{}}
}

func xpReward(kind, expr string) st {
	return st{"ExperienceType": "EExperienceContextType::" + kind, "ExperienceValue": expr}
}

func tableRow(guid int64, name, predicate string, containers ...st) designdb.Row {
	var cs list
	for _, c := range containers {
		cs = append(cs, c)
	}
	return mkRow(guid, name, st{"Predicate": predicate, "SubTableSelectionAlgorithm": "ERewardSelectionAlgorithm::None", "RewardDefContainers": cs,
		"NumberOfSubtablesToSelect": int64(2), "Callout": "CalloutType::None"})
}

func actionRow(guid int64, name, kind string, class int64, tables ...int64) designdb.Row {
	var ts list
	for _, t := range tables {
		ts = append(ts, idv(t, content.TypeRewardTable, "None"))
	}
	return mkRow(guid, name, st{"ResponseType": "EActionResponse::" + kind, "CharacterClassId": idv(class, content.TypeCharacterClasses, "None"),
		"RemoveUpToQuantity": true, "RewardsIds": ts})
}

// C | Y | B.C O A

// progressionSource adds the plan D1 rows to the synthetic source: three GM-shaped items (a level scroll, a class scroll, a
// gear scroll), one with an expression that is not the formula, two level curves and a Universal tree with two auto-granted
// nodes and one bought.
func progressionSource(t *testing.T) *source {
	t.Helper()
	src := syntheticSource()
	for schema, id := range progressionRecordTypes {
		src.typeIDs[schema] = id
	}
	src.typeIDs["ItemRecordBase"] = content.TypeItem
	src.typeIDs["ItemStatBlockRecordBase"] = content.TypeItemStatBlock
	src.typeIDs["QualityCurveRecordBase"] = content.TypeQualityCurve
	src.typeIDs["StartingEquipmentRecordBase"] = content.TypeStartingEquipment
	src.typeIDs["SetBonusRecordBase"] = content.TypeSetBonus
	for schema, id := range talentRecordTypes {
		src.typeIDs[schema] = id
	}
	src.rows["SkillRequirementRecordBase"] = nil
	src.rows["SkillPointTypeRecordBase"] = []designdb.Row{mkRow(synthPointArchetype, "Combat_Archetype", st{}), mkRow(synthPointStamina, "Combat_Stamina", st{}),
		mkRow(synthPointWeapon, "Combat_Weapon", st{})}
	src.rows["WeaponSkillLinesRecordBase"] = nil
	src.rows["SkillTreeSlotRecordBase"] = nil
	// the level curve the expressions read
	src.rows["StatCurveRecordBase"] = append(src.rows["StatCurveRecordBase"], mkRow(synthLevelCurve, "Adventuring_LevelUpCurve",
		st{"Curve": st{"ExternalCurve": "", "EditorCurveData": st{"DefaultValue": float32(0), "Keys": curveKeys(0, 300, 4000, 11500)}}}))
	// the multiplier stat
	src.rows["StatTypeDefRecordBase"] = append(src.rows["StatTypeDefRecordBase"], statRow(synthMultStat, "AdventuringXPAddMult", 1, st{"base": "0"}))
	item := func(guid int64, name string, arch int64, actions ...int64) designdb.Row {
		var as list
		for _, a := range actions {
			as = append(as, idv(a, content.TypeActionResponse, "None"))
		}
		return mkRow(guid, name, st{"CanEquip": false, "OccupiesAllEquipSlots": false, "Level": int64(1), "DefaultQuality": int64(1), "MaxStackSize": int64(1),
			"StatBlockId": idv(0, content.TypeItemStatBlock, "None"), "QualityCurveId": idv(0, content.TypeQualityCurve, "None"),
			"AppearanceId": idv(0, 7961510888716503441, "None"), "EquipSlots": list{}, "SetBonusIds": list{}, "GameplayTags": list{},
			"ActivationArchetypeId": idv(arch, content.TypeItemActivationArchetype, "None"), "ActivationActionsIds": as})
	}
	src.rows["ItemRecordBase"] = []designdb.Row{
		item(synthLevelScroll, "GM_Scroll_MakeMeLevel3", synthArchConsume, synthActionLevel),
		item(synthClassScroll, "GM_Scroll_PrimaryArchetypeBard", synthArchKeep, synthActionClass),
		item(synthGearScroll, "GM_Scroll_Gear 01", synthArchConsume, synthActionGear),
		item(synthBadXPScroll, "GM_Scroll_Odd", synthArchConsume, synthActionBadXP),
		item(8100000000000000005, "Plain_Sword", 0),
	}
	src.rows["ItemActivationArchetypeRecordBase"] = []designdb.Row{
		mkRow(synthArchConsume, "Consume", st{"ActivationAction": "EItemActivationAction::ActivateActionResponses", "ConsumeOnUse": true, "Cooldown": int64(0), "DisabledInCombat": false}),
		mkRow(synthArchKeep, "Keep", st{"ActivationAction": "EItemActivationAction::ActivateActionResponses", "ConsumeOnUse": false, "Cooldown": int64(5), "DisabledInCombat": true}),
	}
	src.rows["ActionResponseRecordBase"] = []designdb.Row{
		actionRow(synthActionLevel, "Level3Reward", "GiveItem", 0, synthTableLevel, synthTableRandom),
		actionRow(synthActionClass, "SetPrimaryArchetypeBard", "SetPrimaryArchetype", 100), // the synthetic source's first class row (Bard)
		actionRow(synthActionGear, "Gear01", "GiveItem", 0, synthTableGear),
		actionRow(synthActionBadXP, "Odd", "GiveItem", 0, synthTableBadXP),
		actionRow(8100000000000000029, "Unused", "Callout", 0, 8100000000000000039),
	}
	src.rows["RewardTableRecordBase"] = []designdb.Row{
		tableRow(synthTableLevel, "Level3Reward", "GetPlayerLevel() < 3",
			rewardContainer("Always", "All", rewardOf(list{}, list{xpReward("Adventuring", levelScrollExpr("3.0")), xpReward("Weapon", "EvalCurve($#1:2$, 3.0) - 1")}))),
		tableRow(synthTableGear, "Gear01", "",
			rewardContainer("Always", "All", rewardOf(list{rewardItem(8100000000000000005, "1"), rewardItem(8100000000000000005, "20")}, list{}))),
		tableRow(synthTableRandom, "Random", "IsNight()",
			rewardContainer("Random", "OneOf", rewardOf(list{rewardItem(8100000000000000005, "1")}, list{}))),
		tableRow(synthTableBadXP, "Odd", "",
			rewardContainer("Always", "All", rewardOf(list{}, list{xpReward("Adventuring", strings.Replace(levelScrollExpr("3.0"), "+ 10", "* 2 + 10", 1))}))),
		tableRow(8100000000000000039, "Unreferenced", "", rewardContainer("Always", "All", rewardOf(list{}, list{}))),
	}
	src.rows["LevelingTypeDefRecordBase"] = []designdb.Row{
		mkRow(synthAdventuring, "Adventuring", st{"ExperienceType": "EExperienceType::Adventuring", "LevelCapOverride": int64(3), "bUsesExperienceDebt": true,
			"XPMultiplier": "1.0 + GetStat(GetPlayer(), " + refText(content.TypeStatDef, synthMultStat) + ")",
			"Levels":       list{st{"SkillPointContainers": list{}}, st{"SkillPointContainers": list{pointContainer(synthPointArchetype, 1), pointContainer(synthPointStamina, 1)}}, st{"SkillPointContainers": list{pointContainer(synthPointArchetype, 2)}}},
			"LevelUpCurve": st{"ExternalCurve": "", "EditorCurveData": st{"DefaultValue": float32(0), "Keys": curveKeys(0, 300, 4000, 11500)}}}),
		mkRow(synthWeaponType, "Weapon_Test", st{"ExperienceType": "EExperienceType::Weapon", "LevelCapOverride": int64(0), "bUsesExperienceDebt": false,
			"XPMultiplier": "", "Levels": list{st{"SkillPointContainers": list{pointContainer(synthPointWeapon, 1)}}},
			"LevelUpCurve": st{"ExternalCurve": "", "EditorCurveData": st{"DefaultValue": float32(0), "Keys": append(list{st{"Time": float32(0), "Value": float32(0), "InterpMode": "ERichCurveInterpMode::RCIM_Linear"}}, curveKeys(0, 400.5)...)}}}),
	}
	src.rows["SkillTreeRecordBase"] = []designdb.Row{
		mkRow(synthUniversal, "Universal", st{"LevelTypeId": idv(synthAdventuring, content.TypeLevelingTypeDef, "None"), "Skills": list{
			st{"TreeNodeId": idv(synthNodeSprint, content.TypeSkillTreeNode, "None")}, st{"TreeNodeId": idv(synthNodeStam, content.TypeSkillTreeNode, "None")},
			st{"TreeNodeId": idv(synthNodeBought, content.TypeSkillTreeNode, "None")}, st{"TreeNodeId": idv(8100000000000000069, content.TypeSkillTreeNode, "None")}}}),
	}
	node := func(guid int64, name string, auto bool, level int64, skill int64) designdb.Row {
		return mkRow(guid, name, st{"bAutoGranted": auto, "LevelRequirement": level, "SkillId": idv(skill, content.TypeSkill, "None"), "SkillSlotId": idv(0, content.TypeSkillTreeSlot, "None"), "PointRequirement": int64(0),
			"bAutoAddToHotbar": auto, "bAllAreRequired": false, "SkillsRequired": list{}, "MutuallyExclusiveSkillsIds": list{}})
	}
	src.rows["SkillTreeNodeRecordBase"] = []designdb.Row{
		node(synthNodeSprint, "General_Stamina_Passive_Sprint_1", true, 5, synthSkillSprint),
		node(synthNodeStam, "General_Stamina_Passive_StamBoost_1", true, 10, synthSkillStam),
		node(synthNodeBought, "General_Stamina_Sprint_Focused_1", false, 0, synthSkillSprint),
	}
	src.rows["SkillRecordBase"] = []designdb.Row{
		mkRow(synthSkillSprint, "General_Stamina_Passive_Sprint_1", st{"SkillRanksIds": list{idv(synthRankSprint, content.TypeSkillRank, "None")}}),
		mkRow(synthSkillStam, "General_Stamina_Passive_StamBoost_1", st{"SkillRanksIds": list{idv(synthRankStam, content.TypeSkillRank, "None")}}),
	}
	src.rows["SkillRankRecordBase"] = []designdb.Row{
		mkRow(synthRankSprint, "General_Stamina_Passive_Sprint_1", st{"EffectIdId": idv(synthEffectSprint, content.TypeEffect, "None"), "AbilityIdId": idv(0, content.TypeAbility, "None"),
			"SkillCost": st{"SkillPointCosts": list{}, "CurrencyCosts": list{}, "CommodityCosts": list{}}}),
		mkRow(synthRankStam, "General_Stamina_Passive_StamBoost_1", st{"EffectIdId": idv(synthEffectStam, content.TypeEffect, "None"), "AbilityIdId": idv(0, content.TypeAbility, "None"),
			"SkillCost": st{"SkillPointCosts": list{st{"Quantity": int64(2), "SkillPointTypeId": idv(synthPointStamina, content.TypeSkillPointType, "None")}}, "CurrencyCosts": list{}, "CommodityCosts": list{}}}),
	}
	src.rows["EffectRecordBase"] = append(src.rows["EffectRecordBase"],
		effectRow(synthEffectSprint, "General_Stamina_Passive_Sprint_1", st{"EffectDuration": "1", "StatModsIds": list{modID(synthModFormula)}}),
		effectRow(synthEffectStam, "General_Stamina_Passive_StamBoost_1", st{"bInfiniteDuration": true, "StatModsIds": list{modID(synthModStam), modID(synthModFormula)}}),
	)
	src.rows["StatModRecordBase"] = append(src.rows["StatModRecordBase"],
		mkRow(synthModStam, "StamBoost", st{"StatRefId": statID(content.GuidMaxStamina), "Value": "10"}),
		mkRow(synthModFormula, "Formula", st{"StatRefId": statID(statMoveSp), "Value": "0.05 * GetSkillPurchased(GetOwner(), $#1:2$, $#3:4$)"}),
	)
	return src
}

// C Y B : CoA

// buildProgressionSynthetic runs buildTyped and buildProgression over src into a content.db.
func buildProgressionSynthetic(t *testing.T, src *source) (*content.DB, map[string]string, error) {
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
	opt := &Options{Log: io.Discard}
	if err := buildTyped(w, src, opt, meta); err != nil {
		return nil, nil, err
	}
	if err := buildProgression(w, src, src.curves, src.statNames, opt, meta); err != nil {
		return nil, nil, err
	}
	if err := buildTalents(w, src, src.statNames, opt, meta); err != nil {
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

func TestSyntheticProgression(t *testing.T) {
	db, meta, err := buildProgressionSynthetic(t, progressionSource(t))
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.item_use"] != "4" || meta["typed.action_response"] != "4" || meta["typed.reward_table"] != "4" || meta["typed.reward_table_modelled"] != "3" ||
		meta["typed.reward_xp_modelled"] != "1" || meta["typed.leveling_type"] != "2" || meta["typed.skill_auto_grant"] != "2" {
		t.Errorf("meta = %v", meta)
	}

	// item_use: the archetype's activation and consumption, the actions in order
	u, err := db.ItemUse(synthLevelScroll)
	if err != nil || u.Activation != "ActivateActionResponses" || !u.ConsumeOnUse || u.Cooldown != 0 || u.DisabledInCombat || len(u.Actions) != 1 || u.Actions[0] != synthActionLevel {
		t.Errorf("the level scroll's use = %+v, %v", u, err)
	}
	if u, err := db.ItemUse(synthClassScroll); err != nil || u.ConsumeOnUse || u.Cooldown != 5 || !u.DisabledInCombat {
		t.Errorf("the class scroll's use = %+v, %v", u, err)
	}
	if _, err := db.ItemUse(8100000000000000005); !content.IsNoRow(err) {
		t.Errorf("an item with no archetype: %v, want no row", err)
	}

	// action_response
	a, err := db.ActionResponse(synthActionClass)
	if err != nil || a.ResponseType != "SetPrimaryArchetype" || a.ClassGuid != 100 || a.ClassName != "Bard" || len(a.RewardTables) != 0 {
		t.Errorf("SetPrimaryArchetype = %+v, %v", a, err)
	}
	a, err = db.ActionResponse(synthActionLevel)
	if err != nil || a.ResponseType != "GiveItem" || a.ClassGuid != 0 || len(a.RewardTables) != 2 || a.RewardTables[0] != synthTableLevel || a.RewardTables[1] != synthTableRandom || !a.RemoveUpToQuantity {
		t.Errorf("GiveItem = %+v, %v", a, err)
	}
	if _, err := db.ActionResponse(8100000000000000029); !content.IsNoRow(err) {
		t.Errorf("an action no item names is written: %v", err)
	}

	// reward tables: the level scroll's (modelled: the formula's parameters), the gear's (items), the random one (not modelled)
	lt, err := db.RewardTable(synthTableLevel)
	if err != nil || !lt.Modelled || lt.LevelBelow != 3 || lt.Predicate != "GetPlayerLevel() < 3" || len(lt.Items) != 0 || len(lt.Experience) != 2 {
		t.Fatalf("the level table = %+v, %v", lt, err)
	}
	if x := lt.Experience[0]; x.ExperienceType != "Adventuring" || x.TargetLevel != 3 || x.Margin != 10 || x.CurveGuid != synthLevelCurve || x.MultStatGuid != synthMultStat || !strings.Contains(x.Expression, "GetXPDebtRatio") {
		t.Errorf("the Adventuring reward = %+v", x)
	}
	if x := lt.Experience[1]; x.ExperienceType != "Weapon" || x.TargetLevel != 0 {
		t.Errorf("the Weapon reward (not the formula) = %+v", x)
	}
	gt, err := db.RewardTable(synthTableGear)
	if err != nil || !gt.Modelled || gt.LevelBelow != 0 || len(gt.Items) != 2 || gt.Items[0].Quantity != 1 || gt.Items[1].Quantity != 20 || gt.Items[1].ItemGuid != 8100000000000000005 {
		t.Errorf("the gear table = %+v, %v", gt, err)
	}
	rt, err := db.RewardTable(synthTableRandom)
	if err != nil || rt.Modelled || rt.LevelBelow != -1 || len(rt.Items) != 0 {
		t.Errorf("the random table = %+v, %v (want not modelled, an unmodelled predicate, no rows)", rt, err)
	}
	bt, err := db.RewardTable(synthTableBadXP)
	if err != nil || !bt.Modelled || len(bt.Experience) != 1 || bt.Experience[0].TargetLevel != 0 {
		t.Errorf("a table whose expression is not the formula = %+v, %v (want target_level 0)", bt, err)
	}
	if _, err := db.RewardTable(8100000000000000039); !content.IsNoRow(err) {
		t.Errorf("a table no item's action names is written: %v", err)
	}

	// leveling types and curves
	adv, err := db.LevelingTypeByName("Adventuring")
	if err != nil || adv.Guid != synthAdventuring || adv.LevelCap != 3 || !adv.UsesDebt || adv.XPMultStatGuid != synthMultStat || adv.ExperienceType != "Adventuring" {
		t.Fatalf("Adventuring = %+v, %v", adv, err)
	}
	if len(adv.Curve) != 4 || adv.Curve[0] != (content.LevelXP{Level: 1, XP: 0}) || adv.Curve[3] != (content.LevelXP{Level: 4, XP: 11500}) {
		t.Errorf("the Adventuring curve = %v", adv.Curve)
	}
	wt, err := db.LevelingTypeByName("Weapon_Test")
	if err != nil || wt.LevelCap != 0 || wt.UsesDebt || wt.XPMultStatGuid != 0 || len(wt.Curve) != 2 || wt.Curve[1] != (content.LevelXP{Level: 2, XP: 401}) {
		t.Errorf("the weapon type (a level-0 key left out, 400.5 rounded) = %+v, %v", wt, err)
	}

	// the auto grants of the Universal tree, by level, with the StamBoost's constant mod and not the formula mod
	grants, err := db.AutoGrants(synthAdventuring)
	if err != nil || len(grants) != 2 {
		t.Fatalf("auto grants = %+v, %v", grants, err)
	}
	if g := grants[0]; g.TreeGuid != synthUniversal || g.TreeName != "Universal" || g.NodeGuid != synthNodeSprint || g.LevelRequirement != 5 || g.SkillGuid != synthSkillSprint ||
		g.RankGuid != synthRankSprint || g.EffectGuid != synthEffectSprint || len(g.Mods) != 0 {
		t.Errorf("Sprint_1 = %+v", g)
	}
	if g := grants[1]; g.NodeGuid != synthNodeStam || g.LevelRequirement != 10 || g.RankGuid != synthRankStam || len(g.Mods) != 1 || g.Mods[0].StatGuid != content.GuidMaxStamina ||
		g.Mods[0].Value != 10 || g.Mods[0].StatName != "MaxStamina" {
		t.Errorf("StamBoost_1 = %+v", g)
	}
	if other, err := db.AutoGrants(synthWeaponType); err != nil || len(other) != 0 {
		t.Errorf("another leveling type's grants = %v, %v", other, err)
	}
}

// ~ C_Y_B :: C.O.A ~

// The model check: the formula with other debts, multipliers and levels, and expressions that are not it.
func TestLevelScrollModel(t *testing.T) {
	src := progressionSource(t)
	if _, _, err := buildProgressionSynthetic(t, src); err != nil {
		t.Fatal(err)
	}
	curves := src.curves
	m, err := levelScrollModel(levelScrollExpr("4.0"), curves)
	if err != nil || m.target != 4 || m.margin != 10 || m.curve != synthLevelCurve || m.multStat != synthMultStat {
		t.Errorf("the formula = %+v, %v", m, err)
	}
	for name, expr := range map[string]string{
		"another constant":       strings.Replace(levelScrollExpr("2.0"), "+ 10", "+ 25", 1),
		"no debt":                strings.Replace(levelScrollExpr("2.0"), "+ xpDebt", "", 1),
		"no multiplier":          strings.Replace(levelScrollExpr("2.0"), "/mult", "", 1),
		"the weapon form":        "EvalCurve(" + refText(content.TypeStatCurve, synthLevelCurve) + ", 3.0) - EvalCurve(" + refText(content.TypeStatCurve, synthLevelCurve) + ", float(GetPlayerLevel())) + 10",
		"a curve that is none":   strings.ReplaceAll(levelScrollExpr("2.0"), itoa(synthLevelCurve), "77"),
		"no curve at all":        "GetPlayerLevel() * 100",
		"a stat of another type": strings.Replace(levelScrollExpr("2.0"), refText(content.TypeStatDef, synthMultStat), refText(7, synthMultStat), 1),
	} {
		m, err := levelScrollModel(expr, curves)
		switch name {
		case "another constant":
			if err != nil || m.margin != 25 {
				t.Errorf("%s: %+v, %v (another constant is still the formula)", name, m, err)
			}
		default:
			if err == nil {
				t.Errorf("%s: accepted as the formula (%+v)", name, m)
			}
		}
	}
}

// The real design database: the Adventuring curve, the level scrolls, the Universal tree.
func TestRealProgression(t *testing.T) {
	db := builtDB(t)
	adv, err := db.LevelingTypeByName("Adventuring")
	if err != nil {
		t.Fatal(err)
	}
	if adv.Guid != content.GuidAdventuring || adv.LevelCap != 25 || !adv.UsesDebt || adv.XPMultStatGuid != content.GuidAdventuringXPAddMult {
		t.Errorf("Adventuring = %+v", adv)
	}
	want := map[int]int64{1: 0, 2: 300, 10: 418400, 15: 1853000, 20: 6146400, 25: 14295400, 26: 16563400}
	if len(adv.Curve) != 26 {
		t.Errorf("the Adventuring curve has %d levels, want 26", len(adv.Curve))
	}
	for _, k := range adv.Curve {
		if xp, ok := want[k.Level]; ok && xp != k.XP {
			t.Errorf("level %d at %d XP, want %d", k.Level, k.XP, xp)
		}
	}
	for _, name := range []string{"GM_Scroll_MakeMeLevel10", "GM_Scroll_MakeMeLevel15", "GM_Scroll_MakeMeLevel20", "GM_Scroll_MakeMeLevel25"} {
		it, err := db.ItemByName(name)
		if err != nil {
			t.Fatal(err)
		}
		u, err := db.ItemUse(it.Guid)
		if err != nil || !u.ConsumeOnUse || len(u.Actions) != 1 {
			t.Fatalf("%s: use %+v, %v", name, u, err)
		}
		a, err := db.ActionResponse(u.Actions[0])
		if err != nil || a.ResponseType != "GiveItem" || len(a.RewardTables) != 1 {
			t.Fatalf("%s: action %+v, %v", name, a, err)
		}
		rt, err := db.RewardTable(a.RewardTables[0])
		if err != nil || !rt.Modelled || len(rt.Experience) != 2 {
			t.Fatalf("%s: table %+v, %v", name, rt, err)
		}
		target := int(name[len(name)-2]-'0')*10 + int(name[len(name)-1]-'0')
		if x := rt.Experience[0]; x.ExperienceType != "Adventuring" || x.TargetLevel != target || x.Margin != 10 || x.CurveGuid != 6064630643828785675 || x.MultStatGuid != content.GuidAdventuringXPAddMult {
			t.Errorf("%s: the Adventuring reward = %+v", name, x)
		}
		if rt.Experience[1].ExperienceType != "Weapon" || rt.Experience[1].TargetLevel != 0 {
			t.Errorf("%s: the Weapon reward = %+v (not the formula: no multiplier, no debt)", name, rt.Experience[1])
		}
		if (target < 25 && rt.LevelBelow != target) || (target == 25 && rt.LevelBelow != 0) {
			t.Errorf("%s: level_below %d", name, rt.LevelBelow)
		}
	}
	for _, class := range []string{"Bard", "Tank"} {
		it, err := db.ItemByName("GM_Scroll_PrimaryArchetype" + class)
		if err != nil {
			t.Fatal(err)
		}
		u, _ := db.ItemUse(it.Guid)
		if len(u.Actions) != 1 {
			t.Fatalf("%s scroll: %+v", class, u)
		}
		a, err := db.ActionResponse(u.Actions[0])
		if err != nil || a.ResponseType != "SetPrimaryArchetype" || a.ClassName != class || a.ClassGuid == 0 {
			t.Errorf("%s scroll: %+v, %v", class, a, err)
		}
	}
	gear, err := db.ItemByName("GM_Scroll_Gear 01 (Light ) Starter")
	if err != nil {
		t.Fatal(err)
	}
	u, _ := db.ItemUse(gear.Guid)
	a, _ := db.ActionResponse(u.Actions[0])
	rt, err := db.RewardTable(a.RewardTables[0])
	if err != nil || !rt.Modelled || len(rt.Items) != 19 || len(rt.Experience) != 0 {
		t.Errorf("the gear 01 scroll's table: %d items, %d xp, modelled %v (%v)", len(rt.Items), len(rt.Experience), rt.Modelled, err)
	}
	grants, err := db.AutoGrants(content.GuidAdventuring)
	if err != nil {
		t.Fatal(err)
	}
	universal := map[int]string{}
	for _, g := range grants {
		if g.TreeGuid == content.GuidUniversalTree {
			universal[g.LevelRequirement] = g.NodeName
			if g.NodeName == "General_Stamina_Passive_StamBoost_1" && (len(g.Mods) != 1 || g.Mods[0].StatGuid != content.GuidMaxStamina || g.Mods[0].Value != 10) {
				t.Errorf("StamBoost_1's mods = %+v, want MaxStamina +10", g.Mods)
			}
		}
	}
	if universal[5] != "General_Stamina_Passive_Sprint_1" || universal[10] != "General_Stamina_Passive_StamBoost_1" || universal[20] != "General_Stamina_Passive_Sprint_2" ||
		universal[35] != "General_Stamina_Passive_Sprint_3" || len(universal) != 8 {
		t.Errorf("the Universal tree's auto grants by level = %v", universal)
	}
}

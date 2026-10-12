// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Plan D1, stage 2: the talents (skill trees, nodes, ranks, costs, points, weapon items) on synthetic rows
// (TestSyntheticTalents) and on the real design database (TestRealTalents, with AOC_DESIGN_DB).

import (
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

const (
	tClassTree     = 3 // the synthetic classes' ActiveSkillTreeId (the last class row wins: Tank)
	tWeaponTree    = 8200000000000000001
	tGatherTree    = 8200000000000000002
	tStageTree     = 8200000000000000003
	tGatherType    = 8200000000000000010
	tWeaponLine    = 8200000000000000020
	tNodeAuto      = 8200000000000000030 // an auto-granted active ability, on the bar
	tNodeBuy       = 8200000000000000031 // needs the auto skill and 1 point spent; excludes tNodeRival
	tNodeRival     = 8200000000000000032
	tNodePassive   = 8200000000000000033 // an infinite passive with a constant mod, level 5
	tNodeNoSkill   = 8200000000000000034
	tNodeMulti     = 8200000000000000035 // a skill of two ranks
	tNodeWeapon    = 8200000000000000036
	tNodeGather    = 8200000000000000037
	tNodeBroken    = 8200000000000000038 // names a requirement that is no row
	tNodeAny       = 8200000000000000039 // two requirements, any of them
	tNodeSlot      = 8200000000000000110 // no skill of its own: a talent slot offering tSkillOptA and tSkillOptB (the second also has a second rank)
	tNodeNeedSlot  = 8200000000000000111 // needs the slot node, no option named: any option
	tNodeNeedOpt   = 8200000000000000112 // needs the slot node and names the option tSkillOptB (any option still meets it)
	tNodeEmptySlot = 8200000000000000113 // a slot that offers nothing
	tSlot          = 8200000000000000114
	tSlotEmpty     = 8200000000000000115
	tSkillOptA     = 8200000000000000116
	tSkillOptB     = 8200000000000000117
	tRankOptA      = 8200000000000000118
	tRankOptB1     = 8200000000000000119
	tRankOptB2     = 8200000000000000120
	tReqSlotAny    = 8200000000000000121
	tReqSlotOpt    = 8200000000000000122
	tSkillAuto     = 8200000000000000040
	tSkillBuy      = 8200000000000000041
	tSkillRival    = 8200000000000000042
	tSkillPassive  = 8200000000000000043
	tSkillMulti    = 8200000000000000044
	tSkillWeapon   = 8200000000000000045
	tSkillGather   = 8200000000000000046
	tRankAuto      = 8200000000000000050
	tRankBuy       = 8200000000000000051
	tRankRival     = 8200000000000000052
	tRankPassive   = 8200000000000000053
	tRankMulti1    = 8200000000000000054
	tRankMulti2    = 8200000000000000055
	tRankWeapon    = 8200000000000000056
	tRankGather    = 8200000000000000057
	tAbilityAuto   = 8200000000000000060
	tAbilityBuy    = 8200000000000000061
	tEffectPassive = 8200000000000000070
	tEffectMarker  = 8200000000000000071
	tEffectFormula = 8200000000000000072
	tModPassive    = 8200000000000000080
	tReqBuy        = 8200000000000000090
	tReqAny1       = 8200000000000000091
	tReqAny2       = 8200000000000000092
	tReqEmpty      = 8200000000000000093
	tItemStaff     = 8200000000000000100
	tItemRock      = 8200000000000000101
)

// ~ C O A & Cyb ~

func skillRow(guid int64, name string, ranks ...int64) designdb.Row {
	var rs list
	for _, r := range ranks {
		rs = append(rs, idv(r, content.TypeSkillRank, "None"))
	}
	return mkRow(guid, name, st{"SkillRanksIds": rs})
}

func rankRow(guid int64, name string, ability, effect int64, costs ...st) designdb.Row {
	var cs list
	for _, c := range costs {
		cs = append(cs, c)
	}
	return mkRow(guid, name, st{"AbilityIdId": idv(ability, content.TypeAbility, "None"), "EffectIdId": idv(effect, content.TypeEffect, "None"),
		"SkillCost": st{"SkillPointCosts": cs, "CurrencyCosts": list{}, "CommodityCosts": list{}}})
}

func richNode(guid int64, name string, skill int64, level, points int64, auto, hotbar, all bool, required []int64, exclusive []int64) designdb.Row {
	var req, ex list
	for _, r := range required {
		req = append(req, st{"SkillRequirementId": idv(r, content.TypeSkillRequirement, "None"), "bDrawPathFromRequirement": true, "OverridePathFromRequirement": list{}})
	}
	for _, e := range exclusive {
		ex = append(ex, idv(e, content.TypeSkill, "None"))
	}
	return mkRow(guid, name, st{"SkillId": idv(skill, content.TypeSkill, "None"), "SkillSlotId": idv(0, content.TypeSkillTreeSlot, "None"), "LevelRequirement": level, "PointRequirement": points, "bAutoGranted": auto,
		"bAutoAddToHotbar": hotbar, "bAllAreRequired": all, "SkillsRequired": req, "MutuallyExclusiveSkillsIds": ex})
}

// slotNode is a node with no SkillId: its SkillSlotId names a slot of skills to pick from.
func slotNode(guid int64, name string, slot int64, level, points int64, hotbar bool) designdb.Row {
	return mkRow(guid, name, st{"SkillId": idv(0, content.TypeSkill, "None"), "SkillSlotId": idv(slot, content.TypeSkillTreeSlot, "None"), "LevelRequirement": level,
		"PointRequirement": points, "bAutoGranted": false, "bAutoAddToHotbar": hotbar, "bAllAreRequired": false, "SkillsRequired": list{}, "MutuallyExclusiveSkillsIds": list{}})
}

func treeRow(guid int64, name string, levelType int64, nodes ...int64) designdb.Row {
	var ns list
	for _, n := range nodes {
		ns = append(ns, st{"TreeNodeId": idv(n, content.TypeSkillTreeNode, "None"), "X": int64(0), "Y": int64(0)})
	}
	return mkRow(guid, name, st{"LevelTypeId": idv(levelType, content.TypeLevelingTypeDef, "None"), "Skills": ns})
}

// talentsSource is progressionSource plus a class tree, a weapon tree with its line and two items, and a gathering tree.
func talentsSource(t *testing.T) *source {
	t.Helper()
	src := progressionSource(t)
	src.rows["SkillTreeRecordBase"] = append(src.rows["SkillTreeRecordBase"],
		treeRow(tClassTree, "ClassTree", synthAdventuring, tNodeAuto, tNodeBuy, tNodeRival, tNodePassive, tNodeNoSkill, tNodeMulti, tNodeBroken, tNodeAny,
			tNodeSlot, tNodeNeedSlot, tNodeNeedOpt, tNodeEmptySlot),
		treeRow(tWeaponTree, "Weapon_Test", synthWeaponType, tNodeWeapon),
		treeRow(tGatherTree, "Gathering_Test", tGatherType, tNodeGather),
		treeRow(tStageTree, "ClassTree_Stage", synthAdventuring, tNodeAuto))
	src.rows["LevelingTypeDefRecordBase"] = append(src.rows["LevelingTypeDefRecordBase"],
		mkRow(tGatherType, "LevelingTypeDef_Gathering_Test", st{"ExperienceType": "EExperienceType::Gathering", "LevelCapOverride": int64(0), "bUsesExperienceDebt": false,
			"XPMultiplier": "", "Levels": list{}, "LevelUpCurve": st{"ExternalCurve": "", "EditorCurveData": st{"DefaultValue": float32(0), "Keys": curveKeys(0, 100)}}}))
	src.rows["SkillTreeNodeRecordBase"] = append(src.rows["SkillTreeNodeRecordBase"],
		richNode(tNodeAuto, "Test_Auto", tSkillAuto, 1, 0, true, true, false, nil, nil),
		richNode(tNodeBuy, "Test_Buy", tSkillBuy, 2, 1, false, true, true, []int64{tReqBuy}, []int64{tSkillRival}),
		richNode(tNodeRival, "Test_Rival", tSkillRival, 0, 0, false, false, false, nil, nil),
		richNode(tNodePassive, "Test_Passive", tSkillPassive, 5, 0, false, false, false, nil, nil),
		richNode(tNodeNoSkill, "Test_Empty", 0, 0, 0, false, false, false, nil, nil),
		richNode(tNodeMulti, "Test_Multi", tSkillMulti, 0, 0, false, true, false, []int64{tReqEmpty}, nil),
		richNode(tNodeBroken, "Test_Broken", tSkillRival, 0, 0, false, false, false, []int64{8200000000000000999}, nil),
		richNode(tNodeAny, "Test_Any", tSkillRival, 0, 0, false, false, false, []int64{tReqAny1, tReqAny2}, nil),
		slotNode(tNodeSlot, "Test_Slot", tSlot, 3, 2, true),
		richNode(tNodeNeedSlot, "Test_NeedSlot", tSkillPassive, 0, 0, false, false, false, []int64{tReqSlotAny}, nil),
		richNode(tNodeNeedOpt, "Test_NeedOpt", tSkillPassive, 0, 0, false, false, false, []int64{tReqSlotOpt}, nil),
		slotNode(tNodeEmptySlot, "Test_EmptySlot", tSlotEmpty, 0, 0, false),
		richNode(tNodeWeapon, "Test_Weapon", tSkillWeapon, 0, 0, false, true, false, nil, nil),
		richNode(tNodeGather, "Test_Gather", tSkillGather, 0, 0, true, false, false, nil, nil))
	src.rows["SkillRequirementRecordBase"] = []designdb.Row{
		mkRow(tReqBuy, "Test_Req", st{"PointRequirement": int64(1), "SkillRecordId": idv(tSkillAuto, content.TypeSkill, "None"), "SkillTreeNodeRecordId": idv(tNodeAuto, content.TypeSkillTreeNode, "None")}),
		mkRow(tReqAny1, "Test_Any1", st{"PointRequirement": int64(1), "SkillRecordId": idv(0, content.TypeSkill, "None"), "SkillTreeNodeRecordId": idv(tNodeBuy, content.TypeSkillTreeNode, "None")}),
		mkRow(tReqAny2, "Test_Any2", st{"PointRequirement": int64(1), "SkillRecordId": idv(tSkillPassive, content.TypeSkill, "None"), "SkillTreeNodeRecordId": idv(0, content.TypeSkillTreeNode, "None")}),
		mkRow(tReqSlotAny, "Test_SlotAny", st{"PointRequirement": int64(1), "SkillRecordId": idv(0, content.TypeSkill, "None"), "SkillTreeNodeRecordId": idv(tNodeSlot, content.TypeSkillTreeNode, "None")}),
		mkRow(tReqSlotOpt, "Test_SlotOpt", st{"PointRequirement": int64(1), "SkillRecordId": idv(tSkillOptB, content.TypeSkill, "None"), "SkillTreeNodeRecordId": idv(tNodeSlot, content.TypeSkillTreeNode, "None")}),
		mkRow(tReqEmpty, "Test_Empty", st{"PointRequirement": int64(1), "SkillRecordId": idv(0, content.TypeSkill, "None"), "SkillTreeNodeRecordId": idv(0, content.TypeSkillTreeNode, "None")}),
	}
	src.rows["SkillRecordBase"] = append(src.rows["SkillRecordBase"],
		skillRow(tSkillAuto, "Test_Auto", tRankAuto), skillRow(tSkillBuy, "Test_Buy", tRankBuy), skillRow(tSkillRival, "Test_Rival", tRankRival),
		skillRow(tSkillPassive, "Test_Passive", tRankPassive), skillRow(tSkillMulti, "Test_Multi", tRankMulti1, tRankMulti2),
		skillRow(tSkillWeapon, "Test_Weapon", tRankWeapon), skillRow(tSkillGather, "Test_Gather", tRankGather),
		skillRow(tSkillOptA, "Test_OptA", tRankOptA), skillRow(tSkillOptB, "Test_OptB", tRankOptB1, tRankOptB2))
	src.rows["SkillTreeSlotRecordBase"] = []designdb.Row{
		mkRow(tSlot, "Test_SlotRecord", st{"SkillsIds": list{idv(tSkillOptA, content.TypeSkill, "None"), idv(tSkillOptB, content.TypeSkill, "None"),
			idv(8200000000000000999, content.TypeSkill, "None")}}),
		mkRow(tSlotEmpty, "Test_EmptySlotRecord", st{"SkillsIds": list{}}),
	}
	arch := st{"Quantity": int64(1), "SkillPointTypeId": idv(synthPointArchetype, content.TypeSkillPointType, "None")}
	weapon2 := st{"Quantity": int64(2), "SkillPointTypeId": idv(synthPointWeapon, content.TypeSkillPointType, "None")}
	src.rows["SkillRankRecordBase"] = append(src.rows["SkillRankRecordBase"],
		rankRow(tRankAuto, "Test_Auto", tAbilityAuto, 0),
		rankRow(tRankBuy, "Test_Buy", tAbilityBuy, 0, arch),
		rankRow(tRankRival, "Test_Rival", 0, tEffectMarker, arch),
		rankRow(tRankPassive, "Test_Passive", 0, tEffectPassive, arch),
		rankRow(tRankMulti1, "Test_Multi_1", 0, tEffectFormula, arch),
		rankRow(tRankMulti2, "Test_Multi_2", 0, 0, arch, arch),
		rankRow(tRankWeapon, "Test_Weapon", 0, tEffectMarker, weapon2),
		rankRow(tRankGather, "Test_Gather", tAbilityAuto, 0),
		rankRow(tRankOptA, "Test_OptA", tAbilityBuy, 0, arch),
		rankRow(tRankOptB1, "Test_OptB_1", tAbilityAuto, 0, arch),
		rankRow(tRankOptB2, "Test_OptB_2", 0, tEffectPassive, arch))
	src.rows["AoCAbilityRecordBase"] = append(src.rows["AoCAbilityRecordBase"],
		mkRow(tAbilityAuto, "Test_Auto_Ability", st{}), mkRow(tAbilityBuy, "Test_Buy_Ability", st{}))
	src.rows["EffectRecordBase"] = append(src.rows["EffectRecordBase"],
		effectRow(tEffectPassive, "Test_Passive_Effect", st{"EffectDuration": "", "StatModsIds": list{modID(tModPassive), modID(synthModFormula)}}),
		effectRow(tEffectMarker, "Test_Marker_Effect", st{"EffectDuration": "0.1", "StatModsIds": list{modID(synthModStam)}}),
		effectRow(tEffectFormula, "Test_Formula_Effect", st{"EffectDuration": "EvalFormula(" + refText(content.TypeStatCurve, synthLevelCurve) + ")"}))
	src.rows["StatModRecordBase"] = append(src.rows["StatModRecordBase"],
		mkRow(tModPassive, "Test_Passive_Mod", st{"StatRefId": statID(content.GuidMaxStamina), "Value": "7.5"}))
	src.rows["WeaponSkillLinesRecordBase"] = []designdb.Row{mkRow(tWeaponLine, "Weapon_Test", st{"SkillTreeId": idv(tWeaponTree, content.TypeSkillTree, "None")})}
	staff := mkRow(tItemStaff, "Test_Staff", st{"CanEquip": true, "OccupiesAllEquipSlots": false, "Level": int64(1), "DefaultQuality": int64(1), "MaxStackSize": int64(1),
		"StatBlockId": idv(0, content.TypeItemStatBlock, "None"), "QualityCurveId": idv(0, content.TypeQualityCurve, "None"),
		"AppearanceId": idv(0, 7961510888716503441, "None"), "EquipSlots": list{}, "SetBonusIds": list{}, "GameplayTags": list{},
		"ActivationArchetypeId": idv(0, content.TypeItemActivationArchetype, "None"), "ActivationActionsIds": list{},
		"AttackAbilityId": idv(tAbilityAuto, content.TypeAbility, "None"), "WeaponSkillLineId": idv(tWeaponLine, content.TypeWeaponSkillLines, "None")})
	src.rows["ItemRecordBase"] = append(src.rows["ItemRecordBase"], staff)
	return src
}

// CoA / C.Y.B

func TestSyntheticTalents(t *testing.T) {
	db, meta, err := buildProgressionSynthetic(t, talentsSource(t))
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.skill_tree"] == "" || meta["typed.item_weapon"] != "1" {
		t.Errorf("meta = %v", meta)
	}

	// the kinds
	kinds := map[string]string{}
	trees, err := db.SkillTrees()
	if err != nil {
		t.Fatal(err)
	}
	for _, tr := range trees {
		kinds[tr.Name] = tr.Kind
	}
	if kinds["ClassTree"] != content.TreeClass || kinds["Universal"] != "other" || kinds["Weapon_Test"] != content.TreeWeapon || kinds["Gathering_Test"] != content.TreeProfession ||
		kinds["ClassTree_Stage"] != content.TreeOther {
		// (the synthetic Universal tree has another guid than the real one: it is 'other' here)
		t.Errorf("kinds = %v", kinds)
	}
	class, _ := db.SkillTreeByName("ClassTree")
	if class.Guid != tClassTree || class.ClassGuid == 0 || class.LevelingTypeGuid != synthAdventuring {
		t.Errorf("the class tree = %+v", class)
	}
	if w, _ := db.SkillTreeByName("Weapon_Test"); w.WeaponLineGuid != tWeaponLine || w.LevelingTypeGuid != synthWeaponType {
		t.Errorf("the weapon tree = %+v", w)
	}

	// the nodes, in the tree's order, a node without a skill left out
	nodes, err := db.SkillNodes(tClassTree)
	if err != nil {
		t.Fatal(err)
	}
	by := map[string]content.SkillNode{}
	var order []string
	for _, n := range nodes {
		by[n.Name] = n
		order = append(order, n.Name)
	}
	// a slot node is listed once per option (the option whose skill is no row is left out); a node with no skill and no slot, and a slot that offers
	// nothing, are left out
	if strings.Join(order, ",") != "Test_Auto,Test_Buy,Test_Rival,Test_Passive,Test_Multi,Test_Broken,Test_Any,Test_Slot,Test_Slot,Test_NeedSlot,Test_NeedOpt" {
		t.Errorf("the nodes = %v (a node that names no skill is left out, a slot node is listed once per option)", order)
	}
	var slotRows []content.SkillNode
	for _, n := range nodes {
		if n.NodeGuid == tNodeSlot {
			slotRows = append(slotRows, n)
		}
	}
	if len(slotRows) != 2 || slotRows[0].SkillGuid != tSkillOptA || slotRows[1].SkillGuid != tSkillOptB || slotRows[0].Opt != 0 || slotRows[1].Opt != 1 ||
		slotRows[0].SlotGuid != tSlot || slotRows[1].SlotGuid != tSlot || slotRows[0].Ord != slotRows[1].Ord || slotRows[0].LevelRequirement != 3 || slotRows[1].PointRequirement != 2 ||
		!slotRows[0].AutoHotbar {
		t.Errorf("the slot node's rows = %+v", slotRows)
	}
	if n := by["Test_NeedSlot"]; len(n.Requires) != 0 || len(n.RequiresNodes) != 1 || n.RequiresNodes[0] != tNodeSlot {
		t.Errorf("a requirement that names a slot node and no option is met by any option: %+v", n)
	}
	if n := by["Test_NeedOpt"]; len(n.Requires) != 0 || len(n.RequiresNodes) != 1 || n.RequiresNodes[0] != tNodeSlot {
		t.Errorf("a requirement that names a slot node and one of its options is still met by any option (retail accepted a build with the other one): %+v", n)
	}
	if r, _ := db.SkillRanks(tSkillOptB); len(r) != 2 || r[1].RankGuid != tRankOptB2 {
		t.Errorf("a slot option's ranks = %+v", r)
	}
	if n := by["Test_Auto"]; !n.AutoGranted || !n.AutoHotbar || n.LevelRequirement != 1 || n.SkillGuid != tSkillAuto || len(n.Requires) != 0 {
		t.Errorf("Test_Auto = %+v", n)
	}
	if n := by["Test_Buy"]; n.AutoGranted || !n.AllRequired || n.PointRequirement != 1 || n.LevelRequirement != 2 || len(n.Requires) != 1 || n.Requires[0] != tSkillAuto ||
		len(n.Exclusive) != 1 || n.Exclusive[0] != tSkillRival {
		t.Errorf("Test_Buy = %+v (it needs the auto node's SKILL, one point spent, level 2; it excludes the rival)", n)
	}
	if n := by["Test_Multi"]; len(n.Requires) != 1 || n.Requires[0] != 0 {
		t.Errorf("a requirement that names nothing is written with skill 0 (met): %+v", n)
	}
	if n := by["Test_Broken"]; len(n.Requires) != 1 || n.Requires[0] != 0 {
		t.Errorf("a requirement that is no row is written with skill 0: %+v", n)
	}
	if n := by["Test_Any"]; n.AllRequired || len(n.Requires) != 2 || n.Requires[0] != tSkillBuy || n.Requires[1] != tSkillPassive {
		t.Errorf("Test_Any = %+v (one requirement by node, one by skill)", n)
	}

	// the ranks: abilities, effects, durations, costs
	r, err := db.SkillRanks(tSkillAuto)
	if err != nil || len(r) != 1 || r[0].RankGuid != tRankAuto || r[0].AbilityGuid != tAbilityAuto || r[0].AbilityName != "Test_Auto_Ability" || r[0].EffectGuid != 0 || len(r[0].Costs) != 0 {
		t.Errorf("Test_Auto's rank = %+v, %v", r, err)
	}
	r, _ = db.SkillRanks(tSkillBuy)
	if len(r) != 1 || r[0].AbilityGuid != tAbilityBuy || len(r[0].Costs) != 1 || r[0].Costs[0] != (content.RankCost{PointTypeGuid: synthPointArchetype, Quantity: 1}) {
		t.Errorf("Test_Buy's rank = %+v", r)
	}
	r, _ = db.SkillRanks(tSkillPassive)
	if len(r) != 1 || r[0].EffectGuid != tEffectPassive || !r[0].EffectInfinite || r[0].EffectDuration != 0 || r[0].EffectName != "Test_Passive_Effect" {
		t.Errorf("Test_Passive's rank = %+v (an effect with no duration is infinite)", r)
	}
	r, _ = db.SkillRanks(tSkillRival)
	if len(r) != 1 || r[0].EffectInfinite || r[0].EffectDuration != 0.1 {
		t.Errorf("Test_Rival's rank = %+v (a 0.1 s marker)", r)
	}
	r, _ = db.SkillRanks(tSkillMulti)
	if len(r) != 2 || r[0].Ord != 0 || r[1].Ord != 1 || r[0].RankGuid != tRankMulti1 || r[1].RankGuid != tRankMulti2 || !r[0].EffectInfinite || len(r[1].Costs) != 2 {
		t.Errorf("Test_Multi's ranks = %+v (two in order; a formula duration is infinite; two cost entries)", r)
	}
	r, _ = db.SkillRanks(tSkillWeapon)
	if len(r) != 1 || len(r[0].Costs) != 1 || r[0].Costs[0] != (content.RankCost{PointTypeGuid: synthPointWeapon, Quantity: 2}) {
		t.Errorf("Test_Weapon's rank = %+v", r)
	}

	// the constant mods of an infinite effect, a non-auto rank's too; a finite effect's none; a formula's none
	if m, _ := db.RankStatMods(tRankPassive); len(m) != 1 || m[0].StatGuid != content.GuidMaxStamina || m[0].Value != 7.5 {
		t.Errorf("Test_Passive's mods = %+v (the constant one, not the formula)", m)
	}
	if m, _ := db.RankStatMods(tRankRival); len(m) != 0 {
		t.Errorf("a marker's mods = %+v, want none", m)
	}

	// the point types, and what each level gives
	pts, err := db.SkillPointTypes()
	if err != nil || pts["Combat_Archetype"] != synthPointArchetype || pts["Combat_Stamina"] != synthPointStamina || pts["Combat_Weapon"] != synthPointWeapon {
		t.Errorf("point types = %v, %v", pts, err)
	}
	if got, _ := db.LevelingPoints(synthAdventuring, 1); len(got) != 0 {
		t.Errorf("level 1 gives %v", got)
	}
	if got, _ := db.LevelingPoints(synthAdventuring, 2); got[synthPointArchetype] != 1 || got[synthPointStamina] != 1 || len(got) != 2 {
		t.Errorf("level 2 gives %v (an Archetype and a Stamina point)", got)
	}
	if got, _ := db.LevelingPoints(synthAdventuring, 3); got[synthPointArchetype] != 2 || len(got) != 1 {
		t.Errorf("level 3 gives %v", got)
	}
	if got, _ := db.LevelingPoints(synthWeaponType, 1); got[synthPointWeapon] != 1 {
		t.Errorf("the weapon line's level 1 gives %v", got)
	}

	// the weapon item
	w, err := db.ItemWeapon(tItemStaff)
	if err != nil || w.AttackAbilityGuid != tAbilityAuto || w.SkillLineGuid != tWeaponLine || w.SkillLineName != "Weapon_Test" {
		t.Errorf("the staff = %+v, %v", w, err)
	}
	if _, err := db.ItemWeapon(8100000000000000005); !content.IsNoRow(err) {
		t.Errorf("an item that names no weapon data: %v, want no row", err)
	}
}

// The real design database: the eight class trees, the Universal tree, the 14 weapon trees, the professions, and the data the stage's
// rules read.
func TestRealTalents(t *testing.T) {
	db := builtDB(t)
	trees, err := db.SkillTrees()
	if err != nil {
		t.Fatal(err)
	}
	kinds := map[string]int{}
	byName := map[string]content.SkillTree{}
	for _, tr := range trees {
		kinds[tr.Kind]++
		byName[tr.Name] = tr
	}
	if kinds[content.TreeClass] != 8 || kinds[content.TreeUniversal] != 1 || kinds[content.TreeWeapon] != 14 || kinds[content.TreeProfession] < 5 {
		t.Errorf("the tree kinds = %v, want 8 class, 1 Universal, 14 weapon trees and the professions", kinds)
	}
	classes, err := db.Classes()
	if err != nil {
		t.Fatal(err)
	}
	for _, c := range classes {
		tr := byName[c.Name]
		if tr.Kind != content.TreeClass || tr.Guid != c.SkillTreeGuid || tr.ClassGuid != c.Guid {
			t.Errorf("class %s: tree %+v", c.Name, tr)
		}
	}
	pts, err := db.SkillPointTypes()
	if err != nil || pts["Combat_Archetype"] == 0 || pts["Combat_Stamina"] == 0 || pts["Combat_Weapon"] == 0 {
		t.Fatalf("point types = %v, %v", pts, err)
	}

	// the points: Adventuring gives an Archetype and a Stamina point for each level from 2 (49 of each by level 50); a weapon line one of Weapon
	// for each level from 2 to 25 (24)
	adv, err := db.LevelingTypeByName("Adventuring")
	if err != nil {
		t.Fatal(err)
	}
	arch, stam := int64(0), int64(0)
	for level := 1; level <= 50; level++ {
		got, err := db.LevelingPoints(adv.Guid, level)
		if err != nil {
			t.Fatal(err)
		}
		arch += got[pts["Combat_Archetype"]]
		stam += got[pts["Combat_Stamina"]]
		if level == 25 && (arch != 24 || stam != 24) {
			t.Errorf("at level 25 the points are %d Archetype, %d Stamina, want 24 each", arch, stam)
		}
	}
	if arch != 49 || stam != 49 {
		t.Errorf("by level 50 the points are %d Archetype, %d Stamina, want 49 each", arch, stam)
	}
	axe, err := db.LevelingTypeByName("Weapon_1H_Axe")
	if err != nil {
		t.Fatal(err)
	}
	weapon := int64(0)
	for level := 1; level <= 25; level++ {
		got, _ := db.LevelingPoints(axe.Guid, level)
		weapon += got[pts["Combat_Weapon"]]
	}
	if weapon != 24 {
		t.Errorf("a weapon line gives %d Weapon points by level 25, want 24", weapon)
	}

	// the Bard: Bard_Rhetoric is auto-granted at level 1 with QuickWit on the bar; Hopeful has one requirement (the Instrument node: a talent slot, so any option of it) and costs an Archetype point
	bard := byName["Bard"]
	nodes, err := db.SkillNodes(bard.Guid)
	if err != nil || len(nodes) < 30 {
		t.Fatalf("the Bard's nodes = %d, %v", len(nodes), err)
	}
	var rhetoric, hopeful *content.SkillNode
	autos := 0
	for i := range nodes {
		if nodes[i].Name == "Bard_Rhetoric" {
			rhetoric = &nodes[i]
		}
		if nodes[i].Name == "Bard_Music_Hopeful" {
			hopeful = &nodes[i]
		}
		if nodes[i].AutoGranted {
			autos++
		}
	}
	if rhetoric == nil || !rhetoric.AutoGranted || !rhetoric.AutoHotbar || rhetoric.LevelRequirement != 1 || hopeful == nil || hopeful.AutoGranted || len(hopeful.Requires) != 0 || len(hopeful.RequiresNodes) != 1 {
		t.Fatalf("Rhetoric %+v, Hopeful %+v", rhetoric, hopeful)
	}
	if autos != 4 {
		t.Errorf("the Bard has %d auto-granted nodes, want 4 (Rhetoric, Flourish, Resonant Weapon, Fortissimo)", autos)
	}
	r, err := db.SkillRanks(rhetoric.SkillGuid)
	if err != nil || len(r) != 1 || r[0].AbilityName != "Bard_Misc_QuickWit" || len(r[0].Costs) != 0 {
		t.Errorf("Rhetoric's rank = %+v, %v (QuickWit, no cost)", r, err)
	}
	r, err = db.SkillRanks(hopeful.SkillGuid)
	if err != nil || len(r) != 1 || r[0].AbilityGuid == 0 || len(r[0].Costs) != 1 || r[0].Costs[0].PointTypeGuid != pts["Combat_Archetype"] || r[0].Costs[0].Quantity != 1 {
		t.Errorf("Hopeful's rank = %+v, %v (an ability, one Archetype point)", r, err)
	}

	// the talents: the nodes that name a SkillTreeSlot instead of a skill are written once per option (a Bard has nine such nodes: five songs, the
	// instrument and three masteries), so every option is a rank the client can buy; the node's own gates are on each row
	slotRows, slotNodes := 0, map[int64]bool{}
	var hymn []content.SkillNode
	for _, n := range nodes {
		if n.SlotGuid != 0 {
			slotRows++
			slotNodes[n.NodeGuid] = true
		}
		if n.Name == "Bard_Song_Hymn" {
			hymn = append(hymn, n)
		}
	}
	if len(slotNodes) != 9 || slotRows <= 9 {
		t.Errorf("the Bard's tree has %d slot nodes in %d rows, want 9 nodes with more than one option", len(slotNodes), slotRows)
	}
	if len(hymn) != 2 || hymn[0].SkillGuid == hymn[1].SkillGuid || hymn[0].SlotGuid == 0 || hymn[0].SlotGuid != hymn[1].SlotGuid || hymn[0].Ord != hymn[1].Ord ||
		hymn[0].Opt != 0 || hymn[1].Opt != 1 || hymn[0].PointRequirement != 1 || !hymn[0].AutoHotbar {
		t.Errorf("the Hymn slot's rows = %+v", hymn)
	}
	for _, h := range hymn {
		if rr, err := db.SkillRanks(h.SkillGuid); err != nil || len(rr) == 0 {
			t.Errorf("a Hymn option (%s) has no ranks: %v", h.SkillName, err)
		}
	}
	// a requirement that names a slot node and no skill of it is met by any option of the node
	anySlot := 0
	for _, tr := range trees {
		tn, err := db.SkillNodes(tr.Guid)
		if err != nil {
			t.Fatal(err)
		}
		for _, n := range tn {
			anySlot += len(n.RequiresNodes)
		}
	}
	if anySlot == 0 {
		t.Errorf("no requirement names a slot node (the Bard's Serenade, Aria and Lullaby need one of the Hymn slot)")
	}

	// the Universal tree: Endurance Training is level 10 and its mod is MaxStamina +10; a bought Stamina rank costs 2 or 3 Stamina points
	uni := byName["Universal"]
	un, err := db.SkillNodes(uni.Guid)
	if err != nil {
		t.Fatal(err)
	}
	stamCosts := map[int64]int{}
	for _, n := range un {
		rr, _ := db.SkillRanks(n.SkillGuid)
		for _, rank := range rr {
			if !n.AutoGranted {
				for _, c := range rank.Costs {
					if c.PointTypeGuid != pts["Combat_Stamina"] {
						t.Errorf("%s costs points of another type: %+v", rank.Name, c)
					}
					stamCosts[c.Quantity]++
				}
			}
			if n.Name == "General_Stamina_Passive_StamBoost_1" {
				m, _ := db.RankStatMods(rank.RankGuid)
				if len(m) != 1 || m[0].StatGuid != content.GuidMaxStamina || m[0].Value != 10 || !rank.EffectInfinite && rank.EffectDuration == 0 {
					t.Errorf("StamBoost_1: mods %+v, rank %+v", m, rank)
				}
			}
		}
	}
	if stamCosts[2] < 10 || stamCosts[3] < 1 {
		t.Errorf("the Universal tree's costs by quantity = %v, want 2 and 3 Stamina points", stamCosts)
	}

	// a weapon tree: Combat_Weapon costs, and a passive weapon rank's marker duration (0.1 s)
	wt := byName["Weapon_1H_Axe"]
	if wt.Kind != content.TreeWeapon || wt.WeaponLineGuid == 0 || wt.LevelingTypeGuid != axe.Guid {
		t.Errorf("the axe tree = %+v", wt)
	}
	// the weapon items: the Bard's scepter has the Scepter line and its combo
	sc, err := db.ItemByName("Gear_Weapon_Scepter_1H_Starter")
	if err != nil {
		t.Fatal(err)
	}
	w, err := db.ItemWeapon(sc.Guid)
	if err != nil || w.SkillLineName != "Weapon_1H_Scepter" || w.AttackAbilityGuid == 0 || w.SkillLineGuid != 6064632329144307267 {
		t.Errorf("the starter scepter = %+v, %v", w, err)
	}
	// the profession trees' automatic ranks the join lists
	for _, name := range []string{"Mining", "Herbalism", "Lumberjacking", "Hunting", "Fishing"} {
		tr := byName[name]
		if tr.Kind != content.TreeProfession {
			t.Errorf("%s is %q, want profession", name, tr.Kind)
		}
	}
}

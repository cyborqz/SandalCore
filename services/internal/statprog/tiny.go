// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"strings"

	"aocrop/services/internal/aocexpr"
)

// C | O | A with Cyb

// TinyGraph is a small hand-made graph that exercises every instruction of the
// format (the real graph does not use them all): a curve of the level, class
// lists, a multiplying stat, limits and a spawn-at-maximum stat, equations,
// a formula with terms, every state function, branches, boolean logic, and
// (with faults) the things that make the interpreter count a fault. The tests
// and the shared corpus use it: it is small enough to read and to mutate.
//
// Stats (guid = 1000 + n): 1 Str, 2 Dex, 3 Mul, 4 Hp, 5 MaxHp, 6 Regen, 7 Ctx,
// 8 Shield, 9 Formula, 10 Logic, 11 Enum, 12 Named, 13 Plain (not in any list),
// 17 Compl (a multiplying stat whose effects multiply by one minus the mod), 18 Clamped (reads equations with a
// constraint), 19 Speed (an additive stat effects change), 20 Heal (a multiplying stat)
// and with faults 14 Fault, 15 Overflow, 16 Cubic.
func TinyGraph(faults bool) *Graph {
	stat := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatDef, 1000+n) }
	eq := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatEquation, n) }
	fm := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatFormula, n) }
	curve := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatCurve, n) }
	const (
		effectType = 3377240744094973029
		classType  = 297887639323616934
		skillType  = 4146438674247114715
	)
	big := "1" + strings.Repeat("0", 300) + ".0"

	g := &Graph{
		Stats:     map[int64]StatDef{},
		Lists:     map[string][]InitItem{},
		Equations: map[int64]string{},
		Clamps:    map[int64]Clamp{},
		Formulas:  map[int64]Formula{},
		Classes:   []ClassDef{{Name: "Ranger", Enum: 1, List: "Player_Base_Ranger"}, {Name: "Bard", Enum: 2, List: "Player_Base_Bard"}},
		Curves:    map[int64]*aocexpr.Curve{},
		// three city nodes by rank (Nodes 1): the node reads of TinyExprs name them
		Nodes: []GraphNode{{Guid: 5001, Name: "Tiny_Aela"}, {Guid: 5002, Name: "Tiny_Joeva"}, {Guid: 5003, Name: "Tiny_Halcyon"}},
	}
	add := func(n int, name string, rep int, s StatDef) {
		s.Guid, s.Name, s.Replication = int64(1000+n), name, rep
		if s.OnSpawn == "" {
			s.OnSpawn = "Reset_None"
		}
		g.Stats[s.Guid] = s
	}
	add(1, "Str", 1, StatDef{Min: "0", Max: "999"})
	add(2, "Dex", 5, StatDef{
		Base: "const r = GetStat(GetInstigator(), " + stat(1) + ");\nr / (r + EvalE(" + eq(100) + ")) + 1",
		Min:  "0",
	})
	add(3, "Mul", 1, StatDef{MultiplyMods: true, Base: "GetStat(GetOwner(), " + stat(1) + ") * 0.01", Max: "100"})
	add(4, "Hp", 7, StatDef{
		OnSpawn: "Reset_SetMax", Base: "GetStat(GetOwner(), " + stat(1) + ") * EvalE(" + eq(101) + ")",
		Min: "IsUnkillable(GetInstigator())", Max: "GetStat(GetOwner(), " + stat(5) + ")",
	})
	add(5, "MaxHp", 7, StatDef{Base: "GetStat(GetOwner(), " + stat(1) + ") * 3"})
	add(6, "Regen", 0, StatDef{Base: "0", Regen: "GetStat(GetInstigator(), " + stat(6) + ") * 0.1 + GetStat(GetInstigator(), " + stat(4) + ") * 0.01"})
	add(7, "Ctx", 1, StatDef{Base: "GetEffectCount(GetOwner(), $#" + fmt.Sprint(effectType) + ":1$) * 2 + GetEffectCountByTag(GetOwner(), \"pet.summon\") +\n" +
		"SelectFloat(GetSkillPurchased(GetOwner(), $#" + fmt.Sprint(classType) + ":2$, $#" + fmt.Sprint(skillType) + ":3$), 100.0, 0.0) +\n" +
		"SelectFloat(IsInCombat(GetOwner()) || IsMoving(GetOwner()), 1000.0, 0.0) +\n" +
		"GetPlayerProfessionLevel(GetActorGuid(GetInstigator()), \"Fishing\") * 10000.0 + GetConsumedItemStat(" + stat(1) + ") * 100000.0 + GetNpcEliteRank(GetSelf())"})
	add(8, "Shield", 1, StatDef{Base: "var b = 0.0;\nif (EquippedItemInSlotHasTag(EEquipSlot::Secondary, \"Item.Gear.Weapon.Shield\")) {\n  set b = 0.15;\n} else if (IsUnkillable(GetOwner())) {\n  set b = 0.5;\n}\nb + GetStat(GetOwner(), " + stat(1) + ") / 1000.0"})
	add(9, "Formula", 1, StatDef{Base: "EvalFormula(" + fm(200) + ") + GetStat(GetOwner(), " + stat(2) + ") * 0"})
	add(10, "Logic", 1, StatDef{Base: "var v = 1.0;\nconst lvl = GetCharacterLevel(GetOwner());\nGetStat(GetOwner(), " + stat(1) + ");\n" +
		"if (lvl > 10 && !(lvl > 40)) { set v = 2.0; }\nif (lvl <= 10 || lvl >= 40) { set v = v + 10.0; }\n" +
		"Min(v, 100.0) + Max(-1.0, Floor(2.7)) + Ceil(0.2) + Round(2.5) + SelectInt(lvl == 20, 1000, 0) + SelectFloat(lvl != 20, 0.5, 0.25) - 1.0 * -v"})
	add(11, "Enum", 1, StatDef{Base: "SelectFloat(\"a\" == \"a\" && EEquipSlot::Primary != EEquipSlot::Secondary, 1.0, 2.0)"})
	add(12, "Named", 1, StatDef{Base: "GetStat(GetOwner(), $Stat:Str$) * 2 + GetStat(GetOwner(), " + stat(13) + ")"})
	add(13, "Plain", 1, StatDef{Base: "7.5"})
	add(17, "Compl", 5, StatDef{MultiplyMods: true, MultiplyCompliment: true, Base: "1.0 - GetStat(GetOwner(), " + stat(1) + ") / 1000.0", Min: "0", Max: "1"})
	add(18, "Clamped", 1, StatDef{Base: "EvalE(" + eq(103) + ") + EvalFormula(" + fm(201) + ") * 100.0"})
	add(19, "Speed", 7, StatDef{Base: "1.0", Min: "0", Max: "10"})
	add(20, "Heal", 5, StatDef{MultiplyMods: true, Base: "1.0 + GetStat(GetOwner(), " + stat(1) + ") / 4000.0", Min: "0"})
	g.Lists["CombatStats"] = []InitItem{{1001, ""}, {1002, ""}, {1003, ""}, {1004, ""}, {1005, ""}, {1006, ""}, {1007, ""}, {1008, ""}, {1009, ""}, {1010, ""}, {1011, ""}, {1012, ""}, {1017, ""}, {1018, ""}, {1019, ""}, {1020, ""}}
	g.Lists["Base_Artisan_Stats"] = []InitItem{{1001, ""}}
	g.Lists["Player_Base"] = []InitItem{
		{1001, "EvalCurve(" + curve(11) + ", Float(GetCharacterLevel(GetOwner())))"},
		{1003, ""},
		{1004, "GetStatInitRunningTotal() + 20.0"},
		{1006, "-GetStatInitRunningTotal()"},
	}
	g.Lists["Player_Base_Ranger"] = []InitItem{{1001, "Round(GetStatInitRunningTotal() * 1.5)"}, {1002, "7"}}
	g.Lists["Player_Base_Bard"] = []InitItem{{1001, "Round(GetStatInitRunningTotal() * 0.8)"}, {1002, "9"}, {1003, "GetStatInitRunningTotal() + 0.5"}}
	g.Curves[11] = &aocexpr.Curve{Keys: []aocexpr.Key{{Time: 1, Value: 10}, {Time: 10, Value: 55, Interp: aocexpr.InterpConstant}, {Time: 20, Value: 100}, {Time: 50, Value: 400}}}
	g.Equations[100] = "// a coefficient\r\n250"
	g.Equations[101] = "const a = 1.5;\nvar b = 2.0;\nset b = b + a;\nb / 3.5 * 2.0 + 0.0"
	g.Equations[102] = "const m = GetTerm(" + fmt.Sprintf("$#%d:%d$", 6137747221710071900, 300) + ");\nm * 2.0 + GetTerm(" + fmt.Sprintf("$#%d:%d$", 6137747221710071900, 301) + ")"
	g.Formulas[200] = Formula{Equation: 102, Terms: map[int64]string{
		300: "0.5 * EvalE(" + eq(100) + ")",
		301: "GetStat(GetOwner(), " + stat(1) + ") + 1",
	}}
	// Equations with a constraint: 103 is raised to 0 and lowered to 15, 104 (through the formula 201) only lowered to 0.75.
	g.Equations[103] = "GetStat(GetOwner(), " + stat(1) + ") - 20.0"
	g.Clamps[103] = Clamp{HasMin: true, Min: 0, HasMax: true, Max: 15}
	g.Equations[104] = "GetTerm(" + fmt.Sprintf("$#%d:%d$", 6137747221710071900, 302) + ") * 0.01"
	g.Clamps[104] = Clamp{HasMax: true, Max: 0.75}
	g.Formulas[201] = Formula{Equation: 104, Terms: map[int64]string{302: "GetStat(GetOwner(), " + stat(1) + ")"}}
	if faults {
		add(14, "Fault", 1, StatDef{Base: "GetStat(GetOwner(), " + stat(1) + ") / (GetStat(GetOwner(), " + stat(1) + ") - GetStat(GetOwner(), " + stat(1) + "))"})
		add(15, "Overflow", 1, StatDef{Base: "GetStat(GetOwner(), " + stat(1) + ") * " + big + " * " + big})
		add(16, "Cubic", 1, StatDef{Base: "EvalCurve(" + curve(12) + ", Float(GetCharacterLevel(GetOwner())))"})
		g.Lists["CombatStats"] = append(g.Lists["CombatStats"], InitItem{1014, ""}, InitItem{1015, ""}, InitItem{1016, ""})
		g.Curves[12] = &aocexpr.Curve{Keys: []aocexpr.Key{{Time: 1, Value: 1, Interp: aocexpr.InterpCubic}, {Time: 30, Value: 9}}}
	}
	return g
}

// tiny reference ids: the effect, the class tree and the skills of the expression tests.
const (
	tinyEffectType = 3377240744094973029
	tinyClassType  = 297887639323616934
	tinySkillType  = 4146438674247114715
)

// tinyStat is the guid of the stat n of the tiny graph; tinyEffect an effect reference; tinyRank the rank of a skill of the class tree 2.
func tinyStat(n int) int64 { return int64(1000 + n) }

func tinyEffect(n int64) aocexpr.Ref { return aocexpr.Ref{TypeID: tinyEffectType, Guid: n} }

func tinyRank(skill int64) SkillRank {
	return SkillRank{Class: aocexpr.Ref{TypeID: tinyClassType, Guid: 2}, Skill: aocexpr.Ref{TypeID: tinySkillType, Guid: skill}}
}

// built by C | Y | B with CoA

// TinyExprs are the ability expressions of the tiny graph: every host function, the constrained equations, a StatMod's terms, a
// skill sum like the General_Sprint effect's, and one that cannot compile (a function the engine does not have). The last one's
// result must be a failure that leaves the bundle as the others made it.
func TinyExprs() []Expr {
	stat := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatDef, 1000+n) }
	eq := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatEquation, n) }
	effect := func(n int) string { return fmt.Sprintf("$#%d:%d$", tinyEffectType, n) }
	skill := func(n int) string {
		return fmt.Sprintf("GetSkillPurchased(GetOwner(), $#%d:2$, $#%d:%d$)", tinyClassType, tinySkillType, n)
	}
	term := fmt.Sprintf("$#%d:%d$", 6137747221710071900, 300)
	node := func(n int) string { return fmt.Sprintf("$#%d:%d$", aocexpr.TypeCityNode, n) }
	return []Expr{
		{What: "lerp of a stat of the target", Text: "Lerp(GetStat(GetTarget(), " + stat(1) + "), 100.0, 0.25)"},
		{What: "abs", Text: "Abs(GetStat(GetTarget(), " + stat(1) + ") - 200.0) + Abs(-0.5)"},
		{What: "progress", Text: "Lerp(1.0, 1.5, GetAbilityProgressRatio(\"SongGroup\"))"},
		{What: "effects by instigator", Text: "GetEffectCountByRecordAndInstigator(GetTarget(), " + effect(7) + ", GetInstigator()) * 2 + GetEffectCount(GetTarget(), " + effect(7) + ")"},
		{What: "a random number behind a skill test", Text: "SelectFloat(" + skill(3) + " && RandDouble() < 0.5, 1.0, 2.0)"},
		{What: "two random numbers", Text: "RandDouble() + RandDouble() * 10.0"},
		{What: "terms", Text: "GetTerm(" + term + ") * GetStat(GetTarget(), " + stat(2) + ")", Terms: map[int64]string{300: "0.5 + Lerp(0.0, 1.0, GetAbilityProgressRatio(\"Other\"))"}},
		{What: "terms that ask for their siblings", Text: "GetTerm(" + fmt.Sprintf("$#%d:%d$", 6137747221710071900, 301) + ") * GetAbilityProgressRatio()", Terms: map[int64]string{
			300: "0.25 + Lerp(0.0, 1.0, GetAbilityProgressRatio(\"Other\"))",
			301: "GetTerm(" + fmt.Sprintf("$#%d:%d$", 6137747221710071900, 300) + ") * 2.0 + 1.0"}},
		{What: "the sprint sum", Text: "const s1 = " + skill(3) + ";\nconst s2 = " + skill(4) + ";\nconst s3 = " + skill(5) + ";\n0.2 +\nSelectFloat(s1, 0.05, 0.0) +\nSelectFloat(s2, 0.05, 0.0) +\nSelectFloat(s3, 0.05, 0.0)"},
		{What: "a constrained equation", Text: "EvalE(" + eq(103) + ") * 2.0 + GetStat(GetOwner(), " + stat(18) + ")"},
		{What: "a constant lerp", Text: "Lerp(2.0, 4.0, 0.5) + Abs(-3.0)"},
		{What: "an unknown function", Text: "GetWeatherName(GetTarget()) + 1.0"},
		{What: "a stat formula's function in an expression", Text: "GetStat(GetTarget(), " + stat(1) + ") + GetStatInitRunningTotal()"},
		{What: "a stat the graph does not have", Text: "GetStat(GetOwner(), $#" + fmt.Sprint(TypeStatDef) + ":424242$)"},
		{What: "a heal of the hit in progress", Text: "const phys = CheckForDamageTag(GetHitInProgress(), \"ability.Phys\");\nvar heal = 10.0;\nif (phys) { set heal = 20.0; }\nconst crit = HitFlagsContains(GetHitFlags(GetHitInProgress(), GetTarget()), EHitFlag::Crit);\nSelectFloat(crit, heal * 1.5, heal) * (1.0 - GetXPDebtRatio(GetActorGuid(GetInstigator())) * 0.2)"},
		{What: "targets and a clamp", Text: "Lerp(5.0, 50.0, Clamp((GetNumTargets(GetHitInProgress()) - 1) / 4.0, 0.0, 1.0)) + Clamp(GetStat(GetTarget(), " + stat(1) + ") - 100.0, 0.0, 1.0)"},
		{What: "charges and montage choice", Text: "SelectFloat(GetRemainingAbilityCharges($#1841048597116038327:7001$) == 3.0, 10.0, 20.0) + SelectFloat(IsApplicableTarget(GetTarget(), EAffectedTarget::TARGET_Self) && !IsApplicableTarget(GetTarget(), EAffectedTarget::TARGET_Enemy), 1.0, 2.0)"},
		{What: "keys, time and angles", Text: "SelectFloat(IsAnyMovementKeyPressed() && !IsEnemy(GetInstigator(), GetTarget()) && !IsNeutral(GetInstigator(), GetTarget()), 5.0, 6.0) + Now() * 2.0 + GetAngleBetween(GetTarget(), GetInstigator())"},
		{What: "stat change over time and the hit event", Text: "0.5 * GetStatDeltaOverTime(GetInstigator(), 4.0, " + stat(1) + ") + Abs(GetStatModValue(GetHitEvent(), GetOwner(), " + stat(2) + ")) + GetStatModEffectiveValue(GetHitEvent(), " + stat(2) + ") + GetProgressDuration(\"DefaultGroup\")"},
		{What: "a random number in a range", Text: "RandDoubleRange(10.0, 20.0) + RandDoubleRange(0.0, 1.0)"},
		{What: "level and combat", Text: "SelectFloat(IsInCombat(GetTarget()), Float(GetCharacterLevel(GetTarget())), 0.0) + GetEffectCountByTag(GetTarget(), \"pet\")"},
		// Nodes 1, stage 1: the city node reads, the way the reward tables, the vendors and the requirements of the data write them
		{What: "a biome gate", Text: "SelectFloat(GetNodeBiome() == EBiomeType::Riverlands, 10.0, SelectFloat(GetNodeBiome() != EBiomeType::Desert && GetNodeBiome() != EBiomeType::Highlands, 20.0, 30.0))"},
		{What: "the level of the node the player is in", Text: "SelectFloat(EnumInt(Node_GetLevel(GetCurrentNode(GetPlayer()))) >= EnumInt(ENodeLevel::Village), 1.0, 2.0) + SelectFloat(Node_GetNodeType(GetNodeRecord()) == ENodeType::Divine, 100.0, 0.0)"},
		{What: "a node named by reference", Text: "SelectFloat(GetNodeRecord() == " + node(5002) + " && Node_InLevelRangeInclusive(" + node(5002) + ", ENodeLevel::Encampment, ENodeLevel::Town), 7.0, 8.0) + SelectFloat(GetCurrentNode(GetPlayer()) != " + node(5003) + ", 40.0, 0.0)"},
		{What: "citizens and the mayor", Text: "SelectFloat(Node_IsPlayerCitizen(GetPlayer()) || Node_IsPlayerCitizenOfNode(GetPlayer(), GetCurrentNode(GetOwner())) || Node_IsPlayerMayor(GetPlayer()) == true, 1000.0, 2000.0)"},
		{What: "a node and its level in variables", Text: "const n = GetCurrentNode(GetOwner());\nvar lvl = Node_GetLevel(n);\nSelectFloat(lvl == ENodeLevel::Town || Node_GetNodeType(n) == ENodeType::Martial, 5.0, 6.0) + SelectFloat(Node_InLevelRangeInclusive(n, ENodeLevel::Wilderness, ENodeLevel::Crossroads), 0.5, 0.25)"},
		{What: "a node named by its record", Text: "SelectFloat(Node_GetNodeType($CityNode:Tiny_Halcyon$) == ENodeType::Scientific, 1.0, 0.0) + SelectFloat(Node_IsPlayerCitizenOfNode(GetPlayerGuid(), $CityNode:Tiny_Aela$), 9.0, 0.0)"},
	}
}

// hosts are the inputs the expression tests run with.
func tinyHosts() []TableHostSpec {
	return []TableHostSpec{
		{Seed: 0},
		{Seed: 12345, Progress: map[string]float64{"SongGroup": 0.5, "Other": 0.25, "": 0.75}, Inst: map[aocexpr.Ref]float64{tinyEffect(7): 2},
			Tags: map[string]bool{"ability.Phys": true}, Flags: map[string]bool{"Crit": true}, Targets: 3, XPDebt: 0.5,
			// the city node reads (aocexpr.HostNodeBiome ...): a node's rank and an enum's ordinal; the numbers a read is given are not added to its answer
			Fns: map[string]float64{"1|7001": 3, "2|TARGET_Self": 1, "3|": 1, "10|": 12.5, "9|": 90, "4|1001": -40, "5|1002": -7.5, "6|1002": -3.25, "11|DefaultGroup": 2.5,
				"170|": 14, "171|": 1, "172|": 1, "173|": 1, "174|": 3, "175|": 0, "176|": 1, "177|": 0}},
		{Seed: 99, Progress: map[string]float64{"SongGroup": 1, "Other": 0}, Inst: map[aocexpr.Ref]float64{tinyEffect(7): 0.5, tinyEffect(8): 9},
			Flags: map[string]bool{"Glancing": true}, Targets: 7, XPDebt: 1,
			// a desert node of the third rank's neighbour, a Town of Divine type, and a mayor
			Fns: map[string]float64{"170|": 6, "171|": 2, "172|": 0, "173|": 2, "174|": 4, "175|": 1, "176|": 1, "177|": 1}},
	}
}

// TinyBundle compiles TinyGraph(faults) with TinyExprs.
func TinyBundle(faults bool, mode Mode) (*Bundle, []ExprResult, error) {
	return CompileExprs(TinyGraph(faults), Options{Mode: mode}, TinyExprs())
}

// richState is a character with an effect, a tag, a skill, combat, fishing level 7, a consumable and an NPC rank:
// every state function of the format answers something.
func richState() *TableState {
	st := func(n int) aocexpr.Ref { return aocexpr.Ref{TypeID: TypeStatDef, Guid: int64(1000 + n)} }
	return &TableState{
		Effects:        map[aocexpr.Ref]float64{{TypeID: 3377240744094973029, Guid: 1}: 3},
		Tags:           map[string]float64{"pet.summon": 2},
		Skills:         map[[2]aocexpr.Ref]bool{{{TypeID: 297887639323616934, Guid: 2}, {TypeID: 4146438674247114715, Guid: 3}}: true},
		FlagCombat:     true,
		FlagUnkillable: true,
		Actor:          42,
		Professions:    map[string]float64{"Fishing": 7},
		Consumed:       map[aocexpr.Ref]float64{st(1): 11},
		EliteRank:      2,
	}
}

// tinyInputs are the inputs of the TinyGraph tests and the corpus: with rich, mods, one for a stat
// the graph does not have, and equipped tags.
func tinyInputs(class, level int, rich bool) Inputs {
	in := Inputs{Class: class, Level: level}
	if rich {
		in.Mods = []Mod{{1001, 5.5}, {1005, 10}, {1007, -2.25}, {1001, 1}, {99999, 3}}
		in.Equipped = []EquippedTag{{"Secondary", "Item.Gear.Weapon.Shield.Tower"}, {"Primary", "Item.Gear.Weapon.Sword"}}
	}
	return in
}

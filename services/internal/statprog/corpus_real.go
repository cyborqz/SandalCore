// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"database/sql"
	"fmt"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
)

// The corpus's fxcases on the real design data (plan D2): characters with the ranks the game grants by itself, the Sprint effect
// and the retail buffs of the research, and every expression program of the real bundle evaluated against their stats with a host.
// They need the ab_* tables and plan D1's talent tables of the content.db (contentc version 6).

// The reference types GetSkillPurchased names: the skill tree and the skill.
const (
	TypeSkillTree = 297887639323616934
	TypeSkill     = 4146438674247114715
)

// AutoRank is a skill rank the game grants by itself when the character reaches LevelReq.
type AutoRank struct {
	Tree, Skill int64
	LevelReq    int
}

// LoadAutoRanks reads the skills every skill tree grants by itself (plan D1's skill_node: bAutoGranted with its LevelRequirement) of a
// content.db, by tree and skill.
func LoadAutoRanks(db *sql.DB) ([]AutoRank, error) {
	rows, err := db.Query(`SELECT DISTINCT tree_guid, skill_guid, level_requirement FROM skill_node WHERE auto_granted = 1 ORDER BY tree_guid, skill_guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []AutoRank
	for rows.Next() {
		var r AutoRank
		if err := rows.Scan(&r.Tree, &r.Skill, &r.LevelReq); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

// RanksAt are the auto-granted ranks a character of the level holds, as the engine's inputs.
func RanksAt(auto []AutoRank, level int) []SkillRank {
	var out []SkillRank
	for _, r := range auto {
		if r.LevelReq <= level {
			out = append(out, SkillRank{Class: aocexpr.Ref{TypeID: TypeSkillTree, Guid: r.Tree}, Skill: aocexpr.Ref{TypeID: TypeSkill, Guid: r.Skill}})
		}
	}
	return out
}

// -- C | O | A & C | Y | B --

// effectMods returns the stat mods of the effect called name: the stat and the expression id of each value.
func effectMods(db *sql.DB, name string) (effect int64, stats, exprs []int64, err error) {
	if err = db.QueryRow(`SELECT guid FROM ab_effect WHERE name = ?`, name).Scan(&effect); err != nil {
		return 0, nil, nil, fmt.Errorf("effect %s: %w", name, err)
	}
	rows, err := db.Query(`SELECT stat_guid, value_expr FROM ab_mod WHERE owner_kind = 'effect' AND owner_guid = ? ORDER BY ord`, effect)
	if err != nil {
		return 0, nil, nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var s, e int64
		if err := rows.Scan(&s, &e); err != nil {
			return 0, nil, nil, err
		}
		stats, exprs = append(stats, s), append(exprs, e)
	}
	return effect, stats, exprs, rows.Err()
}

// realFxCase is an fxcase of the real bundle.
type realFxCase struct {
	name   string
	bundle string
	in     Inputs
	state  *TableState
	host   TableHostSpec
	retail []CorpusRetail // numbers of the retail captures the case reproduces
}

// realFxCases makes the corpus's fxcases of the real bundle b (compiled with the expressions of the file: results and exprIDs are what
// CompileExprs and LoadExprs gave).
func realFxCases(path string, b *Bundle, results []ExprResult, exprIDs []int64) ([]realFxCase, error) {
	cdb, err := content.Open(path)
	if err != nil {
		return nil, err
	}
	defer cdb.Close()
	db := cdb.SQL()
	auto, err := LoadAutoRanks(db)
	if err != nil {
		return nil, err
	}
	progOf := map[int64]int{}
	for i, id := range exprIDs {
		progOf[id] = results[i].Program
	}
	// mods evaluates the value of each of an effect's stat mods for the character in (as the host does when it applies the effect) and returns the effect.
	mods := func(in Inputs, st aocexpr.State, name string, stacks int) (ActiveEffect, error) {
		guid, stats, exprs, err := effectMods(db, name)
		if err != nil {
			return ActiveEffect{}, err
		}
		run, err := b.Run(in, st)
		if err != nil {
			return ActiveEffect{}, err
		}
		ev, err := b.NewEvaluator(run, in, st)
		if err != nil {
			return ActiveEffect{}, err
		}
		e := ActiveEffect{Effect: aocexpr.Ref{TypeID: content.TypeEffect, Guid: guid}, Stacks: stacks}
		for i := range stats {
			p := progOf[exprs[i]]
			if p < 0 {
				return ActiveEffect{}, fmt.Errorf("effect %s: the value of mod %d does not compile", name, i)
			}
			v, err := ev.Eval(p, nil)
			if err != nil {
				return ActiveEffect{}, err
			}
			e.Mods = append(e.Mods, EffectMod{Stat: stats[i], Value: v})
		}
		return e, nil
	}
	var merry int64
	if err := db.QueryRow(`SELECT guid FROM record WHERE schema = 'SkillRecordBase' AND name = 'Bard_Passive_Merriment'`).Scan(&merry); err != nil {
		return nil, err
	}
	var bardTree int64
	if err := db.QueryRow(`SELECT skill_tree_guid FROM class WHERE name = 'Bard'`).Scan(&bardTree); err != nil {
		return nil, err
	}
	statByName := func(name string) (int64, error) {
		for _, s := range b.Stats {
			if b.Strings[s.Name] == name {
				return s.Guid, nil
			}
		}
		return 0, fmt.Errorf("no stat %s", name)
	}
	var out []realFxCase

	// the retail level-25 Bard sprinting: the Sprint effect with the value its expression gives for the ranks the game grants at level 25
	bard25 := Inputs{Class: 2, Level: 25, Ranks: RanksAt(auto, 25)}
	sprint, err := mods(bard25, nil, "General_Sprint", 1)
	if err != nil {
		return nil, err
	}
	bard25.Effects = []ActiveEffect{sprint}
	moveSpeed, err := statByName("MoveSpeedMult")
	if err != nil {
		return nil, err
	}
	// the Mana costs of capture 44, each k x ManaCostMult(25) = k x 5.6875, and the retail MoveSpeedMult of the sprinting level-25 Bard
	retail := []CorpusRetail{{Kind: "stat", ID: moveSpeed, Value: 1.3, Digits: 4}}
	for _, c := range []struct {
		ability string
		mana    float64
	}{{"Bard_Misc_QuickWit_Jest", 56.875}, {"Bard_Misc_Flourish", 56.875}, {"Bard_Misc_QuickWit_Mock", 56.875}, {"Bard_Dance_Mesmerizing", 85.3125},
		{"Bard_Story_Joy", 85.3125}, {"Bard_Dance_Nimble", 113.75}, {"Bard_Dance_Maddening", 113.75}, {"Bard_Misc_Misdirection", 113.75}, {"Bard_Saga", 199.0625}} {
		var expr int64
		if err := db.QueryRow(`SELECT c.amount_expr FROM ab_cost c JOIN ab_ability a ON a.guid = c.ability_guid WHERE a.name = ? AND c.stat_name = 'Mana'`, c.ability).Scan(&expr); err != nil {
			return nil, fmt.Errorf("the Mana cost of %s: %w", c.ability, err)
		}
		retail = append(retail, CorpusRetail{Kind: "prog", ID: int64(progOf[expr]), Value: c.mana})
	}
	out = append(out,
		realFxCase{name: "real_bard_l25_sprint", bundle: "main", in: bard25, host: TableHostSpec{Seed: 7, Progress: map[string]float64{"SongGroup": 0.5}}, retail: retail},
		realFxCase{name: "real_bard_l25_sprint_double", bundle: "main_double", in: bard25, host: TableHostSpec{Seed: 7, Progress: map[string]float64{"SongGroup": 0.5}}})

	// the buffs of capture 44: Glee (0.03 with the talent Merriment) on Mitigation and Note_Comedy (1.05) on HealingMult, on a Bard whose
	// Mitigation and HealingMult the equipment put at the retail 0.9652 and 1.1025
	buffs := Inputs{Class: 2, Level: 25, Ranks: append(RanksAt(auto, 25), SkillRank{Class: aocexpr.Ref{TypeID: TypeSkillTree, Guid: bardTree}, Skill: aocexpr.Ref{TypeID: TypeSkill, Guid: merry}})}
	run, err := b.Run(buffs, nil)
	if err != nil {
		return nil, err
	}
	for _, tgt := range []struct {
		stat  string
		value float64
	}{{"Mitigation", 0.9652}, {"HealingMult", 1.1025}} {
		g, err := statByName(tgt.stat)
		if err != nil {
			return nil, err
		}
		i, _ := b.StatIndex(g)
		buffs.Mods = append(buffs.Mods, Mod{Stat: g, Value: tgt.value - run.Values[i]})
	}
	for _, name := range []string{"Bard_Glee", "Bard_Note_Comedy"} {
		e, err := mods(buffs, nil, name, 1)
		if err != nil {
			return nil, err
		}
		buffs.Effects = append(buffs.Effects, e)
	}
	buffs.Effects = append(buffs.Effects, sprint)
	mitigation, err := statByName("Mitigation")
	if err != nil {
		return nil, err
	}
	healing, err := statByName("HealingMult")
	if err != nil {
		return nil, err
	}
	// capture 44: Mitigation 0.9652 -> 0.9362 under Glee 0.03, HealingMult 1.1025 -> 1.1576 under Note_Comedy 1.05
	out = append(out, realFxCase{name: "real_bard_l25_buffs", bundle: "main", in: buffs, host: TableHostSpec{Seed: 12345, Progress: map[string]float64{"SongGroup": 0.25},
		Tags: map[string]bool{"ability.PhysicalScalingOverride": true}, Flags: map[string]bool{"Crit": true}, Targets: 3, XPDebt: 0.25},
		retail: []CorpusRetail{{Kind: "stat", ID: mitigation, Value: 0.9362, Digits: 4}, {Kind: "stat", ID: healing, Value: 1.1576, Digits: 4}}})

	// the other classes with their auto-granted ranks, a state, and the Sprint twice
	for _, c := range []struct {
		name         string
		class, level int
		moving       bool
		seed         uint64
		progress     float64
	}{{"real_mage_l40", 8, 40, false, 99, 1}, {"real_cleric_l12_combat", 3, 12, true, 5, 0}, {"real_fighter_l50", 4, 50, false, 31337, 0.75}, {"real_ranger_l1", 1, 1, false, 1, 0.1}} {
		in := Inputs{Class: c.class, Level: c.level, Ranks: RanksAt(auto, c.level)}
		var st *TableState
		if c.moving {
			st = &TableState{FlagCombat: true, FlagMoving: true, Professions: map[string]float64{"Fishing": 4}, Actor: 9}
		}
		e, err := mods(in, nil, "General_Sprint", 2)
		if err != nil {
			return nil, err
		}
		in.Effects = []ActiveEffect{e}
		out = append(out, realFxCase{name: c.name, bundle: "main", in: in, state: st, host: TableHostSpec{Seed: c.seed, Progress: map[string]float64{"SongGroup": c.progress}}})
	}
	return out, nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// Plan D1 (contentc version 5): what using an item does, the level curves and the ranks a level grants.
//
//   item_use, item_action        every ItemRecord with an ActivationArchetypeId: the archetype's activation and
//                                ConsumeOnUse, and the item's ActivationActionsIds
//   action_response, action_reward
//                                every ActionResponse an item names: its type, a SetPrimaryArchetype's class, its RewardsIds
//   reward_table, reward_item, reward_xp
//                                every RewardTable such an action names. The GM items' tables grant always and select all,
//                                which is what is modelled; a table of another shape is written with modelled 0 and no rows.
//                                An ExperienceRewards' ExperienceValue is the level scrolls' formula: contentc reads the
//                                target level, the curve and the multiplier stat out of the text and checks, with package
//                                aocexpr over the real curve, that the expression equals
//                                (curve(target) - curve(level) + debt_ratio * xp_of_level) / max(0.001, 1 + mult) + margin
//                                at every level and several debts and multipliers; an expression that does not is written
//                                with target_level 0 (worldd refuses the reward and says so)
//   leveling_type, leveling_curve
//                                every LevelingTypeDef with its LevelUpCurve keys and the stat its XPMultiplier adds to 1
//   skill_auto_grant             every SkillTreeNode with bAutoGranted and a LevelRequirement, by tree, with its skill, the
//                                skill's first rank and the rank's passive effect (talents.go writes skill_rank_stat_mod, the
//                                constant stat mods of an infinite effect, for every rank)

// progressionRecordTypes are the record structs this builder reads and the TypeIds content names for them.
var progressionRecordTypes = map[string]uint64{
	"ItemActivationArchetypeRecordBase": content.TypeItemActivationArchetype,
	"ActionResponseRecordBase":          content.TypeActionResponse,
	"RewardTableRecordBase":             content.TypeRewardTable,
	"LevelingTypeDefRecordBase":         content.TypeLevelingTypeDef,
	"SkillTreeRecordBase":               content.TypeSkillTree,
	"SkillTreeNodeRecordBase":           content.TypeSkillTreeNode,
	"SkillRecordBase":                   content.TypeSkill,
	"SkillRankRecordBase":               content.TypeSkillRank,
}

// enumValue is an enum string without its type prefix ("EActionResponse::GiveItem" -> "GiveItem").
func enumValue(s string) string {
	if i := strings.LastIndex(s, "::"); i >= 0 {
		return s[i+2:]
	}
	return s
}

func byGuid(rows []designdb.Row) map[int64]*designdb.Row {
	out := make(map[int64]*designdb.Row, len(rows))
	for i := range rows {
		out[rows[i].Guid] = &rows[i]
	}
	return out
}

// ~ C-O-A :: Cyb ~

type progressionSummary struct {
	uses, actions, tables, modelled, xpRewards, xpModelled, levelingTypes, grants int
}

// buildProgression fills the plan D1 tables. curves are the StatCurve rows as buildCurves read them (EvalCurve of the XP
// expressions), statNames the StatTypeDef names by Guid.
func buildProgression(w *writer, src *source, curves *aocexpr.CurveMap, statNames map[int64]string, opt *Options, meta map[string]string) error {
	for schema, want := range progressionRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
		if got := src.typeIDs[schema]; got != want {
			return fmt.Errorf("%s has TypeId %d in the database, content expects %d", schema, got, want)
		}
	}
	var sum progressionSummary

	// item_use and item_action: every item with an activation archetype
	archetypes := byGuid(src.rows["ItemActivationArchetypeRecordBase"])
	actionsWanted := map[int64]bool{}
	for _, r := range src.rows["ItemRecordBase"] {
		f := newFields(fmt.Sprintf("Item %s (%d)", r.Name, r.Guid), r.Fields)
		arch, _ := f.id("ActivationArchetypeId")
		actions := f.ids("ActivationActionsIds")
		if f.err != nil {
			return f.err
		}
		if arch == 0 {
			continue
		}
		a, ok := archetypes[arch]
		if !ok {
			return fmt.Errorf("%s: activation archetype %d is no ItemActivationArchetype row", f.where, arch)
		}
		af := newFields(fmt.Sprintf("ItemActivationArchetype %s (%d)", a.Name, a.Guid), a.Fields)
		activation := enumValue(af.str("ActivationAction"))
		consume := af.boolean("ConsumeOnUse")
		cooldown := float64(af.int("Cooldown")) // an int in the record (seconds)
		inCombat := af.boolean("DisabledInCombat")
		if af.err != nil {
			return af.err
		}
		if err := w.exec(`INSERT INTO item_use (item_guid, activation, consume_on_use, cooldown, disabled_in_combat) VALUES (?, ?, ?, ?, ?)`,
			r.Guid, activation, b2i(consume), cooldown, b2i(inCombat)); err != nil {
			return err
		}
		for i, g := range actions {
			if err := w.exec(`INSERT INTO item_action (item_guid, ord, action_guid) VALUES (?, ?, ?)`, r.Guid, i, g); err != nil {
				return err
			}
			actionsWanted[g] = true
		}
		sum.uses++
	}

	// action_response and action_reward: the actions the items name
	responses := byGuid(src.rows["ActionResponseRecordBase"])
	classNames := map[int64]string{}
	for _, r := range src.rows["CharacterClassesRecordBase"] {
		classNames[r.Guid] = r.Name
	}
	tablesWanted := map[int64]bool{}
	for _, g := range sortedKeys(actionsWanted) {
		r, ok := responses[g]
		if !ok {
			opt.logf("warning: an item names the action response %d, which is no row: skipped", g)
			continue
		}
		f := newFields(fmt.Sprintf("ActionResponse %s (%d)", r.Name, r.Guid), r.Fields)
		kind := enumValue(f.str("ResponseType"))
		class, _ := f.id("CharacterClassId")
		removeUpTo := f.boolean("RemoveUpToQuantity")
		rewards := f.ids("RewardsIds")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO action_response (guid, name, response_type, class_guid, class_name, remove_up_to_quantity) VALUES (?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, kind, class, classNames[class], b2i(removeUpTo)); err != nil {
			return err
		}
		for i, t := range rewards {
			if err := w.exec(`INSERT INTO action_reward (action_guid, ord, reward_table_guid) VALUES (?, ?, ?)`, r.Guid, i, t); err != nil {
				return err
			}
			tablesWanted[t] = true
		}
		sum.actions++
	}

	// reward_table, reward_item, reward_xp
	tables := byGuid(src.rows["RewardTableRecordBase"])
	for _, g := range sortedKeys(tablesWanted) {
		r, ok := tables[g]
		if !ok {
			opt.logf("warning: an action names the reward table %d, which is no row: skipped", g)
			continue
		}
		t, err := readRewardTable(r, curves)
		if err != nil {
			return err
		}
		if err := w.exec(`INSERT INTO reward_table (guid, name, predicate, level_below, modelled) VALUES (?, ?, ?, ?, ?)`,
			r.Guid, r.Name, t.predicate, t.levelBelow, b2i(t.modelled)); err != nil {
			return err
		}
		sum.tables++
		if !t.modelled {
			opt.logf("reward table %s (%d) is not of the shape worldd grants (%s): written with modelled 0", r.Name, r.Guid, t.why)
			continue
		}
		sum.modelled++
		for i, it := range t.items {
			if err := w.exec(`INSERT INTO reward_item (table_guid, ord, item_guid, variation_guid, quantity) VALUES (?, ?, ?, ?, ?)`,
				r.Guid, i, it.item, it.variation, it.quantity); err != nil {
				return err
			}
		}
		for i, x := range t.xp {
			if err := w.exec(`INSERT INTO reward_xp (table_guid, ord, experience_type, target_level, margin, curve_guid, mult_stat_guid, expression)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, i, x.kind, x.target, x.margin, x.curve, x.multStat, x.expression); err != nil {
				return err
			}
			sum.xpRewards++
			if x.target != 0 {
				sum.xpModelled++
			} else {
				opt.logf("reward table %s (%d): the %s experience expression is not the level scrolls' formula (%s): written with target_level 0", r.Name, r.Guid, x.kind, x.why)
			}
		}
	}

	// leveling_type and leveling_curve
	for _, r := range src.rows["LevelingTypeDefRecordBase"] {
		f := newFields(fmt.Sprintf("LevelingTypeDef %s (%d)", r.Name, r.Guid), r.Fields)
		kind := enumValue(f.str("ExperienceType"))
		cap := f.int("LevelCapOverride")
		debt := f.boolean("bUsesExperienceDebt")
		mult := f.str("XPMultiplier")
		curve := newFields(f.where+": LevelUpCurve", f.strukt("LevelUpCurve"))
		ed := newFields(curve.where+".EditorCurveData", curve.strukt("EditorCurveData"))
		keys := ed.list("Keys")
		if err := firstErr(f, curve, ed); err != nil {
			return err
		}
		multStat, err := multiplierStat(mult)
		if err != nil {
			return fmt.Errorf("%s: XPMultiplier: %w", f.where, err)
		}
		if err := w.exec(`INSERT INTO leveling_type (guid, name, experience_type, level_cap, uses_debt, xp_mult_stat_guid) VALUES (?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, kind, cap, b2i(debt), multStat); err != nil {
			return err
		}
		// The player curves (Adventuring, the weapon lines) are whole levels from 1 at whole XP totals; some profession and
		// guild placeholders start at level 0 or carry fractions (Weaponsmith: 100.001). A key that is no whole level from 1,
		// or repeats one, is left out and said so; a fraction of XP is rounded. Nothing of it reaches the Adventuring curve.
		last := int64(0)
		skipped := 0
		for i, kv := range keys {
			ks, ok := kv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: key %d is %T", f.where, i, kv)
			}
			kf := newFields(fmt.Sprintf("%s: key %d", f.where, i), ks)
			level, xp := kf.float("Time"), kf.float("Value")
			if kf.err != nil {
				return kf.err
			}
			if !finite(level, xp) || xp < 0 || xp > 1<<53 {
				return fmt.Errorf("%s: key %d is level %v at %v XP: not a finite XP total", f.where, i, level, xp)
			}
			if level != math.Trunc(level) || level < 1 || level > 255 || int64(level) <= last {
				skipped++
				continue
			}
			last = int64(level)
			if err := w.exec(`INSERT INTO leveling_curve (type_guid, level, xp) VALUES (?, ?, ?)`, r.Guid, last, int64(math.Floor(xp+0.5))); err != nil {
				return err
			}
		}
		if skipped > 0 {
			opt.logf("leveling type %s (%d): %d curve key(s) are no whole level from 1 after the one before: left out", r.Name, r.Guid, skipped)
		}
		sum.levelingTypes++
	}

	// skill_auto_grant and skill_rank_stat_mod
	nodes := byGuid(src.rows["SkillTreeNodeRecordBase"])
	skills := byGuid(src.rows["SkillRecordBase"])
	ranks := byGuid(src.rows["SkillRankRecordBase"])
	for _, tree := range src.rows["SkillTreeRecordBase"] {
		tf := newFields(fmt.Sprintf("SkillTree %s (%d)", tree.Name, tree.Guid), tree.Fields)
		levelType, _ := tf.id("LevelTypeId")
		entries := tf.list("Skills")
		if tf.err != nil {
			return tf.err
		}
		for i, ev := range entries {
			es, ok := ev.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: Skills[%d] is %T", tf.where, i, ev)
			}
			ef := newFields(fmt.Sprintf("%s: Skills[%d]", tf.where, i), es)
			nodeGuid, _ := ef.id("TreeNodeId")
			if ef.err != nil {
				return ef.err
			}
			node, ok := nodes[nodeGuid]
			if !ok {
				continue // a tree entry naming no node: nothing to grant
			}
			nf := newFields(fmt.Sprintf("SkillTreeNode %s (%d)", node.Name, node.Guid), node.Fields)
			auto := nf.boolean("bAutoGranted")
			level := nf.int("LevelRequirement")
			skillGuid, _ := nf.id("SkillId")
			if nf.err != nil {
				return nf.err
			}
			if !auto || level <= 0 {
				continue
			}
			skill, ok := skills[skillGuid]
			if !ok {
				opt.logf("warning: %s names skill %d, which is no row: not granted", nf.where, skillGuid)
				continue
			}
			sf := newFields(fmt.Sprintf("Skill %s (%d)", skill.Name, skill.Guid), skill.Fields)
			rankIds := sf.ids("SkillRanksIds")
			if sf.err != nil {
				return sf.err
			}
			var rankGuid, effectGuid int64
			if len(rankIds) > 0 {
				rankGuid = rankIds[0]
				if rank, ok := ranks[rankGuid]; ok {
					rf := newFields(fmt.Sprintf("SkillRank %s (%d)", rank.Name, rank.Guid), rank.Fields)
					effectGuid, _ = rf.id("EffectIdId")
					if rf.err != nil {
						return rf.err
					}
				}
			}
			if err := w.exec(`INSERT INTO skill_auto_grant (tree_guid, tree_name, leveling_type_guid, node_guid, node_name, level_requirement, skill_guid,
				skill_name, rank_guid, effect_guid) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
				tree.Guid, tree.Name, levelType, node.Guid, node.Name, level, skill.Guid, skill.Name, rankGuid, effectGuid); err != nil {
				return err
			}
			sum.grants++
		}
	}

	opt.logf("item_use: %d items; action_response: %d; reward_table: %d (%d modelled); reward_xp: %d (%d the level scrolls' formula); leveling_type: %d; skill_auto_grant: %d",
		sum.uses, sum.actions, sum.tables, sum.modelled, sum.xpRewards, sum.xpModelled, sum.levelingTypes, sum.grants)
	meta["typed.item_use"] = strconv.Itoa(sum.uses)
	meta["typed.action_response"] = strconv.Itoa(sum.actions)
	meta["typed.reward_table"] = strconv.Itoa(sum.tables)
	meta["typed.reward_table_modelled"] = strconv.Itoa(sum.modelled)
	meta["typed.reward_xp_modelled"] = strconv.Itoa(sum.xpModelled)
	meta["typed.leveling_type"] = strconv.Itoa(sum.levelingTypes)
	meta["typed.skill_auto_grant"] = strconv.Itoa(sum.grants)
	return nil
}

// ---- reward tables ---------------------------------------------------------------------------------------------------------------

type rewardItemRow struct {
	item, variation int64
	quantity        int64
}

type rewardXPRow struct {
	kind       string
	target     int64
	margin     float64
	curve      int64
	multStat   int64
	expression string
	why        string // why target is 0
}

// ~ C_Y_B - C.O.A ~

type rewardTableRows struct {
	predicate  string
	levelBelow int64
	modelled   bool
	why        string
	items      []rewardItemRow
	xp         []rewardXPRow
}

var levelPredicate = regexp.MustCompile(`^GetPlayerLevel\(\)\s*<\s*(\d+)$`)

// readRewardTable reads what a RewardTable grants (the shape of the GM items' tables).
func readRewardTable(r *designdb.Row, curves *aocexpr.CurveMap) (rewardTableRows, error) {
	var t rewardTableRows
	f := newFields(fmt.Sprintf("RewardTable %s (%d)", r.Name, r.Guid), r.Fields)
	t.predicate = strings.TrimSpace(f.str("Predicate"))
	subSelection := enumValue(f.str("SubTableSelectionAlgorithm"))
	containers := f.list("RewardDefContainers")
	if f.err != nil {
		return t, f.err
	}
	switch m := levelPredicate.FindStringSubmatch(t.predicate); {
	case t.predicate == "":
		t.levelBelow = 0
	case m != nil:
		t.levelBelow, _ = strconv.ParseInt(m[1], 10, 64)
	default:
		t.levelBelow = -1
	}
	t.modelled = true
	if subSelection != "None" {
		t.modelled = false
		t.why = "its sub-table selection is " + subSelection
	}
	for i, cv := range containers {
		cs, ok := cv.(designdb.Struct)
		if !ok {
			return t, fmt.Errorf("%s: RewardDefContainers[%d] is %T", f.where, i, cv)
		}
		cf := newFields(fmt.Sprintf("%s: RewardDefContainers[%d]", f.where, i), cs)
		grant := enumValue(cf.str("GrantMethod"))
		selection := enumValue(cf.str("SelectionAlgorithm"))
		rewards := cf.list("Rewards")
		if cf.err != nil {
			return t, cf.err
		}
		if grant != "Always" || selection != "All" {
			t.modelled = false
			t.why = fmt.Sprintf("container %d grants %s and selects %s", i, grant, selection)
		}
		for j, rv := range rewards {
			rs, ok := rv.(designdb.Struct)
			if !ok {
				return t, fmt.Errorf("%s: Rewards[%d] is %T", cf.where, j, rv)
			}
			rf := newFields(fmt.Sprintf("%s: Rewards[%d]", cf.where, j), rs)
			items := rf.list("ItemRewards")
			xps := rf.list("ExperienceRewards")
			if rf.err != nil {
				return t, rf.err
			}
			for k, iv := range items {
				is, ok := iv.(designdb.Struct)
				if !ok {
					return t, fmt.Errorf("%s: ItemRewards[%d] is %T", rf.where, k, iv)
				}
				itf := newFields(fmt.Sprintf("%s: ItemRewards[%d]", rf.where, k), is)
				ref := newFields(itf.where+": Item", itf.strukt("Item"))
				item, _ := ref.id("ItemId")
				variation, _ := ref.id("VariationId")
				qty := strings.TrimSpace(itf.str("Quantity"))
				if err := firstErr(itf, ref); err != nil {
					return t, err
				}
				n, err := strconv.ParseFloat(qty, 64)
				if err != nil || n != math.Trunc(n) || n < 1 || n > 1_000_000 {
					t.modelled = false
					t.why = fmt.Sprintf("item reward %d's quantity is %q", k, qty)
					continue
				}
				t.items = append(t.items, rewardItemRow{item: item, variation: variation, quantity: int64(n)})
			}
			for k, xv := range xps {
				xs, ok := xv.(designdb.Struct)
				if !ok {
					return t, fmt.Errorf("%s: ExperienceRewards[%d] is %T", rf.where, k, xv)
				}
				xf := newFields(fmt.Sprintf("%s: ExperienceRewards[%d]", rf.where, k), xs)
				kind := enumValue(xf.str("ExperienceType"))
				expr := xf.str("ExperienceValue")
				if xf.err != nil {
					return t, xf.err
				}
				row := rewardXPRow{kind: kind, expression: expr}
				if m, err := levelScrollModel(expr, curves); err == nil {
					row.target, row.margin, row.curve, row.multStat = m.target, m.margin, m.curve, m.multStat
				} else {
					row.why = err.Error()
				}
				t.xp = append(t.xp, row)
			}
		}
	}
	return t, nil
}

// ---- the level scrolls' XP formula ----------------------------------------------------------------------------------------------------

type xpModel struct {
	target   int64
	margin   float64
	curve    int64
	multStat int64
}

var (
	evalCurveTarget = regexp.MustCompile(`EvalCurve\(\$#(\d+):(\d+)\$,\s*([0-9]+(?:\.[0-9]+)?)\)`)
	getStatRef      = regexp.MustCompile(`GetStat\(GetPlayer\(\),\s*\$#(\d+):(\d+)\$\)`)
	xpMultiplierRef = regexp.MustCompile(`^1(?:\.0)?\s*\+\s*GetStat\(GetPlayer\(\),\s*\$#(\d+):(\d+)\$\)$`)
)

// multiplierStat reads a LevelingTypeDef's XPMultiplier: "1.0 + GetStat(GetPlayer(), $#stat:guid$)" gives the stat; "" or "1.0" none.
func multiplierStat(expr string) (int64, error) {
	s := strings.TrimSpace(expr)
	if s == "" || s == "1" || s == "1.0" {
		return 0, nil
	}
	m := xpMultiplierRef.FindStringSubmatch(s)
	if m == nil {
		return 0, fmt.Errorf("%q is not 1.0 + a stat", s)
	}
	typeID, _ := strconv.ParseUint(m[1], 10, 64)
	if typeID != content.TypeStatDef {
		return 0, fmt.Errorf("%q names type %d, not a stat", s, typeID)
	}
	guid, _ := strconv.ParseInt(m[2], 10, 64)
	return guid, nil
}

// levelScrollModel reads the parameters of the level scrolls' formula out of expr and proves, with aocexpr over the real
// curve, that expr computes (curve(target) - curve(level) + debt_ratio * xp_of_level) / max(0.001, 1 + mult) + margin for
// every level the curve has and several debt ratios and multipliers. An error says why it does not.
func levelScrollModel(expr string, curves *aocexpr.CurveMap) (xpModel, error) {
	var m xpModel
	cm := evalCurveTarget.FindStringSubmatch(expr)
	if cm == nil {
		return m, fmt.Errorf("no EvalCurve of a curve at a level")
	}
	curveType, _ := strconv.ParseUint(cm[1], 10, 64)
	if curveType != content.TypeStatCurve {
		return m, fmt.Errorf("the curve is of type %d, not a StatCurve", curveType)
	}
	m.curve, _ = strconv.ParseInt(cm[2], 10, 64)
	target, _ := strconv.ParseFloat(cm[3], 64)
	if target != math.Trunc(target) || target < 1 || target > 255 {
		return m, fmt.Errorf("the target level %v is not a whole level", target)
	}
	m.target = int64(target)
	if sm := getStatRef.FindStringSubmatch(expr); sm != nil {
		statType, _ := strconv.ParseUint(sm[1], 10, 64)
		if statType != content.TypeStatDef {
			return m, fmt.Errorf("GetStat names type %d, not a stat", statType)
		}
		m.multStat, _ = strconv.ParseInt(sm[2], 10, 64)
	}
	if curves == nil {
		return m, fmt.Errorf("no curves to check the expression against")
	}
	curve, err := curves.Curve(aocexpr.Ref{TypeID: content.TypeStatCurve, Guid: m.curve})
	if err != nil {
		return m, err
	}
	if len(curve.Keys) < 2 {
		return m, fmt.Errorf("the curve has %d keys", len(curve.Keys))
	}
	at := func(level float64) float64 {
		v, err := curve.Eval(level)
		if err != nil {
			return math.NaN()
		}
		return v
	}
	levels := make([]float64, 0, len(curve.Keys))
	for _, k := range curve.Keys {
		levels = append(levels, k.Time)
	}
	// the margin: at the target level with no debt and no multiplier the formula gives only its constant
	eval := func(level, ratio, xpOfLevel, mult float64) (float64, error) {
		player := aocexpr.Object{Kind: "player"}
		env := &aocexpr.Env{
			Curves: curves,
			FloatFuncs: map[string]bool{
				"GetStat": true, "GetPlayerXPForCurrentLevel": true, "GetXPDebtRatio": true, "float": true, "Float": true,
			},
			Funcs: map[string]aocexpr.Func{
				"GetInstigator": func([]aocexpr.Value) (aocexpr.Value, error) { return player, nil },
				"GetPlayer":     func([]aocexpr.Value) (aocexpr.Value, error) { return player, nil },
				"GetActorGuid":  func([]aocexpr.Value) (aocexpr.Value, error) { return 1.0, nil },
				"GetPlayerXPForCurrentLevel": func(args []aocexpr.Value) (aocexpr.Value, error) {
					if len(args) != 1 {
						return nil, fmt.Errorf("GetPlayerXPForCurrentLevel takes the actor")
					}
					return xpOfLevel, nil
				},
				"GetXPDebtRatio": func(args []aocexpr.Value) (aocexpr.Value, error) {
					if len(args) != 1 {
						return nil, fmt.Errorf("GetXPDebtRatio takes the actor")
					}
					return ratio, nil
				},
				"GetPlayerLevel": func([]aocexpr.Value) (aocexpr.Value, error) { return level, nil },
				"GetStat": func(args []aocexpr.Value) (aocexpr.Value, error) {
					if len(args) != 2 {
						return nil, fmt.Errorf("GetStat takes an object and a stat")
					}
					ref, ok := args[1].(aocexpr.Ref)
					if !ok || ref.Guid != m.multStat {
						return nil, fmt.Errorf("GetStat of a stat other than the multiplier")
					}
					return mult, nil
				},
				"float": func(args []aocexpr.Value) (aocexpr.Value, error) {
					if len(args) != 1 {
						return nil, fmt.Errorf("float takes one argument")
					}
					switch v := args[0].(type) {
					case float64:
						return v, nil
					case bool:
						if v {
							return 1.0, nil
						}
						return 0.0, nil
					}
					return nil, fmt.Errorf("float of a %T", args[0])
				},
			},
		}
		return aocexpr.EvalFloat(expr, env)
	}
	margin, err := eval(target, 0, 0, 0)
	if err != nil {
		return m, fmt.Errorf("the expression does not evaluate: %w", err)
	}
	if !finite(margin) {
		return m, fmt.Errorf("the expression's constant is not finite")
	}
	m.margin = margin
	for _, level := range levels {
		for _, ratio := range []float64{0, 0.5, 1} {
			for _, xpOfLevel := range []float64{0, 300, 12345} {
				for _, mult := range []float64{0, 0.3, -2} {
					got, err := eval(level, ratio, xpOfLevel, mult)
					if err != nil {
						return m, fmt.Errorf("the expression does not evaluate at level %v: %w", level, err)
					}
					want := (at(target)-at(level)+ratio*xpOfLevel)/math.Max(0.001, 1+mult) + margin
					if math.Abs(got-want) > 1e-6*math.Max(1, math.Abs(want)) {
						return m, fmt.Errorf("at level %v, debt %v of %v, multiplier %v the expression gives %v, the model %v", level, ratio, xpOfLevel, mult, got, want)
					}
				}
			}
		}
	}
	return m, nil
}

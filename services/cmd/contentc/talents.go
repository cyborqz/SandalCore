// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"strconv"
	"strings"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// Plan D1, stage 2 (still contentc version 5): the talents.
//
//   skill_point_type, leveling_point
//                          the point types (Combat_Archetype, Combat_Stamina, Combat_Weapon ...) and what reaching each level of
//                          each leveling type gives
//   skill_tree, skill_node, skill_node_require, skill_node_exclusive
//                          every SkillTree with its kind, its nodes in the tree's order and what each node needs. A node names its skill in
//                          SkillId or, when that is empty, a SkillTreeSlot (SkillSlotId): a talent choice whose SkillsIds are the skills the
//                          character may pick for it. A slot node is written once per option (the same node guid and ord, opt 0, 1, ...;
//                          slot_guid names the slot), because each option is a skill with ranks the client can buy
//   skill_rank, skill_rank_cost, skill_rank_stat_mod
//                          each node's skill's ranks with the ability and the passive effect, what they cost, and the constant
//                          stat mods of an infinite effect (the stat engine takes them as an input, as it takes gear)
//   item_weapon            the combo ability and the weapon line of every weapon item

// talentRecordTypes are the record structs this builder reads beyond the trees' and the progression's.
var talentRecordTypes = map[string]uint64{
	"SkillRequirementRecordBase": content.TypeSkillRequirement,
	"SkillPointTypeRecordBase":   content.TypeSkillPointType,
	"WeaponSkillLinesRecordBase": content.TypeWeaponSkillLines,
	"SkillTreeSlotRecordBase":    content.TypeSkillTreeSlot,
}

// The tree kinds of skill_tree.kind.
const (
	treeClass      = "class"
	treeUniversal  = "universal"
	treeWeapon     = "weapon"
	treeProfession = "profession"
	treeOther      = "other"
)

// effectDuration reads an effect's duration the way the E item needs it: an effect with no duration (or one that is a formula,
// which the effect system of plan D2 evaluates) is infinite, else the number of seconds.
func effectDuration(infiniteFlag bool, expr string) (infinite bool, seconds float64, formula bool) {
	expr = strings.TrimSpace(expr)
	if infiniteFlag || expr == "" {
		return true, 0, false
	}
	if v, err := strconv.ParseFloat(expr, 64); err == nil && finite(v) && v >= 0 {
		return false, v, false
	}
	return true, 0, true
}

// ~ C | O | A - C | Y | B ~

type talentSummary struct {
	trees, nodes, ranks, costs, requires, exclusives, mods, weapons, points int
	missingRequirement, formulaDurations, currencyCosts, noSkillNodes       int
	repeatedNodes, slotNodes, slotOptions, slotRequirements                 int
}

func buildTalents(w *writer, src *source, statNames map[int64]string, opt *Options, meta map[string]string) error {
	for schema, want := range talentRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
		if got := src.typeIDs[schema]; got != want {
			return fmt.Errorf("%s has TypeId %d in the database, content expects %d", schema, got, want)
		}
	}
	var sum talentSummary

	// the point types
	pointNames := map[int64]string{}
	for _, r := range src.rows["SkillPointTypeRecordBase"] {
		pointNames[r.Guid] = r.Name
		if err := w.exec(`INSERT INTO skill_point_type (guid, name) VALUES (?, ?)`, r.Guid, r.Name); err != nil {
			return err
		}
	}

	// the leveling types: what each level gives (the entry of index i is the level i+1), and what kind each is
	experienceType := map[int64]string{}
	for _, r := range src.rows["LevelingTypeDefRecordBase"] {
		f := newFields(fmt.Sprintf("LevelingTypeDef %s (%d)", r.Name, r.Guid), r.Fields)
		experienceType[r.Guid] = enumValue(f.str("ExperienceType"))
		levels := f.list("Levels")
		if f.err != nil {
			return f.err
		}
		for i, lv := range levels {
			ls, ok := lv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: Levels[%d] is %T", f.where, i, lv)
			}
			lf := newFields(fmt.Sprintf("%s: Levels[%d]", f.where, i), ls)
			containers := lf.list("SkillPointContainers")
			if lf.err != nil {
				return lf.err
			}
			total := map[int64]int64{}
			var order []int64
			for j, cv := range containers {
				cs, ok := cv.(designdb.Struct)
				if !ok {
					return fmt.Errorf("%s: SkillPointContainers[%d] is %T", lf.where, j, cv)
				}
				cf := newFields(fmt.Sprintf("%s: SkillPointContainers[%d]", lf.where, j), cs)
				qty := cf.int("Quantity")
				pt, _ := cf.id("SkillPointTypeId")
				if cf.err != nil {
					return cf.err
				}
				if qty <= 0 || qty > 1000 || pt == 0 {
					continue
				}
				if _, seen := total[pt]; !seen {
					order = append(order, pt)
				}
				total[pt] += qty
			}
			for _, pt := range order {
				if err := w.exec(`INSERT INTO leveling_point (type_guid, level, point_type_guid, quantity) VALUES (?, ?, ?, ?)`, r.Guid, i+1, pt, total[pt]); err != nil {
					return err
				}
				sum.points++
			}
		}
	}

	// the trees: their kinds
	classOfTree := map[int64]int64{}
	for _, r := range src.rows["CharacterClassesRecordBase"] {
		isPlayer := false
		for _, name := range content.PlayerClasses {
			isPlayer = isPlayer || name == r.Name
		}
		if !isPlayer {
			continue
		}
		f := newFields(fmt.Sprintf("CharacterClasses %s (%d)", r.Name, r.Guid), r.Fields)
		tree, _ := f.id("ActiveSkillTreeId")
		if f.err != nil {
			return f.err
		}
		classOfTree[tree] = r.Guid
	}
	lineOfTree := map[int64]designdb.Row{}
	for _, r := range src.rows["WeaponSkillLinesRecordBase"] {
		f := newFields(fmt.Sprintf("WeaponSkillLines %s (%d)", r.Name, r.Guid), r.Fields)
		tree, _ := f.id("SkillTreeId")
		if f.err != nil {
			return f.err
		}
		lineOfTree[tree] = r
	}
	abilities := byGuid(src.rows["AoCAbilityRecordBase"])
	effects := byGuid(src.rows["EffectRecordBase"])
	mods := byGuid(src.rows["StatModRecordBase"])
	nodes := byGuid(src.rows["SkillTreeNodeRecordBase"])
	slots := byGuid(src.rows["SkillTreeSlotRecordBase"])
	skills := byGuid(src.rows["SkillRecordBase"])
	ranks := byGuid(src.rows["SkillRankRecordBase"])
	requirements := byGuid(src.rows["SkillRequirementRecordBase"])

	writtenSkills := map[int64]bool{}
	writtenCosts := map[int64]bool{}
	writtenMods := map[int64]bool{}
	for _, tree := range src.rows["SkillTreeRecordBase"] {
		tf := newFields(fmt.Sprintf("SkillTree %s (%d)", tree.Name, tree.Guid), tree.Fields)
		levelType, _ := tf.id("LevelTypeId")
		entries := tf.list("Skills")
		if tf.err != nil {
			return tf.err
		}
		kind := treeOther
		var classGuid, lineGuid int64
		if c, ok := classOfTree[tree.Guid]; ok {
			kind, classGuid = treeClass, c
		} else if tree.Guid == content.GuidUniversalTree {
			kind = treeUniversal
		} else if line, ok := lineOfTree[tree.Guid]; ok {
			kind, lineGuid = treeWeapon, line.Guid
		} else if t := experienceType[levelType]; t == "Gathering" || t == "Crafting" || t == "Processing" {
			kind = treeProfession
		}
		if err := w.exec(`INSERT INTO skill_tree (guid, name, kind, leveling_type_guid, class_guid, weapon_line_guid) VALUES (?, ?, ?, ?, ?, ?)`,
			tree.Guid, tree.Name, kind, levelType, classGuid, lineGuid); err != nil {
			return err
		}
		sum.trees++
		seenNodes := map[int64]bool{}
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
				continue
			}
			if seenNodes[nodeGuid] {
				sum.repeatedNodes++ // a node listed twice in one tree (two trees do): the first place counts
				continue
			}
			seenNodes[nodeGuid] = true
			nf := newFields(fmt.Sprintf("SkillTreeNode %s (%d)", node.Name, node.Guid), node.Fields)
			nodeSkill, _ := nf.id("SkillId")
			slotGuid, _ := nf.id("SkillSlotId")
			level := nf.int("LevelRequirement")
			pointReq := nf.int("PointRequirement")
			auto := nf.boolean("bAutoGranted")
			hotbar := nf.boolean("bAutoAddToHotbar")
			all := nf.boolean("bAllAreRequired")
			required := nf.list("SkillsRequired")
			exclusive := nf.ids("MutuallyExclusiveSkillsIds")
			if nf.err != nil {
				return nf.err
			}
			// the skills the node offers: its SkillId, or the SkillsIds of its slot (a talent choice)
			var options []int64
			if nodeSkill != 0 {
				options, slotGuid = []int64{nodeSkill}, 0
			} else if slotGuid != 0 {
				slot, ok := slots[slotGuid]
				if !ok {
					opt.logf("warning: %s names slot %d, which is no row: left out", nf.where, slotGuid)
					sum.noSkillNodes++
					continue
				}
				slf := newFields(fmt.Sprintf("SkillTreeSlot %s (%d)", slot.Name, slot.Guid), slot.Fields)
				options = slf.ids("SkillsIds")
				if slf.err != nil {
					return slf.err
				}
				sum.slotNodes++
			}
			if len(options) == 0 {
				sum.noSkillNodes++
				continue
			}
			gatesWritten, written := false, 0
			for _, skillGuid := range options {
				skill, ok := skills[skillGuid]
				if !ok {
					opt.logf("warning: %s names skill %d, which is no row: left out", nf.where, skillGuid)
					continue
				}
				if err := w.exec(`INSERT INTO skill_node (tree_guid, node_guid, ord, opt, slot_guid, name, skill_guid, skill_name, level_requirement, point_requirement,
				auto_granted, auto_hotbar, all_required) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
					tree.Guid, node.Guid, i, written, slotGuid, node.Name, skillGuid, skill.Name, level, pointReq, b2i(auto), b2i(hotbar), b2i(all)); err != nil {
					return err
				}
				written++
				if slotGuid != 0 {
					sum.slotOptions++
				}
				sum.nodes++
				if !gatesWritten { // the node's requirements and exclusions are written once, with its first option
					gatesWritten = true
					for j, rv := range required {
						rs, ok := rv.(designdb.Struct)
						if !ok {
							return fmt.Errorf("%s: SkillsRequired[%d] is %T", nf.where, j, rv)
						}
						rf := newFields(fmt.Sprintf("%s: SkillsRequired[%d]", nf.where, j), rs)
						reqGuid, _ := rf.id("SkillRequirementId")
						if rf.err != nil {
							return rf.err
						}
						var need, needSlot int64
						if req, ok := requirements[reqGuid]; ok {
							qf := newFields(fmt.Sprintf("SkillRequirement %s (%d)", req.Name, req.Guid), req.Fields)
							skillRef, _ := qf.id("SkillRecordId")
							nodeRef, _ := qf.id("SkillTreeNodeRecordId")
							if qf.err != nil {
								return qf.err
							}
							need = skillRef
							if n, ok := nodes[nodeRef]; ok && nodeRef != 0 {
								nnf := newFields(fmt.Sprintf("SkillTreeNode %s (%d)", n.Name, n.Guid), n.Fields)
								nodeNeed, _ := nnf.id("SkillId")
								nodeSlot, _ := nnf.id("SkillSlotId")
								if nnf.err != nil {
									return nnf.err
								}
								switch {
								case nodeNeed != 0:
									need = nodeNeed
								case nodeSlot != 0:
									// a slot node: any skill picked for the slot meets it, whichever option the record's SkillRecordId names (retail accepted
									// Tank_Passive_Pulverize_Punishing, whose requirement names Tank_Pulverize, with Tank_Pulverize_v2 picked)
									need, needSlot = 0, nodeRef
									sum.slotRequirements++
								default:
									need = 0
								}
							}
						} else {
							sum.missingRequirement++
						}
						if err := w.exec(`INSERT INTO skill_node_require (tree_guid, node_guid, ord, skill_guid, slot_node_guid) VALUES (?, ?, ?, ?, ?)`, tree.Guid, node.Guid, j, need, needSlot); err != nil {
							return err
						}
						sum.requires++
					}
					for j, g := range exclusive {
						if err := w.exec(`INSERT INTO skill_node_exclusive (tree_guid, node_guid, ord, skill_guid) VALUES (?, ?, ?, ?)`, tree.Guid, node.Guid, j, g); err != nil {
							return err
						}
						sum.exclusives++
					}
				}

				// the skill's ranks, once per skill
				if writtenSkills[skillGuid] {
					continue
				}
				writtenSkills[skillGuid] = true
				sf := newFields(fmt.Sprintf("Skill %s (%d)", skill.Name, skill.Guid), skill.Fields)
				rankIds := sf.ids("SkillRanksIds")
				if sf.err != nil {
					return sf.err
				}
				for ord, rankGuid := range rankIds {
					rank, ok := ranks[rankGuid]
					if !ok {
						opt.logf("warning: skill %s names rank %d, which is no row: left out", skill.Name, rankGuid)
						continue
					}
					rkf := newFields(fmt.Sprintf("SkillRank %s (%d)", rank.Name, rank.Guid), rank.Fields)
					abilityGuid, _ := rkf.id("AbilityIdId")
					effectGuid, _ := rkf.id("EffectIdId")
					cost := newFields(rkf.where+": SkillCost", rkf.strukt("SkillCost"))
					pointCosts := cost.list("SkillPointCosts")
					currency := cost.list("CurrencyCosts")
					commodity := cost.list("CommodityCosts")
					if err := firstErr(rkf, cost); err != nil {
						return err
					}
					if len(currency)+len(commodity) > 0 {
						sum.currencyCosts++
					}
					abilityName := ""
					if abilityGuid != 0 {
						if a, ok := abilities[abilityGuid]; ok {
							abilityName = a.Name
						}
					}
					effectName, infinite, seconds := "", false, 0.0
					if effectGuid != 0 {
						if e, ok := effects[effectGuid]; ok {
							effectName = e.Name
							xf := newFields(fmt.Sprintf("Effect %s (%d)", e.Name, e.Guid), e.Fields)
							flag := xf.boolean("bInfiniteDuration")
							dur := xf.str("EffectDuration")
							if xf.err != nil {
								return xf.err
							}
							var formula bool
							infinite, seconds, formula = effectDuration(flag, dur)
							if formula {
								sum.formulaDurations++
							}
						} else {
							opt.logf("warning: rank %s names effect %d, which is no row: no effect", rank.Name, effectGuid)
							effectGuid = 0
						}
					}
					if err := w.exec(`INSERT INTO skill_rank (skill_guid, ord, rank_guid, name, ability_guid, ability_name, effect_guid, effect_name, effect_infinite, effect_duration)
					VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
						skillGuid, ord, rankGuid, rank.Name, abilityGuid, abilityName, effectGuid, effectName, b2i(infinite), seconds); err != nil {
						return err
					}
					sum.ranks++
					if !writtenCosts[rankGuid] {
						writtenCosts[rankGuid] = true
						written := 0
						for k, cv := range pointCosts {
							cs, ok := cv.(designdb.Struct)
							if !ok {
								return fmt.Errorf("%s: SkillPointCosts[%d] is %T", cost.where, k, cv)
							}
							cf := newFields(fmt.Sprintf("%s: SkillPointCosts[%d]", cost.where, k), cs)
							qty := cf.int("Quantity")
							pt, _ := cf.id("SkillPointTypeId")
							if cf.err != nil {
								return cf.err
							}
							if qty <= 0 || qty > 1000 || pt == 0 {
								continue
							}
							if err := w.exec(`INSERT INTO skill_rank_cost (rank_guid, ord, point_type_guid, quantity) VALUES (?, ?, ?, ?)`, rankGuid, written, pt, qty); err != nil {
								return err
							}
							written++
							sum.costs++
						}
					}
					// the constant stat mods of an infinite effect (Endurance Training: MaxStamina +10): a passive that stays on the character;
					// a finite effect's mods (Sprint Training's, 1 s on the sprint) belong to its own ability and to plan D2's effect system
					if effectGuid != 0 && infinite && !writtenMods[rankGuid] {
						writtenMods[rankGuid] = true
						e := effects[effectGuid]
						xf := newFields(fmt.Sprintf("Effect %s (%d)", e.Name, e.Guid), e.Fields)
						modIds := xf.ids("StatModsIds")
						if xf.err != nil {
							return xf.err
						}
						ord := 0
						for _, mg := range modIds {
							m, ok := mods[mg]
							if !ok {
								continue
							}
							mf := newFields(fmt.Sprintf("StatMod %s (%d)", m.Name, m.Guid), m.Fields)
							statGuid, _ := mf.id("StatRefId")
							valueText := mf.str("Value")
							if mf.err != nil {
								return mf.err
							}
							v, err := strconv.ParseFloat(strings.TrimSpace(valueText), 64)
							if err != nil || !finite(v) {
								continue // a mod with a formula is the effect system's (plan D2): not a constant the stat engine takes
							}
							if err := w.exec(`INSERT INTO skill_rank_stat_mod (rank_guid, ord, stat_guid, stat_name, value) VALUES (?, ?, ?, ?, ?)`,
								rankGuid, ord, statGuid, statNames[statGuid], v); err != nil {
								return err
							}
							ord++
							sum.mods++
						}
					}
				}
			}
		}
	}

	// the weapons: the combo ability and the weapon line of every item that names one
	lineNames := map[int64]string{}
	for _, r := range src.rows["WeaponSkillLinesRecordBase"] {
		lineNames[r.Guid] = r.Name
	}
	for _, r := range src.rows["ItemRecordBase"] {
		f := newFields(fmt.Sprintf("Item %s (%d)", r.Name, r.Guid), r.Fields)
		var attack, line int64
		if _, ok := f.get("AttackAbilityId"); ok {
			attack, _ = f.id("AttackAbilityId")
		}
		if _, ok := f.get("WeaponSkillLineId"); ok {
			line, _ = f.id("WeaponSkillLineId")
		}
		if f.err != nil {
			return f.err
		}
		if attack == 0 && line == 0 {
			continue
		}
		if err := w.exec(`INSERT INTO item_weapon (item_guid, attack_ability_guid, skill_line_guid, skill_line_name) VALUES (?, ?, ?, ?)`,
			r.Guid, attack, line, lineNames[line]); err != nil {
			return err
		}
		sum.weapons++
	}

	opt.logf("talents: %d trees, %d nodes (%d without a skill or a slot left out; %d slot nodes written as %d options), %d ranks, %d point costs, %d requirements (%d naming no record: met), %d exclusions, "+
		"%d constant stat mods, %d level point rows, %d weapon items; %d effect durations that are formulas (infinite), %d ranks with a currency or commodity cost, "+
		"%d node entries that repeat a node of their tree; %d requirements name a slot node (any option)",
		sum.trees, sum.nodes, sum.noSkillNodes, sum.slotNodes, sum.slotOptions, sum.ranks, sum.costs, sum.requires, sum.missingRequirement, sum.exclusives, sum.mods, sum.points, sum.weapons,
		sum.formulaDurations, sum.currencyCosts, sum.repeatedNodes, sum.slotRequirements)
	meta["typed.skill_tree"] = strconv.Itoa(sum.trees)
	meta["typed.skill_node"] = strconv.Itoa(sum.nodes)
	meta["typed.skill_rank"] = strconv.Itoa(sum.ranks)
	meta["typed.skill_rank_stat_mod"] = strconv.Itoa(sum.mods)
	meta["typed.item_weapon"] = strconv.Itoa(sum.weapons)
	return nil
}

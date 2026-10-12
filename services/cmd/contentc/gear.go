// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"math"
	"strings"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// gearRecordTypes are the record structs buildGear reads besides the item tables' (plan B3).
var gearRecordTypes = map[string]uint64{
	"SetBonusRecordBase": content.TypeSetBonus,
}

// buildGear fills quality_curve, stat_block_stat, item_set, set_bonus and item_tag (plan B3): everything an item adds to a
// character's stats and says to the stat formulas, read from the same rows as services/internal/statprog's Gear (which reads
// the records' JSON). Every rule it checks is one the engine needs to hold: a curve an item names exists and has its seven
// rarities, every stat block an item names exists, a stat appears once in a block, every set an item names exists, every
// number is finite. A stat key that is no StatTypeDef row is kept (the engine ignores such a mod and counts it).
func buildGear(w *writer, src *source, opt *Options, meta map[string]string) error {
	for schema, want := range gearRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
		if got := src.typeIDs[schema]; got != want {
			return fmt.Errorf("%s has TypeId %d in the database, content expects %d", schema, got, want)
		}
	}

	// Quality curves.
	curves := map[int64]bool{}
	for _, r := range src.rows["QualityCurveRecordBase"] {
		f := newFields(fmt.Sprintf("QualityCurve %s (%d)", r.Name, r.Guid), r.Fields)
		for i, rarity := range rarities {
			bf := newFields(f.where+": "+rarity, f.strukt(rarity))
			x, y := bf.float("X"), bf.float("Y")
			if err := firstErr(f, bf); err != nil {
				return err
			}
			if !finite(x, y) || y < x {
				return fmt.Errorf("%s: %s covers %v to %v", f.where, rarity, x, y)
			}
			if err := w.exec(`INSERT INTO quality_curve (guid, ord, x, y) VALUES (?, ?, ?, ?)`, r.Guid, i, x, y); err != nil {
				return err
			}
		}
		curves[r.Guid] = true
	}

	// Stat blocks: the stats a block gives, by rarity. Only the stats of the design database are allowed.
	stats := map[int64]bool{}
	for _, r := range src.rows["StatTypeDefRecordBase"] {
		stats[r.Guid] = true
	}
	blocks := map[int64]bool{}
	unknownBlockStats := map[int64]bool{} // stat keys of stat blocks and set bonuses that are no StatTypeDef row
	nstats := 0
	for _, r := range src.rows["ItemStatBlockRecordBase"] {
		f := newFields(fmt.Sprintf("ItemStatBlock %s (%d)", r.Name, r.Guid), r.Fields)
		seen := map[int64]bool{}
		for i, p := range f.pairs("Stats") {
			key, ok := p.Key.(int64)
			vs, vok := p.Value.(designdb.Struct)
			if !ok || !vok {
				return fmt.Errorf("%s: Stats[%d] is a %T to a %T", f.where, i, p.Key, p.Value)
			}
			if !stats[key] {
				unknownBlockStats[key] = true // kept: the engine ignores a stat it does not have (and counts it)
			}
			if seen[key] {
				return fmt.Errorf("%s: the stat %d appears twice", f.where, key)
			}
			seen[key] = true
			vf := newFields(fmt.Sprintf("%s: stat %d", f.where, key), vs)
			args := []any{r.Guid, key}
			for _, rarity := range rarities {
				lo, hi := vf.float(rarity+"Min"), vf.float(rarity+"Max")
				if vf.err == nil && !finite(lo, hi) {
					return fmt.Errorf("%s: %s is not finite", vf.where, rarity)
				}
				args = append(args, lo, hi)
			}
			if vf.err != nil {
				return vf.err
			}
			if err := w.exec(`INSERT INTO stat_block_stat (block_guid, stat_guid, common_min, common_max, uncommon_min, uncommon_max,
				rare_min, rare_max, heroic_min, heroic_max, epic_min, epic_max, legendary_min, legendary_max, artifact_min, artifact_max)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, args...); err != nil {
				return err
			}
			nstats++
		}
		if f.err != nil {
			return f.err
		}
		blocks[r.Guid] = true
	}

	// Set bonuses.
	sets := map[int64]bool{}
	nbonus := 0
	for _, r := range src.rows["SetBonusRecordBase"] {
		f := newFields(fmt.Sprintf("SetBonus %s (%d)", r.Name, r.Guid), r.Fields)
		for ti, tp := range f.pairs("SetStatBonuses") {
			pieces, ok := tp.Key.(int64)
			ts, sok := tp.Value.(designdb.Struct)
			if !ok || !sok {
				return fmt.Errorf("%s: SetStatBonuses[%d] is a %T to a %T", f.where, ti, tp.Key, tp.Value)
			}
			tf := newFields(fmt.Sprintf("%s: tier %d", f.where, pieces), ts)
			for bi, bv := range tf.list("StatBonuses") {
				bs, ok := bv.(designdb.Struct)
				if !ok {
					return fmt.Errorf("%s: StatBonuses[%d] is a %T", tf.where, bi, bv)
				}
				bf := newFields(fmt.Sprintf("%s: StatBonuses[%d]", tf.where, bi), bs)
				stat, _ := bf.id("AffectedStat")
				by := bf.pairs("StatBonuses")
				if err := firstErr(tf, bf); err != nil {
					return err
				}
				if !stats[stat] {
					unknownBlockStats[stat] = true
				}
				var vals [7]any
				for ei, e := range by {
					name, ok := e.Key.(string)
					if !ok {
						return fmt.Errorf("%s: StatBonuses[%d] has a %T key", bf.where, ei, e.Key)
					}
					rarity := name[strings.LastIndex(name, ":")+1:]
					idx := -1
					for i, rr := range rarities {
						if rr == rarity {
							idx = i
						}
					}
					if idx < 0 {
						continue // a key that is no rarity matches nothing, as it does not in the engine's rule
					}
					var v float64
					switch x := e.Value.(type) {
					case float32:
						v = decimal32(x)
					case float64:
						v = x
					default:
						return fmt.Errorf("%s: the value of %s is a %T", bf.where, name, e.Value)
					}
					if !finite(v) {
						return fmt.Errorf("%s: the value of %s is not finite", bf.where, name)
					}
					vals[idx] = v // the last entry of a rarity wins, as in the engine's rule
				}
				if err := w.exec(`INSERT INTO set_bonus (set_guid, tier_ord, pieces, bonus_ord, stat_guid, common, uncommon, rare, heroic, epic, legendary,
					artifact) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, ti, pieces, bi, stat,
					vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6]); err != nil {
					return err
				}
				nbonus++
			}
			if tf.err != nil {
				return tf.err
			}
		}
		if f.err != nil {
			return f.err
		}
		sets[r.Guid] = true
	}

	// Items: the sets and the tags; their curve and block must exist.
	ntags, nitemSets := 0, 0
	for _, r := range src.rows["ItemRecordBase"] {
		f := newFields(fmt.Sprintf("Item %s (%d)", r.Name, r.Guid), r.Fields)
		block, _ := f.id("StatBlockId")
		curve, _ := f.id("QualityCurveId")
		setIDs := f.ids("SetBonusIds")
		tags := f.tags("GameplayTags")
		if f.err != nil {
			return f.err
		}
		if block != 0 && !blocks[block] {
			return fmt.Errorf("%s: its stat block %d is no ItemStatBlock", f.where, block)
		}
		if curve != 0 && !curves[curve] {
			return fmt.Errorf("%s: its quality curve %d is no QualityCurve", f.where, curve)
		}
		for i, s := range setIDs {
			if !sets[s] {
				return fmt.Errorf("%s: its set %d is no SetBonus", f.where, s)
			}
			if err := w.exec(`INSERT INTO item_set (item_guid, ord, set_guid) VALUES (?, ?, ?)`, r.Guid, i, s); err != nil {
				return err
			}
			nitemSets++
		}
		for i, t := range tags {
			if err := w.exec(`INSERT INTO item_tag (item_guid, ord, tag) VALUES (?, ?, ?)`, r.Guid, i, t); err != nil {
				return err
			}
			ntags++
		}
	}
	opt.logf("gear: %d stat keys are no StatTypeDef row (kept)", len(unknownBlockStats))
	meta["typed.stat_block_unknown_stats"] = fmt.Sprint(len(unknownBlockStats))
	opt.logf("gear: %d quality curves, %d stat blocks with %d stats, %d set bonuses with %d bonuses, %d item sets, %d item tags",
		len(curves), len(blocks), nstats, len(sets), nbonus, nitemSets, ntags)
	meta["typed.quality_curve"] = fmt.Sprint(len(curves))
	meta["typed.stat_block_stat"] = fmt.Sprint(nstats)
	meta["typed.set_bonus"] = fmt.Sprint(nbonus)
	meta["typed.item_tag"] = fmt.Sprint(ntags)
	return nil
}

// C-O-A : C.Y.B

func finite(xs ...float64) bool {
	for _, x := range xs {
		if math.IsNaN(x) || math.IsInf(x, 0) {
			return false
		}
	}
	return true
}

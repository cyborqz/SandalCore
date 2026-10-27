// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"regexp"
	"sort"
	"strings"
)

// Currencies and the bags' stack sizes (the stacking milestone, step 1, "Glint tiers"; contentc version 24): currency_tier and currency_def from the CurrencyTierDef and CurrencyDef
// records, and bag_stack_mod from the stat blocks of the items that carry a stack-size stat (internal/content/currency.go says what each holds). The records are copied as they
// are: nothing here is evaluated. The item column stack_stats and the names of the Glint kinds are written by items.go.

// maxStackStat is the family of stats that change the size of the stacks a container holds: Artisan_Gathering_Mining_Add_MaxStack (a profession's materials), Artisan_Gathering_Add_MaxStack
// (every gathering material) and Artisan_Processing_Add_MaxStack, each also as _Mult_.
var maxStackStat = regexp.MustCompile(`^Artisan_(Gathering|Processing)(?:_([A-Za-z]+))?_(Add|Mult)_MaxStack$`)

// stackStatTag is the gameplay tag a stack-size stat applies to, from its name, and its action (0 Add, 1 Mult); ok false when the name is not of the family.
func stackStatTag(name string) (tag string, action int, ok bool) {
	m := maxStackStat.FindStringSubmatch(name)
	if m == nil {
		return "", 0, false
	}
	tag = "Artisanship." + m[1]
	if m[2] != "" {
		tag += "." + m[2]
	}
	if m[3] == "Mult" {
		action = 1
	}
	return tag, action, true
}

func buildCurrency(w *writer, src *source, opt *Options, meta map[string]string) error {
	tiers, err := src.need("CurrencyTierDefRecordBase")
	if err != nil {
		return err
	}
	defs, err := src.need("CurrencyDefRecordBase")
	if err != nil {
		return err
	}
	isDef := make(map[int64]bool, len(defs))
	for _, r := range defs {
		isDef[r.Guid] = true
	}
	isItem := make(map[int64]bool, len(src.rows["ItemRecordBase"]))
	for _, r := range src.rows["ItemRecordBase"] {
		isItem[r.Guid] = true
	}

	// the tiers: each lists its kinds in order, smallest first
	type place struct {
		tier int64
		ord  int
	}
	listed := map[int64]place{}
	for _, r := range tiers {
		f := newFields(fmt.Sprintf("CurrencyTierDef %s (%d)", r.Name, r.Guid), r.Fields)
		maxBase := f.int("MaxBaseQuantity")
		kinds := f.ids("TiersIds")
		if f.err != nil {
			return f.err
		}
		for ord, d := range kinds {
			if !isDef[d] {
				return fmt.Errorf("%s: TiersIds[%d] is %d, which is none of the CurrencyDef rows", f.where, ord, d)
			}
			if p, dup := listed[d]; dup {
				return fmt.Errorf("%s: the currency %d is also in the tier %d", f.where, d, p.tier)
			}
			listed[d] = place{r.Guid, ord}
		}
		if err := w.exec(`INSERT INTO currency_tier (guid, name, max_base_quantity) VALUES (?, ?, ?)`, r.Guid, r.Name, maxBase); err != nil {
			return err
		}
	}

	// the kinds
	carried, unknown := 0, 0
	for _, r := range defs {
		f := newFields(fmt.Sprintf("CurrencyDef %s (%d)", r.Name, r.Guid), r.Fields)
		item, _ := f.id("ItemOverrideId")
		rate := f.int("TieredConversionRate")
		if f.err != nil {
			return f.err
		}
		if item != 0 {
			carried++
			if !isItem[item] {
				unknown++ // kept: a kind whose item the database does not hold simply has no item to meet
			}
		}
		p := listed[r.Guid]
		if err := w.exec(`INSERT INTO currency_def (guid, name, tier_guid, ord, item_guid, conversion_rate) VALUES (?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, p.tier, p.ord, item, rate); err != nil {
			return err
		}
	}
	opt.logf("currency: %d tiers, %d currencies (%d carried as items, %d of them with no item record)", len(tiers), len(defs), carried, unknown)
	meta["typed.currency_tier"] = fmt.Sprint(len(tiers))
	meta["typed.currency_def"] = fmt.Sprint(len(defs))

	// the stack-size stats of the stat blocks: stat_block_stat has them already (buildGear runs before this); the tag and the action come from the stat's name
	var statGuids []int64
	for guid, name := range src.statNames {
		if _, _, ok := stackStatTag(name); ok {
			statGuids = append(statGuids, guid)
		}
	}
	sort.Slice(statGuids, func(i, j int) bool { return statGuids[i] < statGuids[j] })
	type mod struct {
		item, stat int64
		values     [14]float64
	}
	var mods []mod
	if len(statGuids) > 0 {
		in := make([]string, len(statGuids))
		for i, g := range statGuids {
			in[i] = fmt.Sprint(g)
		}
		rows, err := w.tx.Query(`SELECT i.guid, s.stat_guid, s.common_min, s.common_max, s.uncommon_min, s.uncommon_max, s.rare_min, s.rare_max, s.heroic_min, s.heroic_max,
				s.epic_min, s.epic_max, s.legendary_min, s.legendary_max, s.artifact_min, s.artifact_max
			FROM item i JOIN stat_block_stat s ON s.block_guid = i.stat_block_guid WHERE s.stat_guid IN (` + strings.Join(in, ",") + `) ORDER BY i.guid, s.stat_guid`)
		if err != nil {
			return err
		}
		for rows.Next() {
			var m mod
			if err := rows.Scan(&m.item, &m.stat, &m.values[0], &m.values[1], &m.values[2], &m.values[3], &m.values[4], &m.values[5], &m.values[6], &m.values[7], &m.values[8],
				&m.values[9], &m.values[10], &m.values[11], &m.values[12], &m.values[13]); err != nil {
				rows.Close()
				return err
			}
			mods = append(mods, m)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		rows.Close()
	}
	for _, m := range mods {
		name := src.statNames[m.stat]
		tag, action, _ := stackStatTag(name)
		args := []any{m.item, m.stat, name, tag, action}
		for _, v := range m.values {
			args = append(args, v)
		}
		if err := w.exec(`INSERT INTO bag_stack_mod (item_guid, stat_guid, stat_name, tag, action, common_min, common_max, uncommon_min, uncommon_max, rare_min, rare_max, heroic_min, heroic_max,
				epic_min, epic_max, legendary_min, legendary_max, artifact_min, artifact_max) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, args...); err != nil {
			return err
		}
	}
	opt.logf("bag_stack_mod: %d stack-size stats of %d stat types in the stat blocks", len(mods), len(statGuids))
	meta["typed.bag_stack_mod"] = fmt.Sprint(len(mods))
	return nil
}

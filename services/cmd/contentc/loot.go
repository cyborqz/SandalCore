// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"
	"strconv"

	"aocrop/services/internal/designdb"
)

// The reward tables (plan E1, stage 3, part 2; contentc version 10): every RewardTableRecordBase as a JSON document in loot_table (the document is
// described in package content, loot.go).  Every expression of a table (the predicates, the percentages and weights, the quantities and amounts, the
// experience values) goes through the ability tables' expression set, so it is compiled into the stat program with the others; one that does not
// compile (a quest state, a node's biome, a profession) has no program and worldd reads it as the table's "does not apply".

type lootReward struct {
	XP       []lootXP       `json:"xp,omitempty"`
	Items    []lootItem     `json:"items,omitempty"`
	Currency []lootCurrency `json:"currency,omitempty"`
}

type lootXP struct {
	Type string `json:"type"`
	Expr int64  `json:"expr"`
}

type lootItem struct {
	Item      int64 `json:"item"`
	Variation int64 `json:"variation,omitempty"`
	Qty       int64 `json:"qty"`
}

type lootCurrency struct {
	Currency int64 `json:"currency"`
	Amount   int64 `json:"amount"`
}

type lootContainer struct {
	Grant string       `json:"grant,omitempty"`
	Pred  int64        `json:"pred,omitempty"`
	Algo  string       `json:"algo,omitempty"`
	N     int64        `json:"n,omitempty"`
	W     []float64    `json:"w,omitempty"`
	Pct   []int64      `json:"pct,omitempty"`
	XW    []int64      `json:"xw,omitempty"`
	R     []lootReward `json:"r,omitempty"`
}

// C | O | A and C:Y:B

type lootDoc struct {
	Pred int64           `json:"pred,omitempty"`
	Algo string          `json:"algo,omitempty"`
	N    int64           `json:"n,omitempty"`
	Subs []int64         `json:"subs,omitempty"`
	W    []float64       `json:"w,omitempty"`
	Pct  []int64         `json:"pct,omitempty"`
	XW   []int64         `json:"xw,omitempty"`
	C    []lootContainer `json:"c,omitempty"`
}

// exprList reads a list of expression texts into ab_expr ids (a number is the expression of its text).
func (b *abBuilder) exprList(f *fields, name string) []int64 {
	var out []int64
	for i, v := range f.list(name) {
		var text string
		switch x := v.(type) {
		case string:
			text = x
		case int64:
			text = strconv.FormatInt(x, 10)
		case float32:
			text = strconv.FormatFloat(decimal32(x), 'g', -1, 64)
		case float64:
			text = strconv.FormatFloat(x, 'g', -1, 64)
		default:
			f.fail(name, fmt.Sprintf("expression %d", i), v, true)
			return nil
		}
		id, err := b.exprs.id(text, nil)
		if err != nil {
			f.err = err
			return nil
		}
		out = append(out, id)
	}
	return out
}

// expr reads one expression text into its ab_expr id (0 for none).
func (b *abBuilder) exprField(f *fields, name string) int64 {
	text := f.str(name)
	if f.err != nil {
		return 0
	}
	id, err := b.exprs.id(text, nil)
	if err != nil {
		f.err = err
	}
	return id
}

// numberList reads a list of numbers.
func numberList(f *fields, name string) []float64 {
	var out []float64
	for _, v := range f.list(name) {
		switch x := v.(type) {
		case int64:
			out = append(out, float64(x))
		case float32:
			out = append(out, decimal32(x))
		case float64:
			out = append(out, x)
		default:
			f.fail(name, "numbers", v, true)
			return nil
		}
	}
	return out
}

// currencyPairs reads a Currency map (an Id key and an expression amount), whether the decoder gives it as pairs or as a list of {Key, Value} structs.
func (b *abBuilder) currency(f *fields, name string) []lootCurrency {
	v, ok := f.get(name)
	if !ok {
		f.fail(name, "map", v, ok)
		return nil
	}
	var pairs []designdb.Pair
	switch x := v.(type) {
	case designdb.Pairs:
		pairs = x
	case []designdb.Value:
		for _, e := range x {
			s, ok := e.(designdb.Struct)
			if !ok {
				f.fail(name, "key/value structs", e, true)
				return nil
			}
			pairs = append(pairs, designdb.Pair{Key: s["Key"], Value: s["Value"]})
		}
	default:
		f.fail(name, "map", v, true)
		return nil
	}
	var out []lootCurrency
	for i, p := range pairs {
		ks, ok := p.Key.(designdb.Struct)
		text, ok2 := p.Value.(string)
		if !ok || !ok2 {
			f.fail(name, fmt.Sprintf("currency %d", i), p, true)
			return nil
		}
		g, _ := idOf(f, fmt.Sprintf("%s: %s[%d]", f.where, name, i), ks)
		id, err := b.exprs.id(text, nil)
		if err != nil {
			f.err = err
			return nil
		}
		out = append(out, lootCurrency{Currency: g, Amount: id})
	}
	return out
}

// CoA.C-Y-B

// buildLoot writes loot_table: every reward table, its expressions in the ability expression set.
func (b *abBuilder) buildLoot(opt *Options, meta map[string]string) error {
	rows := b.src.rows["RewardTableRecordBase"]
	n, nexpr0 := 0, b.exprs.next
	for _, r := range rows {
		f := newFields(fmt.Sprintf("RewardTable %s (%d)", r.Name, r.Guid), r.Fields)
		doc := lootDoc{
			Pred: b.exprField(f, "Predicate"),
			Algo: enumValue(f.str("SubTableSelectionAlgorithm")),
			N:    f.int("NumberOfSubtablesToSelect"),
			Subs: f.ids("SubTablesIds"),
			W:    numberList(f, "WeightsPerSubTable"),
			Pct:  b.exprList(f, "PercentagePerSubTable"),
			XW:   b.exprList(f, "ExpressionWeightsPerSubTable"),
		}
		if doc.Algo == "None" {
			doc.Algo = ""
		}
		for ci, cv := range f.list("RewardDefContainers") {
			cs, ok := cv.(designdb.Struct)
			if !ok {
				f.fail("RewardDefContainers", fmt.Sprintf("struct %d", ci), cv, true)
				break
			}
			cf := newFields(fmt.Sprintf("%s: container %d", f.where, ci), cs)
			c := lootContainer{
				Grant: enumValue(cf.str("GrantMethod")),
				Pred:  b.exprField(cf, "GrantPredicate"),
				Algo:  enumValue(cf.str("SelectionAlgorithm")),
				N:     cf.int("NumberToSelect"),
				W:     numberList(cf, "WeightsPerReward"),
				Pct:   b.exprList(cf, "PercentageDropRatePerReward"),
				XW:    b.exprList(cf, "ExpressionWeightsPerReward"),
			}
			for ri, rv := range cf.list("Rewards") {
				rs, ok := rv.(designdb.Struct)
				if !ok {
					cf.fail("Rewards", fmt.Sprintf("struct %d", ri), rv, true)
					break
				}
				rf := newFields(fmt.Sprintf("%s: reward %d", cf.where, ri), rs)
				var rew lootReward
				for _, xv := range rf.list("ExperienceRewards") {
					xs, ok := xv.(designdb.Struct)
					if !ok {
						rf.fail("ExperienceRewards", "struct", xv, true)
						break
					}
					xf := newFields(rf.where+": experience", xs)
					rew.XP = append(rew.XP, lootXP{Type: enumValue(xf.str("ExperienceType")), Expr: b.exprField(xf, "ExperienceValue")})
					if xf.err != nil {
						rf.err = xf.err
					}
				}
				for _, iv := range rf.list("ItemRewards") {
					is, ok := iv.(designdb.Struct)
					if !ok {
						rf.fail("ItemRewards", "struct", iv, true)
						break
					}
					itf := newFields(rf.where+": item", is)
					ref := newFields(itf.where+": Item", itf.strukt("Item"))
					item, _ := ref.id("ItemId")
					variation, _ := ref.id("VariationId")
					rew.Items = append(rew.Items, lootItem{Item: item, Variation: variation, Qty: b.exprField(itf, "Quantity")})
					if err := firstErr(itf, ref); err != nil {
						rf.err = err
					}
				}
				rew.Currency = b.currency(rf, "Currency")
				if rf.err != nil {
					cf.err = rf.err
					break
				}
				c.R = append(c.R, rew)
			}
			if cf.err != nil {
				f.err = cf.err
				break
			}
			doc.C = append(doc.C, c)
		}
		if f.err != nil {
			return f.err
		}
		data, err := json.Marshal(doc)
		if err != nil {
			return err
		}
		if err := b.w.exec(`INSERT INTO loot_table (guid, name, data) VALUES (?, ?, ?)`, r.Guid, r.Name, string(data)); err != nil {
			return err
		}
		n++
	}
	// the experience decay of the Adventuring leveling type: what a kill is worth to a player of another level than the creature
	for _, r := range b.src.rows["LevelingTypeDefRecordBase"] {
		if r.Name != "Adventuring" {
			continue
		}
		f := newFields(fmt.Sprintf("LevelingTypeDef %s (%d)", r.Name, r.Guid), r.Fields)
		id := b.exprField(f, "ExperienceDecayCurve")
		if f.err != nil {
			return f.err
		}
		if err := b.w.exec(`INSERT INTO xp_rule (key, expr) VALUES ('adventuring_decay', ?)`, id); err != nil {
			return err
		}
	}
	opt.logf("loot_table: %d reward tables, %d new expressions", n, b.exprs.next-nexpr0)
	meta["typed.loot_table"] = strconv.Itoa(n)
	meta["loot.exprs"] = strconv.FormatInt(b.exprs.next-nexpr0, 10)
	return nil
}

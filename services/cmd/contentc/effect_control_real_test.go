// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// The effect-control export (contentc version 39) on the real design database (the shared build of real_test.go).

import (
	"strings"
	"testing"
)

func TestRealEffectControl(t *testing.T) {
	db := builtDB(t)
	// one control row per effect, and the counts of the game's data (compare_source 78 of the 132 enemy-applied effects, 1,059 of all 1,361)
	for _, c := range []struct {
		what  string
		query string
		want  int64
	}{
		{"effects", `SELECT count(*) FROM ab_effect`, 1361},
		{"control rows", `SELECT count(*) FROM ab_effect_control`, 1361},
		{"effects without a control row", `SELECT count(*) FROM ab_effect e WHERE NOT EXISTS (SELECT 1 FROM ab_effect_control c WHERE c.effect_guid = e.guid)`, 0},
		{"effects that compare sources", `SELECT count(*) FROM ab_effect_control WHERE compare_source = 1`, 1059},
		{"effects that recalculate their tick hits", `SELECT count(*) FROM ab_effect_control WHERE recalc_on_tick = 1`, 12},
		{"effects that lose stacks as their time runs out", `SELECT count(*) FROM ab_effect_control WHERE stacks_lost_on_elapse = 1`, 19},
		{"effects with a stack-loss amount", `SELECT count(*) FROM ab_effect_control WHERE stacks_lost_expr <> 0`, 32},
		{"effects with an extension amount", `SELECT count(*) FROM ab_effect_control WHERE extension_expr <> 0`, 22},
		{"effects with a longest extension that is not the default", `SELECT count(*) FROM ab_effect_control WHERE extended_max NOT IN (0, 50)`, 47},
		{"crowd-control records", `SELECT count(*) FROM ab_crowd_control`, 26},
		{"ability-disable records", `SELECT count(*) FROM ab_ability_disable`, 5},
		{"fragility records", `SELECT count(*) FROM ab_fragility`, 26},
		{"effect fragility links", `SELECT count(*) FROM ab_effect_fragility`, 38},
		// every record an effect names is a row
		{"effects naming a crowd-control record that is no row", `SELECT count(*) FROM ab_effect e WHERE e.crowd_control_guid <> 0 AND NOT EXISTS (SELECT 1 FROM ab_crowd_control c WHERE c.guid = e.crowd_control_guid)`, 0},
		{"crowd-control records naming a disable record that is no row", `SELECT count(*) FROM ab_crowd_control c WHERE c.disable_guid <> 0 AND NOT EXISTS (SELECT 1 FROM ab_ability_disable d WHERE d.guid = c.disable_guid)`, 0},
		{"fragility links to a record that is no row", `SELECT count(*) FROM ab_effect_fragility l WHERE NOT EXISTS (SELECT 1 FROM ab_fragility f WHERE f.guid = l.fragility_guid)`, 0},
		// the stack-loss and extension texts are expressions of the table, the extension of an ExtendDuration effect compiles but for the burn aura's
		{"control expressions that are no row", `SELECT count(*) FROM ab_effect_control c WHERE (c.stacks_lost_expr <> 0 AND NOT EXISTS (SELECT 1 FROM ab_expr x WHERE x.id = c.stacks_lost_expr))
			OR (c.extension_expr <> 0 AND NOT EXISTS (SELECT 1 FROM ab_expr x WHERE x.id = c.extension_expr))`, 0},
		{"extension amounts that do not compile", `SELECT count(*) FROM ab_effect_control c JOIN ab_expr x ON x.id = c.extension_expr WHERE c.extension_expr <> 0 AND x.program < 0`, 1},
	} {
		if got := scalarInt(t, db, c.query); got != c.want {
			t.Errorf("%s: %d, want %d", c.what, got, c.want)
		}
	}
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	for k, v := range map[string]string{"ab.crowd_controls": "26", "ab.ability_disables": "5", "ab.fragilities": "26"} {
		if meta[k] != v {
			t.Errorf("meta %s = %q, want %q", k, meta[k], v)
		}
	}

	// the effects of the creature milestone, as the export holds them
	control := func(name string) (c struct {
		compare, recalc bool
		max             float64
		extension       string
		compiles        bool
		guid            int64
	}) {
		t.Helper()
		e, err := db.EffectRowByName(name)
		if err != nil {
			t.Fatal(err)
		}
		r, err := db.EffectControl(e.Guid)
		if err != nil {
			t.Fatal(err)
		}
		c.compare, c.recalc, c.max, c.guid = r.CompareSource, r.RecalcOnTick, r.ExtendedMax, e.Guid
		if r.ExtensionExpr != 0 {
			x, err := db.Expr(r.ExtensionExpr)
			if err != nil {
				t.Fatal(err)
			}
			c.extension, c.compiles = x.Text, x.Program >= 0
		}
		return c
	}
	for _, n := range []string{"Status_Bleeding", "Status_Hemorrhage", "Status_Poisoned"} {
		if c := control(n); !c.compare || !c.recalc || c.extension != "" {
			t.Errorf("%s: compare sources %v, recalculates ticks %v, extension %q; want a bleed's: compares sources, recalculates, no extension", n, c.compare, c.recalc, c.extension)
		}
	}
	for n, max := range map[string]float64{"Status_Shaken": 15, "Status_Volatile": 15, "Status_Staggered": 15, "Status_Dazed": 20, "Status_Weakened": 20, "Status_Demoralized": 20, "Status_Chilled": 15, "Status_BurningAura": 120} {
		c := control(n)
		if c.max != max || c.extension == "" {
			t.Errorf("%s: longest extension %v and extension %q, want %v and a text", n, c.max, c.extension, max)
		}
		if n != "Status_BurningAura" && !c.compiles {
			t.Errorf("%s: its extension amount does not compile: %s", n, c.extension)
		}
	}
	if c := control("Status_BurningAura"); c.compiles || !strings.Contains(c.extension, "GetTerm") {
		t.Errorf("the burn aura's extension amount (%q) is a term of another formula and does not compile (%v)", c.extension, c.compiles)
	}
	if c := control("Status_Shaken"); !strings.Contains(c.extension, "GetEffectRemainingDuration") || !strings.Contains(c.extension, "Max(6.0 - dur, 3.0)") {
		t.Errorf("Shaken's extension amount = %q, want max(6 - what is left, 3)", c.extension)
	}

	// the crowd-control records of the slows, the stun and the trip
	for n, w := range map[string]struct {
		status   int64
		snare    float64
		priority int64
	}{"General_Snare": {16, 0.25, 0}, "Mage_Status_Chilled_Snare": {16, 0.5, 0}, "General_Stun": {7, 0, 200}, "General_Trip": {7, 0, 300}, "General_Root": {1, 0, 0}, "Status_Silence": {4, 0, 0}} {
		var guid int64
		if err := db.SQL().QueryRow(`SELECT guid FROM ab_crowd_control WHERE name = ?`, n).Scan(&guid); err != nil {
			t.Errorf("crowd control %s: %v", n, err)
			continue
		}
		r, err := db.CrowdControl(guid)
		if err != nil {
			t.Fatal(err)
		}
		if r.AppliedStatus != w.status || r.SnareStrength != w.snare || r.Priority != w.priority {
			t.Errorf("crowd control %s = status %d, snare %v, priority %d; want %d, %v, %d", n, r.AppliedStatus, r.SnareStrength, r.Priority, w.status, w.snare, w.priority)
		}
	}
	// a stun stops abilities (the disable record names a filter), a snare does not
	var stun, snare int64
	if err := db.SQL().QueryRow(`SELECT guid FROM ab_crowd_control WHERE name = 'General_Stun'`).Scan(&stun); err != nil {
		t.Fatal(err)
	}
	if err := db.SQL().QueryRow(`SELECT guid FROM ab_crowd_control WHERE name = 'General_Snare'`).Scan(&snare); err != nil {
		t.Fatal(err)
	}
	if r, _ := db.CrowdControl(stun); r.DisableGuid == 0 {
		t.Error("the stun names no ability-disable record")
	} else if d, err := db.AbilityDisable(r.DisableGuid); err != nil || d.Name != "AbilityDisable_All" || d.Filter != "true" {
		t.Errorf("the stun's disable record = %+v, %v", d, err)
	}
	if r, _ := db.CrowdControl(snare); r.DisableGuid != 0 {
		t.Error("the snare names an ability-disable record")
	}
	// the sleep breaks at the first hit: the effect names the InstantBreak record
	sleep, err := db.EffectRowByName("Status_Incapacitated")
	if err != nil {
		t.Fatal(err)
	}
	links, err := db.EffectFragilities(sleep.Guid)
	if err != nil || len(links) != 1 {
		t.Fatalf("the sleep's fragility records = %v, %v", links, err)
	}
	if f, err := db.Fragility(links[0]); err != nil || f.Name != "InstantBreak" {
		t.Errorf("the sleep's fragility = %+v, %v; want InstantBreak", f, err)
	}
}

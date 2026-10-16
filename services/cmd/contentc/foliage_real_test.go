// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Gathering stage 3, "Foliage gathering" (contentc version 15), on the real design database (the shared build of real_test.go): the foliage tables against what the
// research of 2026-10-11 found in the data (402 GatheringFoliageData, 743 GatheringFoliageState, 236 GatheringInteractionData, 194 PlayerInteractionDefs that name one) and
// against region_foliage.tsv, the 73 defs of the Riverlands' 84,218 painted instances.

import (
	"bufio"
	"os"
	"strconv"
	"strings"
	"testing"

	"aocrop/services/internal/content"
)

func TestRealFoliageTables(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	count := func(q string, args ...any) int64 {
		t.Helper()
		return scalarInt(t, db, q, args...)
	}
	for table, want := range map[string]int64{"foliage_def": 402, "foliage_state": 743, "gather_interaction": 236, "gather_interaction_def": 194} {
		if got := count(`SELECT COUNT(*) FROM ` + table); got != want {
			t.Errorf("%s has %d rows, want %d", table, got, want)
		}
		if meta["typed."+table] != strconv.FormatInt(want, 10) {
			t.Errorf("meta typed.%s = %q, want %d", table, meta["typed."+table], want)
		}
	}
	defs, err := db.FoliageDefs()
	if err != nil || len(defs) != 402 {
		t.Fatalf("FoliageDefs = %d, %v", len(defs), err)
	}
	states, err := db.FoliageStates()
	if err != nil || len(states) != 743 {
		t.Fatalf("FoliageStates = %d, %v", len(states), err)
	}
	inters, err := db.GatherInteractions()
	if err != nil || len(inters) != 236 {
		t.Fatalf("GatherInteractions = %d, %v", len(inters), err)
	}
	stateOf := map[int64]content.FoliageState{}
	for _, s := range states {
		stateOf[s.Guid] = s
	}
	interOf := map[int64]content.GatherInteraction{}
	for _, g := range inters {
		interOf[g.Guid] = g
	}
	byName := map[string][]content.FoliageDef{}
	for _, d := range defs {
		byName[d.Name] = append(byName[d.Name], d)
	}

	// the one duplicate name of the schema: two records alike in every number, with state records of their own
	if len(byName["Stone_Limestone_Large_2"]) != 2 {
		t.Errorf("Stone_Limestone_Large_2 has %d defs, want 2", len(byName["Stone_Limestone_Large_2"]))
	}
	dup := 0
	for name, l := range byName {
		if len(l) > 1 {
			dup++
			if name != "Stone_Limestone_Large_2" {
				t.Errorf("%s has %d defs: the only duplicate name the research found is Stone_Limestone_Large_2", name, len(l))
			}
		}
	}
	if dup != 1 {
		t.Errorf("%d duplicate def names, want 1", dup)
	}

	// every def's three state slots are rows of foliage_state (0: none), and every state's interaction a row of gather_interaction (0: none): 424 states have one, 319 do not
	for _, d := range defs {
		for i, g := range d.StateGuids {
			if g == 0 {
				continue
			}
			if _, ok := stateOf[g]; !ok {
				t.Errorf("%s: state %d (%d) is no foliage_state", d.Name, i, g)
			}
		}
		for _, v := range []int{d.PaintState, d.GatherableState, d.MinState, d.MaxState} {
			if v < 0 || v > 3 {
				t.Errorf("%s: a state ordinal %d outside Infancy 0 .. Empty 3", d.Name, v)
			}
		}
	}
	withInteraction := 0
	for _, s := range states {
		if s.InteractionGuid == 0 {
			continue
		}
		withInteraction++
		if _, ok := interOf[s.InteractionGuid]; !ok {
			t.Errorf("state %s names interaction %d, which is no gather_interaction", s.Name, s.InteractionGuid)
		}
	}
	if withInteraction != 424 {
		t.Errorf("%d states have an interaction, want 424 (319 have none)", withInteraction)
	}

	// the Oak (capture 1428: Tree_Oak_Large_1): 300 s empty, then 1800 s as a sapling... the Adult state is permanent and holds the Large interaction, the Juvenile the Medium
	oaks := byName["Tree_Oak_Large_1"]
	if len(oaks) != 1 {
		t.Fatalf("Tree_Oak_Large_1: %d defs", len(oaks))
	}
	oak := oaks[0]
	if oak.EmptyDuration != 300 || oak.PaintState != content.StateAdult || oak.GatherableState != content.StateJuvenile || oak.MinState != content.StateInfancy || oak.MaxState != content.StateAdult {
		t.Errorf("Tree_Oak_Large_1 = %+v", oak)
	}
	adult, juv, inf := stateOf[oak.StateGuids[content.StateAdult]], stateOf[oak.StateGuids[content.StateJuvenile]], stateOf[oak.StateGuids[content.StateInfancy]]
	if adult.LifecycleDuration != 0 || adult.OverrideDistance != 125 || adult.OverrideAngle != 70 || adult.CollisionRadius != 205 {
		t.Errorf("the Oak's Adult state = %+v", adult)
	}
	if juv.LifecycleDuration != 1800 || juv.CollisionRadius != 50 || inf.InteractionGuid != 0 {
		t.Errorf("the Oak's Juvenile = %+v, Infancy = %+v", juv, inf)
	}
	large, medium := interOf[adult.InteractionGuid], interOf[juv.InteractionGuid]
	if large.Name != "Tree_Oak_Large" || large.BaseDuration != 15 || large.ToolSlot != slotAxe || large.CertTier != 0 || large.TargetState != content.StateEmpty ||
		large.InteractRadius != 250 || large.InteractTolerance != 500 || large.DurationExpr == 0 || large.DurabilityExpr == 0 || large.RewardGuid == 0 {
		t.Errorf("the Oak's Adult interaction = %+v", large)
	}
	if medium.Name != "Tree_Oak_Medium" || medium.BaseDuration != 7.5 || medium.ToolSlot != slotAxe {
		t.Errorf("the Oak's Juvenile interaction = %+v", medium)
	}
	// a plant (capture 1451: Plant_Daffodil_2): painted Juvenile, 1800 s empty, the Sickle's 11.25 s interaction, the Daffodil's own radius
	daf := byName["Plant_Daffodil_2"]
	if len(daf) != 1 || daf[0].EmptyDuration != 1800 || daf[0].PaintState != content.StateJuvenile {
		t.Fatalf("Plant_Daffodil_2 = %+v", daf)
	}
	dj := stateOf[daf[0].StateGuids[content.StateJuvenile]]
	di := interOf[dj.InteractionGuid]
	if di.Name != "Plant_Daffodil" || di.BaseDuration != 11.25 || di.ToolSlot != slotSickle || dj.OverrideDistance != 150 {
		t.Errorf("the Daffodil's Juvenile = %+v, interaction %+v", dj, di)
	}

	// the 236 interactions by tool: the nodes' and the foliage's, the 53 hunting ones (the Bow), fishing (11) and the one with no tool type
	bySlot := map[int]int{}
	for _, g := range inters {
		bySlot[g.ToolSlot]++
		if g.TargetState != content.StateEmpty {
			t.Errorf("%s: target state %d, want Empty", g.Name, g.TargetState)
		}
	}
	for slot, want := range map[int]int{slotPickaxe: 78, slotSickle: 53, slotBow: 53, slotAxe: 40, slotFishingRod: 11, -1: 1} {
		if bySlot[slot] != want {
			t.Errorf("%d interactions with tool slot %d, want %d", bySlot[slot], slot, want)
		}
	}
	// the PlayerInteractionDefs that name one: the 53 Hunting_<X> carry the hunting range (200 cm, tolerance 30), keyed by their own guid
	pdefs, err := db.GatherInteractionDefs()
	if err != nil || len(pdefs) != 194 {
		t.Fatalf("GatherInteractionDefs = %d, %v", len(pdefs), err)
	}
	hunting := 0
	for _, p := range pdefs {
		g, ok := interOf[p.GatherGuid]
		if !ok {
			if p.GatherGuid != 0 {
				// one PlayerInteractionDef names a GatheringInteractionData that is no record (the research counted 191 referenced of 236, one not in the table)
				t.Logf("%s names interaction %d, which is no row", p.Name, p.GatherGuid)
			}
			continue
		}
		if strings.HasPrefix(p.Name, "Hunting_") && g.ToolSlot == slotBow {
			hunting++
			if p.InteractRadius != 200 || p.InteractTolerance != 30 {
				t.Errorf("%s: radius %v tolerance %v, want 200 and 30", p.Name, p.InteractRadius, p.InteractTolerance)
			}
		}
	}
	if hunting != 53 {
		t.Errorf("%d Hunting_<X> interaction defs on a Bow interaction, want 53", hunting)
	}
	// the interaction's own range is the one of the def named like it (the Oak: Tree_Oak_Large 250 / 500)
	if n := count(`SELECT COUNT(*) FROM gather_interaction WHERE interaction_guid <> 0`); n != 190 {
		t.Errorf("%d interactions have a PlayerInteractionDef, want 190", n)
	}

	// region_foliage.tsv: its 73 def names each resolve to a foliage_def (Stone_Limestone_Large_2 by the row's own def_id), 69 of them harvestable
	f, err := os.Open(repoFile("dist", "content", "region_foliage.tsv"))
	if err != nil {
		t.Skipf("region_foliage.tsv is not there (%v): the TSV checks are skipped", err)
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	rows := 0
	seen := map[string]bool{}
	byGuid := map[int64]content.FoliageDef{}
	for _, d := range defs {
		byGuid[d.Guid] = d
	}
	for sc.Scan() {
		line := sc.Text()
		if strings.HasPrefix(line, "#") || line == "" {
			continue
		}
		p := strings.Split(line, "\t")
		rows++
		name := p[1]
		cands := byName[name]
		if len(cands) == 0 {
			t.Fatalf("row %d: the def %q is no foliage_def", rows, name)
		}
		if len(cands) > 1 {
			id, _ := strconv.ParseInt(p[0], 10, 64)
			if _, ok := byGuid[id]; !ok && id != 0 {
				t.Errorf("row %d: %s has def_id %d, which is none of the %d defs of the name", rows, name, id, len(cands))
			}
		}
		seen[name] = true
	}
	if rows != 84218 || len(seen) != 73 {
		t.Errorf("the TSV has %d rows and %d def names, want 84,218 and 73", rows, len(seen))
	}
	harvestable := 0
	for name := range seen {
		d := byName[name][0]
		ok := false
		for _, g := range d.StateGuids {
			if g != 0 && stateOf[g].InteractionGuid != 0 {
				ok = true
			}
		}
		if ok {
			harvestable++
		}
	}
	if harvestable != 69 {
		t.Errorf("%d of the 73 defs have a state with an interaction, want 69", harvestable)
	}
}

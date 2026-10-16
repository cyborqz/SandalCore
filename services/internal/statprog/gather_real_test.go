// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"database/sql"
	"math"
	"strings"
	"testing"

	"aocrop/services/internal/content"
)

// The Artisan equations of the real design data (Gathering stage 2, "Gathering data"), compiled with the host functions 180-184 and run on the engine:
// the rarity of a harvest against the analytic tables of the research (rarity_dist.json and t1dist.py), the experience points the captures show (+80, +90,
// +100, and +120 for an Uncommon), the speed factor, the quantity. They need the content.db of contentc version 13 (AOC_CONTENT_DB or dist/content).

// gatherReal is the real bundle with the ability expressions, the id of each expression in it and a level 20 Ranger to run them for.
type gatherReal struct {
	t       *testing.T
	b       *Bundle
	prog    map[int64]int // ab_expr id -> program
	db      *sql.DB
	ev      *Evaluator
	ratings []int64 // the guids of the artisanship rating stats
}

func newGatherReal(t *testing.T) *gatherReal {
	t.Helper()
	b, res, _, ids := compileRealExprs(t)
	g := loadReal(t)
	r := &gatherReal{t: t, b: b, prog: map[int64]int{}}
	for i, id := range ids {
		r.prog[id] = res[i].Program
	}
	cdb, err := content.Open(contentDBPath(t))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { cdb.Close() })
	r.db = cdb.SQL()
	if err := cdb.RequireVersion(); err != nil {
		t.Skip(err)
	}
	for guid, s := range g.Stats {
		if strings.HasPrefix(s.Name, "Artisan") && strings.HasSuffix(s.Name, "_Rating") {
			r.ratings = append(r.ratings, guid)
		}
	}
	in := Inputs{Class: 1, Level: 20}
	run, err := b.Run(in, nil)
	if err != nil {
		t.Fatal(err)
	}
	if r.ev, err = b.NewEvaluator(run, in, nil); err != nil {
		t.Fatal(err)
	}
	return r
}

// program is the program of the ab_expr id.
func (r *gatherReal) program(id int64) int {
	r.t.Helper()
	p, ok := r.prog[id]
	if !ok || p < 0 {
		r.t.Fatalf("expression %d has no program (%v)", id, ok)
	}
	return p
}

func (r *gatherReal) scalar(query string, args ...any) int64 {
	r.t.Helper()
	var n int64
	if err := r.db.QueryRow(query, args...).Scan(&n); err != nil {
		r.t.Fatalf("%s: %v", query, err)
	}
	return n
}

// [C-Y-B ~ CoA]

// withRatings is a context whose artisanship rating stats all read v. The rarity, speed and experience equations of the three gathering professions sum TWO terms (the
// general rating and the profession's: the third term, the tool's, has no value in the data and is 0), the quantity equations three (plus the tool's), so a sum
// of S is v = S/2 for the first and S/3 for the others.
func (r *gatherReal) withRatings(c GatherContext, v float64) GatherContext {
	c.Ratings = map[int64]float64{}
	for _, g := range r.ratings {
		c.Ratings[g] = v
	}
	return c
}

func (r *gatherReal) eval(prog int, h *TableHost) float64 {
	r.t.Helper()
	v, err := r.ev.Eval(prog, h)
	if err != nil {
		r.t.Fatal(err)
	}
	return v
}

// rarityOfQuality is the rarity band of a quality (the QualityCurve's bands: 1, 6, 16, 31, 51, 71, 101).
func rarityOfQuality(q float64) int {
	for band, lo := range []float64{101, 71, 51, 31, 16, 6, 1} {
		if q >= lo {
			return 6 - band
		}
	}
	return -1
}

func TestRealArtisanRarityTables(t *testing.T) {
	r := newGatherReal(t)
	if len(r.ratings) < 100 {
		t.Fatalf("%d artisanship rating stats in the graph, want over 100 (112 are read by the equations)", len(r.ratings))
	}
	// The expected odds in percent per rarity (Common ... Legendary) at rating 0 and rating 8409, from the equation text: rarity_dist.json (the standard
	// equation) and t1dist.py (the _T1_Only one, with the shipped gaps and overlaps in its thresholds), the research's analytic tables.
	type table [6]float64
	want := map[string]map[int]table{
		"Artisan_Gathering_Mining_Pickaxe_Rarity":            {0: {81.9, 11.056, 3.437, 2.096, 1.01, 0.501}, 8409: {45.5, 43.278, 7.186, 2.481, 1.049, 0.505}},
		"Artisan_Gathering_Herbalism_Sickle_Rarity":          {0: {81.9, 11.056, 3.437, 2.096, 1.01, 0.501}, 8409: {45.5, 43.278, 7.186, 2.481, 1.049, 0.505}},
		"Artisan_Gathering_Lumberjacking_Axe_Rarity_T1_Only": {0: {85.365, 10.805, 2.221, 1.098, 0.51, 0.001}, 8409: {47.425, 44.423, 6.106, 1.492, 0.55, 0.005}},
		"Artisan_Gathering_Mining_Pickaxe_Rarity_T1_Only":    {0: {85.365, 10.805, 2.221, 1.098, 0.51, 0.001}, 8409: {47.425, 44.423, 6.106, 1.492, 0.55, 0.005}},
		"Artisan_Gathering_Herbalism_Sickle_Rarity_T1_Only":  {0: {85.365, 10.805, 2.221, 1.098, 0.51, 0.001}, 8409: {47.425, 44.423, 6.106, 1.492, 0.55, 0.005}},
	}
	const trials = 200000
	for name, byRating := range want {
		quality := r.scalar(`SELECT quality_expr FROM item_variation WHERE name = ?`, name)
		prog := r.program(quality)
		for rating, expected := range byRating {
			// the two terms of the rating sum read the same value: 2 x 4204.5 = 8409
			ctx := r.withRatings(GatherContext{}, float64(rating)/2)
			host := &TableHost{Gather: &ctx, Rand: SplitMix{State: uint64(len(name)*1000 + rating)}}
			var got [7]int
			for i := 0; i < trials; i++ {
				got[rarityOfQuality(r.eval(prog, host))]++
			}
			if got[6] != 0 {
				t.Errorf("%s at %d: %d Artifacts", name, rating, got[6])
			}
			for rarity := 0; rarity < 6; rarity++ {
				p := expected[rarity] / 100
				sigma := math.Sqrt(p*(1-p)/trials) * 100
				pct := float64(got[rarity]) / trials * 100
				if tol := math.Max(4*sigma, 0.03); math.Abs(pct-expected[rarity]) > tol {
					t.Errorf("%s at rating %d, rarity %d: %.3f%% of %d harvests, want %.3f%% (+-%.3f)", name, rating, rarity, pct, trials, expected[rarity], tol)
				}
			}
		}
	}
}

// TestRealArtisanQualityValues: the qualities the equation can give are the floors of the bands and the "bonus" values (1, 6, 16, 31, 51, 71), the quality a
// failed bonus roll gives is the band's floor, and a rating high enough to always succeed never gives a floor of the Common band below 6.
func TestRealArtisanQualityValues(t *testing.T) {
	r := newGatherReal(t)
	prog := r.program(r.scalar(`SELECT quality_expr FROM item_variation WHERE name = 'Artisan_Gathering_Mining_Pickaxe_Rarity'`))
	allowed := map[float64]bool{1: true, 6: true, 16: true, 31: true, 51: true, 71: true}
	for _, rating := range []float64{0, 700, 8409} {
		ctx := r.withRatings(GatherContext{}, rating/2)
		host := &TableHost{Gather: &ctx, Rand: SplitMix{State: 77}}
		seen := map[float64]int{}
		for i := 0; i < 50000; i++ {
			seen[r.eval(prog, host)]++
		}
		for q, n := range seen {
			if !allowed[q] {
				t.Errorf("rating %v gave the quality %v %d times", rating, q, n)
			}
		}
		if seen[1] == 0 || seen[6] == 0 || seen[16] == 0 || seen[31] == 0 {
			t.Errorf("rating %v: qualities seen %v", rating, seen)
		}
	}
}

// TestRealArtisanXP: the experience of a harvest is the base (80 Novice, 90 Apprentice, 100 Journeyman and above) times the rarity (1.0 Common, 1.5 Uncommon ...
// 4.0 Artifact) times the experience rating (1 + GatheringXP(rating), 1 at rating 0): +80, +90, +100 and +120 for an Uncommon Novice harvest, as the captures
// of Flax and Snowdrop (1427), Moonbell and Halcyonite (1454, 1469), Rividium (1471) and the Ash (1463) show.
func TestRealArtisanXP(t *testing.T) {
	r := newGatherReal(t)
	// the expressions by their text: BaseXP (80/90/100 x rarity) times the profession's experience rating
	find := func(like string) int64 {
		var id int64
		if err := r.db.QueryRow(`SELECT id FROM ab_expr WHERE program >= 0 AND text LIKE ? ORDER BY id LIMIT 1`, like).Scan(&id); err != nil {
			t.Fatalf("no expression like %q: %v", like, err)
		}
		return id
	}
	mining := r.program(find(`EvalFormula($#4547065559311335368:6064632384153190657$) * EvalFormula($#4547065559311335368:6064630339305209859$)`)) // BaseXP x Artisan_Gathering_Mining_XP
	adventuring := r.program(find(`EvalFormula($#4547065559311335368:6064633469258694658$)`))
	for _, tc := range []struct {
		tier, rarity int
		want         float64
	}{{0, 0, 80}, {1, 0, 90}, {2, 0, 100}, {0, 1, 120}, {1, 1, 135}, {2, 2, 200}, {0, 3, 200}, {2, 3, 250}, {2, 4, 300}, {2, 5, 350}, {2, 6, 400}, {3, 0, 80}, {4, 6, 320}} {
		ctx := GatherContext{ItemRarity: tc.rarity, SourceTier: tc.tier}
		if got := r.eval(mining, &TableHost{Gather: &ctx}); got != tc.want {
			t.Errorf("tier %d rarity %d: %v XP, want %v", tc.tier, tc.rarity, got, tc.want)
		}
	}
	// the experience rating lifts it: GatheringXP(rating) is 0 at rating 0 (the expression reads 1 + the curve)
	rated := r.withRatings(GatherContext{ItemRarity: 0, SourceTier: 0}, 100) // 200 over the two terms
	if got := r.eval(mining, &TableHost{Gather: &rated}); got < 80 {
		t.Errorf("a rated harvest gives %v XP, under the base 80", got)
	}
	// Adventuring experience: the kill experience of the level x 0.2 x the tier's curve x the rarity (1, 1.4, 2, 4, 8, 20): more for a rarer item, same for a tier of 2 and above
	var last float64
	for _, rarity := range []int{0, 1, 2, 3, 4, 5} {
		ctx := GatherContext{ItemRarity: rarity, SourceTier: 0}
		got := r.eval(adventuring, &TableHost{Gather: &ctx})
		if got <= last {
			t.Errorf("Adventuring XP of rarity %d is %v, not above %v", rarity, got, last)
		}
		last = got
	}
	ctx6, ctx5 := GatherContext{ItemRarity: 6, SourceTier: 2}, GatherContext{ItemRarity: 5, SourceTier: 2}
	if r.eval(adventuring, &TableHost{Gather: &ctx6}) != r.eval(adventuring, &TableHost{Gather: &ctx5}) {
		t.Error("rarity 6 (Artifact) should give the Legendary multiplier (>= 5)")
	}
	t2, t3 := GatherContext{ItemRarity: 0, SourceTier: 2}, GatherContext{ItemRarity: 0, SourceTier: 3}
	if r.eval(adventuring, &TableHost{Gather: &t2}) != r.eval(adventuring, &TableHost{Gather: &t3}) {
		t.Error("tiers 2 and above share one curve")
	}
}

// -- C O A x C.Y.B --

// TestRealArtisanSpeedAndWear: the duration of a harvest is the base times GatheringSpeed(rating): 15 s at rating 0, 0.927 of it at a rating of about 215 (the
// carcass harvests of the captures: 13.91 s), and half at the top of the range; a tool loses 1 durability a harvest (2 for a medium axe).
func TestRealArtisanSpeedAndWear(t *testing.T) {
	r := newGatherReal(t)
	var duration, durability, base float64
	var d, w int64
	if err := r.db.QueryRow(`SELECT duration_expr, durability_expr, base_duration FROM gather_def WHERE name = 'Tree_Oak_Large_2'`).Scan(&d, &w, &base); err != nil {
		t.Fatal(err)
	}
	duration, durability = float64(r.program(d)), float64(r.program(w))
	for _, tc := range []struct {
		rating float64 // the sum of the three rating terms
		want   float64
		tol    float64
	}{{0, 15, 0}, {215, 15 * 0.927, 0.01}, {8409, 7.5, 0}, {1e6, 7.5, 0}} {
		ctx := r.withRatings(GatherContext{BaseDuration: base}, tc.rating/2)
		got := r.eval(int(duration), &TableHost{Gather: &ctx})
		if math.Abs(got-tc.want) > tc.tol {
			t.Errorf("a rating of %v: %v s, want %v", tc.rating, got, tc.want)
		}
	}
	ctx := GatherContext{BaseDuration: 11.25}
	if got := r.eval(int(duration), &TableHost{Gather: &ctx}); got != 11.25 {
		t.Errorf("a plant at rating 0 takes %v s, want 11.25", got)
	}
	if got := r.eval(int(durability), &TableHost{Gather: &ctx}); got != 1 {
		t.Errorf("a harvest costs the tool %v durability, want 1", got)
	}
	// the one with a different wear: Artisan_Lumberjacking_Axe_DurabilityLoss_Medium is 2
	var medium int
	if err := r.db.QueryRow(`SELECT COUNT(*) FROM gather_def g JOIN ab_expr e ON e.id = g.durability_expr WHERE e.text LIKE '%6064632547141550081%'`).Scan(&medium); err != nil {
		t.Fatal(err)
	}
	if medium > 0 {
		var e int64
		if err := r.db.QueryRow(`SELECT g.durability_expr FROM gather_def g JOIN ab_expr e ON e.id = g.durability_expr WHERE e.text LIKE '%6064632547141550081%' LIMIT 1`).Scan(&e); err != nil {
			t.Fatal(err)
		}
		if got := r.eval(r.program(e), &TableHost{Gather: &ctx}); got != 2 {
			t.Errorf("a medium axe harvest costs %v, want 2", got)
		}
	}
	// every gather_def's duration equals base x speed(0) = base at rating 0, and every wear is a whole number of at least 1
	rows, err := r.db.Query(`SELECT name, base_duration, duration_expr, durability_expr FROM gather_def ORDER BY guid`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	n := 0
	for rows.Next() {
		var name string
		var base float64
		var de, we int64
		rows.Scan(&name, &base, &de, &we)
		c := GatherContext{BaseDuration: base}
		if got := r.eval(r.program(de), &TableHost{Gather: &c}); got != float64(float32(base)) {
			t.Errorf("%s: %v s at rating 0, want its base %v", name, got, base)
		}
		if got := r.eval(r.program(we), &TableHost{Gather: &c}); got < 1 || got != math.Floor(got) {
			t.Errorf("%s wears the tool by %v", name, got)
		}
		n++
	}
	if n != 339 {
		t.Errorf("%d defs", n)
	}
}

// -- C | O | A & Cyb --

// TestRealArtisanQuantity: a Mining harvest's quantity is 5 + the equation's bonus. At rating 0 the bonus chance is 0 % (GatheringQuantityChance), so
// the harvest gives the base 5; at the top of the rating range half the harvests have a bonus of 1 to the curve's maximum for the quality's rarity, and a
// rolled quality that is no rarity (or an Artifact: 6) gives at most what the equation says.
func TestRealArtisanQuantity(t *testing.T) {
	r := newGatherReal(t)
	var expr int64
	if err := r.db.QueryRow(`SELECT e.id FROM ab_expr e WHERE e.program >= 0 AND e.text LIKE '5 + Evalformula($#4547065559311335368:6064630356244103197$)'`).Scan(&expr); err != nil {
		t.Fatalf("the Lumberjacking quantity expression: %v", err)
	}
	prog := r.program(expr)
	zero := r.withRatings(GatherContext{Quality: 0}, 0)
	for i := 0; i < 2000; i++ {
		if got := r.eval(prog, &TableHost{Gather: &zero, Rand: SplitMix{State: uint64(i)}}); got != 5 {
			t.Fatalf("at rating 0 the quantity is %v, want 5", got)
		}
	}
	// the top rating: the bonus roll succeeds in half of the harvests; the most it can be is the curve's maximum by the rolled quality read as a rarity
	// (0 Common 1..7 ... 5 Legendary 1..2, 6 Artifact 1; 7 matches no branch, MaxQuant stays 0 and RandIntRange(1, 0) is 1)
	top := r.withRatings(GatherContext{}, 8409.0/3)
	for quality, max := range map[float64]float64{0: 7, 1: 6, 2: 5, 3: 4, 4: 3, 5: 2, 6: 1, 7: 1} {
		top.Quality = quality
		host := &TableHost{Gather: &top, Rand: SplitMix{State: 5 + uint64(quality)}}
		bonus := 0
		seen := map[float64]bool{}
		const n = 20000
		for i := 0; i < n; i++ {
			got := r.eval(prog, host) - 5
			seen[got] = true
			if got > 0 {
				bonus++
			}
			if got < 0 || got > max {
				t.Fatalf("quality %v: a bonus of %v, at most %v", quality, got, max)
			}
		}
		if p := float64(bonus) / n; p < 0.45 || p > 0.55 {
			t.Errorf("quality %v: %.3f of the harvests have a bonus, want about 0.5 (GatheringQuantityChance(8409) = 50%%)", quality, p)
		}
		if float64(len(seen)) != max+1 {
			t.Errorf("quality %v: the bonuses seen are %v, want 0 to %v", quality, seen, max)
		}
	}
}

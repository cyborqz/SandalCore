// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynode_test

import (
	"testing"
	"time"

	"aocrop/services/internal/citynode"
	"aocrop/services/internal/citynodetest"
	"aocrop/services/internal/standin"
)

var testLeveling = citynode.Leveling{Enabled: true, Countdown: 5 * time.Second, AtrophyPerHour: 0.01, DelevelGrace: 0.25}

func tick(t *testing.T, e *citynodetest.Env) {
	t.Helper()
	if err := e.Svc.Tick(ctx); err != nil {
		t.Fatal(err)
	}
}

// Nodes 2: a node that reaches its next threshold levels up after the countdown, one level at a time; the seed's levels start at their thresholds.
func TestLevelUpAfterTheCountdown(t *testing.T) {
	standin.Skip(t, "the node constants made from the game install")
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon, citynode.WithLeveling(testLeveling))
	e.Start(t)
	if n := node(t, e.Svc, "Halcyon"); n.Level != 3 || n.Experience != citynode.ExpToLevel[3] {
		t.Fatalf("Halcyon at the start = %+v, want a Village at its threshold", n)
	}
	if n := node(t, e.Svc, "Joeva"); n.Level != 4 || n.Experience != citynode.ExpToLevel[4] {
		t.Fatalf("Joeva at the start = %+v, want a Town at its threshold", n)
	}
	w, err := e.Svc.Watch(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	// Winstead (Wilderness, no lord) gets the XP for two levels
	n, err := e.Svc.AddNodeExperience(ctx, 0, "Winstead", citynode.ExpToLevel[2]+5)
	if err != nil || n.Level != 0 || n.LevelUpAt.IsZero() {
		t.Fatalf("after the XP: %+v, %v; want a countdown at Wilderness", n, err)
	}
	if got := (<-w.Events()).Node; got == nil || got.Cause != citynode.CauseAddExperience {
		t.Fatalf("first event %+v, want the XP", got)
	}
	if got := (<-w.Events()).Node; got == nil || got.Cause != citynode.CauseLevelUpPending || got.Node.LevelUpAt.IsZero() {
		t.Fatalf("second event %+v, want the countdown", got)
	}
	e.Clock.Advance(4 * time.Second)
	tick(t, e)
	if n := node(t, e.Svc, "Winstead"); n.Level != 0 {
		t.Fatalf("before the countdown ends: %+v", n)
	}
	e.Clock.Advance(time.Second)
	tick(t, e)
	if n := node(t, e.Svc, "Winstead"); n.Level != 1 || n.LevelUpAt.IsZero() {
		t.Fatalf("at the countdown's end: %+v, want Crossroads and the next countdown", n)
	}
	if got := (<-w.Events()).Node; got == nil || got.Cause != citynode.CauseLevelUp || got.Node.Level != 1 || got.PreviousLevel != 0 {
		t.Fatalf("the level-up event %+v", got)
	}
	e.Clock.Advance(5 * time.Second)
	tick(t, e)
	if n := node(t, e.Svc, "Winstead"); n.Level != 2 || !n.LevelUpAt.IsZero() {
		t.Fatalf("the second level: %+v, want Encampment and no countdown", n)
	}
}

// A vassal goes up only while its lord may have another vassal of the new level; a node held back keeps no more XP than the next threshold.
func TestVassalHeldBackByItsLord(t *testing.T) {
	standin.Skip(t, "the node constants made from the game install")
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon, citynode.WithLeveling(testLeveling))
	e.Start(t)
	// Halcyon (a Village under the Town Joeva) has a Town lord: no Town vassal is allowed
	n, err := e.Svc.AddNodeExperience(ctx, 0, "Halcyon", citynode.ExpToLevel[4])
	if err != nil || !n.LevelUpAt.IsZero() || n.Experience != citynode.ExpToLevel[4] {
		t.Fatalf("Halcyon with a Town's XP: %+v, %v; want no countdown, the XP stopped at the Town threshold", n, err)
	}
	e.Clock.Advance(10 * time.Second)
	tick(t, e)
	if n := node(t, e.Svc, "Halcyon"); n.Level != 3 {
		t.Fatalf("Halcyon = %+v, want it held at Village", n)
	}
	if n, _ := e.Svc.AddNodeExperience(ctx, 0, "Halcyon", 1000); n.Experience != citynode.ExpToLevel[4] {
		t.Fatalf("more XP while held back: %d, want it to stay at %d", n.Experience, citynode.ExpToLevel[4])
	}
	// at the highest level the XP runs one span past the threshold
	if n, _ := e.Svc.AddNodeExperience(ctx, 0, "Joeva", 1_000_000_000); n.Experience != citynode.ExpToLevel[4]+citynode.Span(4) {
		t.Fatalf("Joeva's XP = %d, want %d", n.Experience, citynode.ExpToLevel[4]+citynode.Span(4))
	}
}

// FULL atrophy: a node loses a part of its level's span every hour, and the level once it falls past the grace below its threshold.
func TestAtrophyTakesTheLevel(t *testing.T) {
	standin.Skip(t, "the node constants made from the game install")
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon, citynode.WithLeveling(testLeveling))
	e.Start(t)
	tick(t, e) // (the atrophy's clock starts)
	e.Clock.Advance(time.Hour)
	tick(t, e)
	want := citynode.ExpToLevel[3] - int64(0.01*float64(citynode.Span(3))+0.5)
	if n := node(t, e.Svc, "Halcyon"); n.Level != 3 || n.Experience != want {
		t.Fatalf("Halcyon after an hour = %+v, want a Village with %d XP", n, want)
	}
	// 25% of the span below it (Encampment to Village) the level goes: 1% of Village's span an hour
	hours := int(0.25*float64(citynode.Span(2))/(0.01*float64(citynode.Span(3)))) + 2
	e.Clock.Advance(time.Duration(hours) * time.Hour)
	tick(t, e)
	if n := node(t, e.Svc, "Halcyon"); n.Level != 2 {
		t.Fatalf("Halcyon after %d more hours = %+v, want Encampment", hours, n)
	}
	// a level a game master sets starts at its threshold
	if n, _, err := e.Svc.SetNodeLevel(ctx, 0, "Winstead", 3, false); err != nil || n.Experience != citynode.ExpToLevel[3] {
		t.Fatalf("SetNodeLevel: %+v, %v", n, err)
	}
}

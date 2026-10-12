// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynode_test

import (
	"context"
	"errors"
	"math"
	"os"
	"sync"
	"testing"
	"time"

	"aocrop/services/internal/citynode"
	"aocrop/services/internal/citynodetest"
)

var ctx = context.Background()

func node(t *testing.T, s *citynode.Service, name string) citynode.Node {
	t.Helper()
	n, err := s.GetNode(ctx, 0, name)
	if err != nil {
		t.Fatalf("GetNode(%s): %v", name, err)
	}
	return n
}

func appliedAt(t *testing.T, e *citynodetest.Env) *time.Time {
	t.Helper()
	var at *time.Time
	if err := e.Pool.QueryRow(ctx, `SELECT applied_at FROM city_node_seed`).Scan(&at); err != nil {
		t.Fatal(err)
	}
	return at
}

// The seed is applied ONCE, at the first start: Joeva a Town, Halcyon a Village and Joeva's vassal, every other node Wilderness.
func TestSeedAppliedOnce(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	if appliedAt(t, e) != nil {
		t.Fatal("the seed is applied before the service started")
	}
	e.Start(t)
	first := appliedAt(t, e)
	if first == nil {
		t.Fatal("the seed was not applied at the first start")
	}
	snap, err := e.Svc.ListNodes(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(snap.Nodes) != 46 {
		t.Fatalf("%d nodes, want 46", len(snap.Nodes))
	}
	for _, n := range snap.Nodes {
		switch n.Name {
		case "Verra_RVR_Joeva":
			if n.Level != 4 || n.ParentGuid != 0 {
				t.Errorf("Joeva = level %d parent %d, want Town and no lord", n.Level, n.ParentGuid)
			}
		case "Verra_RVR_Halcyon":
			if n.Level != 3 || n.ParentGuid != joeva {
				t.Errorf("Halcyon = level %d parent %d, want Village under Joeva", n.Level, n.ParentGuid)
			}
		default:
			if n.Level != 0 || n.ParentGuid != 0 || n.Experience != 0 {
				t.Errorf("%s = level %d parent %d xp %d, want Wilderness", n.Name, n.Level, n.ParentGuid, n.Experience)
			}
		}
	}

	// a GM changes Joeva; a restart (a second service on the same tables) does not re-apply the seed
	if _, _, err := e.Svc.SetNodeLevel(ctx, 0, "Joeva", 2, true); err != nil {
		t.Fatal(err)
	}
	e.Clock.Advance(time.Hour)
	second := e.Another()
	if err := second.Start(ctx); err != nil {
		t.Fatal(err)
	}
	if n := node(t, second, "Joeva"); n.Level != 2 {
		t.Errorf("Joeva is level %d after a restart; the seed was applied again", n.Level)
	}
	if at := appliedAt(t, e); at == nil || !at.Equal(*first) {
		t.Errorf("applied_at moved from %v to %v at a restart", first, at)
	}
	// without force, ApplySeed says so and changes nothing
	res, err := e.Svc.ApplySeed(ctx, false, nil)
	if err != nil || res.Applied || res.Changed != 0 || !res.AppliedAt.Equal(*first) {
		t.Errorf("ApplySeed(false) = %+v, %v; want not applied, applied_at %v", res, err, first)
	}
	if n := node(t, e.Svc, "Joeva"); n.Level != 2 {
		t.Errorf("Joeva is level %d after ApplySeed(false)", n.Level)
	}

	// force applies it again: levels and parents are overwritten, the XP is left
	if _, err := e.Svc.AddNodeExperience(ctx, 0, "Joeva", 777); err != nil {
		t.Fatal(err)
	}
	if _, _, err := e.Svc.SetNodeLevel(ctx, 0, "Winstead", 1, false); err != nil {
		t.Fatal(err)
	}
	res, err = e.Svc.ApplySeed(ctx, true, nil)
	if err != nil || !res.Applied || !res.Forced || res.Changed != 2 {
		t.Fatalf("ApplySeed(force) = %+v, %v; want applied, forced, 2 nodes changed (Joeva back to 4, Winstead back to 0)", res, err)
	}
	if n := node(t, e.Svc, "Joeva"); n.Level != 4 || n.Experience != 777 {
		t.Errorf("Joeva after force: level %d xp %d, want 4 and 777", n.Level, n.Experience)
	}
	if n := node(t, e.Svc, "Winstead"); n.Level != 0 {
		t.Errorf("Winstead (not in the seed) is level %d after force, want 0", n.Level)
	}
	if at := appliedAt(t, e); at == nil || !at.After(*first) {
		t.Errorf("applied_at = %v after force, want later than %v", at, first)
	}
}

// == C | O | A | C.Y.B ==

// A seed file that is missing or wrong stops the start and applies nothing; fixed, the next start applies it.
func TestSeedFileMissingOrWrongStopsTheStart(t *testing.T) {
	e := citynodetest.New(t, "") // no file written
	err := e.Svc.Start(ctx)
	if err == nil || !errors.Is(err, citynode.ErrNotFound) {
		t.Fatalf("Start with no seed file = %v, want ErrNotFound", err)
	}
	if appliedAt(t, e) != nil {
		t.Error("the seed is marked applied though there was no file")
	}
	if err := os.WriteFile(e.SeedPath, []byte(`{"nodes":{"Atlantis":{"level":1}}}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := e.Svc.Start(ctx); err == nil {
		t.Fatal("Start accepted a seed naming a node that does not exist")
	}
	if appliedAt(t, e) != nil {
		t.Error("the seed is marked applied though it was wrong")
	}
	if err := os.WriteFile(e.SeedPath, []byte(citynodetest.SeedJoevaHalcyon), 0o600); err != nil {
		t.Fatal(err)
	}
	e.Start(t)
	if n := node(t, e.Svc, "Joeva"); n.Level != 4 {
		t.Errorf("Joeva = level %d once the file was right", n.Level)
	}
}

// A wrong seed handed to ApplySeed (force) changes nothing.
func TestApplySeedWrongTextChangesNothing(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	for _, body := range []string{`{`, `{"nodes":{"Joeva":{"level":9}}}`, `{"nodes":{"Winstead":{"level":2},"Atlantis":{"level":1}}}`} {
		if _, err := e.Svc.ApplySeed(ctx, true, []byte(body)); !errors.Is(err, citynode.ErrInvalid) && !errors.Is(err, citynode.ErrNotFound) {
			t.Errorf("ApplySeed(%s) = %v, want a refusal", body, err)
		}
	}
	if n := node(t, e.Svc, "Winstead"); n.Level != 0 {
		t.Errorf("Winstead = level %d after wrong seeds", n.Level)
	}
	if n := node(t, e.Svc, "Halcyon"); n.Level != 3 || n.ParentGuid != joeva {
		t.Errorf("Halcyon changed: %+v", n)
	}
	// a seed text overrides the file
	res, err := e.Svc.ApplySeed(ctx, true, []byte(`{"nodes":{"Winstead":{"level":2}}}`))
	if err != nil || res.Changed != 3 { // Winstead up, Joeva and Halcyon down
		t.Errorf("ApplySeed(text) = %+v, %v; want 3 nodes changed", res, err)
	}
	if n := node(t, e.Svc, "Halcyon"); n.Level != 0 || n.ParentGuid != 0 {
		t.Errorf("Halcyon = %+v; the new seed leaves it out", n)
	}
}

// C:Y:B + CoA

func TestSetNodeLevelBounds(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	w, err := e.Svc.Watch(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	for _, bad := range []int{-1, 5, 6, 7, 100, math.MaxInt32} {
		_, _, err := e.Svc.SetNodeLevel(ctx, 0, "Winstead", bad, false)
		if !errors.Is(err, citynode.ErrInvalid) {
			t.Errorf("SetNodeLevel(%d) = %v, want ErrInvalid", bad, err)
		}
	}
	if n := node(t, e.Svc, "Winstead"); n.Level != 0 {
		t.Errorf("a refused level changed the node: %+v", n)
	}
	for _, st := range []struct {
		level   int
		changed bool
	}{{0, false}, {1, true}, {2, true}, {3, true}, {4, true}, {0, true}} {
		n, changed, err := e.Svc.SetNodeLevel(ctx, 0, "Winstead", st.level, false)
		if err != nil || n.Level != st.level || changed != st.changed {
			t.Errorf("SetNodeLevel(%d) = %+v, changed %v, %v; want changed %v", st.level, n, changed, err, st.changed)
		}
	}
	if _, _, err := e.Svc.SetNodeLevel(ctx, 0, "Atlantis", 1, false); !errors.Is(err, citynode.ErrNotFound) {
		t.Errorf("unknown node: %v", err)
	}
	if _, _, err := e.Svc.SetNodeLevel(ctx, 0, "", 1, false); !errors.Is(err, citynode.ErrInvalid) {
		t.Errorf("no node: %v", err)
	}
	// 0->0 sent nothing; 0->1->2->3->4->0 are five changes
	for i := 1; i <= 5; i++ {
		select {
		case ev := <-w.Events():
			if ev.Seq != uint64(i) || ev.Node == nil || ev.Node.Cause != citynode.CauseSetLevel {
				t.Fatalf("event %d = %+v", i, ev)
			}
		case <-time.After(5 * time.Second):
			t.Fatalf("no event %d", i)
		}
	}
	select {
	case ev := <-w.Events():
		t.Errorf("an unexpected event: %+v", ev)
	case <-time.After(100 * time.Millisecond):
	}
}

// SetNodeLevel touches the level only: the XP and the vassal link stay; the flag travels with the change.
func TestSetNodeLevelTouchesOnlyTheLevel(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	if _, err := e.Svc.AddNodeExperience(ctx, 0, "Halcyon", 5000); err != nil {
		t.Fatal(err)
	}
	w, _ := e.Svc.Watch(ctx)
	defer w.Close()
	n, changed, err := e.Svc.SetNodeLevel(ctx, 0, "Halcyon", 1, true)
	if err != nil || !changed {
		t.Fatal(n, changed, err)
	}
	if n.Level != 1 || n.Experience != 5000 || n.ParentGuid != joeva {
		t.Errorf("Halcyon = %+v; want level 1 with its XP and its lord", n)
	}
	ev := <-w.Events()
	if ev.Node == nil || ev.Node.PreviousLevel != 3 || !ev.Node.VassalshipDisabled || ev.Node.PreviousExperience != 5000 {
		t.Errorf("event = %+v", ev.Node)
	}
}

func TestAddNodeExperience(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	if _, _, err := e.Svc.SetNodeLevel(ctx, 0, "Winstead", 1, false); err != nil {
		t.Fatal(err)
	}
	// halfway from level 1's threshold (2,636,934) to level 2's (10,547,736)
	n, err := e.Svc.AddNodeExperience(ctx, 0, "Winstead", 6592335)
	if err != nil || n.Experience != 6592335 || n.Percent != 50 || n.ToNext != 10547736-6592335 || n.Level != 1 {
		t.Fatalf("after +6592335: %+v, %v", n, err)
	}
	// past the next threshold: the progress is full and the LEVEL STAYS (no level-up from XP)
	n, err = e.Svc.AddNodeExperience(ctx, 0, "Winstead", 50_000_000)
	if err != nil || n.Level != 1 || n.Percent != 100 || n.ToNext != 0 || n.Experience != 56592335 {
		t.Fatalf("after +50M: %+v, %v", n, err)
	}
	if got := node(t, e.Svc, "Winstead"); got.Level != 1 || got.Experience != 56592335 {
		t.Errorf("stored: %+v", got)
	}
	// XP can be taken away, but not below 0, and 0 and overflow are refused
	if n, err = e.Svc.AddNodeExperience(ctx, 0, "Winstead", -56592335); err != nil || n.Experience != 0 {
		t.Errorf("down to 0: %+v, %v", n, err)
	}
	for _, bad := range []int64{0, -1, math.MinInt64} {
		if _, err := e.Svc.AddNodeExperience(ctx, 0, "Winstead", bad); !errors.Is(err, citynode.ErrInvalid) {
			t.Errorf("amount %d: %v, want ErrInvalid", bad, err)
		}
	}
	if _, err := e.Svc.AddNodeExperience(ctx, 0, "Winstead", math.MaxInt64); err != nil {
		t.Fatal(err)
	}
	if _, err := e.Svc.AddNodeExperience(ctx, 0, "Winstead", 1); !errors.Is(err, citynode.ErrInvalid) {
		t.Errorf("overflow: %v, want ErrInvalid", err)
	}
	// the highest level shows no progress, as retail's level-4 nodes do
	if n := node(t, e.Svc, "Joeva"); n.Level != 4 || n.Percent != 0 || n.ToNext != 0 {
		t.Errorf("Joeva = %+v", n)
	}
	if _, err := e.Svc.AddNodeExperience(ctx, 0, "Atlantis", 1); !errors.Is(err, citynode.ErrNotFound) {
		t.Errorf("unknown node: %v", err)
	}
}

// built by Cyb with C-O-A

// WatchNodes: the whole state first, then every change in the order it was made, with no gap and no repeat.
func TestWatchOrdering(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	w, err := e.Svc.Watch(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	if w.Snapshot.Seq != 0 || len(w.Snapshot.Nodes) != 46 || w.Snapshot.Clock.Speed != 1 {
		t.Fatalf("snapshot = seq %d, %d nodes, %+v", w.Snapshot.Seq, len(w.Snapshot.Nodes), w.Snapshot.Clock)
	}
	steps := []func() error{
		func() error { _, _, err := e.Svc.SetNodeLevel(ctx, 0, "Winstead", 2, false); return err },
		func() error { _, err := e.Svc.AddNodeExperience(ctx, 0, "Miraleth", 10); return err },
		func() error { _, err := e.Svc.AdvanceClock(ctx, 60); return err },
		func() error { _, err := e.Svc.SetClockSpeed(ctx, 10); return err },
		func() error { _, _, err := e.Svc.SetNodeLevel(ctx, 0, "Winstead", 3, true); return err },
		func() error { _, err := e.Svc.ApplySeed(ctx, true, nil); return err }, // Winstead 3 -> 0: one change
	}
	for i, step := range steps {
		if err := step(); err != nil {
			t.Fatalf("step %d: %v", i, err)
		}
	}
	type want struct {
		node  string
		clock bool
		cause citynode.Cause
	}
	for i, wnt := range []want{
		{node: "Verra_RVR_Winstead", cause: citynode.CauseSetLevel},
		{node: "Verra_RVR_Miraleth", cause: citynode.CauseAddExperience},
		{clock: true},
		{clock: true},
		{node: "Verra_RVR_Winstead", cause: citynode.CauseSetLevel},
		{node: "Verra_RVR_Winstead", cause: citynode.CauseSeed},
	} {
		select {
		case ev := <-w.Events():
			if ev.Seq != uint64(i+1) {
				t.Errorf("event %d has seq %d", i, ev.Seq)
			}
			switch {
			case wnt.clock && ev.Clock == nil:
				t.Errorf("event %d = %+v, want a clock change", i, ev)
			case !wnt.clock && (ev.Node == nil || ev.Node.Node.Name != wnt.node || ev.Node.Cause != wnt.cause):
				t.Errorf("event %d = %+v, want %s by %v", i, ev.Node, wnt.node, wnt.cause)
			}
		case <-time.After(5 * time.Second):
			t.Fatalf("no event %d", i)
		}
	}

	// a second watcher that starts now gets the state as of change 6, and its first change is 7
	w2, err := e.Svc.Watch(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer w2.Close()
	if w2.Snapshot.Seq != 6 {
		t.Fatalf("second snapshot seq = %d, want 6", w2.Snapshot.Seq)
	}
	if _, err := e.Svc.AddNodeExperience(ctx, 0, "Joeva", 1); err != nil {
		t.Fatal(err)
	}
	if ev := <-w2.Events(); ev.Seq != 7 {
		t.Errorf("first change after the snapshot has seq %d, want 7", ev.Seq)
	}
}

// [CoA x C-Y-B]

// Many writers and a watcher: the changes arrive numbered 1..N in order, and replaying them on the snapshot gives the stored state.
func TestWatchUnderConcurrentChanges(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	names := []string{"Winstead", "Miraleth", "NewAela", "Lionhold", "Korrin", "Seahook"}
	w, err := e.Svc.Watch(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	const writers, each = 6, 12
	var wg sync.WaitGroup
	for i := 0; i < writers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < each; j++ {
				name := names[(i+j)%len(names)]
				if j%2 == 0 {
					if _, _, err := e.Svc.SetNodeLevel(ctx, 0, name, (i+j)%5, false); err != nil {
						t.Error(err)
					}
				} else if _, err := e.Svc.AddNodeExperience(ctx, 0, name, int64(i+1)); err != nil {
					t.Error(err)
				}
			}
		}()
	}
	wg.Wait()

	state := map[int64]citynode.Node{}
	for _, n := range w.Snapshot.Nodes {
		state[n.Guid] = n
	}
	last := w.Snapshot.Seq
	for {
		select {
		case ev := <-w.Events():
			if ev.Seq != last+1 {
				t.Fatalf("change %d follows change %d", ev.Seq, last)
			}
			last = ev.Seq
			if ev.Node != nil {
				state[ev.Node.Node.Guid] = ev.Node.Node
			}
			continue
		case <-time.After(300 * time.Millisecond):
		}
		break
	}
	if last == 0 {
		t.Fatal("no changes arrived")
	}
	stored, err := e.Svc.ListNodes(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if stored.Seq != last {
		t.Errorf("ListNodes says change %d, the watcher saw %d", stored.Seq, last)
	}
	for _, n := range stored.Nodes {
		got := state[n.Guid]
		if got.Level != n.Level || got.Experience != n.Experience || got.ParentGuid != n.ParentGuid {
			t.Errorf("%s: replayed level %d xp %d, stored level %d xp %d", n.Name, got.Level, got.Experience, n.Level, n.Experience)
		}
	}
}

// A reader that falls too far behind is cut off (ErrLagged) and the writers are never held up.
func TestWatchLagged(t *testing.T) {
	defer citynode.SetWatchBuffer(3)()
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	w, err := e.Svc.Watch(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 6; i++ {
		if _, err := e.Svc.AddNodeExperience(ctx, 0, "Winstead", 1); err != nil {
			t.Fatal(err)
		}
	}
	n := 0
	for range w.Events() { // closed after the buffered events
		n++
	}
	if n != 3 || !errors.Is(w.Err(), citynode.ErrLagged) {
		t.Errorf("read %d events, Err = %v; want 3 and ErrLagged", n, w.Err())
	}
	// asking again gives the whole state
	w2, err := e.Svc.Watch(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer w2.Close()
	if w2.Snapshot.Seq != 6 {
		t.Errorf("new snapshot seq = %d, want 6", w2.Snapshot.Seq)
	}
}

func TestWatchShutdown(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	w, err := e.Svc.Watch(ctx)
	if err != nil {
		t.Fatal(err)
	}
	e.Svc.Shutdown()
	select {
	case _, ok := <-w.Events():
		if ok {
			t.Error("an event after shutdown")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("the watch did not end at shutdown")
	}
	if !errors.Is(w.Err(), citynode.ErrShutdown) {
		t.Errorf("Err = %v, want ErrShutdown", w.Err())
	}
	if _, err := e.Svc.Watch(ctx); !errors.Is(err, citynode.ErrShutdown) {
		t.Errorf("Watch after shutdown = %v", err)
	}
}

// The node clock = real time x speed + offset.
func TestClock(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	real0 := e.Clock.Now()
	c, err := e.Svc.GetClock(ctx)
	if err != nil || c.Speed != 1 || c.OffsetSeconds != 0 || c.NodeUnixMs != real0.UnixMilli() || c.RealUnixMs != real0.UnixMilli() {
		t.Fatalf("a fresh clock is real time: %+v, %v", c, err)
	}
	// the GM's skip: +1 hour
	c, err = e.Svc.AdvanceClock(ctx, 3600)
	if err != nil || c.NodeUnixMs != real0.UnixMilli()+3600_000 || c.OffsetSeconds != 3600 {
		t.Fatalf("AdvanceClock(3600) = %+v, %v", c, err)
	}
	// real time passes: node time follows at speed 1
	e.Clock.Advance(10 * time.Second)
	if c, _ = e.Svc.GetClock(ctx); c.NodeUnixMs != real0.UnixMilli()+3600_000+10_000 {
		t.Errorf("10 s later: %+v", c)
	}
	// the speed knob: node time does not jump, then runs 60 times faster
	before, _ := e.Svc.GetClock(ctx)
	after, err := e.Svc.SetClockSpeed(ctx, 60)
	if err != nil || after.Speed != 60 {
		t.Fatalf("SetClockSpeed(60) = %+v, %v", after, err)
	}
	if d := after.NodeUnixMs - before.NodeUnixMs; d < -500 || d > 500 {
		t.Errorf("node time jumped by %d ms when the speed changed", d)
	}
	e.Clock.Advance(10 * time.Second)
	later, _ := e.Svc.GetClock(ctx)
	if d := later.NodeUnixMs - after.NodeUnixMs; d != 600_000 {
		t.Errorf("10 real seconds at x60 moved node time by %d ms, want 600000", d)
	}
	// a skip at speed 60 is still seconds of NODE time
	skipped, err := e.Svc.AdvanceClock(ctx, 100)
	if err != nil || skipped.NodeUnixMs-later.NodeUnixMs != 100_000 {
		t.Errorf("AdvanceClock(100) at x60 moved node time by %d ms (%v), want 100000", skipped.NodeUnixMs-later.NodeUnixMs, err)
	}
	// it is stored: a restarted service reads the same clock
	again, err := e.Another().GetClock(ctx)
	if err != nil || again != skipped {
		t.Errorf("after a restart: %+v, %v; want %+v", again, err, skipped)
	}
	// and slowed down, and back
	if c, err = e.Svc.SetClockSpeed(ctx, 0.5); err != nil || c.Speed != 0.5 {
		t.Errorf("SetClockSpeed(0.5) = %+v, %v", c, err)
	}
	if c, err = e.Svc.SetClockSpeed(ctx, 1); err != nil || c.Speed != 1 {
		t.Errorf("SetClockSpeed(1) = %+v, %v", c, err)
	}
}

// -- Cyb & C-O-A --

func TestClockBounds(t *testing.T) {
	e := citynodetest.New(t, citynodetest.SeedJoevaHalcyon)
	e.Start(t)
	for _, f := range []float64{0, -1, 0.0001, 100001, 1e9, math.NaN(), math.Inf(1), math.Inf(-1)} {
		if _, err := e.Svc.SetClockSpeed(ctx, f); !errors.Is(err, citynode.ErrInvalid) {
			t.Errorf("SetClockSpeed(%g) = %v, want ErrInvalid", f, err)
		}
	}
	for _, s := range []int64{0, -5, math.MinInt64, math.MaxInt64, citynode.MaxAdvanceSeconds + 1} {
		if _, err := e.Svc.AdvanceClock(ctx, s); !errors.Is(err, citynode.ErrInvalid) {
			t.Errorf("AdvanceClock(%d) = %v, want ErrInvalid", s, err)
		}
	}
	for _, f := range []float64{citynode.MinClockSpeed, citynode.MaxClockSpeed} {
		if c, err := e.Svc.SetClockSpeed(ctx, f); err != nil || c.Speed != f {
			t.Errorf("SetClockSpeed(%g) = %+v, %v", f, c, err)
		}
	}
	if c, err := e.Svc.AdvanceClock(ctx, citynode.MaxAdvanceSeconds); err != nil {
		t.Errorf("AdvanceClock(max) = %+v, %v", c, err)
	}
	if c, err := e.Svc.GetClock(ctx); err != nil || c.Speed != citynode.MaxClockSpeed {
		t.Errorf("a refused call changed the speed: %+v, %v", c, err)
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"strings"
	"testing"

	"aocrop/services/internal/aocexpr"
)

// The city node reads (Nodes 1, stage 1): what they compile to, what they give with the NodeWorld host, and that the compiled programs and the
// reference evaluator agree.

func nodeRef(n int) string { return fmt.Sprintf("$#%d:%d$", aocexpr.TypeCityNode, n) }

// nodeExprs are the node expressions of the tiny set by name.
func nodeExprs() []Expr {
	var out []Expr
	for _, e := range TinyExprs() {
		switch e.What {
		case "a biome gate", "the level of the node the player is in", "a node named by reference", "citizens and the mayor", "a node and its level in variables",
			"a node named by its record":
			out = append(out, e)
		}
	}
	return out
}

func compileOne(t *testing.T, mode Mode, text string) (*Bundle, ExprResult) {
	t.Helper()
	b, res, err := CompileExprs(TinyGraph(false), Options{Mode: mode}, []Expr{{What: "test", Text: text}})
	if err != nil {
		t.Fatal(err)
	}
	return b, res[0]
}

// -- CoA x C-Y-B --

func TestNodeReadsCompileOrRefuse(t *testing.T) {
	for name, tc := range map[string]struct {
		text, err string // "" compiles
	}{
		"a biome":                           {"GetNodeBiome() == EBiomeType::Riverlands", ""},
		"a biome the other way":             {"EBiomeType::Riverlands == GetNodeBiome()", ""},
		"a biome list":                      {"GetNodeBiome() != EBiomeType::Desert && GetNodeBiome() != EBiomeType::Highlands && GetNodeBiome() != EBiomeType::Riverlands", ""},
		"a level as a number":               {"EnumInt(Node_GetLevel(GetNodeRecord())) < EnumInt(ENodeLevel::Village)", ""},
		"a level against a name":            {"Node_GetLevel(GetCurrentNode(GetPlayer())) == ENodeLevel::Encampment", ""},
		"an empty node":                     {"GetNodeRecord() == " + nodeRef(0), ""},
		"the unknown node":                  {"Node_GetLevel(" + nodeRef(4242) + ") == ENodeLevel::Town", "is no city node record"},
		"the wrong enum":                    {"GetNodeBiome() == ENodeType::Divine", "cannot compare a number with an enum value"},
		"an enum that is not one":           {"GetNodeBiome() == EBiomeType::Atlantis", "is not a value of EBiomeType"},
		"a level bound that is not a level": {"Node_InLevelRangeInclusive(GetNodeRecord(), EBiomeType::Desert, ENodeLevel::Town)", "not an ENodeLevel"},
		"a node that is a number":           {"Node_GetLevel(3) == ENodeLevel::Town", "not a city node"},
		"a player where an actor is wanted": {"GetStat(GetPlayer(), $#" + fmt.Sprint(TypeStatDef) + ":1001$)", "not an object"},
		"no player":                         {"Node_IsPlayerMayor(GetOwner())", "not GetPlayer()"},
	} {
		_, r := compileOne(t, ModeFloat32, "SelectFloat("+tc.text+", 1.0, 2.0)")
		if tc.err == "" {
			if r.Program < 0 {
				t.Errorf("%s: %q did not compile: %s", name, tc.text, r.Err)
			}
			continue
		}
		if r.Program != -1 || !strings.Contains(r.Err, tc.err) {
			t.Errorf("%s: %q gave program %d, error %q; want an error with %q", name, tc.text, r.Program, r.Err, tc.err)
		}
	}
	// a graph with no nodes (a content.db before version 12) refuses a node reference, not a read of the host
	g := TinyGraph(false)
	g.Nodes = nil
	_, res, err := CompileExprs(g, Options{}, []Expr{
		{What: "ref", Text: "SelectFloat(GetNodeRecord() == " + nodeRef(5001) + ", 1.0, 2.0)"},
		{What: "biome", Text: "SelectFloat(GetNodeBiome() == EBiomeType::Jungle, 1.0, 2.0)"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if res[0].Program != -1 || !strings.Contains(res[0].Err, "no city nodes") || res[1].Program < 0 {
		t.Errorf("a graph with no nodes: %+v", res)
	}
}

// TestNodeReadsGive runs the node expressions of the tiny set against NodeWorld hosts and checks the numbers by hand.
func TestNodeReadsGive(t *testing.T) {
	exprs := nodeExprs()
	b, res, err := CompileExprs(TinyGraph(false), Options{}, exprs)
	if err != nil {
		t.Fatal(err)
	}
	in := tinyInputs(2, 20, false)
	run, _ := b.Run(in, nil)
	ev, _ := b.NewEvaluator(run, in, nil)
	// ranks: 0 Aela (Default, Wilderness), 1 Joeva (Scientific, Town), 2 Halcyon (Divine, Village)
	town := NodeWorld{Types: []int{0, 1, 2}, Levels: []int{0, 4, 3}, Node: 1, Biome: 14, ActorNode: 1, PlayerNode: 2}
	road := NodeWorld{Types: []int{0, 1, 2}, Levels: []int{0, 4, 3}, Node: -1, Biome: 0, ActorNode: -1, PlayerNode: -1}
	desert := NodeWorld{Types: []int{0, 1, 2}, Levels: []int{0, 4, 3}, Node: 2, Biome: 6, ActorNode: 0, PlayerNode: 1}
	want := map[string][3]float64{ // town, road, desert
		// the biome gate: Riverlands 10; not Desert and not Highlands 20; else 30
		"a biome gate": {10, 20, 30},
		// EnumInt(level of the player's node) >= Village (1 or 2) + the context node Divine (100 or 0)
		"the level of the node the player is in": {1 + 0, 2 + 0, 1 + 100},
		// the context node is Joeva (rank 1) and Joeva is in Encampment..Town (7 or 8), and the player's node is not Halcyon (rank 2) (40 or 0)
		"a node named by reference": {7 + 0, 8 + 40, 8 + 40},
		// no citizen, no mayor
		"citizens and the mayor": {2000, 2000, 2000},
		// the actor's node is a Town or Martial (5 or 6), and in Wilderness..Crossroads (0.5 or 0.25)
		"a node and its level in variables": {5 + 0.25, 6 + 0.5, 6 + 0.5},
		// Halcyon (rank 2) is Divine, not Scientific (0); the player is no citizen of Aela (0)
		"a node named by its record": {0, 0, 0},
	}
	for i, ex := range exprs {
		for w, world := range []NodeWorld{town, road, desert} {
			world := world
			host := &TableHost{Nodes: &world}
			got, err := ev.Eval(res[i].Program, host)
			if err != nil {
				t.Fatalf("%s: %v", ex.What, err)
			}
			if got != want[ex.What][w] {
				t.Errorf("%s in world %d = %v, want %v", ex.What, w, got, want[ex.What][w])
			}
		}
	}
	if ev.Faults() != 0 {
		t.Errorf("%d faults", ev.Faults())
	}
}

// Cyb x C.O.A

// TestNodeReadsAgreeWithTheReference: the compiled programs and the reference evaluator (the text over aocexpr) give the same numbers for the node
// worlds, in every arithmetic.
func TestNodeReadsAgreeWithTheReference(t *testing.T) {
	g := TinyGraph(false)
	exprs := append(nodeExprs(), Expr{What: "levels as numbers", Text: "EnumInt(Node_GetLevel(GetNodeRecord())) * 10.0 + EnumInt(Node_GetNodeType(GetCurrentNode(GetOwner()))) + EnumInt(GetNodeBiome())"},
		Expr{What: "a range of one level", Text: "SelectFloat(Node_InLevelRangeInclusive(GetCurrentNode(GetPlayer()), ENodeLevel::Town, ENodeLevel::Town), 3.0, 4.0)"})
	worlds := []NodeWorld{
		{Types: []int{0, 1, 2}, Levels: []int{0, 4, 3}, Node: 1, Biome: 14, ActorNode: 1, PlayerNode: 2},
		{Types: []int{0, 1, 2}, Levels: []int{0, 4, 3}, Node: -1, Biome: 0, ActorNode: -1, PlayerNode: -1},
		{Types: []int{3, 4, 1}, Levels: []int{2, 2, 1}, Node: 0, Biome: 21, ActorNode: 2, PlayerNode: 0},
		{Types: []int{0, 1, 2}, Levels: []int{0, 4, 3}, Node: 7, Biome: 22, ActorNode: 9, PlayerNode: 3}, // ranks past the registry read as no node
	}
	for _, mode := range []Mode{ModeFloat32, ModeDouble, ModeSingle} {
		b, res, err := CompileExprs(g, Options{Mode: mode}, exprs)
		if err != nil {
			t.Fatal(err)
		}
		in := tinyInputs(1, 11, false)
		run, err := b.Run(in, nil)
		if err != nil {
			t.Fatal(err)
		}
		ev, err := b.NewEvaluator(run, in, nil)
		if err != nil {
			t.Fatal(err)
		}
		ref, err := NewRefEngine(g, in, nil, mode)
		if err != nil {
			t.Fatal(err)
		}
		for wi, w := range worlds {
			for i, ex := range exprs {
				if res[i].Program < 0 {
					t.Fatalf("%s did not compile: %s", ex.What, res[i].Err)
				}
				w1, w2 := w, w
				got, err := ev.Eval(res[i].Program, &TableHost{Nodes: &w1})
				if err != nil {
					t.Fatalf("%s: %v", ex.What, err)
				}
				// a rank past the registry is no node: the reference evaluator cannot name it, so only the worlds whose ranks it can are compared
				if w.Node >= len(w.Types) || w.ActorNode >= len(w.Types) || w.PlayerNode >= len(w.Types) {
					continue
				}
				want, err := ref.EvalExpr(ex.Text, ex.Terms, &TableHost{Nodes: &w2})
				if err != nil {
					t.Fatalf("%v world %d %s: reference: %v", mode, wi, ex.What, err)
				}
				if math.Float64bits(got) != math.Float64bits(want) {
					t.Errorf("%v world %d: %s = %v, reference %v", mode, wi, ex.What, got, want)
				}
			}
		}
	}
}

func TestNodeWorldAnswers(t *testing.T) {
	w := NodeWorld{Types: []int{0, 3}, Levels: []int{1, 4}, Node: 1, Biome: 14, ActorNode: 0, PlayerNode: -1}
	for _, tc := range []struct {
		fn   int
		args []float64
		want float64
	}{
		{aocexpr.HostNodeBiome, nil, 14}, {aocexpr.HostNodeRecord, nil, 1},
		{aocexpr.HostCurrentNode, []float64{aocexpr.ActorPlayer}, -1}, {aocexpr.HostCurrentNode, []float64{0}, 0}, {aocexpr.HostCurrentNode, []float64{1}, 0},
		{aocexpr.HostNodeType, []float64{1}, 3}, {aocexpr.HostNodeLevel, []float64{1}, 4}, {aocexpr.HostNodeLevel, []float64{0}, 1},
		{aocexpr.HostNodeType, []float64{-1}, 0}, {aocexpr.HostNodeLevel, []float64{2}, 0}, {aocexpr.HostNodeLevel, []float64{0.5}, 0}, {aocexpr.HostNodeLevel, nil, 0},
		{aocexpr.HostPlayerCitizen, nil, 0}, {aocexpr.HostPlayerCitizenOfNode, []float64{1}, 0}, {aocexpr.HostPlayerMayor, nil, 0},
	} {
		if got, ok := w.HostFn(tc.fn, tc.args); !ok || got != tc.want {
			t.Errorf("HostFn(%d, %v) = %v, %v; want %v", tc.fn, tc.args, got, ok, tc.want)
		}
	}
	if _, ok := w.HostFn(aocexpr.HostNow, nil); ok {
		t.Error("Now is no node read")
	}
}

// (C O A : C Y B)

func TestNodeHostCasesRoundTrip(t *testing.T) {
	for _, c := range nodeHostCases() {
		if err := c.check(); err != nil {
			t.Error(err)
		}
	}
	// the corpus text of a block reads back as the block
	text := "nodehost x\nnodes 2\nnode 0 1 4\nnode 1 2 3\nctx 1 14 0 -1\nq 174 1 = 3\nq 172 = 1\nq 170 = 14\nend\n"
	c, err := ParseCorpus(strings.NewReader(text))
	if err != nil || len(c.NodeHosts) != 1 {
		t.Fatalf("ParseCorpus = %v, %v", c, err)
	}
	h := c.NodeHosts[0]
	if h.World.Types[1] != 2 || h.World.Levels[0] != 4 || h.World.PlayerNode != -1 || len(h.Queries) != 3 || h.Queries[0].Args[0] != 1 || h.Queries[0].Want != 3 {
		t.Errorf("block = %+v", h)
	}
	if err := h.check(); err != nil {
		t.Error(err)
	}
	bad := strings.Replace(text, "q 174 1 = 3", "q 174 1 = 5", 1)
	c, _ = ParseCorpus(strings.NewReader(bad))
	if c == nil || c.Replay() == nil {
		t.Error("a wrong answer replayed")
	}
	for _, line := range []string{"q 170 = 1\n", "nodehost\n", "nodehost x\nnodes 1\nnode 5 0 0\nend\n", "nodehost x\nq 170 5\nend\n", "nodehost x\nctx 1 2\nend\n", "nodehost x\n"} {
		if _, err := ParseCorpus(strings.NewReader(line)); err == nil {
			t.Errorf("%q was accepted", line)
		}
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"strconv"

	"aocrop/services/internal/aocexpr"
)

// The city node reads of an ability expression (Nodes 1, stage 1; contentc version 12): GetNodeBiome, GetCurrentNode, GetNodeRecord, Node_GetNodeType,
// Node_GetLevel, Node_InLevelRangeInclusive, Node_IsPlayerCitizen, Node_IsPlayerCitizenOfNode and Node_IsPlayerMayor, with GetPlayer and EnumInt
// that the data writes around them.
//
// They are host functions (aocexpr/hostfn.go HostNodeBiome ... HostPlayerMayor, the engine's OpHost0..2) over numbers: a node is its rank (the index of
// its CityNodeRecord in the list of CityNodeRecordBase sorted by guid, -1 for none: Graph.Nodes), an enum answer is the value's ordinal
// (aocexpr/enums.go). A reference to a node record in the text ($#8574414081702566619:<guid>$, or $CityNode:<record name>$) becomes the rank, and
// an enum literal that a host answer is compared with becomes its ordinal, so the engine only ever compares numbers.
//
// Node_InLevelRangeInclusive is not a host function of its own: it is the level the host gives, compared with the two bounds, in the program.

// GraphNode is a CityNodeRecord the compiler knows by rank.
type GraphNode struct {
	Guid int64
	Name string // the record name, "Verra_RVR_Joeva"
}

// nodeRank is the rank of the node record r names, or an error.
func (g *Graph) nodeRank(r aocexpr.Ref) (int, error) {
	if len(g.Nodes) == 0 {
		return 0, fmt.Errorf("unsupported: the graph has no city nodes (content.db before contentc version 12)")
	}
	switch {
	case r.Kind == aocexpr.KindCityNode:
		for i, n := range g.Nodes {
			if n.Name == r.Name {
				return i, nil
			}
		}
		return 0, fmt.Errorf("%v is no city node record", r)
	case r.Kind == "" && r.TypeID == aocexpr.TypeCityNode:
		if r.Guid == 0 {
			return -1, nil // the empty reference: no node
		}
		for i, n := range g.Nodes {
			if n.Guid == r.Guid {
				return i, nil
			}
		}
		return 0, fmt.Errorf("%v is no city node record", r)
	}
	return 0, fmt.Errorf("%v is no city node reference", r)
}

// NodeRank and NodeGuid make a context over a graph a aocexpr.NodeRanker (the reference evaluator's way to turn a node value into a reference).
func (g *Graph) NodeRank(guid int64, name string) (int, bool) {
	r := aocexpr.Ref{TypeID: aocexpr.TypeCityNode, Guid: guid}
	if guid == 0 && name != "" {
		r = aocexpr.Ref{Kind: aocexpr.KindCityNode, Name: name}
	}
	rank, err := g.nodeRank(r)
	return rank, err == nil
}

// ~ C.Y.B :: C O A ~

func (g *Graph) NodeGuid(rank int) (int64, bool) {
	if rank == -1 {
		return 0, true
	}
	if rank < 0 || rank >= len(g.Nodes) {
		return 0, false
	}
	return g.Nodes[rank].Guid, true
}

// nodeCall compiles a city node read; ok is false when e is not one.
func (f *fnc) nodeCall(e aocexpr.Call, args []val) (v val, ok bool, err error) {
	switch e.Name {
	case "GetPlayer", "GetPlayerGuid", "GetNodeBiome", "GetNodeRecord", "GetCurrentNode", "Node_GetNodeType", "Node_GetLevel",
		"Node_InLevelRangeInclusive", "Node_IsPlayerCitizen", "Node_IsPlayerCitizenOfNode", "Node_IsPlayerMayor", "EnumInt":
	default:
		return val{}, false, nil
	}
	v, err = f.nodeRead(e, args)
	return v, true, err
}

func (f *fnc) nodeRead(e aocexpr.Call, args []val) (val, error) {
	count := func(n int) error { return argc(e, n) }
	// arg i is a city node: a node value (GetCurrentNode ...) or a reference to a node record, which becomes its rank
	node := func(i int) (val, error) {
		a := f.loose(args[i])
		switch {
		case a.t == tNum && a.etype == etypeNode:
			return a, nil
		case a.t == tRef:
			rank, err := f.c.g.nodeRank(a.ref)
			if err != nil {
				return val{}, errAt(e.Args[i].Pos(), "%s: %v", e.Name, err)
			}
			return val{t: tNum, n: nConst{float64(rank)}, etype: etypeNode}, nil
		}
		return val{}, errAt(e.Args[i].Pos(), "%s: argument %d is %v, not a city node", e.Name, i+1, a.t)
	}
	player := func(i int) error {
		if args[i].t != tPlayer {
			return errAt(e.Args[i].Pos(), "%s: argument %d is %v, not GetPlayer()", e.Name, i+1, args[i].t)
		}
		return nil
	}
	// a host answer: a number that is not a float, with the enum it is the ordinal of
	answer := func(v val, err error, etype string, t vtype) (val, error) {
		if err != nil {
			return v, err
		}
		v.t, v.float, v.etype = t, false, etype
		return v, nil
	}
	// arg i is an ENodeLevel: a literal or a node level read
	level := func(i int) (val, error) {
		a := args[i]
		switch {
		case a.t == tEnum && a.enum.Type == "ENodeLevel":
			ord, ok := aocexpr.EnumOrdinal(a.enum.Type, a.enum.Value)
			if !ok {
				return val{}, errAt(e.Args[i].Pos(), "%s: %v is not a value of ENodeLevel", e.Name, a.enum)
			}
			return val{t: tNum, n: nConst{float64(ord)}, etype: "ENodeLevel"}, nil
		case a.t == tNum && a.etype == "ENodeLevel":
			return a, nil
		}
		return val{}, errAt(e.Args[i].Pos(), "%s: argument %d is %v, not an ENodeLevel", e.Name, i+1, a.t)
	}
	num := func(n int) val { return val{t: tNum, n: nConst{float64(n)}} }

	switch e.Name {
	case "GetPlayer", "GetPlayerGuid":
		if err := count(0); err != nil {
			return val{}, err
		}
		return val{t: tPlayer}, nil
	case "GetNodeBiome":
		if err := count(0); err != nil {
			return val{}, err
		}
		v, err := f.hostCall(e, aocexpr.HostNodeBiome, "", tNum)
		return answer(v, err, "EBiomeType", tNum)
	case "GetNodeRecord":
		if err := count(0); err != nil {
			return val{}, err
		}
		v, err := f.hostCall(e, aocexpr.HostNodeRecord, "", tNum)
		return answer(v, err, etypeNode, tNum)
	case "GetCurrentNode":
		if err := count(1); err != nil {
			return val{}, err
		}
		who := 0
		switch args[0].t {
		case tPlayer:
			who = aocexpr.ActorPlayer
		case tObj:
			who = args[0].role
		default:
			return val{}, errAt(e.Args[0].Pos(), "GetCurrentNode: argument 1 is %v, not an actor", args[0].t)
		}
		v, err := f.hostCall(e, aocexpr.HostCurrentNode, "", tNum, num(who))
		return answer(v, err, etypeNode, tNum)
	case "Node_GetNodeType", "Node_GetLevel":
		if err := count(1); err != nil {
			return val{}, err
		}
		n, err := node(0)
		if err != nil {
			return val{}, err
		}
		fn, etype := aocexpr.HostNodeType, "ENodeType"
		if e.Name == "Node_GetLevel" {
			fn, etype = aocexpr.HostNodeLevel, "ENodeLevel"
		}
		v, err := f.hostCall(e, fn, "", tNum, n)
		return answer(v, err, etype, tNum)
	case "Node_InLevelRangeInclusive":
		if err := count(3); err != nil {
			return val{}, err
		}
		n, err := node(0)
		if err != nil {
			return val{}, err
		}
		lo, err := level(1)
		if err != nil {
			return val{}, err
		}
		hi, err := level(2)
		if err != nil {
			return val{}, err
		}
		lv, err := f.hostCall(e, aocexpr.HostNodeLevel, "", tNum, n)
		if err != nil {
			return val{}, err
		}
		return val{t: tBool, n: nAnd{nBin{OpGe, lv.n, lo.n}, nBin{OpLe, lv.n, hi.n}}}, nil
	case "Node_IsPlayerCitizen", "Node_IsPlayerMayor":
		if err := count(1); err != nil {
			return val{}, err
		}
		if err := player(0); err != nil {
			return val{}, err
		}
		fn := aocexpr.HostPlayerCitizen
		if e.Name == "Node_IsPlayerMayor" {
			fn = aocexpr.HostPlayerMayor
		}
		v, err := f.hostCall(e, fn, "", tBool)
		return answer(v, err, "", tBool)
	case "Node_IsPlayerCitizenOfNode":
		if err := count(2); err != nil {
			return val{}, err
		}
		if err := player(0); err != nil {
			return val{}, err
		}
		n, err := node(1)
		if err != nil {
			return val{}, err
		}
		v, err := f.hostCall(e, aocexpr.HostPlayerCitizenOfNode, "", tBool, n)
		return answer(v, err, "", tBool)
	case "EnumInt":
		if err := count(1); err != nil {
			return val{}, err
		}
		a := args[0]
		switch {
		case a.t == tEnum:
			ord, ok := aocexpr.EnumOrdinal(a.enum.Type, a.enum.Value)
			if !ok {
				return val{}, errAt(e.Args[0].Pos(), "EnumInt: %v is not an enum the compiler knows", a.enum)
			}
			return val{t: tNum, n: nConst{float64(ord)}}, nil
		case a.t == tNum && a.etype != "" && a.etype != etypeNode:
			a.etype, a.float = "", false
			return a, nil
		}
		return val{}, errAt(e.Args[0].Pos(), "EnumInt: argument 1 is %v, not an enum value", a.t)
	}
	return val{}, unsupportedAt(e.At, "function %q", e.Name)
}

// == C | O | A | Cyb ==

// NodeWorld is the host's model of the city nodes behind the node reads, and the reference for the C++ host (worldd/world/city_nodes.h): the nodes by
// rank with their type and level (as the ordinals of aocexpr/enums.go), and the context an expression runs in. The shared corpus writes `nodehost`
// blocks from it and the C++ test checks its own host against the same answers.
//
//	GetNodeBiome()            the context's biome (Biome)
//	GetCurrentNode(actor)     the node the player is in (PlayerNode) for aocexpr.ActorPlayer, else the node of the expression's own actor (ActorNode)
//	GetNodeRecord()           the node of the context (Node)
//	Node_GetNodeType(rank)    the node's type; Default (0) for no node (-1) or a rank the registry does not have
//	Node_GetLevel(rank)       the node's level; Wilderness (0) for the same
//	the citizen and mayor reads   0 until the citizenship milestone
type NodeWorld struct {
	Types, Levels         []int // by rank
	Node, Biome           int   // the context: its node's rank (-1: none) and its biome (EBiomeType; Default for none)
	ActorNode, PlayerNode int   // the node of the expression's own actor and of the player (-1: none)
}

// HostFn answers a node read; ok is false for a function that is no node read.
func (w *NodeWorld) HostFn(fn int, args []float64) (v float64, ok bool) {
	at := func(table []int) float64 {
		if len(args) < 1 {
			return 0
		}
		r := int(args[0])
		if float64(r) != args[0] || r < 0 || r >= len(table) {
			return 0
		}
		return float64(table[r])
	}
	switch fn {
	case aocexpr.HostNodeBiome:
		return float64(w.Biome), true
	case aocexpr.HostCurrentNode:
		if len(args) >= 1 && args[0] == aocexpr.ActorPlayer {
			return float64(w.PlayerNode), true
		}
		return float64(w.ActorNode), true
	case aocexpr.HostNodeRecord:
		return float64(w.Node), true
	case aocexpr.HostNodeType:
		return at(w.Types), true
	case aocexpr.HostNodeLevel:
		return at(w.Levels), true
	case aocexpr.HostPlayerCitizen, aocexpr.HostPlayerCitizenOfNode, aocexpr.HostPlayerMayor:
		return 0, true
	}
	return 0, false
}

// NodeHostQuery is a read of a NodeWorld: the function, up to two arguments and the answer (whole numbers).
type NodeHostQuery struct {
	Fn   int
	Args []int
	Want int
}

// NodeHostCase is a NodeWorld with the reads the host must answer.
type NodeHostCase struct {
	Name    string
	World   NodeWorld
	Queries []NodeHostQuery
}

func argsOfInts(a []int) []float64 {
	out := make([]float64, len(a))
	for i, x := range a {
		out[i] = float64(x)
	}
	return out
}

// check runs the queries against the model.
func (c NodeHostCase) check() error {
	for i, q := range c.Queries {
		got, ok := c.World.HostFn(q.Fn, argsOfInts(q.Args))
		if !ok || got != float64(q.Want) {
			return fmt.Errorf("nodehost %s: query %d (fn %d %v) = %v, want %d", c.Name, i, q.Fn, q.Args, got, q.Want)
		}
	}
	return nil
}

// nodeHostCases are the worlds of the corpus: a world of five nodes seen from inside one and from the road, a world with no node in the context, and an
// empty registry. Every function is asked with a node, with no node, and with a rank past the registry.
func nodeHostCases() []NodeHostCase {
	worlds := []struct {
		name  string
		world NodeWorld
	}{
		{"nodes_in_a_town", NodeWorld{Types: []int{0, 1, 2, 3, 4}, Levels: []int{0, 4, 3, 3, 2}, Node: 1, Biome: 14, ActorNode: 1, PlayerNode: 2}},
		{"nodes_on_the_road", NodeWorld{Types: []int{0, 1, 2, 3, 4}, Levels: []int{0, 4, 3, 3, 2}, Node: -1, Biome: 0, ActorNode: -1, PlayerNode: -1}},
		{"no_nodes", NodeWorld{Node: -1, ActorNode: -1, PlayerNode: -1, Biome: 18}},
	}
	var out []NodeHostCase
	for _, w := range worlds {
		c := NodeHostCase{Name: w.name, World: w.world}
		ask := func(fn int, args ...int) {
			v, _ := c.World.HostFn(fn, argsOfInts(args))
			c.Queries = append(c.Queries, NodeHostQuery{Fn: fn, Args: args, Want: int(v)})
		}
		ask(aocexpr.HostNodeBiome)
		for _, who := range []int{aocexpr.RoleCaster, aocexpr.RoleTarget, aocexpr.ActorPlayer} {
			ask(aocexpr.HostCurrentNode, who)
		}
		ask(aocexpr.HostNodeRecord)
		for _, rank := range []int{-1, 0, 1, 2, 4, 5, 99} {
			ask(aocexpr.HostNodeType, rank)
			ask(aocexpr.HostNodeLevel, rank)
			ask(aocexpr.HostPlayerCitizenOfNode, rank)
		}
		ask(aocexpr.HostPlayerCitizen)
		ask(aocexpr.HostPlayerMayor)
		out = append(out, c)
	}
	return out
}

// -- C:Y:B & C.O.A --

// parseLine reads a line of a nodehost block.
func (c *NodeHostCase) parseLine(f []string) error {
	ints := func(from, to int) ([]int, error) {
		var out []int
		for _, s := range f[from:to] {
			n, err := strconv.Atoi(s)
			if err != nil {
				return nil, fmt.Errorf("%q is no whole number", s)
			}
			out = append(out, n)
		}
		return out, nil
	}
	switch f[0] {
	case "nodes":
		n, err := ints(1, len(f))
		if err != nil || len(n) != 1 || n[0] < 0 {
			return fmt.Errorf("nodes N")
		}
		c.World.Types, c.World.Levels = make([]int, n[0]), make([]int, n[0])
	case "node":
		n, err := ints(1, len(f))
		if err != nil || len(n) != 3 || n[0] < 0 || n[0] >= len(c.World.Types) {
			return fmt.Errorf("node RANK TYPE LEVEL (after nodes N)")
		}
		c.World.Types[n[0]], c.World.Levels[n[0]] = n[1], n[2]
	case "ctx":
		n, err := ints(1, len(f))
		if err != nil || len(n) != 4 {
			return fmt.Errorf("ctx NODE BIOME ACTOR PLAYER")
		}
		c.World.Node, c.World.Biome, c.World.ActorNode, c.World.PlayerNode = n[0], n[1], n[2], n[3]
	case "q":
		eq := -1
		for i, s := range f {
			if s == "=" {
				eq = i
			}
		}
		if eq < 2 || eq != len(f)-2 || len(f) > 6 {
			return fmt.Errorf("q FN ARGS... = WANT")
		}
		head, err := ints(1, eq)
		if err != nil {
			return err
		}
		want, err := strconv.Atoi(f[eq+1])
		if err != nil {
			return fmt.Errorf("q: the answer %q is no whole number", f[eq+1])
		}
		c.Queries = append(c.Queries, NodeHostQuery{Fn: head[0], Args: head[1:], Want: want})
	}
	return nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"encoding/binary"
	"sort"
	"strconv"
)

// progInfo is what verifying a program found out about it.
type progInfo struct {
	maxStack int
	stats    []uint16 // the stats it reads (OpStat), sorted, without repeats
	host     bool     // it uses a host operation (OpProgress, OpEffectInst, OpRand): only an expression program may
	total    bool     // it reads the running total (OpTotal): an initializer entry that sees the stat's value so far (engine.go)
}

// -- Cyb . C-O-A --

// analyze checks one program and returns what it needs. The rules, which the C++
// reader applies too:
//   - every opcode is known and its operands are inside the code;
//   - an operand indexes a table that has it (constants, stats, curves, references,
//     strings, locals);
//   - jumps are forward, land on an instruction, and every path to an instruction
//     arrives with the same stack height; code after an unconditional jump is
//     reachable only by a jump;
//   - the stack never goes below 0 or above MaxStack;
//   - the code ends in the only OpRet, with exactly one value on the stack.
//
// So a program runs at most one instruction per code byte, and cannot loop.
func (b *Bundle) analyze(pi int) (progInfo, error) {
	p := b.Progs[pi]
	code := p.Code
	n := len(code)
	if n < 1 || n > MaxCodeBytes {
		return progInfo{}, bad("program %d: code length %d", pi, n)
	}
	if int(p.Locals) > MaxLocals {
		return progInfo{}, bad("program %d: %d locals (the most is %d)", pi, p.Locals, MaxLocals)
	}
	height := make([]int, n+1)
	for i := range height {
		height[i] = -1
	}
	starts := make([]bool, n+1)
	var targets []int
	seen := map[uint16]bool{}
	var info progInfo
	cur, reachable, pc := 0, true, 0
	var last byte
	for pc < n {
		op := code[pc]
		sz := operandBytes(op)
		if sz < 0 {
			return info, bad("program %d: unknown opcode %#x at %d", pi, op, pc)
		}
		if pc+1+sz > n {
			return info, bad("program %d: opcode at %d is cut off", pi, pc)
		}
		starts[pc] = true
		if isHostOp(op) {
			info.host = true
		}
		if op == OpTotal {
			info.total = true
		}
		switch {
		case height[pc] >= 0:
			if reachable && cur != height[pc] {
				return info, bad("program %d: stack height %d falls into %d at %d", pi, cur, height[pc], pc)
			}
			cur = height[pc]
		case !reachable:
			return info, bad("program %d: unreachable code at %d", pi, pc)
		default:
			height[pc] = cur
		}
		reachable = true
		u16 := func() int { return int(binary.LittleEndian.Uint16(code[pc+1:])) }
		at := func(i int) int { return int(binary.LittleEndian.Uint16(code[pc+1+2*i:])) }
		fail := func(what string, idx, limit int) error {
			return bad("program %d: %s %d at %d is not below %d", pi, what, idx, pc, limit)
		}
		switch op {
		case OpConst:
			if u16() >= len(b.Consts) {
				return info, fail("constant", u16(), len(b.Consts))
			}
		case OpLoad, OpStore:
			if int(code[pc+1]) >= int(p.Locals) {
				return info, fail("local", int(code[pc+1]), int(p.Locals))
			}
		case OpActor:
			if int(code[pc+1]) >= MaxRoles {
				return info, fail("actor role", int(code[pc+1]), MaxRoles)
			}
		case OpNpcDist:
			if u16() >= 8 {
				return info, fail("stat distribution index", u16(), 8)
			}
		case OpStat:
			if u16() >= len(b.Stats) {
				return info, fail("stat", u16(), len(b.Stats))
			}
			if !seen[uint16(u16())] {
				seen[uint16(u16())] = true
				info.stats = append(info.stats, uint16(u16()))
			}
		case OpCurve:
			if u16() >= len(b.Curves) {
				return info, fail("curve", u16(), len(b.Curves))
			}
		case OpEffect, OpConsumed, OpEffectInst:
			if u16() >= len(b.Refs) {
				return info, fail("reference", u16(), len(b.Refs))
			}
		case OpSkill:
			for i := 0; i < 2; i++ {
				if at(i) >= len(b.Refs) {
					return info, fail("reference", at(i), len(b.Refs))
				}
			}
		case OpEffectTag, OpProfession, OpProgress, OpHitTag, OpHitFlag:
			if u16() >= len(b.Strings) {
				return info, fail("string", u16(), len(b.Strings))
			}
		case OpEquipTag:
			for i := 0; i < 2; i++ {
				if at(i) >= len(b.Strings) {
					return info, fail("string", at(i), len(b.Strings))
				}
			}
		case OpHost0, OpHost1, OpHost2:
			if at(1) >= len(b.Strings) {
				return info, fail("string", at(1), len(b.Strings))
			}
			if at(0) < 1 || at(0) > 255 {
				return info, fail("host function", at(0), 256)
			}
		}
		pop, push := stackEffect(op)
		switch op {
		case OpRet:
			if cur != 1 {
				return info, bad("program %d: the stack holds %d values at the return (one is the result)", pi, cur)
			}
			if pc+1 != n {
				return info, bad("program %d: code after the return at %d", pi, pc)
			}
			reachable = false
		case OpJmp, OpJz, OpJnz:
			if op != OpJmp {
				if cur < 1 {
					return info, bad("program %d: stack underflow at %d", pi, pc)
				}
				cur--
			}
			t := u16()
			if t < pc+3 || t >= n {
				return info, bad("program %d: jump at %d to %d (forward, inside the code)", pi, pc, t)
			}
			if height[t] >= 0 && height[t] != cur {
				return info, bad("program %d: jump at %d arrives at %d with height %d, not %d", pi, pc, t, cur, height[t])
			}
			height[t] = cur
			targets = append(targets, t)
			if op == OpJmp {
				reachable = false
			}
		default:
			if cur < pop {
				return info, bad("program %d: stack underflow at %d", pi, pc)
			}
			cur = cur - pop + push
			if cur > MaxStack {
				return info, bad("program %d: the stack is deeper than %d at %d", pi, MaxStack, pc)
			}
			if cur > info.maxStack {
				info.maxStack = cur
			}
		}
		last = op
		pc += 1 + sz
	}
	if last != OpRet {
		return info, bad("program %d does not end in a return", pi)
	}
	for _, t := range targets {
		if !starts[t] {
			return info, bad("program %d: a jump lands inside an instruction (offset %d)", pi, t)
		}
	}
	sort.Slice(info.stats, func(i, j int) bool { return info.stats[i] < info.stats[j] })
	return info, nil
}

// (C.Y.B : C.O.A)

// verify checks a decoded bundle's tables, programs and order.
func (b *Bundle) verify() error {
	ns, np := len(b.Stats), len(b.Progs)
	infos := make([]progInfo, np)
	for i := range b.Progs {
		inf, err := b.analyze(i)
		if err != nil {
			return err
		}
		infos[i] = inf
	}
	guids := map[int64]bool{}
	progOK := func(p int32) bool { return p == -1 || (p >= 0 && int(p) < np) }
	for i, s := range b.Stats {
		switch {
		case int(s.Name) >= len(b.Strings):
			return bad("stat %d: name string %d is not below %d", i, s.Name, len(b.Strings))
		case guids[s.Guid]:
			return bad("stat %d: guid %d is a second stat's", i, s.Guid)
		case s.Replication > 7:
			return bad("stat %d: replication %d", i, s.Replication)
		case s.Flags&^statFlagsAllowed != 0:
			return bad("stat %d: flags %#x", i, s.Flags)
		case !progOK(s.Base) || !progOK(s.Min) || !progOK(s.Max) || !progOK(s.Regen):
			return bad("stat %d: a program index is outside the %d programs", i, np)
		}
		guids[s.Guid] = true
	}
	checkSteps := func(what string, steps []Step) error {
		for i, s := range steps {
			if int(s.Stat) >= ns || int(s.Prog) >= np {
				return bad("%s %d: stat %d or program %d is outside the tables", what, i, s.Stat, s.Prog)
			}
		}
		return nil
	}
	checkMembers := func(what string, members []uint32) error {
		for i, m := range members {
			if int(m) >= ns {
				return bad("%s %d: stat %d is not below %d", what, i, m, ns)
			}
		}
		return nil
	}
	if err := checkSteps("common step", b.CommonSteps); err != nil {
		return err
	}
	if err := checkMembers("common member", b.CommonMembers); err != nil {
		return err
	}
	enums := map[uint8]bool{}
	for i, c := range b.Classes {
		switch {
		case c.Enum == 0 || enums[c.Enum]:
			return bad("class %d: enum %d is 0 or a second class's", i, c.Enum)
		case int(c.Name) >= len(b.Strings):
			return bad("class %d: name string %d is not below %d", i, c.Name, len(b.Strings))
		}
		enums[c.Enum] = true
		if err := checkSteps("class step", c.Steps); err != nil {
			return err
		}
		if err := checkMembers("class member", c.Members); err != nil {
			return err
		}
	}

	// The order: every stat's two nodes once, the initial value before the value.
	if len(b.Order) != 2*ns {
		return bad("the order has %d nodes, %d stats need %d", len(b.Order), ns, 2*ns)
	}
	pos := make([]int, 2*ns)
	for i := range pos {
		pos[i] = -1
	}
	for i, node := range b.Order {
		if int(node>>1) >= ns {
			return bad("order %d: stat %d is not below %d", i, node>>1, ns)
		}
		if pos[node] != -1 {
			return bad("order %d: node %d appears twice", i, node)
		}
		pos[node] = i
	}
	for s := 0; s < ns; s++ {
		if pos[2*s+NodeInit] > pos[2*s+NodeValue] {
			return bad("stat %d: its value comes before its initial value", s)
		}
	}
	// A program reads only stats settled before the node that runs it.
	readsBefore := func(prog int32, node int, what string) error {
		if prog < 0 {
			return nil
		}
		for _, t := range infos[prog].stats {
			if pos[2*int(t)+NodeValue] >= pos[node] {
				return bad("%s reads stat %d, which is not settled before it", what, t)
			}
		}
		return nil
	}
	// A step that reads the running total sees the stat's value so far, Base included (engine.go): the Base runs at the
	// initial-value node too, so what it reads must be settled before that node.
	stepReadsBefore := func(s Step, what string) error {
		if err := readsBefore(int32(s.Prog), 2*int(s.Stat)+NodeInit, what); err != nil {
			return err
		}
		if infos[s.Prog].total {
			return readsBefore(b.Stats[s.Stat].Base, 2*int(s.Stat)+NodeInit, "the Base of stat "+strconv.Itoa(int(s.Stat))+", which "+what+" reads through the running total,")
		}
		return nil
	}
	for _, s := range b.CommonSteps {
		if err := stepReadsBefore(s, "a common initializer step of stat "+strconv.Itoa(int(s.Stat))); err != nil {
			return err
		}
	}
	for _, c := range b.Classes {
		for _, s := range c.Steps {
			if err := stepReadsBefore(s, "a class initializer step of stat "+strconv.Itoa(int(s.Stat))); err != nil {
				return err
			}
		}
	}
	for i, s := range b.Stats {
		for _, p := range []int32{s.Base, s.Min, s.Max} {
			if err := readsBefore(p, 2*i+NodeValue, "a formula of stat "+strconv.Itoa(i)); err != nil {
				return err
			}
		}
	}

	// The expression programs: in range, strictly increasing; and no formula of the stat graph uses a host operation (it has no host).
	for i, p := range b.Exprs {
		switch {
		case int(p) >= np:
			return bad("expression %d: program %d is not below %d", i, p, np)
		case i > 0 && p <= b.Exprs[i-1]:
			return bad("expression %d: program %d does not come after program %d", i, p, b.Exprs[i-1])
		}
	}
	noHost := func(prog int32, what string) error {
		if prog >= 0 && infos[prog].host {
			return bad("%s uses a host operation (only an expression program may)", what)
		}
		return nil
	}
	for _, s := range b.CommonSteps {
		if err := noHost(int32(s.Prog), "a common initializer step of stat "+strconv.Itoa(int(s.Stat))); err != nil {
			return err
		}
	}
	for _, c := range b.Classes {
		for _, s := range c.Steps {
			if err := noHost(int32(s.Prog), "a class initializer step of stat "+strconv.Itoa(int(s.Stat))); err != nil {
				return err
			}
		}
	}
	for i, s := range b.Stats {
		for _, p := range []int32{s.Base, s.Min, s.Max, s.Regen} {
			if err := noHost(p, "a formula of stat "+strconv.Itoa(i)); err != nil {
				return err
			}
		}
	}
	return nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"

	"aocrop/services/internal/aocexpr"
)

// The interpreter and the stat engine over a verified Bundle (Decode, or the
// result of Compile). The C++ one in server/worldd/stats runs the same programs
// to the same bits: the arithmetic is IEEE double, one operation per instruction,
// in the same order, with the values the engine keeps rounded to float32 (Mode).
//
// What a run computes, per stat, in the order Bundle.Order gives:
//
//	initial value   the stat's initializer entries in list order (the common lists,
//	                then the class's), each seeing the value so far as
//	                GetStatInitRunningTotal(); a stat starts at 0, or at 1 when it
//	                multiplies (bMultiplyStatMods)
//	value           base: the Base formula gives b, and value = initial + b (or initial * b
//	                for a multiplying stat); no Base leaves the initial value;
//	                then the equipment's contribution (item stat blocks and set
//	                bonuses, given by the caller) is added; then the stat is raised
//	                to its ValueMin and lowered to its ValueMax; a stat that
//	                spawns at its maximum (Health, Mana, Stamina) is its maximum
//
// Effects (talents, food, guild) apply after the equipment, in the form each stat has (effects.go):
// the active effects of Inputs.Effects add to an additive stat and multiply a multiplying one, then the
// limits apply. The same effects, the ranks of Inputs.Ranks and the aocexpr.State answer the context
// operations (effect counts, skills). A finished run keeps every stat's value (Result.Values) so that
// an Evaluator can run the expression programs of an ability against them.

// Mod is a stat's change from the equipment: the sum of the equipped items'
// stat blocks at their qualities and of the set bonuses of their sets. A guid
// that is not a stat of the graph is ignored (Result.IgnoredMods counts it).
type Mod struct {
	Stat  int64
	Value float64
}

// built with CoA and C | Y | B

// EquippedTag says the item worn in a slot has a gameplay tag (Slot is the
// EEquipSlot value's name, "Secondary"; give every tag of the item).
type EquippedTag struct {
	Slot string
	Tag  string
}

// Inputs are what a run needs of the character.
type Inputs struct {
	Class    int // ECharacterClass: Ranger 1 ... Mage 8
	Level    int // 1 to MaxLevel
	Mods     []Mod
	Equipped []EquippedTag
	// Effects are the active effects (plan D2) and Ranks the skill ranks the character holds, purchased and
	// auto-granted: see effects.go.
	Effects []ActiveEffect
	Ranks   []SkillRank
}

// StatOut is one stat of the class, with the audience mask of its replication.
type StatOut struct {
	Stat        int // index in Bundle.Stats
	Guid        int64
	Replication uint8 // 0 not replicated, 1 Owner, 5 OwnerProxy, 7 EveryoneProxy, 6 ProxyOnly
	Value       float64
	Base        float64 // the value before the equipment's contribution
	Equip       float64 // the equipment's contribution
	Regen       float64 // the stat's regeneration rate, when it has one
	HasRegen    bool
}

// Result is a run's stats: the common lists' and the class's, in stat order.
type Result struct {
	Stats       []StatOut
	Faults      int // divisions by zero, curve reads of NaN, and values that were not finite (made 0)
	IgnoredMods int
	// IgnoredEffectMods counts the effect mods that name a stat the bundle does not have (they change nothing).
	IgnoredEffectMods int
	// Values is every stat of the bundle's settled value by index in Bundle.Stats, the stats that are no member
	// of the class's lists too (an expression program may read any of them).
	Values []float64
	// Instructions is how many the run executed (at most MaxRunInstructions).
	Instructions int
}

func (b *Bundle) guidIndex(guid int64) (int, bool) {
	b.ix.once.Do(func() {
		b.ix.byGuid = make(map[int64]int, len(b.Stats))
		for i, s := range b.Stats {
			b.ix.byGuid[s.Guid] = i
		}
	})
	i, ok := b.ix.byGuid[guid]
	return i, ok
}

// StatIndex is the index of the stat with the guid in Stats.
func (b *Bundle) StatIndex(guid int64) (int, bool) { return b.guidIndex(guid) }

// ClassByEnum is the class with the ECharacterClass value.
func (b *Bundle) ClassByEnum(enum int) (*Class, bool) {
	for i := range b.Classes {
		if int(b.Classes[i].Enum) == enum {
			return &b.Classes[i], true
		}
	}
	return nil, false
}

// actorCtx is what the reads of one actor use: its settled stat values, its level, its state (effects, skills, flags) and its inputs.
type actorCtx struct {
	vals  []float64
	level float64
	state aocexpr.State
	in    *Inputs
}

// machine runs programs.
type machine struct {
	b     *Bundle
	vals  []float64 // settled stat values (of the actor the program is reading now: see OpActor)
	total float64
	level float64
	state aocexpr.State
	in    *Inputs
	// caster is the caster's own context, and actors the contexts of the other actors of a hit by role (nil: not supplied, read as the caster).
	caster actorCtx
	actors [MaxRoles]*actorCtx
	host   ExprHost // the expression programs' inputs (nil: NoHost)
	faults int
	// used counts the instructions executed in all programs of the run; at MaxRunInstructions the
	// run is exhausted: every program returns at once and Run reports the error.
	used      int
	exhausted bool
	locals    [MaxLocals]float64
	stack     [MaxStack]float64
}

// useActor makes the reads that follow read the actor with the role: its own context when one was supplied, else the caster's.
func (m *machine) useActor(role int) {
	a := m.actors[role]
	if a == nil {
		a = &m.caster
	}
	m.vals, m.level, m.state, m.in = a.vals, a.level, a.state, a.in
}

func (m *machine) refOf(i uint16) aocexpr.Ref {
	r := m.b.Refs[i]
	return aocexpr.Ref{TypeID: r.TypeID, Guid: r.Guid}
}

// effectCount is GetEffectCount: the stacks of the active effects with the record, and what the state adds.
func (m *machine) effectCount(r aocexpr.Ref) float64 {
	n := m.state.EffectCount(r)
	for i := range m.in.Effects {
		if m.in.Effects[i].Effect == r {
			n += float64(m.in.Effects[i].Stacks)
		}
	}
	return n
}

// effectCountByTag is GetEffectCountByTag: the stacks of the active effects with a tag that matches, and what the state adds.
func (m *machine) effectCountByTag(tag string) float64 {
	n := m.state.EffectCountByTag(tag)
	for i := range m.in.Effects {
		for _, t := range m.in.Effects[i].Tags {
			if aocexpr.TagMatches(t, tag) {
				n += float64(m.in.Effects[i].Stacks)
				break
			}
		}
	}
	return n
}

// skillPurchased is GetSkillPurchased: a rank the character holds, or the state says so.
func (m *machine) skillPurchased(class, skill aocexpr.Ref) bool {
	for _, r := range m.in.Ranks {
		if r.Class == class && r.Skill == skill {
			return true
		}
	}
	return m.state.SkillPurchased(class, skill)
}

func (m *machine) equipTag(slot, tag string) bool {
	for _, e := range m.in.Equipped {
		if e.Slot == slot && aocexpr.TagMatches(e.Tag, tag) {
			return true
		}
	}
	return m.state.EquippedItemInSlotHasTag(aocexpr.Enum{Type: "EEquipSlot", Value: slot}, tag)
}

// C | O | A x C_Y_B

func goMin(x, y float64) float64 {
	switch {
	case math.IsInf(x, -1) || math.IsInf(y, -1):
		return math.Inf(-1)
	case math.IsNaN(x) || math.IsNaN(y):
		return math.NaN()
	case x == 0 && x == y:
		if math.Signbit(x) {
			return x
		}
		return y
	}
	if x < y {
		return x
	}
	return y
}

func goMax(x, y float64) float64 {
	switch {
	case math.IsInf(x, 1) || math.IsInf(y, 1):
		return math.Inf(1)
	case math.IsNaN(x) || math.IsNaN(y):
		return math.NaN()
	case x == 0 && x == y:
		if math.Signbit(x) {
			return y
		}
		return x
	}
	if x > y {
		return x
	}
	return y
}

// evalCurve is aocexpr.Curve.Eval over a bundle curve.
func evalCurve(c *Curve, t float64) (v float64, fault bool) {
	ks := c.Keys
	switch {
	case t != t:
		return ks[0].Value, true
	case t <= ks[0].Time:
		return ks[0].Value, false
	case t >= ks[len(ks)-1].Time:
		return ks[len(ks)-1].Value, false
	}
	i := sort.Search(len(ks), func(i int) bool { return ks[i].Time > t }) - 1
	a, b := ks[i], ks[i+1]
	if t == a.Time || a.Interp == InterpConstant {
		return a.Value, false
	}
	if a.Interp == InterpCubic {
		return aocexpr.CubicSegment(a.Time, a.Value, a.Leave, b.Time, b.Value, b.Arrive, t), false
	}
	return a.Value + (b.Value-a.Value)*(t-a.Time)/(b.Time-a.Time), false
}

// run executes a verified program and returns its value.
func (m *machine) run(p *Program) float64 {
	single := m.b.Mode == ModeSingle
	code := p.Code
	sp := 0
	for i := range p.Locals {
		m.locals[i] = 0
	}
	for pc, steps := 0, 0; ; {
		// A verified program runs at most one instruction per code byte (jumps go forward).
		if steps++; steps > len(code) {
			m.faults++
			return 0
		}
		if m.used++; m.used > MaxRunInstructions {
			m.exhausted = true
			return 0
		}
		op := code[pc]
		switch op {
		case OpConst:
			m.stack[sp] = m.b.Consts[binary.LittleEndian.Uint16(code[pc+1:])]
			sp++
			pc += 3
		case OpLoad:
			m.stack[sp] = m.locals[code[pc+1]]
			sp++
			pc += 2
		case OpStore:
			sp--
			m.locals[code[pc+1]] = m.stack[sp]
			pc += 2
		case OpStat:
			m.stack[sp] = m.vals[binary.LittleEndian.Uint16(code[pc+1:])]
			sp++
			pc += 3
		case OpTotal:
			m.stack[sp] = m.total
			sp++
			pc++
		case OpLevel:
			m.stack[sp] = m.level
			sp++
			pc++
		case OpPop:
			sp--
			pc++
		case OpAdd, OpSub, OpMul:
			sp--
			m.stack[sp-1] = arith(op, m.stack[sp-1], m.stack[sp], single)
			pc++
		case OpDiv:
			sp--
			if m.stack[sp] == 0 {
				m.faults++
				m.stack[sp-1] = 0
			} else {
				m.stack[sp-1] = arith(op, m.stack[sp-1], m.stack[sp], single)
			}
			pc++
		case OpMod:
			sp--
			if int64(m.stack[sp]) == 0 {
				m.faults++
				m.stack[sp-1] = 0
			} else {
				m.stack[sp-1] = float64(int64(m.stack[sp-1]) % int64(m.stack[sp]))
			}
			pc++
		case OpNeg:
			m.stack[sp-1] = -m.stack[sp-1] // exact in float32 too
			pc++
		case OpNot:
			m.stack[sp-1] = b2f(m.stack[sp-1] == 0)
			pc++
		case OpLt, OpLe, OpGt, OpGe, OpEq, OpNe:
			sp--
			a, b := m.stack[sp-1], m.stack[sp]
			var r bool
			switch op {
			case OpLt:
				r = a < b
			case OpLe:
				r = a <= b
			case OpGt:
				r = a > b
			case OpGe:
				r = a >= b
			case OpEq:
				r = a == b
			default:
				r = a != b
			}
			m.stack[sp-1] = b2f(r)
			pc++
		case OpJmp:
			pc = int(binary.LittleEndian.Uint16(code[pc+1:]))
		case OpJz:
			sp--
			if m.stack[sp] == 0 {
				pc = int(binary.LittleEndian.Uint16(code[pc+1:]))
			} else {
				pc += 3
			}
		case OpJnz:
			sp--
			if m.stack[sp] != 0 {
				pc = int(binary.LittleEndian.Uint16(code[pc+1:]))
			} else {
				pc += 3
			}
		case OpRound, OpFloor, OpCeil:
			m.stack[sp-1] = roundOp(op, m.stack[sp-1])
			pc++
		case OpMin:
			sp--
			m.stack[sp-1] = goMin(m.stack[sp-1], m.stack[sp])
			pc++
		case OpMax:
			sp--
			m.stack[sp-1] = goMax(m.stack[sp-1], m.stack[sp])
			pc++
		case OpSel:
			sp -= 2
			if m.stack[sp-1] != 0 {
				m.stack[sp-1] = m.stack[sp]
			} else {
				m.stack[sp-1] = m.stack[sp+1]
			}
			pc++
		case OpLerp:
			sp -= 2
			d := arith(OpSub, m.stack[sp], m.stack[sp-1], single)
			p := arith(OpMul, m.stack[sp+1], d, single)
			m.stack[sp-1] = arith(OpAdd, m.stack[sp-1], p, single)
			pc++
		case OpAbs:
			m.stack[sp-1] = math.Abs(m.stack[sp-1])
			pc++
		case OpCurve:
			v, fault := evalCurve(&m.b.Curves[binary.LittleEndian.Uint16(code[pc+1:])], m.stack[sp-1])
			if fault {
				m.faults++
			}
			if single {
				v = rnd(v)
			}
			m.stack[sp-1] = v
			pc += 3
		case OpEffect:
			m.stack[sp] = m.effectCount(m.refOf(binary.LittleEndian.Uint16(code[pc+1:])))
			sp++
			pc += 3
		case OpEffectTag:
			m.stack[sp] = m.effectCountByTag(m.b.Strings[binary.LittleEndian.Uint16(code[pc+1:])])
			sp++
			pc += 3
		case OpSkill:
			m.stack[sp] = b2f(m.skillPurchased(m.refOf(binary.LittleEndian.Uint16(code[pc+1:])), m.refOf(binary.LittleEndian.Uint16(code[pc+3:]))))
			sp++
			pc += 5
		case OpCombat:
			m.stack[sp] = b2f(m.state.InCombat())
			sp++
			pc++
		case OpMoving:
			m.stack[sp] = b2f(m.state.Moving())
			sp++
			pc++
		case OpUnkillable:
			m.stack[sp] = b2f(m.state.Unkillable())
			sp++
			pc++
		case OpEquipTag:
			m.stack[sp] = b2f(m.equipTag(m.b.Strings[binary.LittleEndian.Uint16(code[pc+1:])], m.b.Strings[binary.LittleEndian.Uint16(code[pc+3:])]))
			sp++
			pc += 5
		case OpProfession:
			m.stack[sp] = m.state.ProfessionLevel(m.state.ActorGuid(), m.b.Strings[binary.LittleEndian.Uint16(code[pc+1:])])
			sp++
			pc += 3
		case OpConsumed:
			m.stack[sp] = m.state.ConsumedItemStat(m.refOf(binary.LittleEndian.Uint16(code[pc+1:])))
			sp++
			pc += 3
		case OpNpcRank:
			m.stack[sp] = m.state.NpcEliteRank()
			sp++
			pc++
		case OpNpcDist:
			m.stack[sp] = m.state.NpcStatDistribution(int(binary.LittleEndian.Uint16(code[pc+1:])))
			sp++
			pc += 3
		case OpActor:
			m.useActor(int(code[pc+1]))
			pc += 2
		case OpProgress:
			m.stack[sp] = m.host.AbilityProgress(m.b.Strings[binary.LittleEndian.Uint16(code[pc+1:])])
			sp++
			pc += 3
		case OpEffectInst:
			m.stack[sp] = m.host.EffectCountByInstigator(m.refOf(binary.LittleEndian.Uint16(code[pc+1:])))
			sp++
			pc += 3
		case OpRand:
			m.stack[sp] = m.host.RandDouble()
			sp++
			pc++
		case OpHitTag:
			m.stack[sp] = b2f(m.host.HitHasTag(m.b.Strings[binary.LittleEndian.Uint16(code[pc+1:])]))
			sp++
			pc += 3
		case OpHitFlag:
			m.stack[sp] = b2f(m.host.HitHasFlag(m.b.Strings[binary.LittleEndian.Uint16(code[pc+1:])]))
			sp++
			pc += 3
		case OpXpDebt:
			m.stack[sp] = m.host.XPDebtRatio()
			sp++
			pc++
		case OpHitTargets:
			m.stack[sp] = m.host.HitTargets()
			sp++
			pc++
		case OpHost0, OpHost1, OpHost2:
			n := int(op - OpHost0)
			fn := int(binary.LittleEndian.Uint16(code[pc+1:]))
			name := m.b.Strings[binary.LittleEndian.Uint16(code[pc+3:])]
			var args [2]float64
			for i := n - 1; i >= 0; i-- {
				sp--
				args[i] = m.stack[sp]
			}
			m.stack[sp] = m.host.HostFn(fn, name, args[:n])
			sp++
			pc += 5
		case OpRet:
			if m.b.Mode != ModeDouble {
				return rnd(m.stack[sp-1])
			}
			return m.stack[sp-1]
		default: // not reachable in a verified program
			m.faults++
			return 0
		}
	}
}

// settle makes a stat's value the number it is stored as (a float32, except in
// ModeDouble); a value that is not finite (the formulas gave NaN or an infinity)
// is 0.
func (m *machine) settle(v float64) float64 {
	if m.b.Mode != ModeDouble {
		v = rnd(v)
	}
	if math.IsNaN(v) || math.IsInf(v, 0) {
		m.faults++
		return 0
	}
	return v
}

// Run computes the stats of a character of a class and level with the given
// equipment, against the state of part 2 (nil: aocexpr.NoState).
func (b *Bundle) Run(in Inputs, st aocexpr.State) (*Result, error) {
	if st == nil {
		st = aocexpr.NoState{}
	}
	if in.Level < 1 || in.Level > MaxLevel {
		return nil, fmt.Errorf("statprog: level %d is not 1 to %d", in.Level, MaxLevel)
	}
	cls, ok := b.ClassByEnum(in.Class)
	if !ok {
		return nil, fmt.Errorf("statprog: no class %d in the bundle", in.Class)
	}
	if err := checkEffects(in.Effects); err != nil {
		return nil, err
	}
	ns := len(b.Stats)
	chains := make([][]uint32, ns)
	npc := int(cls.Enum) >= NpcClassBase
	if !npc {
		for _, s := range b.CommonSteps {
			chains[s.Stat] = append(chains[s.Stat], s.Prog)
		}
	}
	for _, s := range cls.Steps {
		chains[s.Stat] = append(chains[s.Stat], s.Prog)
	}
	res := &Result{}
	mods := make([]float64, ns)
	for _, md := range in.Mods {
		i, ok := b.guidIndex(md.Stat)
		if !ok {
			res.IgnoredMods++
			continue
		}
		mods[i] += md.Value
	}
	// The effects' mods by stat, in the order given.
	fx := make([][]effectApp, ns)
	for _, e := range in.Effects {
		if e.Stacks == 0 {
			continue
		}
		for _, md := range e.Mods {
			i, ok := b.guidIndex(md.Stat)
			if !ok {
				res.IgnoredEffectMods++
				continue
			}
			fx[i] = append(fx[i], effectApp{md.Value, e.Stacks})
		}
	}
	member := make([]bool, ns)
	if !npc {
		for _, s := range b.CommonMembers {
			member[s] = true
		}
	}
	for _, s := range cls.Members {
		member[s] = true
	}

	m := &machine{b: b, vals: make([]float64, ns), level: float64(in.Level), state: st, in: &in}
	initV := make([]float64, ns)
	baseV := make([]float64, ns)
	for _, node := range b.Order {
		s := int(node >> 1)
		stat := &b.Stats[s]
		mul := stat.Flags&StatMulMods != 0
		if node&1 == NodeInit {
			v := 0.0
			if mul {
				v = 1
			}
			for _, p := range chains[s] {
				if stat.Base >= 0 && b.readsTotal(int(p)) {
					// The entry sees the stat's value so far, Base included, and its result is the stat's new value (plan E1, stage 3: the
					// captured NPCs' MaxHealth, MaxMana and MoveSpeedMult are the elite scalar's `GetStatInitRunningTotal() * f` over
					// initial + Base, 279 of 279; a player's `GetStatInitRunningTotal() + v` is the same either way). The initial value
					// stored is what the value pass adds the Base to again.
					base := m.run(&b.Progs[stat.Base])
					if mul {
						m.total = arith(OpMul, base, v, b.Mode == ModeSingle)
						r := m.run(&b.Progs[p])
						if base == 0 {
							m.faults++
							v = 0
						} else {
							v = arith(OpDiv, r, base, b.Mode == ModeSingle)
						}
					} else {
						m.total = arith(OpAdd, v, base, b.Mode == ModeSingle)
						v = arith(OpSub, m.run(&b.Progs[p]), base, b.Mode == ModeSingle)
					}
					continue
				}
				m.total = v
				v = m.run(&b.Progs[p])
			}
			m.total = 0
			initV[s] = m.settle(v)
			continue
		}
		v := initV[s]
		if stat.Base >= 0 {
			x := m.run(&b.Progs[stat.Base])
			if mul {
				v = arith(OpMul, x, v, b.Mode == ModeSingle)
			} else {
				v = arith(OpAdd, v, x, b.Mode == ModeSingle)
			}
		}
		baseV[s] = m.settle(v)
		v = arith(OpAdd, baseV[s], mods[s], b.Mode == ModeSingle)
		v = applyEffects(v, stat.Flags, fx[s], b.Mode == ModeSingle)
		lo, hi := math.NaN(), math.NaN()
		if stat.Min >= 0 {
			lo = m.run(&b.Progs[stat.Min])
			if lo > v {
				v = lo
			}
		}
		if stat.Max >= 0 {
			hi = m.run(&b.Progs[stat.Max])
			if hi < v {
				v = hi
			}
		}
		if stat.Flags&StatSpawnMax != 0 && stat.Max >= 0 {
			v = hi
		}
		if stat.Flags&StatSpawnMin != 0 && stat.Min >= 0 {
			v = lo
		}
		m.vals[s] = m.settle(v)
	}
	for s := 0; s < ns; s++ {
		if !member[s] {
			continue
		}
		o := StatOut{
			Stat: s, Guid: b.Stats[s].Guid, Replication: b.Stats[s].Replication,
			Value: m.vals[s], Base: baseV[s],
			// a sum that is not finite, or beyond a float32, is a fault: a clamp can hide it from the value
			Equip: m.settle(mods[s]),
		}
		if b.Stats[s].Regen >= 0 {
			o.Regen = m.settle(m.run(&b.Progs[b.Stats[s].Regen]))
			o.HasRegen = true
		}
		res.Stats = append(res.Stats, o)
	}
	if m.exhausted {
		return nil, fmt.Errorf("statprog: the run needs more than %d instructions", MaxRunInstructions)
	}
	res.Faults = m.faults
	res.Instructions = m.used
	res.Values = m.vals
	return res, nil
}

// [C.O.A ~ C-Y-B]

// applyEffects applies the effect mods of a stat to its value, in the stat's form (effects.go): added times the stacks to an
// additive stat, multiplied once per stack into a multiplying one, by the mod or by one minus it (a complement).
func applyEffects(v float64, flags uint8, apps []effectApp, single bool) float64 {
	for _, a := range apps {
		switch {
		case flags&StatMulMods == 0:
			v = arith(OpAdd, v, arith(OpMul, a.value, float64(a.stacks), single), single)
		default:
			f := a.value
			if flags&StatMulCompl != 0 {
				f = arith(OpSub, 1, a.value, single)
			}
			for k := 0; k < a.stacks; k++ {
				v = arith(OpMul, v, f, single)
			}
		}
	}
	return v
}

// Evaluator runs the expression programs of a bundle (Bundle.Exprs) against the stats of a finished run: an ability's costs,
// cooldowns, cast times and requirements, and the values of an effect's stat mods. It reads the run's Values, the character's
// level, effects and ranks, and the host's inputs. The programs of the stat graph are not its to run.
type Evaluator struct {
	m *machine
}

// NewEvaluator prepares the evaluation of expressions for the character of the run res (the result of b.Run(in, st)).
func (b *Bundle) NewEvaluator(res *Result, in Inputs, st aocexpr.State) (*Evaluator, error) {
	if res == nil || len(res.Values) != len(b.Stats) {
		return nil, fmt.Errorf("statprog: the run has no stat values for this bundle")
	}
	if st == nil {
		st = aocexpr.NoState{}
	}
	if err := checkEffects(in.Effects); err != nil {
		return nil, err
	}
	if in.Level < 1 || in.Level > MaxLevel {
		return nil, fmt.Errorf("statprog: level %d is not 1 to %d", in.Level, MaxLevel)
	}
	m := &machine{b: b, vals: res.Values, level: float64(in.Level), state: st, in: &in}
	m.caster = actorCtx{m.vals, m.level, m.state, m.in}
	return &Evaluator{m: m}, nil
}

// SetActor supplies the context of another actor of the hit the expressions are run for (aocexpr.RoleTarget ...): the result of its own run, its inputs
// and its state. A role never supplied reads as the caster, which is what a hit on the caster itself is.
func (e *Evaluator) SetActor(role int, res *Result, in Inputs, st aocexpr.State) error {
	if role < 1 || role >= MaxRoles {
		return fmt.Errorf("statprog: no actor role %d", role)
	}
	if res == nil || len(res.Values) != len(e.m.b.Stats) {
		return fmt.Errorf("statprog: the run has no stat values for this bundle")
	}
	if st == nil {
		st = aocexpr.NoState{}
	}
	if err := checkEffects(in.Effects); err != nil {
		return err
	}
	if in.Level < 1 || in.Level > MaxLevel {
		return fmt.Errorf("statprog: level %d is not 1 to %d", in.Level, MaxLevel)
	}
	e.m.actors[role] = &actorCtx{res.Values, float64(in.Level), st, &in}
	return nil
}

// ClearActors forgets the other actors: every role reads as the caster again.
func (e *Evaluator) ClearActors() { e.m.actors = [MaxRoles]*actorCtx{} }

// IsExpr says the program is an expression program of the bundle.
func (b *Bundle) IsExpr(prog int) bool {
	i := sort.Search(len(b.Exprs), func(i int) bool { return int(b.Exprs[i]) >= prog })
	return prog >= 0 && i < len(b.Exprs) && int(b.Exprs[i]) == prog
}

// Eval runs the expression program prog (an index in Bundle.Progs listed in Bundle.Exprs) with the host (nil is NoHost) and
// returns its value; a boolean is 1 or 0. Faults (a division by zero, a curve read at NaN) count in Faults and give 0.
func (e *Evaluator) Eval(prog int, host ExprHost) (float64, error) {
	if !e.m.b.IsExpr(prog) {
		return 0, fmt.Errorf("statprog: program %d is not an expression program", prog)
	}
	if host == nil {
		host = NoHost{}
	}
	e.m.host = host
	e.m.used = 0
	e.m.exhausted = false
	e.m.useActor(0)
	v := e.m.run(&e.m.b.Progs[prog])
	if e.m.exhausted {
		return 0, fmt.Errorf("statprog: the expression needs more than %d instructions", MaxRunInstructions)
	}
	return v, nil
}

// Faults is how many faults the evaluations so far counted.
func (e *Evaluator) Faults() int { return e.m.faults }

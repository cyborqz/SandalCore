// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"math"
	"sync"
)

// The stat program format, version 3 (the C++ reader in server/worldd/stats
// implements the same; README.md documents both). Version 2 (plan D2) adds the opcodes Lerp, Abs and the three
// that read the host (ability progress, effects by instigator, a random number), the stat flag for the
// complement multiplication, and the table of expression programs: the programs a host runs for an ability's
// costs, cooldowns and effect values, as against those the stat graph runs. Version 3 (plan E1, stage 3) adds the
// opcode that reads an NPC's stat distribution, the tangents of a curve's keys (a cubic segment is evaluated, as
// Unreal's rich curve does: the NPCs' power curves) and the NPC profiles: a class whose enum is NpcClassBase or more is
// the whole initializer chain of a kind of NPC (CombatStats, NPC_Base, an elite scalar), and the common lists do not
// apply to it.
//
// A Bundle is everything the stat engine needs of the stat formula graph: the
// stats, their programs, the curves they read and the order to evaluate them
// in. It is plain data: no pointers, so it serializes to one block of bytes
// (Encode) and a reader checks all of it before running any of it (Decode).

// Format constants and limits. A reader refuses a bundle past any of them.
const (
	Magic   = "AOSP"
	Version = 3

	// NpcClassBase is the first class enum of an NPC profile (ECharacterClass has eight values: 1 to 8). A profile's steps and members hold the
	// whole chain of initializer lists of that kind of NPC; the bundle's common steps and members belong to the player classes only.
	NpcClassBase = 32

	// Header flags: the arithmetic mode (Mode); both set is an error.
	FlagSingle = 1 // ModeSingle
	FlagDouble = 2 // ModeDouble

	headerSize = 16

	MaxBundleBytes = 4 << 20
	MaxConsts      = 65535
	MaxStrings     = 65535
	MaxStringLen   = 255
	MaxRefs        = 65535
	MaxCurves      = 4096
	MaxCurveKeys   = 4096
	MaxPrograms    = 65535
	MaxCodeBytes   = 8192 // also the most instructions a program can execute: jumps only go forward
	MaxLocals      = 64
	MaxStack       = 64
	MaxStats       = 4096
	MaxSteps       = 65535
	MaxClasses     = 16
	MaxRoles       = 7 // aocexpr.RoleCount: the actors of a hit an expression can read
	MaxLevel       = 255
	MaxExprs       = MaxPrograms
	// MaxEffectStacks is the most stacks one active effect may have, and MaxActiveEffects the most the inputs of a
	// run may hold; a run refuses more (the engine multiplies once per stack).
	MaxEffectStacks  = 4096
	MaxActiveEffects = 4096

	// MaxRunInstructions is the most instructions one Run may execute in all: a bundle can be
	// valid and still ask for a great deal (65,535 initializer steps of an 8 KiB program each);
	// the run stops with an error instead. A run of the real graph executes about 3,000.
	MaxRunInstructions = 1 << 22
)

// C-Y-B captures, CoA code

// Opcodes. A program is a sequence of instructions on a stack of float64
// (a boolean is 1.0 or 0.0), ending in OpRet. Operands follow the opcode,
// little-endian: u8 or u16. Jump targets are absolute offsets into the code and
// must be forward.
const (
	OpConst byte = 0x01 // u16 constant: push consts[i]
	OpLoad  byte = 0x02 // u8 local: push locals[i]
	OpStore byte = 0x03 // u8 local: pop into locals[i]
	OpStat  byte = 0x04 // u16 stat: push the stat's value
	OpTotal byte = 0x05 // push GetStatInitRunningTotal (0 outside an initializer)
	OpLevel byte = 0x06 // push the character level
	OpPop   byte = 0x07 // drop the top

	OpAdd byte = 0x10 // b a -> a+b (a is the top)
	OpSub byte = 0x11
	OpMul byte = 0x12
	OpDiv byte = 0x13 // a division by zero pushes 0 and counts a fault
	OpNeg byte = 0x14
	OpNot byte = 0x15 // 1 if the top is 0, else 0

	OpLt byte = 0x20 // push 1 or 0
	OpLe byte = 0x21
	OpGt byte = 0x22
	OpGe byte = 0x23
	OpEq byte = 0x24
	OpNe byte = 0x25

	OpJmp byte = 0x30 // u16 target
	OpJz  byte = 0x31 // u16 target: pop; jump if it is 0
	OpJnz byte = 0x32 // u16 target: pop; jump if it is not 0

	OpRound byte = 0x40 // floor(x + 0.5)
	OpFloor byte = 0x41
	OpCeil  byte = 0x42
	OpMin   byte = 0x43
	OpMax   byte = 0x44
	OpSel   byte = 0x45 // c a b -> (c != 0 ? a : b)
	OpCurve byte = 0x46 // u16 curve: pop x, push the curve at x
	OpLerp  byte = 0x47 // a b t -> a + t * (b - a), the subtraction, the product and the sum each rounded as a stat formula's are
	OpAbs   byte = 0x48 // |x|
	OpMod   byte = 0x49 // b a -> a % b: the remainder of the two numbers truncated to whole ones (a combo count), 0 and a fault when b is 0

	// Context operations: the state of part 2 (aocexpr.State), answered by the
	// engine's state (NoState until effects, consumables and the rest exist).
	OpEffect     byte = 0x50 // u16 ref: push the stacks of the effect
	OpEffectTag  byte = 0x51 // u16 string: push the count of effects with the tag
	OpSkill      byte = 0x52 // u16 ref (class) u16 ref (skill): push 1 if bought
	OpCombat     byte = 0x53 // push IsInCombat
	OpMoving     byte = 0x54 // push IsMoving
	OpUnkillable byte = 0x55 // push IsUnkillable
	OpEquipTag   byte = 0x56 // u16 string (the slot's name) u16 string (the tag): push 1 if the item in the slot has it
	OpProfession byte = 0x57 // u16 string (the profession): push the character's level in it
	OpConsumed   byte = 0x58 // u16 ref (a stat): push the stat on the consumable in use
	OpNpcRank    byte = 0x59 // push GetNpcEliteRank
	// OpActor u8 role: the operations that follow read the actor of the hit with that role (aocexpr.Role...): the stats, effects, skills, level and flags
	// of its own context, which the evaluation's caller supplies (a role it did not supply is the caster: a self hit's target is the caster). An
	// expression program only; the compiler brackets one read with it and OpActor 0.
	OpActor   byte = 0x66
	OpNpcDist byte = 0x65 // u16 index (0 to 7, the order of EStatDistributionStat): push the NPC's share of its stat budget for that stat (0 for a player)

	// Host operations (plan D2): inputs only the host of an ability expression has. A program that uses one is an expression
	// program (Bundle.Exprs) and no formula of the stat graph.
	OpProgress   byte = 0x5A // u16 string (a montage group): push the progress ratio of the running ability of the group (0 to 1)
	OpEffectInst byte = 0x5B // u16 ref: push the stacks of the effect that the instigator applied (the caster's own: it is the target too)
	OpRand       byte = 0x5C // push the host's next random number in [0, 1)
	// The hit in progress (a heal or a shield formula reads it): the host answers.
	OpHitTag     byte = 0x5D // u16 string (a gameplay tag): push 1 if the hit in progress (or its ability) has it
	OpHitFlag    byte = 0x5E // u16 string (an EHitFlag name, "Crit"): push 1 if the hit on the target has the flag
	OpXpDebt     byte = 0x5F // push the instigator's experience debt ratio
	OpHitTargets byte = 0x60 // push how many targets the hit in progress has
	// A call of a named host function (plan D2, stage 2): u16 function (the Host... constants), u16 string (its name operand: an ability or stat guid
	// as decimal text, an EAffectedTarget value, a montage group; "" when it has none). The 0, 1 or 2 numbers it takes are popped; the host answers.
	OpHost0 byte = 0x61
	OpHost1 byte = 0x62
	OpHost2 byte = 0x63
	OpRet   byte = 0xFF // end: the value is the top, and the stack holds only it
)

// isHostOp says an opcode reads the host of an expression (a stat formula may not).
func isHostOp(op byte) bool {
	switch op {
	case OpProgress, OpEffectInst, OpRand, OpHitTag, OpHitFlag, OpXpDebt, OpHitTargets, OpHost0, OpHost1, OpHost2, OpActor:
		return true
	}
	return false
}

// operandBytes is the size of an opcode's operands, or -1 for an unknown opcode.
func operandBytes(op byte) int {
	switch op {
	case OpTotal, OpLevel, OpPop, OpAdd, OpSub, OpMul, OpDiv, OpMod, OpNeg, OpNot, OpLt, OpLe, OpGt, OpGe, OpEq, OpNe,
		OpRound, OpFloor, OpCeil, OpMin, OpMax, OpSel, OpCombat, OpMoving, OpUnkillable, OpNpcRank, OpRet,
		OpLerp, OpAbs, OpRand, OpXpDebt, OpHitTargets:
		return 0
	case OpLoad, OpStore, OpActor:
		return 1
	case OpConst, OpStat, OpJmp, OpJz, OpJnz, OpCurve, OpEffect, OpEffectTag, OpProfession, OpConsumed, OpProgress, OpEffectInst, OpHitTag, OpHitFlag, OpNpcDist:
		return 2
	case OpSkill, OpEquipTag, OpHost0, OpHost1, OpHost2:
		return 4
	}
	return -1
}

// [C-O-A + Cyb]

// stackEffect is how many values an opcode pops and pushes.
func stackEffect(op byte) (pop, push int) {
	switch op {
	case OpConst, OpLoad, OpStat, OpTotal, OpLevel, OpEffect, OpEffectTag, OpSkill, OpCombat, OpMoving, OpUnkillable,
		OpEquipTag, OpProfession, OpConsumed, OpNpcRank, OpNpcDist, OpProgress, OpEffectInst, OpRand, OpHitTag, OpHitFlag, OpXpDebt, OpHitTargets, OpHost0:
		return 0, 1
	case OpHost1:
		return 1, 1
	case OpHost2:
		return 2, 1
	case OpStore, OpPop, OpJz, OpJnz:
		return 1, 0
	case OpActor:
		return 0, 0
	case OpAdd, OpSub, OpMul, OpDiv, OpMod, OpLt, OpLe, OpGt, OpGe, OpEq, OpNe, OpMin, OpMax:
		return 2, 1
	case OpNeg, OpNot, OpRound, OpFloor, OpCeil, OpCurve, OpAbs:
		return 1, 1
	case OpSel, OpLerp:
		return 3, 1
	}
	return 0, 0 // OpJmp, OpRet (the verifier handles it)
}

// Stat flags.
const (
	StatMulMods    = 1 << 0 // bMultiplyStatMods: the base expression multiplies the initial value, effects multiply
	StatClampFinal = 1 << 1 // bClampStatFinalValue (read, not used: the capture clamps every stat)
	StatSpawnMax   = 1 << 2 // OnSpawn = Reset_SetMax: the stat starts at its maximum
	StatSpawnMin   = 1 << 3 // OnSpawn = Reset_SetMin: the stat starts at its minimum
	StatBuckets    = 1 << 4 // bReplicatesBaseAndEquipmentBuckets: the replicated item carries base and equipment
	StatFloatArray = 1 << 5 // StatType is Type_FloatArray (TempHealth): replicated in the float arrays, not the Int32 ones
	// StatMulCompl is bMultiplyCompliment (plan D2), meaningful with StatMulMods: the stat's effect mods multiply it by
	// (1 - value) instead of by value (the mitigations: Mitigation 0.9652 under an effect of 0.03 is 0.9652 * 0.97).
	StatMulCompl     = 1 << 6
	statFlagsAllowed = StatMulMods | StatClampFinal | StatSpawnMax | StatSpawnMin | StatBuckets | StatFloatArray | StatMulCompl
)

// Node kinds of the evaluation order.
const (
	NodeInit  = 0 // the stat's initial value: its initializer entries, in list order
	NodeValue = 1 // the stat's value: base, equipment, limits
)

// Interp of a curve key.
const (
	InterpLinear   = 0
	InterpConstant = 1
	// InterpCubic is a segment the design data gives a cubic spline: a Bezier through the control points the first key's LeaveTangent and the
	// next key's ArriveTangent make (aocexpr.CubicSegment; Unreal's rich curve). No player stat reads one: the NPCs' power curves have them.
	InterpCubic = 2
)

// Ref is a record reference an operation names (an effect, a skill, a class,
// a stat).
type Ref struct {
	TypeID uint64
	Guid   int64
}

// CurveKey is a point of a curve; Arrive and Leave are the tangents of a cubic segment (zero for the others).
type CurveKey struct {
	Time, Value   float64
	Interp        uint8
	Arrive, Leave float64
}

// Curve is a StatCurve: linear or constant keys, in increasing time.
type Curve struct{ Keys []CurveKey }

// Program is the code of one formula.
type Program struct {
	Locals uint16
	Code   []byte
}

// Stat is one stat of the graph.
type Stat struct {
	Guid        int64
	Name        uint32 // index into Strings
	Replication uint8  // 0 none, 1 Owner, 5 OwnerProxy, 7 EveryoneProxy, 6 ProxyOnly
	Flags       uint8
	// Base, Min, Max, Regen are program indexes, -1 when the stat has no such formula.
	Base, Min, Max, Regen int32
}

// Step is one initializer entry: a program that gives Stat its next value.
type Step struct {
	Stat uint32
	Prog uint32
}

// Class is one player class: its own initializer list and the stats it has.
type Class struct {
	Enum    uint8 // ECharacterClass
	Name    uint32
	Steps   []Step   // the class's list, in order, after the common lists
	Members []uint32 // the stats of the class's own list
}

// [C_Y_B - C O A]

// Mode is the arithmetic of a bundle.
type Mode uint8

const (
	// ModeFloat32 (the default): every value the engine stores or returns is a
	// float32 (a formula's result, a stat's initial value, a stat's value), and
	// the arithmetic between them is float64. This is what reproduces the captured
	// stats of the level-1 Bard to the bit (442 of 442; ModeDouble gets 440 and ModeSingle
	// 440: the engine's formulas return floats, and its stats are stored as floats).
	ModeFloat32 Mode = iota
	// ModeDouble keeps every value a float64.
	ModeDouble
	// ModeSingle also rounds every arithmetic result, constant and curve value to
	// float32, as aocexpr's Env.Single does.
	ModeSingle
)

func (m Mode) String() string {
	switch m {
	case ModeFloat32:
		return "float32"
	case ModeDouble:
		return "double"
	case ModeSingle:
		return "single"
	}
	return fmt.Sprintf("Mode(%d)", uint8(m))
}

// Bundle is the compiled stat graph.
type Bundle struct {
	Mode    Mode
	Consts  []float64
	Strings []string
	Refs    []Ref
	Curves  []Curve
	Progs   []Program
	Stats   []Stat

	// CommonSteps are the entries of the lists every class starts from
	// (CombatStats, Base_Artisan_Stats, Player_Base), in order; CommonMembers the
	// stats they name.
	CommonSteps   []Step
	CommonMembers []uint32
	Classes       []Class

	// Order is the nodes in an order that never reads a stat before it is
	// settled: stat<<1 | kind. Every stat has both nodes.
	Order []uint32

	// Exprs are the programs a host runs for an ability's expressions (plan D2), by index in Progs, strictly
	// increasing. They read the stats of a finished run (OpStat) and may use the host operations.
	Exprs []uint32

	ix struct { // the stat guid index, built on first use
		once   sync.Once
		byGuid map[int64]int
	}
	tot struct { // which programs read the running total (OpTotal), built on first use
		once  sync.Once
		reads []bool
	}
}

// readsTotal says program p reads the running total (OpTotal): an initializer entry that sees the stat's value so far (engine.go, Run).
// It is a static property of the code, the same the verifier records (progInfo.total), so a read behind a branch counts.
func (b *Bundle) readsTotal(p int) bool {
	b.tot.once.Do(func() {
		b.tot.reads = make([]bool, len(b.Progs))
		for i := range b.Progs {
			code := b.Progs[i].Code
			for pc := 0; pc < len(code); {
				op := code[pc]
				sz := operandBytes(op)
				if sz < 0 || pc+1+sz > len(code) {
					break // not a verified program: Run never gets here
				}
				if op == OpTotal {
					b.tot.reads[i] = true
					break
				}
				pc += 1 + sz
			}
		}
	})
	return p >= 0 && p < len(b.tot.reads) && b.tot.reads[p]
}

// ---- encoding ----
// C.O.A : Cyb

type writer struct{ b []byte }

func (w *writer) u8(v uint8)    { w.b = append(w.b, v) }
func (w *writer) u16(v uint16)  { w.b = binary.LittleEndian.AppendUint16(w.b, v) }
func (w *writer) u32(v uint32)  { w.b = binary.LittleEndian.AppendUint32(w.b, v) }
func (w *writer) u64(v uint64)  { w.b = binary.LittleEndian.AppendUint64(w.b, v) }
func (w *writer) f64(v float64) { w.u64(math.Float64bits(v)) }
func (w *writer) bytes(v []byte) {
	w.b = append(w.b, v...)
}

// Encode serializes the bundle. It does not check it: Decode does.
func (b *Bundle) Encode() []byte {
	var w writer
	w.u32(uint32(len(b.Consts)))
	for _, c := range b.Consts {
		w.f64(c)
	}
	w.u32(uint32(len(b.Strings)))
	for _, s := range b.Strings {
		w.u8(uint8(len(s)))
		w.bytes([]byte(s))
	}
	w.u32(uint32(len(b.Refs)))
	for _, r := range b.Refs {
		w.u64(r.TypeID)
		w.u64(uint64(r.Guid))
	}
	w.u32(uint32(len(b.Curves)))
	for _, c := range b.Curves {
		w.u32(uint32(len(c.Keys)))
		for _, k := range c.Keys {
			w.f64(k.Time)
			w.f64(k.Value)
			w.u8(k.Interp)
			w.f64(k.Arrive)
			w.f64(k.Leave)
		}
	}
	w.u32(uint32(len(b.Progs)))
	for _, p := range b.Progs {
		w.u16(p.Locals)
		w.u32(uint32(len(p.Code)))
		w.bytes(p.Code)
	}
	w.u32(uint32(len(b.Stats)))
	for _, s := range b.Stats {
		w.u64(uint64(s.Guid))
		w.u32(s.Name)
		w.u8(s.Replication)
		w.u8(s.Flags)
		w.u32(uint32(s.Base))
		w.u32(uint32(s.Min))
		w.u32(uint32(s.Max))
		w.u32(uint32(s.Regen))
	}
	putSteps := func(steps []Step) {
		w.u32(uint32(len(steps)))
		for _, s := range steps {
			w.u32(s.Stat)
			w.u32(s.Prog)
		}
	}
	putU32s := func(v []uint32) {
		w.u32(uint32(len(v)))
		for _, x := range v {
			w.u32(x)
		}
	}
	putSteps(b.CommonSteps)
	putU32s(b.CommonMembers)
	w.u32(uint32(len(b.Classes)))
	for _, c := range b.Classes {
		w.u8(c.Enum)
		w.u32(c.Name)
		putSteps(c.Steps)
		putU32s(c.Members)
	}
	putU32s(b.Order)
	putU32s(b.Exprs)

	var flags uint16
	switch b.Mode {
	case ModeDouble:
		flags = FlagDouble
	case ModeSingle:
		flags = FlagSingle
	}
	out := make([]byte, 0, headerSize+len(w.b))
	out = append(out, Magic...)
	out = binary.LittleEndian.AppendUint16(out, Version)
	out = binary.LittleEndian.AppendUint16(out, flags)
	out = binary.LittleEndian.AppendUint32(out, uint32(len(w.b)))
	out = binary.LittleEndian.AppendUint32(out, crc32.ChecksumIEEE(w.b))
	return append(out, w.b...)
}

// ~ C_Y_B :: C O A ~

// ---- decoding ----

// ErrFormat is wrapped by every Decode error.
var ErrFormat = errors.New("statprog: bad stat program bundle")

func bad(format string, args ...any) error {
	return fmt.Errorf("%w: %s", ErrFormat, fmt.Sprintf(format, args...))
}

type reader struct {
	b   []byte
	off int
	err error
}

func (r *reader) need(n int) bool {
	if r.err != nil {
		return false
	}
	if n < 0 || len(r.b)-r.off < n {
		r.err = bad("truncated at byte %d", headerSize+r.off)
		return false
	}
	return true
}
func (r *reader) u8() uint8 {
	if !r.need(1) {
		return 0
	}
	v := r.b[r.off]
	r.off++
	return v
}
func (r *reader) u16() uint16 {
	if !r.need(2) {
		return 0
	}
	v := binary.LittleEndian.Uint16(r.b[r.off:])
	r.off += 2
	return v
}
func (r *reader) u32() uint32 {
	if !r.need(4) {
		return 0
	}
	v := binary.LittleEndian.Uint32(r.b[r.off:])
	r.off += 4
	return v
}
func (r *reader) u64() uint64 {
	if !r.need(8) {
		return 0
	}
	v := binary.LittleEndian.Uint64(r.b[r.off:])
	r.off += 8
	return v
}
func (r *reader) f64() float64 { return math.Float64frombits(r.u64()) }

// count reads a u32 count that must not exceed max and whose items, each at
// least itemSize bytes, must fit in what is left.
func (r *reader) count(what string, max, itemSize int) int {
	n := r.u32()
	if r.err != nil {
		return 0
	}
	if int64(n) > int64(max) {
		r.err = bad("%s: %d is more than the limit %d", what, n, max)
		return 0
	}
	if itemSize > 0 && int64(n)*int64(itemSize) > int64(len(r.b)-r.off) {
		r.err = bad("%s: %d items do not fit in the %d bytes left", what, n, len(r.b)-r.off)
		return 0
	}
	return int(n)
}

// ~ C O A - C_Y_B ~

// Decode reads and verifies a bundle. A bundle that passes cannot make the
// interpreter read outside its tables, overflow its stack, loop, or read a stat
// before it is settled.
func Decode(data []byte) (*Bundle, error) {
	if len(data) < headerSize {
		return nil, bad("%d bytes is shorter than the %d-byte header", len(data), headerSize)
	}
	if len(data) > MaxBundleBytes {
		return nil, bad("%d bytes is more than the limit %d", len(data), MaxBundleBytes)
	}
	if string(data[:4]) != Magic {
		return nil, bad("magic %q", data[:4])
	}
	if v := binary.LittleEndian.Uint16(data[4:]); v != Version {
		return nil, bad("version %d (this reader reads %d)", v, Version)
	}
	flags := binary.LittleEndian.Uint16(data[6:])
	if flags&^uint16(FlagSingle|FlagDouble) != 0 || flags == FlagSingle|FlagDouble {
		return nil, bad("flags %#x", flags)
	}
	if n := binary.LittleEndian.Uint32(data[8:]); int64(n) != int64(len(data)-headerSize) {
		return nil, bad("body length %d, %d bytes follow the header", n, len(data)-headerSize)
	}
	body := data[headerSize:]
	if crc := binary.LittleEndian.Uint32(data[12:]); crc != crc32.ChecksumIEEE(body) {
		return nil, bad("crc32 %08x, the body has %08x", crc, crc32.ChecksumIEEE(body))
	}

	r := &reader{b: body}
	b := &Bundle{}
	switch flags {
	case FlagDouble:
		b.Mode = ModeDouble
	case FlagSingle:
		b.Mode = ModeSingle
	}
	n := r.count("constants", MaxConsts, 8)
	b.Consts = make([]float64, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		c := r.f64()
		if math.IsNaN(c) || math.IsInf(c, 0) {
			r.err = bad("constant %d is not finite", i)
		}
		b.Consts = append(b.Consts, c)
	}
	n = r.count("strings", MaxStrings, 1)
	b.Strings = make([]string, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		l := int(r.u8())
		if r.need(l) {
			b.Strings = append(b.Strings, string(r.b[r.off:r.off+l]))
			r.off += l
		}
	}
	n = r.count("references", MaxRefs, 16)
	b.Refs = make([]Ref, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		t := r.u64()
		g := int64(r.u64())
		b.Refs = append(b.Refs, Ref{t, g})
	}
	n = r.count("curves", MaxCurves, 4)
	b.Curves = make([]Curve, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		nk := r.count("curve keys", MaxCurveKeys, 33)
		if r.err == nil && nk == 0 {
			r.err = bad("curve %d has no keys", i)
		}
		c := Curve{Keys: make([]CurveKey, 0, nk)}
		for k := 0; k < nk && r.err == nil; k++ {
			key := CurveKey{Time: r.f64(), Value: r.f64(), Interp: r.u8(), Arrive: r.f64(), Leave: r.f64()}
			switch {
			case math.IsNaN(key.Time) || math.IsInf(key.Time, 0) || math.IsNaN(key.Value) || math.IsInf(key.Value, 0) || math.IsNaN(key.Arrive) || math.IsInf(key.Arrive, 0) ||
				math.IsNaN(key.Leave) || math.IsInf(key.Leave, 0):
				r.err = bad("curve %d key %d is not finite", i, k)
			case key.Interp > InterpCubic:
				r.err = bad("curve %d key %d has interpolation %d", i, k, key.Interp)
			case k > 0 && !(key.Time > c.Keys[k-1].Time):
				r.err = bad("curve %d: key %d is not after key %d", i, k, k-1)
			}
			c.Keys = append(c.Keys, key)
		}
		b.Curves = append(b.Curves, c)
	}
	n = r.count("programs", MaxPrograms, 6)
	b.Progs = make([]Program, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		p := Program{Locals: r.u16()}
		cl := int(r.u32())
		if r.err == nil && (cl < 1 || cl > MaxCodeBytes) {
			r.err = bad("program %d: code length %d (1 to %d)", i, cl, MaxCodeBytes)
		}
		if r.err == nil && r.need(cl) {
			p.Code = append([]byte(nil), r.b[r.off:r.off+cl]...)
			r.off += cl
		}
		b.Progs = append(b.Progs, p)
	}
	n = r.count("stats", MaxStats, 30)
	b.Stats = make([]Stat, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		s := Stat{Guid: int64(r.u64()), Name: r.u32(), Replication: r.u8(), Flags: r.u8()}
		s.Base, s.Min, s.Max, s.Regen = int32(r.u32()), int32(r.u32()), int32(r.u32()), int32(r.u32())
		b.Stats = append(b.Stats, s)
	}
	getSteps := func(what string) []Step {
		n := r.count(what, MaxSteps, 8)
		out := make([]Step, 0, n)
		for i := 0; i < n && r.err == nil; i++ {
			out = append(out, Step{Stat: r.u32(), Prog: r.u32()})
		}
		return out
	}
	getU32s := func(what string, max int) []uint32 {
		n := r.count(what, max, 4)
		out := make([]uint32, 0, n)
		for i := 0; i < n && r.err == nil; i++ {
			out = append(out, r.u32())
		}
		return out
	}
	b.CommonSteps = getSteps("common steps")
	b.CommonMembers = getU32s("common members", MaxStats)
	n = r.count("classes", MaxClasses, 13)
	b.Classes = make([]Class, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		c := Class{Enum: r.u8(), Name: r.u32()}
		c.Steps = getSteps("class steps")
		c.Members = getU32s("class members", MaxStats)
		b.Classes = append(b.Classes, c)
	}
	b.Order = getU32s("order", 2*MaxStats)
	b.Exprs = getU32s("expression programs", MaxExprs)
	if r.err != nil {
		return nil, r.err
	}
	if r.off != len(r.b) {
		return nil, bad("%d bytes after the last table", len(r.b)-r.off)
	}
	if err := b.verify(); err != nil {
		return nil, err
	}
	return b, nil
}

// C | O | A.C-Y-B

// The host functions of OpHost0, OpHost1 and OpHost2 (plan D2, stage 2). The host answers with a number (a boolean is 1 or 0). In a world without other
// actors the caster is every target and every instigator, and the defaults of NoHost are: no charges, the caster is applicable only as Self, no key
// pressed, no change of any stat over time, no hit event, no enemy and nobody neutral, no angle, the host's clock, a duration of 0.
const (
	HostRemainingCharges = 1  // GetRemainingAbilityCharges(ability): name = the ability's guid in decimal
	HostApplicableTarget = 2  // IsApplicableTarget(target, EAffectedTarget::X): name = "TARGET_Self" ...
	HostMovementKey      = 3  // IsAnyMovementKeyPressed()
	HostStatDelta        = 4  // GetStatDeltaOverTime(actor, seconds, stat): name = the stat's guid, one argument (the seconds)
	HostHitStatMod       = 5  // GetStatModValue(GetHitEvent(), [actor,] stat): name = the stat's guid
	HostHitStatEffective = 6  // GetStatModEffectiveValue(GetHitEvent(), [actor,] stat)
	HostIsNeutral        = 7  // IsNeutral(a, b)
	HostIsEnemy          = 8  // IsEnemy(a, b)
	HostAngleBetween     = 9  // GetAngleBetween(a, b), degrees
	HostNow              = 10 // Now(), seconds
	HostProgressDuration = 11 // GetProgressDuration(group): name = the montage group
)

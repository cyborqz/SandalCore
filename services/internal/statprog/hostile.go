// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"math"
	"sort"
	"strconv"
	"strings"
)

// The hostile bundles: bytes the readers must refuse (Decode here, the C++ reader
// in server/worldd/stats). They are made from the TinyGraph's bundle, small enough
// to ship in the shared corpus, and cover the header, every table, the order and
// the program verifier.

// Hostile is a blob a reader must refuse.
type Hostile struct {
	Name string
	Blob []byte
}

// structuralMutations break a valid bundle one way each (by name): every one must be refused by Decode.
func structuralMutations() map[string]func(b *Bundle) {
	return map[string]func(b *Bundle){
		"no programs reference": func(b *Bundle) { b.Stats[0].Base = int32(len(b.Progs)) },
		"negative program":      func(b *Bundle) { b.Stats[0].Min = -2 },
		"stat name outside":     func(b *Bundle) { b.Stats[0].Name = uint32(len(b.Strings)) },
		"duplicate guid":        func(b *Bundle) { b.Stats[1].Guid = b.Stats[0].Guid },
		"replication 8":         func(b *Bundle) { b.Stats[0].Replication = 8 },
		"unknown stat flag":     func(b *Bundle) { b.Stats[0].Flags |= 0x80 },
		"step stat outside":     func(b *Bundle) { b.CommonSteps[0].Stat = uint32(len(b.Stats)) },
		"step program outside":  func(b *Bundle) { b.CommonSteps[0].Prog = uint32(len(b.Progs)) },
		"member outside":        func(b *Bundle) { b.CommonMembers[0] = uint32(len(b.Stats)) },
		"class member outside":  func(b *Bundle) { b.Classes[0].Members[0] = uint32(len(b.Stats)) },
		"class enum 0":          func(b *Bundle) { b.Classes[0].Enum = 0 },
		"class enum twice":      func(b *Bundle) { b.Classes[1].Enum = b.Classes[0].Enum },
		"class name outside":    func(b *Bundle) { b.Classes[0].Name = uint32(len(b.Strings)) },
		"order too short":       func(b *Bundle) { b.Order = b.Order[1:] },
		"order node twice":      func(b *Bundle) { b.Order[1] = b.Order[0] },
		"order stat outside":    func(b *Bundle) { b.Order[0] = uint32(2*len(b.Stats) + 2) },
		"value before init": func(b *Bundle) {
			var i, j int
			for k, n := range b.Order {
				switch n {
				case 2*0 + NodeInit:
					i = k
				case 2*0 + NodeValue:
					j = k
				}
			}
			b.Order[i], b.Order[j] = b.Order[j], b.Order[i]
		},
		"reads before settled": func(b *Bundle) {
			// move a stat's value node to the front: whatever reads it now reads an unsettled stat
			for i, n := range b.Order {
				if n&1 == NodeValue && i > 0 {
					copy(b.Order[1:i+1], b.Order[:i])
					b.Order[0] = n
					return
				}
			}
		},
		"constant NaN":      func(b *Bundle) { b.Consts[0] = math.NaN() },
		"constant infinity": func(b *Bundle) { b.Consts[0] = math.Inf(1) },
		"curve no keys":     func(b *Bundle) { b.Curves[0].Keys = nil },
		"curve time order":  func(b *Bundle) { b.Curves[0].Keys[1].Time = b.Curves[0].Keys[0].Time },
		"curve key NaN":     func(b *Bundle) { b.Curves[0].Keys[0].Value = math.NaN() },
		"curve interp 3":    func(b *Bundle) { b.Curves[0].Keys[0].Interp = 3 },
		"program no code":   func(b *Bundle) { b.Progs[0].Code = nil },
		"program too long":  func(b *Bundle) { b.Progs[0].Code = make([]byte, MaxCodeBytes+1) },
		"too many locals":   func(b *Bundle) { b.Progs[0].Locals = MaxLocals + 1 },
		"unknown opcode":    func(b *Bundle) { b.Progs[0].Code = append([]byte{0x77}, b.Progs[0].Code...) },
		"missing return":    func(b *Bundle) { c := b.Progs[0].Code; b.Progs[0].Code = c[:len(c)-1] },
		"return twice":      func(b *Bundle) { b.Progs[0].Code = append([]byte{OpRet}, b.Progs[0].Code...) },
		"code after return": func(b *Bundle) { b.Progs[0].Code = append(b.Progs[0].Code, OpRet) },
		"cut off operand":   func(b *Bundle) { b.Progs[0].Code = []byte{OpConst, 0} },
		"constant outside":  func(b *Bundle) { b.Progs[0].Code = []byte{OpConst, 0xff, 0xff, OpRet} },
		"stat outside":      func(b *Bundle) { b.Progs[0].Code = []byte{OpStat, 0xff, 0xff, OpRet} },
		"curve outside":     func(b *Bundle) { b.Progs[0].Code = []byte{OpConst, 0, 0, OpCurve, 0xff, 0xff, OpRet} },
		"reference outside": func(b *Bundle) { b.Progs[0].Code = []byte{OpEffect, 0xff, 0xff, OpRet} },
		"string outside":    func(b *Bundle) { b.Progs[0].Code = []byte{OpEffectTag, 0xff, 0xff, OpRet} },
		"local outside":     func(b *Bundle) { b.Progs[0].Locals = 1; b.Progs[0].Code = []byte{OpLoad, 1, OpRet} },
		"store local outside": func(b *Bundle) {
			b.Progs[0].Locals = 0
			b.Progs[0].Code = []byte{OpConst, 0, 0, OpStore, 0, OpConst, 0, 0, OpRet}
		},
		"stack underflow": func(b *Bundle) { b.Progs[0].Code = []byte{OpAdd, OpRet} },
		"empty at return": func(b *Bundle) { b.Progs[0].Code = []byte{OpRet} },
		"two values at return": func(b *Bundle) {
			b.Progs[0].Code = []byte{OpConst, 0, 0, OpConst, 0, 0, OpRet}
		},
		"stack overflow": func(b *Bundle) {
			code := bytes.Repeat([]byte{OpConst, 0, 0}, MaxStack+1)
			for i := 0; i < MaxStack; i++ {
				code = append(code, OpAdd)
			}
			b.Progs[0].Code = append(code, OpRet)
		},
		"backward jump": func(b *Bundle) { b.Progs[0].Code = []byte{OpConst, 0, 0, OpJmp, 0, 0, OpRet} },
		"loop on itself": func(b *Bundle) {
			b.Progs[0].Code = []byte{OpConst, 0, 0, OpJmp, 3, 0, OpRet}
		},
		"jump past the end": func(b *Bundle) { b.Progs[0].Code = []byte{OpConst, 0, 0, OpJmp, 99, 0, OpRet} },
		"jump into an operand": func(b *Bundle) {
			b.Progs[0].Code = []byte{OpConst, 0, 0, OpJz, 7, 0, OpConst, 0, 0, OpRet}
		},
		"jump heights differ": func(b *Bundle) {
			// if the condition holds, one value more arrives at the return than when it falls through
			b.Progs[0].Code = []byte{OpConst, 0, 0, OpConst, 0, 0, OpJz, 12, 0, OpConst, 0, 0, OpRet}
		},
		"unreachable code": func(b *Bundle) {
			b.Progs[0].Code = []byte{OpConst, 0, 0, OpJmp, 9, 0, OpConst, 0, 0, OpRet}
		},
		"conditional underflow": func(b *Bundle) { b.Progs[0].Code = []byte{OpJz, 4, 0, OpRet} },
		"too many stats": func(b *Bundle) {
			for len(b.Stats) <= MaxStats {
				s := b.Stats[0]
				s.Guid = int64(1<<40 + len(b.Stats))
				b.Stats = append(b.Stats, s)
			}
		},
		"string too long": func(b *Bundle) { b.Strings[0] = strings.Repeat("x", 300) },

		// plan D2: the expression programs and the host operations
		"expression outside the programs": func(b *Bundle) { b.Exprs = append(b.Exprs, uint32(len(b.Progs))) },
		"expressions not increasing":      func(b *Bundle) { b.Exprs[0], b.Exprs[1] = b.Exprs[1], b.Exprs[0] },
		"an expression twice":             func(b *Bundle) { b.Exprs[1] = b.Exprs[0] },
		"too many expression programs":    func(b *Bundle) { b.Exprs = make([]uint32, MaxExprs+1) },
		"a random number in a base formula": func(b *Bundle) {
			b.Stats[0].Base = hostProgram(b, OpRand)
		},
		"a progress ratio in a limit": func(b *Bundle) { b.Stats[0].Max = hostProgram(b, OpProgress, 0, 0) },
		"an instigator count in a regeneration": func(b *Bundle) {
			b.Stats[0].Regen = hostProgram(b, OpEffectInst, 0, 0)
		},
		"a random number in an initializer step": func(b *Bundle) { b.CommonSteps[0].Prog = uint32(hostProgram(b, OpRand)) },
		"a random number in a class step":        func(b *Bundle) { b.Classes[0].Steps[0].Prog = uint32(hostProgram(b, OpRand)) },
		"a hit tag in a base formula":            func(b *Bundle) { b.Stats[0].Base = hostProgram(b, OpHitTag, 0, 0) },
		"a hit flag in a limit":                  func(b *Bundle) { b.Stats[0].Min = hostProgram(b, OpHitFlag, 0, 0) },
		"the experience debt in a regeneration":  func(b *Bundle) { b.Stats[0].Regen = hostProgram(b, OpXpDebt) },
		"the hit's targets in an initializer":    func(b *Bundle) { b.CommonSteps[0].Prog = uint32(hostProgram(b, OpHitTargets)) },
		"a hit tag string outside":               func(b *Bundle) { b.Progs[0].Code = []byte{OpHitTag, 0xff, 0xff, OpRet} },
		"a hit flag string outside":              func(b *Bundle) { b.Progs[0].Code = []byte{OpHitFlag, 0xff, 0xff, OpRet} },
		"a progress string outside":              func(b *Bundle) { b.Progs[0].Code = []byte{OpProgress, 0xff, 0xff, OpRet} },
		"an instigator reference outside":        func(b *Bundle) { b.Progs[0].Code = []byte{OpEffectInst, 0xff, 0xff, OpRet} },
		"lerp with two values":                   func(b *Bundle) { b.Progs[0].Code = []byte{OpConst, 0, 0, OpConst, 0, 0, OpLerp, OpRet} },
		"abs of nothing":                         func(b *Bundle) { b.Progs[0].Code = []byte{OpAbs, OpRet} },
		"an unknown opcode after the new ones":   func(b *Bundle) { b.Progs[0].Code = []byte{0x64, OpRet} }, // (0x61 to 0x63 are the host functions: cut off, not unknown)
		// the host functions: their operands (a function id 1..255 and a string) and what they pop
		"host function 0":            func(b *Bundle) { b.Progs[0].Code = []byte{OpHost0, 0x00, 0x00, 0x00, 0x00, OpRet} },
		"host function 256":          func(b *Bundle) { b.Progs[0].Code = []byte{OpHost0, 0x00, 0x01, 0x00, 0x00, OpRet} },
		"a host name outside":        func(b *Bundle) { b.Progs[0].Code = []byte{OpHost0, 0x01, 0x00, 0xff, 0xff, OpRet} },
		"a host function of one cut": func(b *Bundle) { b.Progs[0].Code = []byte{OpHost1, 0x01, 0x00, 0x00} },
		"host1 on an empty stack":    func(b *Bundle) { b.Progs[0].Code = []byte{OpHost1, 0x01, 0x00, 0x00, 0x00, OpRet} },
		"host2 with one value":       func(b *Bundle) { b.Progs[0].Code = []byte{OpConst, 0, 0, OpHost2, 0x01, 0x00, 0x00, 0x00, OpRet} },
	}
}

// hostProgram appends a program that is one host operation (with its operands) and a return, and returns its index.
func hostProgram(b *Bundle, op byte, operands ...byte) int32 {
	code := append([]byte{op}, operands...)
	code = append(code, OpRet)
	b.Progs = append(b.Progs, Program{Code: code})
	return int32(len(b.Progs) - 1)
}

// HostileCases returns the blobs, in a fixed order: structural breaks of every table,
// damaged headers and bodies, and truncations.
func HostileCases() ([]Hostile, error) {
	base, _, err := TinyBundle(true, ModeFloat32)
	if err != nil {
		return nil, err
	}
	enc := base.Encode()
	var out []Hostile
	muts := structuralMutations()
	names := make([]string, 0, len(muts))
	for n := range muts {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		b, err := Decode(enc)
		if err != nil {
			return nil, err
		}
		muts[n](b)
		out = append(out, Hostile{"structure: " + n, b.Encode()})
	}
	patch := func(off int, v ...byte) []byte {
		c := append([]byte(nil), enc...)
		copy(c[off:], v)
		return c
	}
	crcFixed := func(c []byte) []byte {
		binary.LittleEndian.PutUint32(c[12:], crc32.ChecksumIEEE(c[headerSize:]))
		return c
	}
	out = append(out,
		Hostile{"header: magic", patch(0, 'X')},
		Hostile{"header: version 1", patch(4, 1)},
		Hostile{"header: version 4", patch(4, 4)},
		Hostile{"header: unknown flag", patch(6, 4)},
		Hostile{"header: both modes", patch(6, 3)},
		Hostile{"header: body length one more", patch(8, byte(len(enc)-headerSize+1))},
		Hostile{"header: crc", patch(12, enc[12]^1)},
		Hostile{"body: a flipped bit", patch(headerSize+3, enc[headerSize+3]^0x40)},
		Hostile{"body: a byte after the last table", append(append([]byte(nil), enc...), 0)},
		Hostile{"body: 2^32-1 constants", crcFixed(patch(headerSize, 0xff, 0xff, 0xff, 0xff))},
		Hostile{"body: 65536 constants", crcFixed(patch(headerSize, 0, 0, 1, 0))},
	)
	for _, n := range []int{0, 1, 7, 8, 15, 16, 17, len(enc) / 3, len(enc) / 2, len(enc) - 9, len(enc) - 1} {
		out = append(out, Hostile{"truncated to " + strconv.Itoa(n), enc[:n]})
	}
	return out, nil
}

// C.Y.B captures, C | O | A code

// AbuseBundle is a valid bundle that asks too much: 1,100 initializer steps of a program of 4,094
// instructions, 4.5 million in all, more than a run may execute (MaxRunInstructions). Decode accepts
// it; Run (and the C++ run_stats) must refuse to finish it.
func AbuseBundle() []byte {
	var code []byte
	for i := 0; i < 2046; i++ {
		code = append(code, OpConst, 0, 0, OpPop)
	}
	code = append(code, OpConst, 0, 0, OpRet)
	b := &Bundle{
		Consts:  []float64{1},
		Strings: []string{"Abuse", "Class"},
		Progs:   []Program{{Code: code}},
		Stats:   []Stat{{Guid: 1, Name: 0, Replication: 1, Base: -1, Min: -1, Max: -1, Regen: -1}},
		Classes: []Class{{Enum: 1, Name: 1, Members: []uint32{0}}},
		Order:   []uint32{0, 1},
	}
	for i := 0; i < 1100; i++ {
		b.CommonSteps = append(b.CommonSteps, Step{Stat: 0, Prog: 0})
	}
	return b.Encode()
}

// FuzzParity damages a bundle n times, deterministically from seed, and says what a reader makes of
// each: refused, or accepted and run (a character of the first class at level 20), folding the verdicts
// and the digest of every result into one hash. The C++ test makes the same damage with the same
// generator and must get the same count and hash: the two readers and interpreters agree on random
// programs, not only on the ones the compiler writes.
func FuzzParity(base []byte, seed uint64, n int) (accepted int, hash uint64) {
	x := seed
	next := func() uint64 {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		return x
	}
	hash = 14695981039346656037
	fold := func(b byte) {
		hash ^= uint64(b)
		hash *= 1099511628211
	}
	for i := 0; i < n; i++ {
		m := append([]byte(nil), base...)
		for k := int(next()%4) + 1; k > 0; k-- {
			op := next() % 5
			at := headerSize + int(next()%uint64(len(m)-headerSize))
			switch op {
			case 0, 1, 2:
				bit := uint(next() % 8)
				m[at] ^= byte(1) << bit
			case 3:
				m[at] = byte(next())
			default:
				m = append(m[:at], m[at+1:]...)
				binary.LittleEndian.PutUint32(m[8:], uint32(len(m)-headerSize))
			}
		}
		if next()%10 != 0 {
			binary.LittleEndian.PutUint32(m[12:], crc32.ChecksumIEEE(m[headerSize:]))
		}
		b, err := Decode(m)
		if err != nil {
			fold(0)
			continue
		}
		accepted++
		if len(b.Classes) == 0 {
			fold(3)
			continue
		}
		res, err := b.Run(Inputs{Class: int(b.Classes[0].Enum), Level: 20}, nil)
		if err != nil {
			fold(2)
			continue
		}
		fold(1)
		d := Digest(res)
		for j := 0; j < 8; j++ {
			fold(byte(d >> (8 * j)))
		}
	}
	return accepted, hash
}

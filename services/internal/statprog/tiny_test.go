// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"math"
	"math/rand"
	"strings"
	"testing"

	"aocrop/services/internal/aocexpr"
)

// The tests that need no content.db: the TinyGraph covers every instruction.

func tinyBundle(t testing.TB, faults bool, mode Mode) *Bundle {
	t.Helper()
	b, _, err := TinyBundle(faults, mode)
	if err != nil {
		t.Fatalf("TinyBundle(%v, %v): %v", faults, mode, err)
	}
	return b
}

// sameResults compares a compiled run with the reference engine's, bit for bit.
func sameResults(t *testing.T, label string, got *Result, want map[int64]StatOut) {
	t.Helper()
	if len(got.Stats) != len(want) {
		t.Errorf("%s: %d stats, the reference has %d", label, len(got.Stats), len(want))
	}
	for _, o := range got.Stats {
		w, ok := want[o.Guid]
		if !ok {
			t.Errorf("%s: stat %d is not in the reference", label, o.Guid)
			continue
		}
		bits := math.Float64bits
		if bits(o.Value) != bits(w.Value) || bits(o.Base) != bits(w.Base) || bits(o.Equip) != bits(w.Equip) ||
			o.HasRegen != w.HasRegen || (o.HasRegen && bits(o.Regen) != bits(w.Regen)) || o.Replication != w.Replication {
			t.Errorf("%s: stat %d: engine value %v base %v equip %v regen %v/%v rep %d; reference %v %v %v %v/%v rep %d", label, o.Guid,
				o.Value, o.Base, o.Equip, o.Regen, o.HasRegen, o.Replication, w.Value, w.Base, w.Equip, w.Regen, w.HasRegen, w.Replication)
		}
	}
}

// C.Y.B and CoA

func TestTinyAgreesWithAocexpr(t *testing.T) {
	g := TinyGraph(false)
	for _, mode := range []Mode{ModeFloat32, ModeDouble, ModeSingle} {
		b := tinyBundle(t, false, mode)
		for _, class := range []int{1, 2} {
			for _, level := range []int{1, 5, 10, 11, 20, 39, 40, 41, 50, 255} {
				for _, rich := range []bool{false, true} {
					in := tinyInputs(class, level, rich)
					var st aocexpr.State
					if rich {
						st = richState()
					}
					got, err := b.Run(in, st)
					if err != nil {
						t.Fatal(err)
					}
					want, err := RefRun(g, in, st, mode)
					if err != nil {
						t.Fatalf("%v class %d level %d: RefRun: %v", mode, class, level, err)
					}
					sameResults(t, fmt.Sprintf("%v class %d level %d rich %v", mode, class, level, rich), got, want)
					if got.Faults != 0 {
						t.Errorf("%v class %d level %d: %d faults", mode, class, level, got.Faults)
					}
					if wantIgnored := map[bool]int{false: 0, true: 1}[rich]; got.IgnoredMods != wantIgnored {
						t.Errorf("ignored mods %d, want %d", got.IgnoredMods, wantIgnored)
					}
				}
			}
		}
	}
}

func byName(b *Bundle, res *Result) map[string]StatOut {
	out := map[string]StatOut{}
	for _, s := range res.Stats {
		out[b.Strings[b.Stats[s.Stat].Name]] = s
	}
	return out
}

func TestTinyValues(t *testing.T) {
	b := tinyBundle(t, false, ModeFloat32)
	res, err := b.Run(tinyInputs(1, 20, false), nil)
	if err != nil {
		t.Fatal(err)
	}
	v := byName(b, res)
	// Ranger level 20: Str = Round(curve(20) * 1.5) = 150; Dex = 7 + 150 / (150 + 250) + 1 = 8.375;
	// Hp = 20 + 150 * 2 = 320 and spawns at MaxHp = 450; the Mul stat multiplies: 1 * 150 * 0.01.
	str, thousand := 150.0, 1000.0
	want := map[string]float64{
		"Str": 150, "Dex": 8.375, "Hp": 450, "MaxHp": 450, "Mul": 1.5, "Enum": 1, "Named": 307.5,
		"Shield": float64(float32(str / thousand)),
	}
	for name, w := range want {
		if s, ok := v[name]; !ok || s.Value != w {
			t.Errorf("%s = %v (%v), want %v", name, s.Value, ok, w)
		}
	}
	if _, ok := v["Plain"]; ok {
		t.Error("Plain is in no list, so it is not a member of the result")
	}
	if !v["Regen"].HasRegen || v["Regen"].Value != 0 {
		t.Errorf("Regen = %+v", v["Regen"])
	}
	// Regen reads Regen (0) and Hp (450): 450 * 0.01 as a float32.
	if r := v["Regen"].Regen; r != float64(float32(450*0.01)) {
		t.Errorf("Regen's rate = %v", r)
	}
	// Ctx with no state: profession level 1 -> 10000.
	if c := v["Ctx"].Value; c != 10000 {
		t.Errorf("Ctx = %v", c)
	}
	// The same character with a shield and a bard instead of a ranger.
	res, _ = b.Run(Inputs{Class: 2, Level: 20, Equipped: []EquippedTag{{"Secondary", "Item.Gear.Weapon.Shield"}}}, richState())
	v = byName(b, res)
	// Bard: Str = Round(100 * 0.8) = 80; Mul starts at 1 and the Bard's entry reads the running total, which is the value so far with the
	// Base folded in (1 * Str * 0.01 = 0.8): the entry's 0.8 + 0.5 = 1.3 is the stat's value, stored as the initial value 1.3 / 0.8 that the
	// Base multiplies again (each step a float32); Shield 0.15 + 0.08.
	mulBase := float64(float32(80 * 0.01))
	mulEntry := float64(float32(1*mulBase + 0.5))
	mulInit := float64(float32(mulEntry / mulBase))
	if v["Str"].Value != 80 || v["Mul"].Value != float64(float32(mulBase*mulInit)) {
		t.Errorf("Bard: Str %v Mul %v", v["Str"].Value, v["Mul"].Value)
	}
	a, c := 0.15, 80.0/1000.0
	if s := v["Shield"].Value; s != float64(float32(a+c)) {
		t.Errorf("Bard Shield = %v", s)
	}
	// Ctx: 3 effects * 2 + 2 tagged + skill 100 + in combat 1000 + Fishing 7 * 10000 + consumed 11 * 100000 + rank 2.
	if c := v["Ctx"].Value; c != 3*2+2+100+1000+70000+1100000+2 {
		t.Errorf("Ctx with a state = %v", c)
	}
}

// [C-O-A ~ Cyb]

func TestTinyFaults(t *testing.T) {
	g := TinyGraph(true)
	// A literal that is not a float32 cannot be compiled for single arithmetic.
	if _, err := Compile(g, Options{Mode: ModeSingle}); err == nil || !strings.Contains(err.Error(), "float32") {
		t.Errorf("1e300 in single mode: %v", err)
	}
	for _, mode := range []Mode{ModeFloat32, ModeDouble} {
		b, err := Compile(g, Options{Mode: mode})
		if err != nil {
			t.Fatal(err)
		}
		// Level 5 is inside the cubic segment of curve 12.
		res, err := b.Run(Inputs{Class: 1, Level: 5}, nil)
		if err != nil {
			t.Fatal(err)
		}
		v := byName(b, res)
		for _, n := range []string{"Fault", "Overflow"} {
			if v[n].Value != 0 {
				t.Errorf("%v: %s = %v, want 0", mode, n, v[n].Value)
			}
		}
		// Fault: a division by zero (1), Overflow: a value that is not finite (1); the cubic segment is evaluated (a Bezier with flat tangents).
		if res.Faults != 2 {
			t.Errorf("%v: %d faults, want 2", mode, res.Faults)
		}
		tt := 4.0 / 29
		if want := 1 + 8*(3*tt*tt-2*tt*tt*tt); math.Abs(v["Cubic"].Value-want) > 1e-5 {
			t.Errorf("%v: Cubic = %v, want %v", mode, v["Cubic"].Value, want)
		}
		// Past the curve's last key nothing is read from the cubic segment.
		res, _ = b.Run(Inputs{Class: 1, Level: 40}, nil)
		if res.Faults != 2 || byName(b, res)["Cubic"].Value != 9 {
			t.Errorf("%v: level 40: %d faults, Cubic %v", mode, res.Faults, byName(b, res)["Cubic"].Value)
		}
		// The reference engine stops where the compiled one counts.
		if _, err := RefRun(g, Inputs{Class: 1, Level: 5}, nil, mode); err == nil {
			t.Errorf("%v: RefRun of a division by zero did not fail", mode)
		}
	}
}

// A sum of the equipment that no float32 holds is a fault even when the stat's clamp hides it from the value: the client is sent the
// equipment's contribution of a bucket stat too, and a non-finite number there would be stored and refused at the next login.
func TestTinyEquipmentBehindAClampIsAFault(t *testing.T) {
	g := TinyGraph(false)
	inf := math.Inf(1)
	for _, mode := range []Mode{ModeFloat32, ModeSingle, ModeDouble} {
		b := tinyBundle(t, false, mode)
		for _, tc := range []struct {
			name   string
			value  float64
			faults int // Str's Max is 999, which rescues any of these values
		}{{"1e39", 1e39, 1}, {"-1e39", -1e39, 1}, {"+Inf", inf, 1}, {"-Inf", -inf, 1}, {"3.5e38", 3.5e38, 1}, {"3e38", 3e38, 0}, {"1e30", 1e30, 0}} {
			want := tc.faults
			if mode == ModeDouble && !math.IsInf(tc.value, 0) {
				want = 0 // a double holds 1e39
			}
			in := Inputs{Class: 1, Level: 20, Mods: []Mod{{1001, tc.value}}}
			res, err := b.Run(in, nil)
			if err != nil {
				t.Fatalf("%v %s: %v", mode, tc.name, err)
			}
			str := byName(b, res)["Str"]
			if res.Faults != want {
				t.Errorf("%v %s: %d faults, want %d", mode, tc.name, res.Faults, want)
			}
			if math.IsNaN(str.Equip) || math.IsInf(str.Equip, 0) {
				t.Errorf("%v %s: the equipment is %v: a fault is a 0", mode, tc.name, str.Equip)
			}
			if want == 1 && str.Equip != 0 {
				t.Errorf("%v %s: the faulted equipment is %v, want 0", mode, tc.name, str.Equip)
			}
			if want == 0 && str.Equip != float64(float32(tc.value)) && mode != ModeDouble {
				t.Errorf("%v %s: the equipment is %v, want %v", mode, tc.name, str.Equip, float64(float32(tc.value)))
			}
			// the clamp did hide it from the value
			if mode != ModeDouble || want == 0 {
				if str.Value != 999 && tc.value > 0 {
					t.Errorf("%v %s: Str = %v, want the clamp's 999", mode, tc.name, str.Value)
				}
			}
			// the reference engine stops where the compiled one counts
			_, rerr := RefRun(g, in, nil, mode)
			if (rerr != nil) != (want == 1) {
				t.Errorf("%v %s: RefRun error %v, the engine counted %d fault(s)", mode, tc.name, rerr, want)
			}
		}
	}
}

func TestTinyRoundTripAndDeterminism(t *testing.T) {
	for _, mode := range []Mode{ModeFloat32, ModeDouble} {
		a := tinyBundle(t, true, mode)
		enc := a.Encode()
		if !bytes.Equal(enc, tinyBundle(t, true, mode).Encode()) {
			t.Errorf("%v: two compiles differ", mode)
		}
		d, err := Decode(enc)
		if err != nil {
			t.Fatalf("%v: Decode: %v", mode, err)
		}
		if !bytes.Equal(d.Encode(), enc) {
			t.Errorf("%v: Decode then Encode changed the bytes", mode)
		}
		if d.Mode != mode {
			t.Errorf("mode %v decoded as %v", mode, d.Mode)
		}
		// A decoded bundle runs like the compiled one.
		r1, _ := a.Run(tinyInputs(2, 17, true), richState())
		r2, _ := d.Run(tinyInputs(2, 17, true), richState())
		if fmt.Sprint(r1) != fmt.Sprint(r2) {
			t.Errorf("%v: decoded bundle runs differently", mode)
		}
	}
	// The float32 and double bundles differ in their header flags only; single also in its constants.
	f, d, s := tinyBundle(t, false, ModeFloat32).Encode(), tinyBundle(t, false, ModeDouble).Encode(), tinyBundle(t, false, ModeSingle).Encode()
	if !bytes.Equal(f[8:], d[8:]) || bytes.Equal(f[6:8], d[6:8]) || bytes.Equal(f[6:8], s[6:8]) {
		t.Error("float32 and double bundles should differ in the flags only")
	}
}

// ---- compile errors ----

func miniGraph(base string, extra func(*Graph)) *Graph {
	g := &Graph{
		Stats: map[int64]StatDef{
			1: {Guid: 1, Name: "X", Replication: 1, Base: base, OnSpawn: "Reset_None"},
			2: {Guid: 2, Name: "Y", Replication: 1, Base: "3.0", OnSpawn: "Reset_None"},
		},
		Lists:     map[string][]InitItem{"CombatStats": {{1, ""}, {2, ""}}, "Base_Artisan_Stats": nil, "Player_Base": nil, "Player_Base_Ranger": nil},
		Classes:   []ClassDef{{Name: "Ranger", Enum: 1, List: "Player_Base_Ranger"}},
		Equations: map[int64]string{},
		Formulas:  map[int64]Formula{},
		Curves:    map[int64]*aocexpr.Curve{},
	}
	if extra != nil {
		extra(g)
	}
	return g
}

// -- CoA . C.Y.B --

func TestCompileErrors(t *testing.T) {
	statY := fmt.Sprintf("$#%d:2$", TypeStatDef)
	statX := fmt.Sprintf("$#%d:1$", TypeStatDef)
	eqRef := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatEquation, n) }
	for _, tt := range []struct {
		name, base, want string
		extra            func(*Graph)
	}{
		{"unknown function", "Nope(1)", `unsupported function "Nope"`, nil},
		{"integer division", "7 / 2", "division where no operand is proven", nil},
		{"level division", "GetCharacterLevel(GetOwner()) / 2", "division where no operand is proven", nil},
		{"unknown variable", "q + 1", `unknown variable "q"`, nil},
		{"set of const", "const a = 1.0; set a = 2.0; a", "set of const", nil},
		{"set of undeclared", "set a = 2.0; 1.0", "undeclared", nil},
		{"redeclared", "const a = 1.0; const a = 2.0; a", "already declared", nil},
		{"set changes type", "var a = 1.0; set a = true; a", "set of", nil},
		{"running total outside an initializer", "GetStatInitRunningTotal()", "outside a stat initializer", nil},
		{"unknown stat", "GetStat(GetOwner(), $#" + fmt.Sprint(TypeStatDef) + ":777$)", "no stat 777", nil},
		{"not a stat ref", "GetStat(GetOwner(), $#5:1$)", "not a StatTypeDef reference", nil},
		{"named stat missing", "GetStat(GetOwner(), $Stat:Zed$)", `called "Zed"`, nil},
		{"named kind", "GetStat(GetOwner(), $Effect:Zed$)", "not a stat reference", nil},
		{"GetStat arguments", "GetStat(" + statY + ")", "takes 2 argument(s)", nil},
		{"GetStat object", "GetStat(1, " + statY + ")", "argument 1 is a number", nil},
		{"condition not boolean", "var a = 1.0; if (a) { set a = 2.0; } a", "not a boolean", nil},
		{"and of numbers", "1.0 && 2.0", "needs booleans", nil},
		{"not of number", "!1.0", "needs a boolean", nil},
		{"minus of boolean", "-true", "needs a number", nil},
		{"compare number to bool", "1.0 == true", "cannot compare", nil},
		{"add bool", "1.0 + true", "needs numbers", nil},
		{"result is not a number", "GetOwner()", "not a number", nil},
		{"var holds object", "var o = GetOwner(); 1.0", "only numbers and booleans can change", nil},
		{"EvalE of a stat", "EvalE(" + statY + ")", "only $#<TypeId>:<Guid>$ of the equation table", nil},
		{"EvalE missing", "EvalE(" + eqRef(55) + ")", "no equation 55", nil},
		{"EvalFormula missing", "EvalFormula(" + fmt.Sprintf("$#%d:5$", TypeStatFormula) + ")", "no formula 5", nil},
		{"equation recursion", "EvalE(" + eqRef(1) + ")", "calls itself", func(g *Graph) { g.Equations[1] = "EvalE(" + eqRef(1) + ")" }},
		{"mutual recursion", "EvalE(" + eqRef(1) + ")", "calls itself", func(g *Graph) {
			g.Equations[1] = "EvalE(" + eqRef(2) + ")"
			g.Equations[2] = "EvalE(" + eqRef(1) + ")"
		}},
		{"equation parse error", "EvalE(" + eqRef(1) + ")", "EvalE 1", func(g *Graph) { g.Equations[1] = "1 +" }},
		{"equation is boolean", "EvalE(" + eqRef(1) + ")", "not a number", func(g *Graph) { g.Equations[1] = "true" }},
		{"GetTerm outside a formula", "GetTerm(" + eqRef(1) + ")", "GetTerm outside a formula", nil},
		{"GetTerm inside EvalE", "EvalE(" + eqRef(1) + ")", "GetTerm outside a formula", func(g *Graph) { g.Equations[1] = "GetTerm(" + eqRef(9) + ")" }},
		{"formula without the term", "EvalFormula(" + fmt.Sprintf("$#%d:5$", TypeStatFormula) + ")", "no term", func(g *Graph) {
			g.Equations[1] = "GetTerm(" + eqRef(9) + ")"
			g.Formulas[5] = Formula{Equation: 1, Terms: map[int64]string{}}
		}},
		{"curve missing", "EvalCurve($#" + fmt.Sprint(TypeStatCurve) + ":9$, 1.0)", "no curve 9", nil},
		{"curve ref type", "EvalCurve(" + statY + ", 1.0)", "not a StatCurve reference", nil},
		{"enum not a slot", `EquippedItemInSlotHasTag(EFoo::Bar, "x") && true`, "not an EEquipSlot", nil},
		{"string too long", `GetEffectCountByTag(GetOwner(), "` + strings.Repeat("x", 300) + `")`, "string of 300 bytes", nil},
		{"actor argument", `GetPlayerProfessionLevel(5, "x")`, "argument 1 is a number", nil},
		{"syntax", "1 +", "expected a value", nil},
		{"empty", "  ", "", nil}, // an empty Base is no formula: compiles
		{"self cycle", "GetStat(GetOwner(), " + statX + ")", "depend on themselves", nil},
		{"mutual cycle", "GetStat(GetOwner(), " + statY + ")", "depend on themselves", func(g *Graph) {
			s := g.Stats[2]
			s.Base = "GetStat(GetOwner(), " + statX + ")"
			g.Stats[2] = s
		}},
	} {
		_, err := Compile(miniGraph(tt.base, tt.extra), Options{})
		if tt.want == "" {
			if err != nil {
				t.Errorf("%s: %v", tt.name, err)
			}
			continue
		}
		if err == nil || !strings.Contains(err.Error(), tt.want) {
			t.Errorf("%s: error %v, want one with %q", tt.name, err, tt.want)
		}
	}
	// Graph-level errors.
	if _, err := Compile(&Graph{Lists: map[string][]InitItem{}}, Options{}); err == nil {
		t.Error("a graph with no lists compiled")
	}
	g := miniGraph("1.0", nil)
	g.Lists["Player_Base_Ranger"] = []InitItem{{99, "1.0"}}
	if _, err := Compile(g, Options{}); err == nil || !strings.Contains(err.Error(), "no stat 99") {
		t.Errorf("an unknown stat in a list: %v", err)
	}
	g = miniGraph("1.0", nil)
	s := g.Stats[1]
	s.Replication = 8
	g.Stats[1] = s
	if _, err := Compile(g, Options{}); err == nil || !strings.Contains(err.Error(), "replication 8") {
		t.Errorf("replication 8: %v", err)
	}
	// Too many locals or too deep a stack are refused, not truncated.
	var sb strings.Builder
	for i := 0; i < MaxLocals+1; i++ {
		fmt.Fprintf(&sb, "var v%d = 1.0;\n", i)
	}
	sb.WriteString("1.0")
	if _, err := Compile(miniGraph(sb.String(), nil), Options{}); err == nil || !strings.Contains(err.Error(), "local variables") {
		t.Errorf("too many locals: %v", err)
	}
	deep := strings.Repeat("(GetStat(GetOwner(), "+statY+") + ", MaxStack+2) + "1.0" + strings.Repeat(")", MaxStack+2)
	// The parser's nesting cap (200 levels) stops a text before its stack could pass MaxStack (64): either
	// refusal is the right one, and a text that nests less compiles.
	if _, err := Compile(miniGraph(deep, nil), Options{}); err == nil || !(strings.Contains(err.Error(), "stack gets") || strings.Contains(err.Error(), "levels deep")) {
		t.Errorf("a deep stack: %v", err)
	}
	ok := strings.Repeat("(GetStat(GetOwner(), "+statY+") + ", 20) + "1.0" + strings.Repeat(")", 20)
	if _, err := Compile(miniGraph(ok, nil), Options{}); err != nil {
		t.Errorf("a stack 21 deep: %v", err)
	}
}

// ---- verifying hostile bundles ----

func TestVerifyRefusesHostileBundles(t *testing.T) {
	base := tinyBundle(t, true, ModeFloat32)
	if _, err := Decode(base.Encode()); err != nil {
		t.Fatal(err)
	}
	for name, mutate := range structuralMutations() {
		b, err := Decode(base.Encode()) // a fresh copy
		if err != nil {
			t.Fatal(err)
		}
		mutate(b)
		var enc []byte
		func() {
			defer func() {
				if r := recover(); r != nil {
					t.Errorf("%s: Encode panicked: %v", name, r)
				}
			}()
			enc = b.Encode()
		}()
		if enc == nil {
			continue
		}
		_, err = Decode(enc)
		if err == nil || !errors.Is(err, ErrFormat) {
			t.Errorf("%s: Decode = %v, want an ErrFormat error", name, err)
		}
	}
}

func TestDecodeRefusesDamagedBytes(t *testing.T) {
	enc := tinyBundle(t, true, ModeFloat32).Encode()
	// Every truncation.
	for n := 0; n < len(enc); n++ {
		if _, err := Decode(enc[:n]); err == nil {
			t.Fatalf("a bundle cut to %d of %d bytes was accepted", n, len(enc))
		}
	}
	// Header fields.
	patch := func(off int, v ...byte) []byte {
		c := append([]byte(nil), enc...)
		copy(c[off:], v)
		return c
	}
	for name, c := range map[string][]byte{
		"magic":         patch(0, 'X'),
		"version":       patch(4, 4),
		"version 1":     patch(4, 1),
		"unknown flag":  patch(6, 4),
		"both modes":    patch(6, 3),
		"body length":   patch(8, byte(len(enc)-headerSize+1)),
		"body too long": append(append([]byte(nil), enc...), 0),
		"crc":           patch(12, enc[12]^1),
		"body byte":     patch(headerSize+3, enc[headerSize+3]^0x40),
		"empty":         nil,
		"just a header": enc[:headerSize],
	} {
		if _, err := Decode(c); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	// Counts that do not fit are refused before anything is allocated for them.
	huge := append([]byte(nil), enc...)
	huge[headerSize], huge[headerSize+1], huge[headerSize+2], huge[headerSize+3] = 0xff, 0xff, 0xff, 0xff
	fixCRC(huge)
	if _, err := Decode(huge); err == nil || !strings.Contains(err.Error(), "constants") {
		t.Errorf("a count of 2^32-1 constants: %v", err)
	}
	// An oversized input.
	if _, err := Decode(make([]byte, MaxBundleBytes+1)); err == nil {
		t.Error("an oversized bundle was accepted")
	}
}

// [C.O.A ~ C-Y-B]

func fixCRC(enc []byte) {
	binary.LittleEndian.PutUint32(enc[12:], crc32.ChecksumIEEE(enc[headerSize:]))
}

func binaryPutBodyLen(enc []byte) {
	binary.LittleEndian.PutUint32(enc[8:], uint32(len(enc)-headerSize))
}

// TestFuzzDecodeAndRun mutates a valid bundle at random: Decode must never panic
// and whatever it accepts must run without panicking.
func TestFuzzDecodeAndRun(t *testing.T) {
	enc := tinyBundle(t, true, ModeFloat32).Encode()
	rng := rand.New(rand.NewSource(1))
	accepted := 0
	for i := 0; i < 30000; i++ {
		c := append([]byte(nil), enc...)
		for k := rng.Intn(4) + 1; k > 0; k-- {
			switch rng.Intn(5) {
			case 0, 1, 2:
				c[headerSize+rng.Intn(len(c)-headerSize)] ^= byte(1 << uint(rng.Intn(8)))
			case 3:
				c[headerSize+rng.Intn(len(c)-headerSize)] = byte(rng.Intn(256))
			case 4:
				n := headerSize + rng.Intn(len(c)-headerSize)
				c = append(c[:n], c[n+1:]...)
				binaryPutBodyLen(c)
			}
		}
		if rng.Intn(10) != 0 {
			fixCRC(c)
		}
		b, err := Decode(c)
		if err != nil {
			continue
		}
		accepted++
		for _, cls := range b.Classes {
			for _, level := range []int{1, 30, MaxLevel} {
				if _, err := b.Run(Inputs{Class: int(cls.Enum), Level: level, Mods: []Mod{{1001, 1}}}, nil); err != nil {
					t.Fatalf("a verified bundle failed to run: %v", err)
				}
			}
		}
	}
	t.Logf("%d of 30000 mutations passed the verifier and ran", accepted)
}

func FuzzDecode(f *testing.F) {
	b := tinyBundle(f, true, ModeFloat32)
	f.Add(b.Encode())
	f.Fuzz(func(t *testing.T, data []byte) {
		d, err := Decode(data)
		if err != nil {
			return
		}
		for _, cls := range d.Classes {
			if _, err := d.Run(Inputs{Class: int(cls.Enum), Level: 7}, nil); err != nil {
				t.Fatal(err)
			}
		}
	})
}

func TestRunRefusesBadInputs(t *testing.T) {
	b := tinyBundle(t, false, ModeFloat32)
	for _, in := range []Inputs{{Class: 1, Level: 0}, {Class: 1, Level: MaxLevel + 1}, {Class: 1, Level: -1}, {Class: 3, Level: 1}, {Class: 0, Level: 1}} {
		if _, err := b.Run(in, nil); err == nil {
			t.Errorf("Run(%+v) did not fail", in)
		}
		if _, err := RefRun(TinyGraph(false), in, nil, ModeFloat32); err == nil {
			t.Errorf("RefRun(%+v) did not fail", in)
		}
	}
	if i, ok := b.StatIndex(1001); !ok || b.Stats[i].Guid != 1001 {
		t.Error("StatIndex(1001)")
	}
	if _, ok := b.StatIndex(5); ok {
		t.Error("StatIndex(5)")
	}
}

// C.Y.B captures, C-O-A code

func TestMinMaxMatchGo(t *testing.T) {
	nan, inf := math.NaN(), math.Inf(1)
	negz := math.Copysign(0, -1)
	vals := []float64{0, negz, 1, -1, 2.5, inf, -inf, nan}
	for _, a := range vals {
		for _, b := range vals {
			same := func(x, y float64) bool { return math.Float64bits(x) == math.Float64bits(y) || (x != x && y != y) }
			if !same(goMin(a, b), math.Min(a, b)) || !same(goMax(a, b), math.Max(a, b)) {
				t.Errorf("min/max of %v, %v differ from math", a, b)
			}
		}
	}
}

func TestHostileCasesAreRefused(t *testing.T) {
	cases, err := HostileCases()
	if err != nil {
		t.Fatal(err)
	}
	total := 0
	for _, h := range cases {
		total += len(h.Blob)
		if _, err := Decode(h.Blob); err == nil || !errors.Is(err, ErrFormat) {
			t.Errorf("%s: Decode = %v, want an ErrFormat error", h.Name, err)
		}
	}
	t.Logf("%d hostile blobs, %d bytes", len(cases), total)
}

func TestRunInstructionBudget(t *testing.T) {
	b, err := Decode(AbuseBundle())
	if err != nil {
		t.Fatalf("the abusive bundle is valid: %v", err)
	}
	if _, err := b.Run(Inputs{Class: 1, Level: 1}, nil); err == nil || !strings.Contains(err.Error(), "instructions") {
		t.Errorf("Run of the abusive bundle: %v", err)
	}
	// A normal bundle uses a small part of the budget.
	tb := tinyBundle(t, true, ModeFloat32)
	res, err := tb.Run(tinyInputs(1, 20, true), richState())
	if err != nil {
		t.Fatal(err)
	}
	if res.Instructions <= 0 || res.Instructions > 5000 {
		t.Errorf("the tiny graph ran %d instructions", res.Instructions)
	}
	// No program of a verified bundle runs more instructions than it has bytes: a program of n bytes ends.
	for i, p := range tb.Progs {
		if _, err := tb.analyze(i); err != nil || len(p.Code) > MaxCodeBytes {
			t.Errorf("program %d: %v", i, err)
		}
	}
}

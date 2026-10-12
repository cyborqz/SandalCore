// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"sort"
	"testing"

	"aocrop/services/internal/content"
)

// The golden tests: the engine against the stats captured from the original
// server. bard_l1.json is the first StatsComponent send of a level-1 Bard with its
// 13 starter items (442 stats); ranger_l20.json is a retail level-20 Ranger with
// 29 items, whose 34 effects (talents, food, guild) are not applied here.

var arrayMask = map[string]uint8{"StatRepInt32Owner": 1, "StatRepInt32OwnerProxy": 5, "StatRepInt32EveryoneProxy": 7}

// goldenRun runs a capture's character through a bundle.
func goldenRun(t testing.TB, b *Bundle, c *capture, rd Rounding) (*Result, []Mod) {
	t.Helper()
	gear, err := OpenGear(contentDBPath(t))
	if err != nil {
		t.Fatal(err)
	}
	defer gear.Close()
	items := c.gearItems()
	mods, err := gear.Mods(items, rd)
	if err != nil {
		t.Fatal(err)
	}
	tags, err := gear.Tags(items)
	if err != nil {
		t.Fatal(err)
	}
	res, err := b.Run(Inputs{Class: classEnum(c.Class), Level: c.Level, Mods: mods, Equipped: tags}, nil)
	if err != nil {
		t.Fatal(err)
	}
	return res, mods
}

// C | O | A / C:Y:B

func TestGoldenBard(t *testing.T) {
	c := loadCapture(t, "bard_l1.json")
	if len(c.Stats) != 442 || len(c.Items) != 13 {
		t.Fatalf("the capture has %d stats and %d items, want 442 and 13", len(c.Stats), len(c.Items))
	}
	// Per arithmetic mode: how many of the 442 are equal as float32, and the ones that are not.
	for _, tt := range []struct {
		mode  Mode
		exact int
		off   []string // the stats a float32 away from the capture
	}{
		{ModeFloat32, 442, nil},
		{ModeDouble, 440, []string{"HealingCriticalPower", "PhysicalSpeed"}},
		{ModeSingle, 440, []string{"HealthRegeneration", "ManaRegeneration"}},
	} {
		b := compileReal(t, tt.mode)
		res, _ := goldenRun(t, b, c, RoundNone)
		if res.Faults != 0 {
			t.Errorf("%v: %d faults", tt.mode, res.Faults)
		}
		got := map[int64]StatOut{}
		for _, s := range res.Stats {
			got[s.Guid] = s
		}
		exact := 0
		var off []string
		for _, st := range c.Stats {
			o, ok := got[st.Guid]
			if !ok {
				t.Errorf("%v: captured stat %d is not in the result", tt.mode, st.Guid)
				continue
			}
			name := b.Strings[b.Stats[o.Stat].Name]
			switch compare(o.Value, f32(st.Bits)) {
			case matchExact:
				exact++
			case matchNear:
				off = append(off, name)
			default:
				t.Errorf("%v: %s = %v, captured %v", tt.mode, name, o.Value, f32(st.Bits))
			}
			if want := arrayMask[st.Array]; o.Replication != want {
				t.Errorf("%v: %s is in %s (mask %d), its replication is %d", tt.mode, name, st.Array, want, o.Replication)
			}
			if st.BaseBits != 0 || st.EquipBits != 0 {
				if compare(o.Base, f32(st.BaseBits)) != matchExact || compare(o.Equip, f32(st.EquipBits)) != matchExact {
					t.Errorf("%v: %s base %v equipment %v, captured %v and %v", tt.mode, name, o.Base, o.Equip, f32(st.BaseBits), f32(st.EquipBits))
				}
			}
		}
		sort.Strings(off)
		if exact != tt.exact || fmt.Sprint(off) != fmt.Sprint(tt.off) {
			t.Errorf("%v: %d exact, off by a float32 ulp: %v; want %d and %v", tt.mode, exact, off, tt.exact, tt.off)
		}
		t.Logf("%v: %d of 442 exact as float32, %d within 1e-6 %v", tt.mode, exact, len(off), off)
	}

	// The replicated stats the capture does not carry: one of the 443 stats of the class with an
	// audience.
	b := compileReal(t, ModeFloat32)
	res, _ := goldenRun(t, b, c, RoundNone)
	captured := map[int64]bool{}
	for _, st := range c.Stats {
		captured[st.Guid] = true
	}
	var extra []string
	for _, s := range res.Stats {
		if s.Replication != 0 && !captured[s.Guid] {
			extra = append(extra, b.Strings[b.Stats[s.Stat].Name])
		}
	}
	t.Logf("stats of the Bard with an audience that the capture does not carry: %v (of %d members)", extra, len(res.Stats))
	if len(extra) != 1 {
		t.Errorf("%d replicated stats are not in the capture, want 1: %v", len(extra), extra)
	}
}

func TestGoldenRangerWithoutEffects(t *testing.T) {
	c := loadCapture(t, "ranger_l20.json")
	b := compileReal(t, ModeFloat32)
	for _, tt := range []struct {
		rd    Rounding
		exact int
	}{{RoundNone, 358}, {RoundHalfUp, 376}} {
		res, _ := goldenRun(t, b, c, tt.rd)
		got := map[int64]StatOut{}
		for _, s := range res.Stats {
			got[s.Guid] = s
		}
		exact, near := 0, 0
		missed := map[int]bool{}
		for _, st := range c.Stats {
			o := got[st.Guid]
			switch compare(o.Value, f32(st.Bits)) {
			case matchExact:
				exact++
			case matchNear:
				near++
			default:
				missed[o.Stat] = true
			}
		}
		// A miss is a root cause when none of the stats it reads missed: the others follow from it.
		var roots, rest []string
		for s := range missed {
			isRoot := true
			for _, r := range b.reads(s) {
				if missed[int(r)] {
					isRoot = false
				}
			}
			name := b.Strings[b.Stats[s].Name]
			if isRoot {
				roots = append(roots, name)
			} else {
				rest = append(rest, name)
			}
		}
		sort.Strings(roots)
		t.Logf("Ranger L20 without effects, item rounding %d: %d of 442 equal as float32 (%.1f%%), %d near, %d missed: %d are the origin of the difference %v; %d follow from them",
			tt.rd, exact, 100*float64(exact)/442, near, len(missed), len(roots), roots, len(rest))
		if exact != tt.exact {
			t.Errorf("rounding %d: %d exact, want %d (the rate this model has had; a change is a change of the model)", tt.rd, exact, tt.exact)
		}
		if res.Faults != 0 {
			t.Errorf("%d faults", res.Faults)
		}
	}
}

// reads lists the stats a stat's formulas and initializer entries read.
func (b *Bundle) reads(stat int) []uint16 {
	seen := map[uint16]bool{}
	add := func(p int32) {
		if p < 0 {
			return
		}
		inf, _ := b.analyze(int(p))
		for _, s := range inf.stats {
			seen[s] = true
		}
	}
	add(b.Stats[stat].Base)
	add(b.Stats[stat].Min)
	add(b.Stats[stat].Max)
	for _, steps := range [][]Step{b.CommonSteps} {
		for _, s := range steps {
			if int(s.Stat) == stat {
				add(int32(s.Prog))
			}
		}
	}
	for _, c := range b.Classes {
		for _, s := range c.Steps {
			if int(s.Stat) == stat {
				add(int32(s.Prog))
			}
		}
	}
	var out []uint16
	for s := range seen {
		out = append(out, s)
	}
	return out
}

// [Cyb x C.O.A]

func TestRealBundle(t *testing.T) {
	b := compileReal(t, ModeFloat32)
	enc := b.Encode()
	d, err := Decode(enc)
	if err != nil {
		t.Fatal(err)
	}
	if string(d.Encode()) != string(enc) {
		t.Error("Decode then Encode changed the bytes")
	}
	// The same graph compiles to the same bytes.
	b2, err := Compile(loadReal(t), Options{})
	if err != nil {
		t.Fatal(err)
	}
	if string(b2.Encode()) != string(enc) {
		t.Error("two compiles of the real graph differ")
	}
	maxCode := 0
	for _, p := range b.Progs {
		if len(p.Code) > maxCode {
			maxCode = len(p.Code)
		}
	}
	t.Logf("real graph: %d bytes; %d stats, %d programs (longest %d bytes), %d constants, %d curves, %d references, %d strings; %d classes",
		len(enc), len(b.Stats), len(b.Progs), maxCode, len(b.Consts), len(b.Curves), len(b.Refs), len(b.Strings), len(b.Classes))
	if len(b.Classes) != 8+len(NpcProfiles) {
		t.Errorf("%d classes, want %d", len(b.Classes), 8+len(NpcProfiles))
	}
	for _, s := range b.Stats {
		if s.Replication != 0 && s.Replication != 1 && s.Replication != 5 && s.Replication != 6 && s.Replication != 7 {
			t.Errorf("%s: replication %d", b.Strings[s.Name], s.Replication)
		}
	}
	// Every initial value and formula of every class runs: no faults for any class at any level.
	maxInstr := 0
	for _, cl := range b.Classes {
		for level := 1; level <= 50; level++ {
			res, err := b.Run(Inputs{Class: int(cl.Enum), Level: level}, nil)
			if err != nil || res.Faults != 0 {
				t.Fatalf("%s level %d: %v, %d faults", b.Strings[cl.Name], level, err, res.Faults)
			}
			maxInstr = max(maxInstr, res.Instructions)
		}
	}
	t.Logf("a run of the real graph executes at most %d instructions (the budget is %d)", maxInstr, MaxRunInstructions)
	if maxInstr*20 > MaxRunInstructions {
		t.Errorf("a run takes %d instructions: the budget is too near", maxInstr)
	}
}

// The six attributes of every class at every level equal contentc's class_stat_base (computed by
// aocexpr alone, in float64).
func TestAttributesAgreeWithClassStatBase(t *testing.T) {
	b := compileReal(t, ModeFloat32)
	db, err := content.Open(contentDBPath(t))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	names := []string{"Strength", "Dexterity", "Constitution", "Intelligence", "Wisdom", "Mentality"}
	n := 0
	for _, cl := range b.Classes {
		if int(cl.Enum) >= NpcClassBase {
			continue
		}
		cname := b.Strings[cl.Name]
		for level := 1; level <= 50; level++ {
			want, err := db.ClassStatBase(cname, level)
			if err != nil {
				t.Fatal(err)
			}
			res, err := b.Run(Inputs{Class: int(cl.Enum), Level: level}, nil)
			if err != nil {
				t.Fatal(err)
			}
			for _, o := range res.Stats {
				name := b.Strings[b.Stats[o.Stat].Name]
				for _, a := range names {
					if name == a {
						n++
						if w := want[a].Value; o.Value != w {
							t.Errorf("%s level %d %s = %v, class_stat_base has %v", cname, level, a, o.Value, w)
						}
					}
				}
			}
		}
	}
	if n != 8*50*6 {
		t.Errorf("compared %d attribute values, want %d", n, 8*50*6)
	}
}

// == C | O | A | C_Y_B ==

// The compiled engine agrees with the reference engine (aocexpr over the text) on every stat of
// every class, at several levels, with and without the class's starting kit, in all three modes.
func TestRealAgreesWithAocexpr(t *testing.T) {
	g := loadReal(t)
	gear, err := OpenGear(contentDBPath(t))
	if err != nil {
		t.Fatal(err)
	}
	defer gear.Close()
	for _, mode := range []Mode{ModeFloat32, ModeDouble, ModeSingle} {
		b := compileReal(t, mode)
		levels := []int{1, 13, 20, 50}
		if mode != ModeFloat32 {
			levels = []int{1, 20}
		}
		for _, cl := range b.Classes {
			if int(cl.Enum) >= NpcClassBase {
				continue
			}
			cname := b.Strings[cl.Name]
			kit, err := gear.StartingKit(cname)
			if err != nil {
				t.Fatal(err)
			}
			mods, err := gear.Mods(kit, RoundNone)
			if err != nil {
				t.Fatal(err)
			}
			tags, err := gear.Tags(kit)
			if err != nil {
				t.Fatal(err)
			}
			for _, level := range levels {
				for _, withKit := range []bool{false, true} {
					in := Inputs{Class: int(cl.Enum), Level: level}
					if withKit {
						in.Mods, in.Equipped = mods, tags
					}
					got, err := b.Run(in, nil)
					if err != nil {
						t.Fatal(err)
					}
					want, err := RefRun(g, in, nil, mode)
					if err != nil {
						t.Fatalf("%v %s level %d: RefRun: %v", mode, cname, level, err)
					}
					sameResults(t, fmt.Sprintf("%v %s level %d kit %v", mode, cname, level, withKit), got, want)
					if got.Faults != 0 {
						t.Errorf("%v %s level %d: %d faults", mode, cname, level, got.Faults)
					}
				}
			}
		}
	}
}

// The state the formulas read is answered by aocexpr.State: an effect, a bought skill, combat.
func TestRealStateChangesStats(t *testing.T) {
	b := compileReal(t, ModeFloat32)
	g := loadReal(t)
	in := Inputs{Class: 4, Level: 20} // a Fighter: its Momentum stats read effects and skills
	base, err := b.Run(in, nil)
	if err != nil {
		t.Fatal(err)
	}
	st := &TableState{FlagCombat: true, Professions: map[string]float64{"Fishing": 6}}
	moved, err := b.Run(in, st)
	if err != nil {
		t.Fatal(err)
	}
	cmp := func(res *Result) map[string]float64 {
		out := map[string]float64{}
		for _, s := range res.Stats {
			out[b.Strings[b.Stats[s.Stat].Name]] = s.Value
		}
		return out
	}
	a, c := cmp(base), cmp(moved)
	// Out of combat the regeneration is five times what it is in combat; the fishing power
	// follows the profession level.
	if h, hc := a["HealthRegeneration"], c["HealthRegeneration"]; hc == 0 || math.Abs(h-5*hc) > 1e-5*h {
		t.Errorf("HealthRegeneration out of combat %v, in combat %v", h, hc)
	}
	if c["Artisan_Gathering_Fishing_Sport_Power"] != 60 || a["Artisan_Gathering_Fishing_Sport_Power"] != 10 {
		t.Errorf("fishing power %v (level 1), %v (level 6)", a["Artisan_Gathering_Fishing_Sport_Power"], c["Artisan_Gathering_Fishing_Sport_Power"])
	}
	// And the reference engine says the same.
	want, err := RefRun(g, in, st, ModeFloat32)
	if err != nil {
		t.Fatal(err)
	}
	sameResults(t, "fighter in combat", moved, want)
}

// ~ CoA - C-Y-B ~

// The starting kits: each class's kit gives items, and the Tank's shield (an item tag, in the Secondary
// slot) is what turns on BlockChance and BlockMitigation.
func TestStartingKitsAndShield(t *testing.T) {
	b := compileReal(t, ModeFloat32)
	gear, err := OpenGear(contentDBPath(t))
	if err != nil {
		t.Fatal(err)
	}
	defer gear.Close()
	for _, cl := range b.Classes {
		if int(cl.Enum) >= NpcClassBase {
			continue
		}
		name := b.Strings[cl.Name]
		kit, err := gear.StartingKit(name)
		if err != nil || len(kit) < 8 {
			t.Fatalf("%s: kit %v, %v", name, kit, err)
		}
		mods, err := gear.Mods(kit, RoundNone)
		if err != nil || len(mods) == 0 {
			t.Fatalf("%s: mods %v, %v", name, mods, err)
		}
	}
	kit, _ := gear.StartingKit("Tank")
	mods, _ := gear.Mods(kit, RoundNone)
	tags, _ := gear.Tags(kit)
	shield := false
	for _, tg := range tags {
		if tg.Slot == "Secondary" && tg.Tag == "Item.Gear.Weapon.Shield" {
			shield = true
		}
	}
	if !shield {
		t.Fatalf("the Tank's Secondary item has no shield tag: %v", tags)
	}
	with, err := b.Run(Inputs{Class: classEnum("Tank"), Level: 1, Mods: mods, Equipped: tags}, nil)
	if err != nil {
		t.Fatal(err)
	}
	without, _ := b.Run(Inputs{Class: classEnum("Tank"), Level: 1, Mods: mods}, nil)
	get := func(r *Result, name string) float64 {
		for _, s := range r.Stats {
			if b.Strings[b.Stats[s.Stat].Name] == name {
				return s.Value
			}
		}
		t.Fatalf("no stat %s", name)
		return 0
	}
	if d := get(with, "BlockChance") - get(without, "BlockChance"); math.Abs(d-0.15) > 1e-6 {
		t.Errorf("a shield adds %v to BlockChance, want 0.15", d)
	}
	if get(with, "BlockMitigation") >= get(without, "BlockMitigation") {
		t.Errorf("a shield should lower BlockMitigation (0.75 against 0.9): %v against %v", get(with, "BlockMitigation"), get(without, "BlockMitigation"))
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"sort"
	"strconv"

	"aocrop/services/internal/content"
)

// The damage part of the shared corpus (plan E1, stage 3): a DamageProfile, views of the stats of two combatants, and calls of the seven damage functions
// with the bits each must give. Both engines hold to them (damage.go here, server/worldd/stats/damage.cpp there).
//
//	dprofile NAME                 the profile of the following lines, up to "end":
//	  dp KEY GUID                     a stat of the profile (content.CombatProfileKeys: PowerMult, Physical.Damage ...)
//	  de TAG DAMAGE MULT ADDMULT MIT  an element's four stats
//	dview NAME                    the stats of one combatant, up to "end":
//	  v GUID F64                      the current value of a stat (one not listed reads 0)
//	damage NAME PROFILE FN TAG PHYS FLAGS INSTIGATOR TARGET WANT
//	                              the function FN (statprog host number 20 to 26) of the hit: TAG the element tag ("-" for none), PHYS 1 when it scales
//	                              with the physical aspect, FLAGS the EHitFlag bits; WANT the float64 bits it gives

// CorpusProfile is a named DamageProfile of the corpus.
type CorpusProfile struct {
	Name string
	P    *DamageProfile
}

// CorpusView is a named set of stat values.
type CorpusView struct {
	Name string
	V    MapView
}

// MapView is a StatView read from a map.
type MapView map[int64]float64

// built with Cyb and C O A

// StatValue is the value of the stat, 0 when the view has none.
func (m MapView) StatValue(guid int64) float64 { return m[guid] }

// CorpusDamage is one call of a damage function.
type CorpusDamage struct {
	Name, Profile, Instigator, Target string
	Fn                                int
	Hit                               DamageHit
	Want                              uint64
}

// Combat is the profile as the content.db's combat tables hold it.
func (p *DamageProfile) Combat() *content.Combat {
	c := &content.Combat{Profile: map[string]int64{}, Config: map[string]float64{}}
	c.Profile["PowerMult"], c.Profile["PowerAddMult"] = p.PowerMult, p.PowerAddMult
	c.Profile["DamagePowerMult"], c.Profile["DamagePowerAddMult"] = p.DamagePowerMult, p.DamagePowerAddMult
	c.Profile["Corruption"] = p.CorruptionStat
	for _, a := range []struct {
		name string
		a    DamageAspect
	}{{"Physical", p.Physical}, {"Magical", p.Magical}} {
		c.Profile[a.name+".Damage"], c.Profile[a.name+".DamageMult"] = a.a.Damage, a.a.DamageMult
		c.Profile[a.name+".DamageAddMult"], c.Profile[a.name+".Mitigation"] = a.a.DamageAddMult, a.a.Mitigation
		c.Profile[a.name+".Penetration"], c.Profile[a.name+".CriticalPower"] = a.a.Penetration, a.a.CriticalPower
		c.Profile[a.name+".CriticalMitigation"], c.Profile[a.name+".BlockMitigation"] = a.a.CriticalMitigation, a.a.BlockMitigation
	}
	for _, e := range p.Elements {
		c.Elements = append(c.Elements, content.CombatElement{Tag: e.Tag, Damage: e.Damage, DamageMult: e.DamageMult, DamageAddMult: e.DamageAddMult, Mitigation: e.Mitigation})
	}
	return c
}

// Guids are every stat the profile reads, sorted.
func (p *DamageProfile) Guids() []int64 {
	c := p.Combat()
	seen := map[int64]bool{}
	var out []int64
	add := func(g int64) {
		if !seen[g] {
			seen[g] = true
			out = append(out, g)
		}
	}
	for _, k := range content.CombatProfileKeys {
		add(c.Profile[k])
	}
	for _, e := range c.Elements {
		add(e.Damage)
		add(e.DamageMult)
		add(e.DamageAddMult)
		add(e.Mitigation)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

func (w *corpusWriter) profile(name string, p *DamageProfile) {
	w.line("dprofile %s", name)
	c := p.Combat()
	for _, k := range content.CombatProfileKeys {
		w.line("dp %s %d", k, c.Profile[k])
	}
	for _, e := range c.Elements {
		w.line("de %s %d %d %d %d", e.Tag, e.Damage, e.DamageMult, e.DamageAddMult, e.Mitigation)
	}
	w.line("end")
}

func (w *corpusWriter) view(name string, v MapView) {
	w.line("dview %s", name)
	guids := make([]int64, 0, len(v))
	for g := range v {
		guids = append(guids, g)
	}
	sort.Slice(guids, func(i, j int) bool { return guids[i] < guids[j] })
	for _, g := range guids {
		w.line("v %d %s", g, f64hex(v[g]))
	}
	w.line("end")
}

// ~ C | Y | B - C-O-A ~

// damageCases writes the profile, the views and the calls: every function over a spread of hits of the instigator and the target views.
func (w *corpusWriter) damageCases(profName string, p *DamageProfile, views []CorpusView) {
	w.profile(profName, p)
	for _, v := range views {
		w.view(v.Name, v.V)
	}
	tags := []string{"", "Element.Physical.Slash", "Element.Magical.Fire", "Element.Magical.Spirit", "Element.Physical.Unknown"}
	flags := []uint8{0, HitFlagCrit, HitFlagBlock, HitFlagBlock | HitFlagActiveBlock, HitFlagCrit | HitFlagBlock, HitFlagLethal | HitFlagMiss}
	n := 0
	for i, iv := range views {
		for _, d := range []int{1, 2} {
			tv := views[(i+d)%len(views)]
			for ti, tag := range tags {
				physical := ti%2 == 0
				hit := DamageHit{Tag: tag, Physical: physical, Flags: flags[(n+ti)%len(flags)]}
				for fn := 20; fn <= 26; fn++ {
					// each function is run for a hit with and without the flags it reads
					for _, fl := range []uint8{0, flags[(n+fn)%len(flags)]} {
						h := hit
						h.Flags = fl
						want := p.Calculate(fn, iv.V, tv.V, h)
						ptag := tag
						if ptag == "" {
							ptag = "-"
						}
						ph := 0
						if physical {
							ph = 1
						}
						n++
						w.line("damage d%d %s %d %s %d %d %s %s %s", n, profName, fn, ptag, ph, h.Flags, iv.Name, tv.Name, f64hex(want))
					}
				}
			}
		}
	}
}

// syntheticView is a stat view of the profile's stats with arbitrary values (a double that is no float32, a negative, a zero, a large one), made from a seed.
func syntheticView(p *DamageProfile, seed uint64) MapView {
	rng := SplitMix{State: seed}
	v := MapView{}
	for _, g := range p.Guids() {
		x := rng.RandDouble()
		switch rng.Next() % 5 {
		case 0:
			x = 0
		case 1:
			x = x*400 - 100
		case 2:
			x = 1 + x/3
		case 3:
			x = math.Round(x*1000) / 1000
		}
		v[g] = x
	}
	return v
}

// parseDamage handles the damage lines of ParseCorpus; ok is false for a line that is none of them.
func (c *Corpus) parseDamageLine(f []string, cur *damageCursor) (ok bool, err error) {
	u := func(s string, base, bits int) uint64 {
		v, e := strconv.ParseUint(s, base, bits)
		if e != nil {
			panic(fmt.Errorf("bad number %q", s))
		}
		return v
	}
	ok = true
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("%v in %v", r, f)
		}
	}()
	switch f[0] {
	case "dprofile":
		if len(f) != 2 {
			panic(fmt.Errorf("dprofile NAME"))
		}
		cur.profile = &CorpusProfile{Name: f[1], P: &DamageProfile{}}
		cur.keys = &content.Combat{Profile: map[string]int64{}}
	case "dp":
		if cur.profile == nil || len(f) != 3 {
			panic(fmt.Errorf("dp KEY GUID inside a dprofile"))
		}
		cur.keys.Profile[f[1]] = int64(u(f[2], 10, 64))
	case "de":
		if cur.profile == nil || len(f) != 6 {
			panic(fmt.Errorf("de TAG D M A MIT inside a dprofile"))
		}
		cur.keys.Elements = append(cur.keys.Elements, content.CombatElement{Tag: f[1], Damage: int64(u(f[2], 10, 64)), DamageMult: int64(u(f[3], 10, 64)),
			DamageAddMult: int64(u(f[4], 10, 64)), Mitigation: int64(u(f[5], 10, 64))})
	case "dview":
		if len(f) != 2 {
			panic(fmt.Errorf("dview NAME"))
		}
		cur.view = &CorpusView{Name: f[1], V: MapView{}}
	case "v":
		if cur.view == nil || len(f) != 3 {
			panic(fmt.Errorf("v GUID F64 inside a dview"))
		}
		cur.view.V[int64(u(f[1], 10, 64))] = math.Float64frombits(u(f[2], 16, 64))
	case "damage":
		if len(f) != 10 {
			panic(fmt.Errorf("damage NAME PROFILE FN TAG PHYS FLAGS INSTIGATOR TARGET WANT"))
		}
		tag := f[4]
		if tag == "-" {
			tag = ""
		}
		c.Damages = append(c.Damages, CorpusDamage{Name: f[1], Profile: f[2], Fn: int(u(f[3], 10, 32)), Hit: DamageHit{Tag: tag, Physical: f[5] == "1", Flags: uint8(u(f[6], 10, 8))},
			Instigator: f[7], Target: f[8], Want: u(f[9], 16, 64)})
	case "end":
		switch {
		case cur.profile != nil:
			p, e := ProfileFromCombat(cur.keys)
			if e != nil {
				panic(e)
			}
			cur.profile.P = p
			c.Profiles = append(c.Profiles, *cur.profile)
		case cur.view != nil:
			c.Views = append(c.Views, *cur.view)
		default:
			ok = false
		}
		cur.profile, cur.view, cur.keys = nil, nil, nil
	default:
		ok = false
	}
	return ok, err
}

// damageCursor is where the damage lines of a corpus file are being read.
type damageCursor struct {
	profile *CorpusProfile
	keys    *content.Combat
	view    *CorpusView
}

func (cur *damageCursor) open() bool { return cur.profile != nil || cur.view != nil }

// replayDamage checks every damage call against the engine.
func (c *Corpus) replayDamage() error {
	profiles := map[string]*DamageProfile{}
	for _, p := range c.Profiles {
		profiles[p.Name] = p.P
	}
	views := map[string]MapView{}
	for _, v := range c.Views {
		views[v.Name] = v.V
	}
	for _, d := range c.Damages {
		p, iv, tv := profiles[d.Profile], views[d.Instigator], views[d.Target]
		if p == nil || iv == nil || tv == nil {
			return fmt.Errorf("damage %s: no profile %s or view %s or %s", d.Name, d.Profile, d.Instigator, d.Target)
		}
		if got := p.Calculate(d.Fn, iv, tv, d.Hit); math.Float64bits(got) != d.Want {
			return fmt.Errorf("damage %s: function %d gives %016x, the corpus has %016x", d.Name, d.Fn, math.Float64bits(got), d.Want)
		}
	}
	return nil
}

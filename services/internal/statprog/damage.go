// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"

	"aocrop/services/internal/content"
	"strings"
)

// The damage formulas (plan E1, stage 3): the seven functions the design data's DamageMod equation calls and the game implements natively
// (GameSystemsPlugin/Private/Combat/Formulas/DamageFormula.cpp: "calculate_damage" and its six companions, registered by name as the expression functions
// CalculateDamage, CalculateDamageMultiplier, CalculateDamageAdditiveMultiplier, CalculateDamageMitigation, CalculateDamageCritical,
// CalculateDamagePenetration and CalculateDamageDampening).
//
//	DamageMod = GlobalMod * (Base * CalculateDamage() + BaseAdd) * DamageMult * DamageAddMult * CritDamage * ContestedMit * Dampening * ... (a StatEquationType)
//
// Each function reads a few stats of the instigator or the target by the references of the combat config (DefaultCombat.ini, Stats.DamageProfile: the Power
// and Mitigation stats of the physical and the magical aspect and one set per element) and the hit in progress: its element tag, whether it scales with
// the physical or the magical aspect, and its flags (EHitFlag: Crit 1, Block 4, ActiveBlock 64). The code below is read from the game's own functions in the
// client exe (the seven handlers registered at 0x1463ff3b0; the arithmetic is the exe's: float32 reads, float32 products and sums in the order the machine code
// runs them, the final value widened to double):
//
//	0x146178250  CalculateDamage             aspect.Damage + element.Damage                           (the element's term is 0 when the hit has no element of the table)
//	0x146178de0  ...Multiplier               DamagePowerMult * PowerMult * aspect.DamageMult * element.DamageMult   (each 1.0 when absent)
//	0x1461784b0  ...AdditiveMultiplier       PowerAddMult + 1.0 + DamagePowerAddMult + aspect.DamageAddMult + element.DamageAddMult
//	0x146178b10  ...Mitigation               1 - (critMit * aspect.Mitigation * element.Mitigation * blockMit): the instigator is the target here, the stats are the
//	                                         target's; critMit is the aspect's CriticalMitigation when the hit crit, blockMit its BlockMitigation when the hit was
//	                                         blocked and not actively (flags & 0x44 == 4)
//	0x146178700  ...Critical                 the aspect's CriticalPower when the hit crit, else 1.0
//	0x146179030  ...Penetration              1 - aspect.Penetration
//	0x146178890  ...Dampening                1 - the corruption dampening (0 for everything but a corrupted player fighting a combatant: modelled as 0)
//
// The twin of this file is server/worldd/stats/damage.cpp; the shared corpus (corpus.go, "damage" cases) holds both to the same bits.

// DamageAspect is the stat references of one aspect of damage (physical or magical).
type DamageAspect struct {
	Damage, DamageMult, DamageAddMult, Mitigation, Penetration, CriticalPower, CriticalMitigation, BlockMitigation int64
}

// C:Y:B : C O A

// DamageElement is the stat references of one element (Element.Physical.Slash, Element.Magical.Fire ...).
type DamageElement struct {
	Tag                                           string
	Damage, DamageMult, DamageAddMult, Mitigation int64
}

// DamageProfile is DefaultCombat.ini's DamageProfile as stat guids (content.db's combat_profile and combat_element).
type DamageProfile struct {
	PowerMult, PowerAddMult, DamagePowerMult, DamagePowerAddMult int64
	Physical, Magical                                            DamageAspect
	CorruptionStat                                               int64
	Elements                                                     []DamageElement
}

// Element is the entry for a tag, nil when the table has none.
func (p *DamageProfile) Element(tag string) *DamageElement {
	for i := range p.Elements {
		if strings.EqualFold(p.Elements[i].Tag, tag) { // gameplay tags are FNames: without regard to case, as every other tag compare
			return &p.Elements[i]
		}
	}
	return nil
}

// HitFlags are EHitFlag values.
const (
	HitFlagCrit        = 1
	HitFlagLethal      = 2
	HitFlagBlock       = 4
	HitFlagMiss        = 8
	HitFlagReflect     = 16
	HitFlagRedirect    = 32
	HitFlagActiveBlock = 64
)

// DamageHit is the hit in progress as the damage functions see it.
type DamageHit struct {
	Tag      string // the element tag of the hit ("Element.Physical.Slash"); "" none
	Physical bool   // the hit scales with the physical aspect (else the magical one)
	Flags    uint8  // EHitFlag bits
}

// StatView reads the current value of a stat of an actor by guid (0 for a stat it has none of).
type StatView interface {
	StatValue(guid int64) float64
}

func (p *DamageProfile) aspect(physical bool) *DamageAspect {
	if physical {
		return &p.Physical
	}
	return &p.Magical
}

// read is a stat as a float32 (the exe reads stats as floats).
func read(v StatView, guid int64) float32 { return float32(v.StatValue(guid)) }

// CalculateDamage is the power of the hit: the aspect's Power stat plus the element's own damage stat.
func (p *DamageProfile) CalculateDamage(instigator StatView, hit DamageHit) float64 {
	a := p.aspect(hit.Physical)
	var base, elem float32
	base = read(instigator, a.Damage)
	if e := p.Element(hit.Tag); e != nil {
		elem = read(instigator, e.Damage)
	}
	sum := elem + base
	return float64(sum)
}

// CalculateDamageMultiplier is the product of the multipliers of the power, the damage, the aspect and the element.
func (p *DamageProfile) CalculateDamageMultiplier(instigator StatView, hit DamageHit) float64 {
	a := p.aspect(hit.Physical)
	var powerMult, damageMult, aspectMult float32 = 1, 1, 1
	var elemMult float32 = 1
	powerMult = read(instigator, p.PowerMult)
	damageMult = read(instigator, p.DamagePowerMult)
	aspectMult = read(instigator, a.DamageMult)
	if e := p.Element(hit.Tag); e != nil {
		elemMult = read(instigator, e.DamageMult)
	}
	m1 := damageMult * powerMult
	m2 := m1 * aspectMult
	m3 := m2 * elemMult
	return float64(m3)
}

// CalculateDamageAdditiveMultiplier is 1 plus the additive bonuses of the power, the damage, the aspect and the element.
func (p *DamageProfile) CalculateDamageAdditiveMultiplier(instigator StatView, hit DamageHit) float64 {
	a := p.aspect(hit.Physical)
	var powerAdd, damageAdd, aspectAdd, elemAdd float32
	powerAdd = read(instigator, p.PowerAddMult)
	damageAdd = read(instigator, p.DamagePowerAddMult)
	aspectAdd = read(instigator, a.DamageAddMult)
	if e := p.Element(hit.Tag); e != nil {
		elemAdd = read(instigator, e.DamageAddMult)
	}
	s1 := powerAdd + 1
	s2 := s1 + damageAdd
	s3 := s2 + aspectAdd
	s4 := s3 + elemAdd
	return float64(s4)
}

// -- Cyb x C-O-A --

// CalculateDamageMitigation is the share of the damage the target's mitigation takes off: 1 minus the product of what the target lets through.
func (p *DamageProfile) CalculateDamageMitigation(target StatView, hit DamageHit) float64 {
	a := p.aspect(hit.Physical)
	var crit, aspect, block float32 = 1, 1, 1
	var elem float32 = 1
	aspect = read(target, a.Mitigation)
	if hit.Flags&HitFlagCrit != 0 {
		crit = read(target, a.CriticalMitigation)
	}
	if hit.Flags&(HitFlagBlock|HitFlagActiveBlock) == HitFlagBlock {
		block = read(target, a.BlockMitigation)
	}
	if e := p.Element(hit.Tag); e != nil {
		elem = read(target, e.Mitigation)
	}
	m1 := crit * aspect
	m2 := m1 * elem
	m3 := m2 * block
	return 1.0 - float64(m3)
}

// CalculateDamageCritical is the damage multiplier of a critical hit (the aspect's CriticalPower), 1 for a hit that is none.
func (p *DamageProfile) CalculateDamageCritical(instigator StatView, hit DamageHit) float64 {
	var v float32 = 1
	if hit.Flags&HitFlagCrit != 0 {
		v = read(instigator, p.aspect(hit.Physical).CriticalPower)
	}
	return float64(v)
}

// CalculateDamagePenetration is the share of the target's mitigation the instigator ignores: 1 minus the aspect's Penetration stat (a stat that is the share
// that is NOT penetrated: 1 with no penetration).
func (p *DamageProfile) CalculateDamagePenetration(instigator StatView, hit DamageHit) float64 {
	var v float32 = 1
	v = read(instigator, p.aspect(hit.Physical).Penetration)
	return 1.0 - float64(v)
}

// CalculateDamageDampening is 1 minus the corruption dampening: modelled as none (a character with no corruption, which is every character this server has).
func (p *DamageProfile) CalculateDamageDampening(instigator, target StatView, hit DamageHit) float64 {
	_ = instigator
	_ = target
	_ = hit
	return 1.0
}

// Calculate runs the function with the host number HostCalcDamage..HostCalcLast (aocexpr/hostfn.go): the instigator's stats for all but the mitigation, which
// is the target's.
func (p *DamageProfile) Calculate(fn int, instigator, target StatView, hit DamageHit) float64 {
	switch fn {
	case 20:
		return p.CalculateDamage(instigator, hit)
	case 21:
		return p.CalculateDamageMultiplier(instigator, hit)
	case 22:
		return p.CalculateDamageAdditiveMultiplier(instigator, hit)
	case 23:
		return p.CalculateDamageMitigation(target, hit)
	case 24:
		return p.CalculateDamageCritical(instigator, hit)
	case 25:
		return p.CalculateDamagePenetration(instigator, hit)
	case 26:
		return p.CalculateDamageDampening(instigator, target, hit)
	}
	return 0
}

// ProfileFromCombat is the DamageProfile of the content database's combat tables.
func ProfileFromCombat(c *content.Combat) (*DamageProfile, error) {
	var missing string
	get := func(key string) int64 {
		g, ok := c.Profile[key]
		if !ok && missing == "" {
			missing = key
		}
		return g
	}
	aspect := func(a string) DamageAspect {
		return DamageAspect{
			Damage: get(a + ".Damage"), DamageMult: get(a + ".DamageMult"), DamageAddMult: get(a + ".DamageAddMult"), Mitigation: get(a + ".Mitigation"),
			Penetration: get(a + ".Penetration"), CriticalPower: get(a + ".CriticalPower"), CriticalMitigation: get(a + ".CriticalMitigation"),
			BlockMitigation: get(a + ".BlockMitigation"),
		}
	}
	p := &DamageProfile{
		PowerMult: get("PowerMult"), PowerAddMult: get("PowerAddMult"), DamagePowerMult: get("DamagePowerMult"), DamagePowerAddMult: get("DamagePowerAddMult"),
		Physical: aspect("Physical"), Magical: aspect("Magical"), CorruptionStat: get("Corruption"),
	}
	if missing != "" {
		return nil, fmt.Errorf("statprog: the content.db's combat_profile has no %s (built by an older contentc?)", missing)
	}
	for _, e := range c.Elements {
		p.Elements = append(p.Elements, DamageElement{Tag: e.Tag, Damage: e.Damage, DamageMult: e.DamageMult, DamageAddMult: e.DamageAddMult, Mitigation: e.Mitigation})
	}
	return p, nil
}

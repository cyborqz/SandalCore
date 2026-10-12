// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"strconv"

	"aocrop/services/internal/aocexpr"
)

// Effect state (plan D2): the active effects of a character and the skill ranks it holds, as inputs of a run.
//
// An active effect has stacks and the stat mods it applies, each already a number (the value of ONE stack: the host
// ran the mod's expression program with Evaluator.Eval when it applied the effect). A mod changes its stat after the
// equipment, in the stat's own form:
//
//	an additive stat               value += mod * stacks                      (MoveSpeedMult, Evasion, MaxStamina)
//	a multiplying stat             value *= mod, once per stack               (HealingMult: 1.1025 under 1.05 is 1.157625)
//	a multiplying "compliment"     value *= (1 - mod), once per stack         (Mitigation: 0.9652 under 0.03 is 0.936244)
//
// The form is the stat's: bMultiplyStatMods (StatMulMods) chooses between the first and the other two, and
// bMultiplyCompliment (StatMulCompl) between multiplying by the mod and by one minus it (24 of the 41 multiplying stats are
// complements: the mitigations and the penetrations). The caller never says which. The effects apply in the order given and
// each effect's mods in theirs; the stat's own limits (ValueMin, ValueMax) bind the result.
//
// The same effects answer the state functions of the formulas: GetEffectCount is the stacks of the effects with the record,
// GetEffectCountByTag the stacks of the effects one of whose tags matches (a tag matches itself and its descendants,
// "Debuff.Staggered" matches "Debuff"), and GetSkillPurchased is true for a rank the character holds (a purchased one or
// an auto-granted one: the caller gives both). Whatever aocexpr.State says besides is added, so a host that tracks some
// effects elsewhere can keep doing it.

// EffectMod is a stat mod of an effect: the stat and the value of one stack.
type EffectMod struct {
	Stat  int64
	Value float64
}

// ActiveEffect is an effect on the character.
type ActiveEffect struct {
	Effect aocexpr.Ref // the Effect record (type id and guid): what GetEffectCount asks for
	Stacks int         // 0 (it counts for nothing) to MaxEffectStacks
	Tags   []string    // the effect's gameplay tags, for GetEffectCountByTag
	Mods   []EffectMod
}

// SkillRank is a skill rank the character holds: GetSkillPurchased(class tree, skill).
type SkillRank struct {
	Class, Skill aocexpr.Ref
}

// -- CoA x C Y B --

// maxEffectApplications bounds the multiplications a run does for its effects (stacks times mods, summed).
const maxEffectApplications = 1 << 20

// checkEffects refuses effects a run cannot take: stacks outside 0..MaxEffectStacks, more than MaxActiveEffects effects, or
// more multiplications than maxEffectApplications.
func checkEffects(fx []ActiveEffect) error {
	if len(fx) > MaxActiveEffects {
		return fmt.Errorf("statprog: %d active effects (the most is %d)", len(fx), MaxActiveEffects)
	}
	apps := 0
	for i, e := range fx {
		if e.Stacks < 0 || e.Stacks > MaxEffectStacks {
			return fmt.Errorf("statprog: effect %d has %d stacks (0 to %d)", i, e.Stacks, MaxEffectStacks)
		}
		apps += e.Stacks * len(e.Mods)
		if apps > maxEffectApplications {
			return fmt.Errorf("statprog: the effects ask for more than %d applications", maxEffectApplications)
		}
	}
	return nil
}

// effectApp is one mod of one effect, for a stat.
type effectApp struct {
	value  float64
	stacks int
}

// ExprHost is what the host of an ability expression gives it: the three inputs the character's stats do not hold.
type ExprHost interface {
	// AbilityProgress is GetAbilityProgressRatio(group): how far the running ability of the montage group is, 0 to 1.
	AbilityProgress(group string) float64
	// EffectCountByInstigator is GetEffectCountByRecordAndInstigator: the stacks of the effect that the instigator
	// applied (the caster itself: there are no other actors yet, so it is the target too).
	EffectCountByInstigator(effect aocexpr.Ref) float64
	// RandDouble is RandDouble(): a number in [0, 1) from the host's seeded source.
	RandDouble() float64
	// HitHasTag is CheckForDamageTag(GetHitInProgress(), tag): the hit in progress, or its ability, has the gameplay tag.
	HitHasTag(tag string) bool
	// HitHasFlag is HitFlagsContains(GetHitFlags(hit, target), EHitFlag::flag): the hit on its target has the flag ("Crit").
	HitHasFlag(flag string) bool
	// HitTargets is GetNumTargets(GetHitInProgress()).
	HitTargets() float64
	// XPDebtRatio is GetXPDebtRatio of the instigator.
	XPDebtRatio() float64
	// HostFn is a named host function (the Host... constants of format.go): its string operand and the numbers it takes.
	HostFn(fn int, name string, args []float64) float64
}

// NoHost is an ExprHost with nothing running: progress 0, no effect applied by anyone, every random number 0, no hit in progress (no tag,
// no flag, no targets) and no experience debt.
type NoHost struct{}

func (NoHost) AbilityProgress(string) float64              { return 0 }
func (NoHost) EffectCountByInstigator(aocexpr.Ref) float64 { return 0 }
func (NoHost) RandDouble() float64                         { return 0 }
func (NoHost) HitHasTag(string) bool                       { return false }
func (NoHost) HitHasFlag(string) bool                      { return false }
func (NoHost) HitTargets() float64                         { return 0 }
func (NoHost) XPDebtRatio() float64                        { return 0 }
func (NoHost) HostFn(int, string, []float64) float64       { return 0 }

// SplitMix is the random source of the tests and the shared corpus: SplitMix64, whose output the C++ engine
// (stats::SplitMix) reproduces bit for bit. RandDouble is the top 53 bits of the next output over 2^53.
type SplitMix struct{ State uint64 }

// Next is the next 64-bit output.
func (s *SplitMix) Next() uint64 {
	s.State += 0x9E3779B97F4A7C15
	z := s.State
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
	z = (z ^ (z >> 27)) * 0x94D049BB133111EB
	return z ^ (z >> 31)
}

// RandDouble is a number in [0, 1).
func (s *SplitMix) RandDouble() float64 { return float64(s.Next()>>11) * (1.0 / 9007199254740992.0) }

// TableHost is an ExprHost read from tables, with a SplitMix source: the host of the tests and the corpus.
type TableHost struct {
	Progress     map[string]float64
	ByInstigator map[aocexpr.Ref]float64
	Rand         SplitMix
	Tags, Flags  map[string]bool // the hit in progress's gameplay tags and EHitFlag names
	Targets      float64
	XPDebt       float64
	Fns          map[string]float64 // HostFn answers by "FN|NAME" (the numbers it is given are added to the answer)
	// Nodes answers the city node reads (aocexpr.HostNodeBiome ... HostNodeLast) when it is set, instead of Fns (Nodes 1, stage 1).
	Nodes *NodeWorld
}

func (h *TableHost) AbilityProgress(g string) float64              { return h.Progress[g] }
func (h *TableHost) EffectCountByInstigator(r aocexpr.Ref) float64 { return h.ByInstigator[r] }
func (h *TableHost) RandDouble() float64                           { return h.Rand.RandDouble() }
func (h *TableHost) HitHasTag(t string) bool                       { return h.Tags[t] }
func (h *TableHost) HitHasFlag(f string) bool                      { return h.Flags[f] }
func (h *TableHost) HitTargets() float64                           { return h.Targets }
func (h *TableHost) XPDebtRatio() float64                          { return h.XPDebt }
func (h *TableHost) HostFn(fn int, name string, args []float64) float64 {
	if h.Nodes != nil && fn >= aocexpr.HostNodeBiome && fn <= aocexpr.HostNodeLast {
		v, _ := h.Nodes.HostFn(fn, args)
		return v
	}
	v := h.Fns[strconv.Itoa(fn)+"|"+name]
	if fn >= aocexpr.HostNodeBiome && fn <= aocexpr.HostNodeLast {
		return v // a node read's answer is a rank or an ordinal: the numbers it is given are not added to it
	}
	for _, a := range args {
		v += a
	}
	return v
}

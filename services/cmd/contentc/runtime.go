// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"errors"
	"fmt"
	"math"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// The ability runtime tables (plan A7): ability_runtime, effect_runtime and
// effect_stat_runtime hold, as plain numbers, what worldd needs to run Sprint,
// Dodge and Rest. They are resolved from the rows buildSkills wrote and the
// rules buildStaminaRules found, by running each formula (package aocexpr) in
// the world the character is in at login: out of combat, no skill bought, no
// effect on it, and, for the Sprint's cost and the movement-dependent
// formulas, moving or standing still as the formula asks. A formula that
// reads anything else, or a shape the runtime does not know, stops the build
// with the row's name: worldd never has to guess what a number meant.

// runtimeAbilities are the abilities worldd runs, in the order of their rows.
var runtimeAbilities = []int64{content.GuidSprint, content.GuidDodge, content.GuidRest}

// buildAbilityRuntime writes the three tables and returns the number of ability and effect rows.
func buildAbilityRuntime(w *writer, sk *skillSet, statNames map[int64]string) (abilities, effects int, err error) {
	if sk.disables == nil {
		return 0, 0, errors.New("the ability runtime needs the stamina rules first: stamina_regen_disable is not built")
	}
	effectGuids := []int64{}
	seen := map[int64]bool{}
	addEffect := func(g int64) {
		if g != 0 && !seen[g] {
			seen[g] = true
			effectGuids = append(effectGuids, g)
		}
	}
	for _, guid := range runtimeAbilities {
		a := sk.abilities[guid]
		if a == nil {
			return 0, 0, fmt.Errorf("no Ability row %d in the ability set", guid)
		}
		row, err := abilityRuntimeRow(sk, a)
		if err != nil {
			return 0, 0, err
		}
		if err := w.exec(`INSERT INTO ability_runtime (ability_guid, name, held_behavior, requires_out_of_combat, min_stamina,
			cost_stat_guid, cost_amount, cost_requires_moving, charge_interval, cooldown, triggers_cooldown, disable_during_cooldown,
			targets_self, effect_guid, montage_path, cancel_section, source)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			a.Guid, a.Name, row.held, b2i(row.outOfCombat), row.minStamina, row.costStat, row.costAmount, b2i(row.costMoving),
			row.chargeInterval, row.cooldown, b2i(row.triggersCooldown), b2i(row.disableDuringCooldown), b2i(row.targetsSelf),
			row.effect, row.montage, row.cancelSection, row.source); err != nil {
			return 0, 0, err
		}
		abilities++
		addEffect(row.effect)
	}
	// Each effect, then the regeneration-disabling effect its End hit applies (which is also an effect row).
	for i := 0; i < len(effectGuids); i++ {
		e := sk.effects[effectGuids[i]]
		if e == nil {
			return 0, 0, fmt.Errorf("effect %d is not in the effect set", effectGuids[i])
		}
		row, err := effectRuntimeRow(sk, e)
		if err != nil {
			return 0, 0, err
		}
		addEffect(row.endEffect)
		dis := sk.disables[e.Guid]
		if err := w.exec(`INSERT INTO effect_runtime (effect_guid, name, infinite, duration, tick_timer, stops_regen_standing,
			stops_regen_moving, end_effect_guid, end_effect_duration, source) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			e.Guid, e.Name, b2i(row.infinite), row.duration, row.tick, b2i(dis.standing), b2i(dis.moving), row.endEffect,
			row.endDuration, row.source); err != nil {
			return 0, 0, err
		}
		effects++
		mods, err := effectStatMods(sk, e, statNames)
		if err != nil {
			return 0, 0, err
		}
		for ord, m := range mods {
			if err := w.exec(`INSERT INTO effect_stat_runtime (effect_guid, ord, stat_guid, stat_name, source_stat_guid, source_stat_name,
				factor, addend) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
				e.Guid, ord, m.stat, statNames[m.stat], m.source, statNames[m.source], m.factor, m.addend); err != nil {
				return 0, 0, err
			}
		}
	}
	return abilities, effects, nil
}

// C Y B + C O A

// abilityRuntime is one ability_runtime row.
type abilityRuntime struct {
	held                  string
	outOfCombat           bool
	minStamina            float64
	costStat              int64
	costAmount            float64
	costMoving            bool
	chargeInterval        float64
	cooldown              float64
	triggersCooldown      bool
	disableDuringCooldown bool
	targetsSelf           bool
	effect                int64
	montage               string
	cancelSection         string
	source                string
}

// CoA / C:Y:B

func abilityRuntimeRow(sk *skillSet, a *designdb.Row) (abilityRuntime, error) {
	where := fmt.Sprintf("Ability %s (%d)", a.Name, a.Guid)
	f := newFields(where, a.Fields)
	held := strings.TrimPrefix(f.str("HeldAbilityBehavior"), "EHeldAbilityPressBehavior::")
	activation, cooldownExpr := f.str("ActivationRequirements"), f.str("Cooldown")
	disableDuring, targets, cancel := f.boolean("bDisableDuringCooldown"), f.int("ApplicableTargets"), f.str("CancelSectionName")
	effectKeys, effectGuids := f.keyedIds("EffectsIds")
	montages := f.list("AnimMontages")
	if f.err != nil {
		return abilityRuntime{}, f.err
	}
	r := abilityRuntime{held: held, disableDuringCooldown: disableDuring, targetsSelf: targets == 1}
	if cancel != "None" {
		r.cancelSection = cancel
	}
	for i, k := range effectKeys {
		if k == 1 {
			r.effect = effectGuids[i]
		}
	}
	if len(montages) > 0 {
		ms, ok := montages[0].(designdb.Struct)
		if !ok {
			return abilityRuntime{}, fmt.Errorf("%s: AnimMontages[0] is %T", where, montages[0])
		}
		mf := newFields(where+": AnimMontages[0]", ms)
		paths := mf.list("AbilityAnimationSoftPaths")
		if mf.err != nil {
			return abilityRuntime{}, mf.err
		}
		if len(paths) > 0 {
			p, ok := paths[0].(string)
			if !ok {
				return abilityRuntime{}, fmt.Errorf("%s: AnimMontages[0] path is %T", where, paths[0])
			}
			r.montage = p
		}
	}

	var err error
	if r.minStamina, r.outOfCombat, err = probeActivation(activation); err != nil {
		return abilityRuntime{}, fmt.Errorf("%s: ActivationRequirements: %w", where, err)
	}
	if strings.TrimSpace(cooldownExpr) != "" {
		if r.cooldown, err = aocexpr.EvalFloat(cooldownExpr, ruleEnv{}.env()); err != nil {
			return abilityRuntime{}, fmt.Errorf("%s: Cooldown: %w", where, err)
		}
	}

	costs, err := staminaCosts(a)
	if err != nil {
		return abilityRuntime{}, err
	}
	src := []string{where + ": ActivationRequirements, Cooldown, EffectsIds, AnimMontages, CancelSectionName, ApplicableTargets"}
	switch len(costs) {
	case 0:
	case 1:
		cost := costs[0]
		r.costStat = content.GuidStamina
		r.triggersCooldown = cost.TriggersCooldown
		prog, err := aocexpr.Parse(cost.Expression)
		if err != nil {
			return abilityRuntime{}, fmt.Errorf("%s cost: %w", where, err)
		}
		if r.costAmount, err = prog.EvalFloat(ruleEnv{}.env()); err != nil {
			return abilityRuntime{}, fmt.Errorf("%s cost: %w", where, err)
		}
		if r.costMoving, err = costNeedsMoving(cost.Enabled); err != nil {
			return abilityRuntime{}, fmt.Errorf("%s cost group: Enabled: %w", where, err)
		}
		src = append(src, "Costs, the stamina cost formula run out of combat with no effect on the character")
		if r.costMoving {
			// A cost charged only while moving is charged on a cadence the Ability row does not hold: the Sprint's drain.
			if a.Guid != content.GuidSprint {
				return abilityRuntime{}, fmt.Errorf("%s: its cost is charged only while moving, and the runtime knows the cadence of the Sprint's alone", where)
			}
			drain, err := sprintDrain(sk, cost.Expression)
			if err != nil {
				return abilityRuntime{}, err
			}
			r.chargeInterval = drain.interval
			src = append(src, fmt.Sprintf("charge interval: Effect %s (%d) TickTimer, the Sprint's drain by decision (spec section 7: no record references it; its tick hit takes the cost row's amount)", drain.name, drain.guid))
		}
	default:
		return abilityRuntime{}, fmt.Errorf("%s has %d stamina costs, the runtime handles at most 1", where, len(costs))
	}
	if r.held != "Smart" && r.held != "Toggle" && r.held != "Naive" {
		return abilityRuntime{}, fmt.Errorf("%s: HeldAbilityBehavior %q is none of Smart, Toggle, Naive", where, r.held)
	}
	r.source = strings.Join(src, "; ")
	return r, nil
}

// probeActivation reads an ActivationRequirements formula by running it: the Stamina at which it starts to hold
// (0 when it holds at 0 or never reads Stamina) and whether it fails in combat. It may read the caster's Stamina
// and whether it is in combat, and nothing else (any other stat is an error from the environment). An empty text
// is no requirement.
func probeActivation(expr string) (minStamina float64, outOfCombat bool, err error) {
	if strings.TrimSpace(expr) == "" {
		return 0, false, nil
	}
	prog, err := aocexpr.Parse(expr)
	if err != nil {
		return 0, false, err
	}
	holds := func(stamina float64, inCombat bool) (bool, error) {
		v, err := prog.Eval(ruleEnv{inCombat: inCombat, stats: map[int64]float64{content.GuidStamina: stamina}}.env())
		if err != nil {
			return false, err
		}
		b, ok := v.(bool)
		if !ok {
			return false, fmt.Errorf("the requirement gives %v, not a condition", v)
		}
		return b, nil
	}
	const plenty = 1e9
	out, err := holds(plenty, false)
	if err != nil {
		return 0, false, err
	}
	if !out {
		return 0, false, errors.New("the requirement fails out of combat with plenty of Stamina: no character can use the ability")
	}
	inCombat, err := holds(plenty, true)
	if err != nil {
		return 0, false, err
	}
	outOfCombat = !inCombat
	// The Stamina threshold, out of combat: bisect [0, plenty] for where the condition starts to hold.
	zero, err := holds(0, false)
	if err != nil {
		return 0, false, err
	}
	if zero {
		return 0, outOfCombat, nil
	}
	lo, hi := 0.0, plenty
	for i := 0; i < 200 && hi-lo > 1e-9; i++ {
		mid := (lo + hi) / 2
		ok, err := holds(mid, false)
		if err != nil {
			return 0, false, err
		}
		if ok {
			hi = mid
		} else {
			lo = mid
		}
	}
	min := math.Round(hi*1e6) / 1e6
	for _, s := range []float64{min * 2, min + 1, plenty} {
		if ok, _ := holds(s, false); !ok {
			return 0, false, errors.New("the requirement is not a threshold on Stamina")
		}
	}
	return min, outOfCombat, nil
}

// costNeedsMoving reads a cost group's Enabled formula: empty or always true is "always", and a formula that is true
// exactly while the character moves (in and out of combat alike) is "only while moving". Anything else is an error.
func costNeedsMoving(enabled string) (bool, error) {
	if strings.TrimSpace(enabled) == "" {
		return false, nil
	}
	prog, err := aocexpr.Parse(enabled)
	if err != nil {
		return false, err
	}
	results := map[[2]bool]bool{}
	for _, moving := range []bool{false, true} {
		for _, inCombat := range []bool{false, true} {
			v, err := prog.Eval(ruleEnv{moving: moving, inCombat: inCombat}.env())
			if err != nil {
				return false, err
			}
			b, ok := v.(bool)
			if !ok {
				return false, fmt.Errorf("gives %v, not a condition", v)
			}
			results[[2]bool{moving, inCombat}] = b
		}
	}
	alwaysTrue := results[[2]bool{false, false}] && results[[2]bool{false, true}] && results[[2]bool{true, false}] && results[[2]bool{true, true}]
	whileMoving := !results[[2]bool{false, false}] && !results[[2]bool{false, true}] && results[[2]bool{true, false}] && results[[2]bool{true, true}]
	switch {
	case alwaysTrue:
		return false, nil
	case whileMoving:
		return true, nil
	}
	return false, fmt.Errorf("is neither always true nor true exactly while moving (%v)", results)
}

// -- C O A & Cyb --

// effectRuntime is one effect_runtime row.
type effectRuntime struct {
	infinite    bool
	duration    float64
	tick        float64
	endEffect   int64
	endDuration float64
	source      string
}

func effectRuntimeRow(sk *skillSet, e *designdb.Row) (effectRuntime, error) {
	where := fmt.Sprintf("Effect %s (%d)", e.Name, e.Guid)
	f := newFields(where, e.Fields)
	infinite, durationExpr, tick := f.boolean("bInfiniteDuration"), f.str("EffectDuration"), f.float("TickTimer")
	endHits := f.ids("EndHitsIds")
	if f.err != nil {
		return effectRuntime{}, f.err
	}
	r := effectRuntime{infinite: infinite, tick: tick, source: where + ": bInfiniteDuration, EffectDuration, TickTimer, EndHitsIds"}
	if !infinite {
		if strings.TrimSpace(durationExpr) == "" {
			return effectRuntime{}, fmt.Errorf("%s: finite and no EffectDuration", where)
		}
		d, err := aocexpr.EvalFloat(durationExpr, ruleEnv{}.env())
		if err != nil {
			return effectRuntime{}, fmt.Errorf("%s: EffectDuration: %w", where, err)
		}
		r.duration = d
	}

	// The End hits: which regeneration-disabling effect they apply to the owner, and for how long.
	type applied struct {
		guid     int64
		override string
	}
	var found []applied
	for _, g := range endHits {
		if g == 0 {
			continue
		}
		h := sk.hits[g]
		if h == nil {
			return effectRuntime{}, fmt.Errorf("%s: EndHitsIds names hit %d, which is not in the hit set", where, g)
		}
		hf := newFields(fmt.Sprintf("AbilityHit %s (%d)", h.Name, h.Guid), h.Fields)
		for i, v := range hf.list("ApplyEffects") {
			s, ok := v.(designdb.Struct)
			if !ok {
				return effectRuntime{}, fmt.Errorf("%s: ApplyEffects[%d] is %T", hf.where, i, v)
			}
			ef := newFields(fmt.Sprintf("%s: ApplyEffects[%d]", hf.where, i), s)
			eg, _ := ef.id("EffectId")
			dur := ef.str("EffectDuration")
			if ef.err != nil {
				return effectRuntime{}, ef.err
			}
			if _, disables := sk.disables[eg]; disables {
				found = append(found, applied{eg, dur})
			}
		}
		for i, v := range hf.list("EffectsAppliedToSelf") {
			s, ok := v.(designdb.Struct)
			if !ok {
				return effectRuntime{}, fmt.Errorf("%s: EffectsAppliedToSelf[%d] is %T", hf.where, i, v)
			}
			ef := newFields(fmt.Sprintf("%s: EffectsAppliedToSelf[%d]", hf.where, i), s)
			eg, _ := ef.id("Effect")
			if ef.err != nil {
				return effectRuntime{}, ef.err
			}
			if _, disables := sk.disables[eg]; disables {
				found = append(found, applied{eg, ""})
			}
		}
		if hf.err != nil {
			return effectRuntime{}, hf.err
		}
	}
	for _, a := range found {
		if a.guid != found[0].guid || a.override != found[0].override {
			return effectRuntime{}, fmt.Errorf("%s: its End hits apply %d and %d, the runtime handles one regeneration pause", where, found[0].guid, a.guid)
		}
	}
	if len(found) > 0 {
		r.endEffect = found[0].guid
		target := sk.effects[r.endEffect]
		if target == nil {
			return effectRuntime{}, fmt.Errorf("%s: its End hit applies effect %d, which is not in the effect set", where, r.endEffect)
		}
		tf := newFields(fmt.Sprintf("Effect %s (%d)", target.Name, target.Guid), target.Fields)
		expr, inf := found[0].override, tf.boolean("bInfiniteDuration")
		own := tf.str("EffectDuration")
		if tf.err != nil {
			return effectRuntime{}, tf.err
		}
		if strings.TrimSpace(expr) == "" {
			expr = own
		}
		if inf && strings.TrimSpace(found[0].override) == "" {
			return effectRuntime{}, fmt.Errorf("%s: its End hit applies the infinite effect %s", where, target.Name)
		}
		d, err := aocexpr.EvalFloat(expr, ruleEnv{}.env())
		if err != nil {
			return effectRuntime{}, fmt.Errorf("%s: the duration of %s: %w", where, target.Name, err)
		}
		r.endDuration = d
		r.source += fmt.Sprintf("; the End hit applies Effect %s (%d) for %v s (its EffectDuration unless the hit gives one)", target.Name, target.Guid, d)
	}
	return r, nil
}

// statMod is one effect_stat_runtime row.
type statMod struct {
	stat, source   int64
	factor, addend float64
}

// effectStatMods reads the stat mods of an effect (StatModsIds) as a value linear in one stat of the owner.
func effectStatMods(sk *skillSet, e *designdb.Row, statNames map[int64]string) ([]statMod, error) {
	where := fmt.Sprintf("Effect %s (%d)", e.Name, e.Guid)
	f := newFields(where, e.Fields)
	ids := f.ids("StatModsIds")
	if f.err != nil {
		return nil, f.err
	}
	var out []statMod
	for _, g := range ids {
		if g == 0 {
			continue
		}
		m := sk.mods[g]
		if m == nil {
			return nil, fmt.Errorf("%s: StatModsIds names StatMod %d, which is not a row", where, g)
		}
		mf := newFields(fmt.Sprintf("StatMod %s (%d)", m.Name, m.Guid), m.Fields)
		stat, _ := mf.id("StatRefId")
		value := mf.str("Value")
		if mf.err != nil {
			return nil, mf.err
		}
		if _, known := statNames[stat]; !known {
			return nil, fmt.Errorf("%s: names stat %d, which is not a StatTypeDef row", mf.where, stat)
		}
		sm, err := linearMod(value)
		if err != nil {
			return nil, fmt.Errorf("%s: Value: %w", mf.where, err)
		}
		sm.stat = stat
		out = append(out, sm)
	}
	return out, nil
}

// linearMod runs a StatMod's formula as a function of the one stat it reads (GetStat on the instigator): at 0, 100 and
// 250 of that stat, and requires the three results on one line. No stat read is a constant.
func linearMod(expr string) (statMod, error) {
	prog, err := aocexpr.Parse(expr)
	if err != nil {
		return statMod{}, err
	}
	var stats []int64
	seen := map[int64]bool{}
	for _, ref := range prog.Refs() {
		if ref.TypeID == content.TypeStatDef && !seen[ref.Guid] {
			seen[ref.Guid] = true
			stats = append(stats, ref.Guid)
		}
	}
	at := func(x float64) (float64, error) {
		env := ruleEnv{}
		if len(stats) == 1 {
			env.stats = map[int64]float64{stats[0]: x}
		}
		return prog.EvalFloat(env.env())
	}
	switch len(stats) {
	case 0:
		v, err := at(0)
		if err != nil {
			return statMod{}, err
		}
		return statMod{addend: v}, nil
	case 1:
		var y [3]float64
		for i, x := range [3]float64{0, 100, 250} {
			if y[i], err = at(x); err != nil {
				return statMod{}, err
			}
		}
		factor := (y[1] - y[0]) / 100
		addend := y[0]
		if math.Abs(addend+factor*250-y[2]) > 1e-9*math.Max(1, math.Abs(y[2])) {
			return statMod{}, fmt.Errorf("is not linear in stat %d (%v, %v, %v at 0, 100, 250)", stats[0], y[0], y[1], y[2])
		}
		// the decimals the designer typed: 1.9 - 1 is not 0.9 in binary
		return statMod{source: stats[0], factor: round9(factor), addend: round9(addend)}, nil
	}
	return statMod{}, fmt.Errorf("reads %d stats, the runtime handles one", len(stats))
}

// round9 drops the binary noise of a sum or difference of decimals (0.1 + 0.2).
func round9(v float64) float64 { return math.Round(v*1e9) / 1e9 }

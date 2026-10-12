// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"

	"encoding/json"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// skillSet is what buildSkills wrote, for the stamina rules.
type skillSet struct {
	abilities map[int64]*designdb.Row // Sprint, Dodge and Rest
	effects   map[int64]*designdb.Row // the effects in the effect table
	hits      map[int64]*designdb.Row // the hits in the hit table
	mods      map[int64]*designdb.Row // every StatMod row
	names     map[int64]string        // every Effect row's name, for references
	// regenEffects are the effects the StaminaRegeneration formula names, in
	// the order it names them.
	regenEffects []int64
	regenExpr    string
	// disables is what the StaminaRegeneration formula does with each of its effects on the owner: set by
	// buildStaminaRules, read by buildAbilityRuntime.
	disables map[int64]regenDisable
}

// regenDisable says whether an effect on the owner stops stamina regenerating while it stands still and while it moves.
type regenDisable struct {
	standing, moving bool
}

// effectPhases are an Effect's lists of hits.
var effectPhases = []struct{ field, phase string }{
	{"PreApplyHitsIds", "PreApply"},
	{"ApplyHitsIds", "Apply"},
	{"TickHitsIds", "Tick"},
	{"ConsumeHitsIds", "Consume"},
	{"RemoveHitsIds", "Remove"},
	{"ExpireHitsIds", "Expire"},
	{"EndHitsIds", "End"},
}

// [C:Y:B - C-O-A]

// buildSkills fills ability, ability_cost, ability_effect, ability_hit, effect,
// effect_hit, hit, hit_effect and owner_stat_mod for Sprint, Dodge and Rest and
// what they reach:
//
//   - the effects an ability lists (EffectsIds) and those its own hits apply;
//   - the effects the StaminaRegeneration formula names (the ones that stop
//     stamina regenerating);
//   - the Sprint's stamina drain, an effect no record references (plan A7: see
//     content.GuidSprintStaminaDrain);
//   - one step further: the effects the hits of those effects apply.
//
// Every effect in that set is written with its hits, and each hit with what it
// applies. The effects a hit applies that are one step beyond the set appear
// in hit_effect by Guid and name only.
func buildSkills(w *writer, src *source, statNames map[int64]string) (*skillSet, error) {
	sk := &skillSet{
		abilities: map[int64]*designdb.Row{},
		effects:   map[int64]*designdb.Row{},
		hits:      map[int64]*designdb.Row{},
		mods:      map[int64]*designdb.Row{},
		names:     map[int64]string{},
	}
	abilityRows := src.rows["AoCAbilityRecordBase"]
	effectRows := src.rows["EffectRecordBase"]
	hitRows := src.rows["AbilityHitRecordBase"]
	modRows := src.rows["StatModRecordBase"]

	allEffects := make(map[int64]*designdb.Row, len(effectRows))
	for i := range effectRows {
		allEffects[effectRows[i].Guid] = &effectRows[i]
		sk.names[effectRows[i].Guid] = effectRows[i].Name
	}
	allHits := make(map[int64]*designdb.Row, len(hitRows))
	for i := range hitRows {
		allHits[hitRows[i].Guid] = &hitRows[i]
	}
	allMods := sk.mods
	for i := range modRows {
		allMods[modRows[i].Guid] = &modRows[i]
	}
	for _, guid := range []int64{content.GuidSprint, content.GuidDodge, content.GuidRest} {
		for i := range abilityRows {
			if abilityRows[i].Guid == guid {
				sk.abilities[guid] = &abilityRows[i]
			}
		}
		if sk.abilities[guid] == nil {
			return nil, fmt.Errorf("no Ability row %d", guid)
		}
	}

	// The effects the StaminaRegeneration formula names.
	regen := (*designdb.Row)(nil)
	for i := range src.rows["StatTypeDefRecordBase"] {
		if r := &src.rows["StatTypeDefRecordBase"][i]; r.Guid == content.GuidStaminaRegeneration {
			regen = r
		}
	}
	if regen == nil {
		return nil, fmt.Errorf("no StatTypeDef row %d (StaminaRegeneration)", content.GuidStaminaRegeneration)
	}
	rf := newFields("StatTypeDef StaminaRegeneration", regen.Fields)
	sk.regenExpr = rf.str("base")
	if rf.err != nil {
		return nil, rf.err
	}
	prog, err := aocexpr.Parse(sk.regenExpr)
	if err != nil {
		return nil, fmt.Errorf("StaminaRegeneration base: %w", err)
	}
	for _, ref := range prog.Refs() {
		if ref.TypeID != content.TypeEffect {
			return nil, fmt.Errorf("StaminaRegeneration base names %v, which is not an effect", ref)
		}
		sk.regenEffects = append(sk.regenEffects, ref.Guid)
	}

	// The seeds.
	hitEffects := func(h *designdb.Row) ([]int64, error) { // the effects a hit applies
		hf := newFields(fmt.Sprintf("AbilityHit %s (%d)", h.Name, h.Guid), h.Fields)
		var out []int64
		for i, v := range hf.list("ApplyEffects") {
			s, ok := v.(designdb.Struct)
			if !ok {
				return nil, fmt.Errorf("%s: ApplyEffects[%d] is %T", hf.where, i, v)
			}
			ef := newFields(fmt.Sprintf("%s: ApplyEffects[%d]", hf.where, i), s)
			g, _ := ef.id("EffectId")
			if ef.err != nil {
				return nil, ef.err
			}
			out = append(out, g)
		}
		for i, v := range hf.list("EffectsAppliedToSelf") {
			s, ok := v.(designdb.Struct)
			if !ok {
				return nil, fmt.Errorf("%s: EffectsAppliedToSelf[%d] is %T", hf.where, i, v)
			}
			ef := newFields(fmt.Sprintf("%s: EffectsAppliedToSelf[%d]", hf.where, i), s)
			g, _ := ef.id("Effect")
			if ef.err != nil {
				return nil, ef.err
			}
			out = append(out, g)
		}
		return out, hf.err
	}
	effectHits := func(e *designdb.Row) ([]int64, error) { // the hits an effect fires, any phase
		ef := newFields(fmt.Sprintf("Effect %s (%d)", e.Name, e.Guid), e.Fields)
		var out []int64
		for _, p := range effectPhases {
			out = append(out, ef.ids(p.field)...)
		}
		return out, ef.err
	}
	addEffect := func(g int64) error {
		if g == 0 {
			return nil
		}
		e := allEffects[g]
		if e == nil {
			return fmt.Errorf("no Effect row %d", g)
		}
		sk.effects[g] = e
		return nil
	}
	addHit := func(g int64) error {
		if g == 0 {
			return nil
		}
		h := allHits[g]
		if h == nil {
			return fmt.Errorf("no AbilityHit row %d", g)
		}
		sk.hits[g] = h
		return nil
	}

	for _, guid := range sortedKeys(sk.abilities) {
		af := newFields(fmt.Sprintf("Ability %s (%d)", sk.abilities[guid].Name, guid), sk.abilities[guid].Fields)
		_, effects := af.keyedIds("EffectsIds")
		_, hits := af.keyedIds("HitsIds")
		if af.err != nil {
			return nil, af.err
		}
		for _, g := range effects {
			if err := addEffect(g); err != nil {
				return nil, err
			}
		}
		for _, g := range hits {
			if err := addHit(g); err != nil {
				return nil, err
			}
			applied, err := hitEffects(allHits[g])
			if err != nil {
				return nil, err
			}
			for _, e := range applied {
				if err := addEffect(e); err != nil {
					return nil, err
				}
			}
		}
	}
	for _, g := range sk.regenEffects {
		if err := addEffect(g); err != nil {
			return nil, err
		}
	}
	// The Sprint's drain: referenced by no ability, hit or effect row, so reached from here and not from the Sprint's.
	if err := addEffect(content.GuidSprintStaminaDrain); err != nil {
		return nil, err
	}
	// One step further: what the seeds' hits apply.
	seeds := sortedKeys(sk.effects)
	for _, g := range seeds {
		hs, err := effectHits(sk.effects[g])
		if err != nil {
			return nil, err
		}
		for _, h := range hs {
			if h == 0 {
				continue
			}
			if err := addHit(h); err != nil {
				return nil, err
			}
			applied, err := hitEffects(allHits[h])
			if err != nil {
				return nil, err
			}
			for _, e := range applied {
				if err := addEffect(e); err != nil {
					return nil, err
				}
			}
		}
	}
	// The hits of the effects the last step added.
	for _, g := range sortedKeys(sk.effects) {
		hs, err := effectHits(sk.effects[g])
		if err != nil {
			return nil, err
		}
		for _, h := range hs {
			if err := addHit(h); err != nil {
				return nil, err
			}
		}
	}

	// ability, ability_cost, ability_effect, ability_hit.
	for _, guid := range sortedKeys(sk.abilities) {
		if err := writeAbility(w, sk.abilities[guid], statNames, sk.names, allHits); err != nil {
			return nil, err
		}
	}
	// effect, effect_hit, owner_stat_mod (effects).
	for _, guid := range sortedKeys(sk.effects) {
		if err := writeEffect(w, sk.effects[guid], allHits, allMods, statNames); err != nil {
			return nil, err
		}
	}
	// hit, hit_effect, owner_stat_mod (hits).
	for _, guid := range sortedKeys(sk.hits) {
		if err := writeHit(w, sk.hits[guid], sk.names, allMods, statNames); err != nil {
			return nil, err
		}
	}
	return sk, nil
}

func writeAbility(w *writer, r *designdb.Row, statNames map[int64]string, effectNames map[int64]string, hits map[int64]*designdb.Row) error {
	f := newFields(fmt.Sprintf("Ability %s (%d)", r.Name, r.Guid), r.Fields)
	tags, combo := f.tags("AbilityTags"), f.list("ComboInputs")
	var comboNames []string
	for _, v := range combo {
		s, ok := v.(string)
		if !ok {
			return fmt.Errorf("%s: ComboInputs holds a %T", f.where, v)
		}
		comboNames = append(comboNames, s)
	}
	tj, _ := json.Marshal(tags)
	cj, _ := json.Marshal(append([]string{}, comboNames...))
	args := []any{
		r.Guid, r.Name, f.text("AbilityName"), string(tj), f.str("ActivationRequirements"), f.str("Cooldown"),
		b2i(f.boolean("bDisableDuringCooldown")), f.str("SharedCooldownTag"), f.str("HeldAbilityBehavior"),
		b2i(f.boolean("bOverrideHeldBehavior")), b2i(f.boolean("bHoldAnimation")), f.float("MaximumHoldTime"),
		string(cj), f.str("AbilityActivationInput"), b2i(f.boolean("bUsableInAir")), b2i(f.boolean("bUsableWhileSwimming")),
		b2i(f.boolean("bUsableWhileWalking")), b2i(f.boolean("bUsableWhileMounted")), f.float("NetworkingMaxSpeed"),
	}
	costs := f.list("Costs")
	effectKeys, effectGuids := f.keyedIds("EffectsIds")
	hitKeys, hitGuids := f.keyedIds("HitsIds")
	if f.err != nil {
		return f.err
	}
	if err := w.exec(`INSERT INTO ability (guid, name, display_name, tags, activation_requirements, cooldown_expr,
		disable_during_cooldown, shared_cooldown_tag, held_behavior, override_held_behavior, hold_animation, max_hold_time,
		combo_inputs, activation_input, usable_in_air, usable_while_swimming, usable_while_walking, usable_while_mounted,
		networking_max_speed) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, args...); err != nil {
		return err
	}

	for ci, cv := range costs {
		cs, ok := cv.(designdb.Struct)
		if !ok {
			return fmt.Errorf("%s: Costs[%d] is %T", f.where, ci, cv)
		}
		cf := newFields(fmt.Sprintf("%s: Costs[%d]", f.where, ci), cs)
		enabled, charge, trig := cf.str("Enabled"), cf.str("ChargeCost"), cf.boolean("bTriggersCooldown")
		stats := cf.list("StatCosts")
		if cf.err != nil {
			return cf.err
		}
		if len(stats) == 0 { // a group without a stat cost: stat_ord -1
			if err := w.exec(`INSERT INTO ability_cost (ability_guid, cost_ord, stat_ord, enabled_expr, triggers_cooldown,
				charge_cost_expr, stat_guid, stat_name, expression) VALUES (?, ?, -1, ?, ?, ?, 0, '', '')`,
				r.Guid, ci, enabled, b2i(trig), charge); err != nil {
				return err
			}
		}
		for si, sv := range stats {
			ss, ok := sv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: StatCosts[%d] is %T", cf.where, si, sv)
			}
			sf := newFields(fmt.Sprintf("%s: StatCosts[%d]", cf.where, si), ss)
			stat, _ := sf.id("Stat")
			expr := sf.str("Value")
			if sf.err != nil {
				return sf.err
			}
			if _, known := statNames[stat]; !known {
				return fmt.Errorf("%s: names stat %d, which is not a StatTypeDef row", sf.where, stat)
			}
			if err := w.exec(`INSERT INTO ability_cost (ability_guid, cost_ord, stat_ord, enabled_expr, triggers_cooldown,
				charge_cost_expr, stat_guid, stat_name, expression) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, ci, si, enabled, b2i(trig), charge, stat, statNames[stat], expr); err != nil {
				return err
			}
		}
	}
	for i, g := range effectGuids {
		name, ok := effectNames[g]
		if g != 0 && !ok {
			return fmt.Errorf("%s: EffectsIds names effect %d, which is not an Effect row", f.where, g)
		}
		if err := w.exec(`INSERT INTO ability_effect (ability_guid, ord, effect_guid, effect_name) VALUES (?, ?, ?, ?)`,
			r.Guid, effectKeys[i], g, name); err != nil {
			return err
		}
	}
	for i, g := range hitGuids {
		h := hits[g]
		if h == nil {
			return fmt.Errorf("%s: HitsIds names hit %d, which is not an AbilityHit row", f.where, g)
		}
		if err := w.exec(`INSERT INTO ability_hit (ability_guid, ord, hit_guid, hit_name) VALUES (?, ?, ?, ?)`,
			r.Guid, hitKeys[i], g, h.Name); err != nil {
			return err
		}
	}
	return nil
}

// built with C-O-A and C-Y-B

func writeEffect(w *writer, r *designdb.Row, hits map[int64]*designdb.Row, mods map[int64]*designdb.Row, statNames map[int64]string) error {
	f := newFields(fmt.Sprintf("Effect %s (%d)", r.Name, r.Guid), r.Fields)
	tags, etags := f.tags("Tags"), f.tags("EffectTags")
	tj, _ := json.Marshal(tags)
	ej, _ := json.Marshal(etags)
	args := []any{
		r.Guid, r.Name, f.text("EffectName"), f.str("EffectCategory"), string(tj), string(ej), f.str("ApplicationRequirements"),
		b2i(f.boolean("bInfiniteDuration")), f.str("EffectDuration"), f.float("TickTimer"), b2i(f.boolean("bIgnoreInitialTick")),
		f.str("EffectStackType"), f.int("GlobalStackLimit"), f.str("StackDurationRefresh"),
		b2i(f.boolean("bPersistsThroughDeath")), b2i(f.boolean("bPersistsRelog")), b2i(f.boolean("bDispellable")),
		b2i(f.boolean("bSignificant")),
	}
	if f.err != nil {
		return f.err
	}
	if err := w.exec(`INSERT INTO effect (guid, name, display_name, category, tags, effect_tags, application_requirements,
		infinite_duration, duration_expr, tick_timer, ignore_initial_tick, stack_type, global_stack_limit,
		stack_duration_refresh, persists_through_death, persists_relog, dispellable, significant)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, args...); err != nil {
		return err
	}
	for _, p := range effectPhases {
		ids := f.ids(p.field)
		if f.err != nil {
			return f.err
		}
		for i, g := range ids {
			if g == 0 {
				continue
			}
			h := hits[g]
			if h == nil {
				return fmt.Errorf("%s: %s names hit %d, which is not an AbilityHit row", f.where, p.field, g)
			}
			if err := w.exec(`INSERT INTO effect_hit (effect_guid, phase, ord, hit_guid, hit_name) VALUES (?, ?, ?, ?, ?)`,
				r.Guid, p.phase, i, g, h.Name); err != nil {
				return err
			}
		}
	}
	return writeStatMods(w, "effect", r, f, mods, statNames)
}

func writeHit(w *writer, r *designdb.Row, effectNames map[int64]string, mods map[int64]*designdb.Row, statNames map[int64]string) error {
	f := newFields(fmt.Sprintf("AbilityHit %s (%d)", r.Name, r.Guid), r.Fields)
	req := f.str("ApplicationRequirements")
	if f.err != nil {
		return f.err
	}
	if err := w.exec(`INSERT INTO hit (guid, name, application_requirements) VALUES (?, ?, ?)`, r.Guid, r.Name, req); err != nil {
		return err
	}
	apply := func(kind, field, idField string) error {
		for i, v := range f.list(field) {
			s, ok := v.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: %s[%d] is %T", f.where, field, i, v)
			}
			ef := newFields(fmt.Sprintf("%s: %s[%d]", f.where, field, i), s)
			g, _ := ef.id(idField)
			stack := ef.str("StackCount")
			var requirement, duration string
			if kind == "apply" {
				requirement, duration = ef.str("EffectApplicationRequirement"), ef.str("EffectDuration")
			}
			if ef.err != nil {
				return ef.err
			}
			if g == 0 {
				continue
			}
			name, ok := effectNames[g]
			if !ok {
				return fmt.Errorf("%s: names effect %d, which is not an Effect row", ef.where, g)
			}
			if err := w.exec(`INSERT INTO hit_effect (hit_guid, kind, ord, effect_guid, effect_name, requirement_expr,
				stack_count_expr, duration_expr) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, kind, i, g, name, requirement, stack, duration); err != nil {
				return err
			}
		}
		return f.err
	}
	if err := apply("apply", "ApplyEffects", "EffectId"); err != nil {
		return err
	}
	if err := apply("self", "EffectsAppliedToSelf", "Effect"); err != nil {
		return err
	}
	return writeStatMods(w, "hit", r, f, mods, statNames)
}

// [C O A - Cyb]

// writeStatMods writes the StatMod rows an effect or hit lists (StatModsIds).
func writeStatMods(w *writer, kind string, r *designdb.Row, f *fields, mods map[int64]*designdb.Row, statNames map[int64]string) error {
	ids := f.ids("StatModsIds")
	if f.err != nil {
		return f.err
	}
	for i, g := range ids {
		if g == 0 {
			continue
		}
		m := mods[g]
		if m == nil {
			return fmt.Errorf("%s: StatModsIds names StatMod %d, which is not a row", f.where, g)
		}
		mf := newFields(fmt.Sprintf("StatMod %s (%d)", m.Name, m.Guid), m.Fields)
		stat, _ := mf.id("StatRefId")
		value := mf.str("Value")
		if mf.err != nil {
			return mf.err
		}
		name, known := statNames[stat]
		if !known {
			return fmt.Errorf("%s: names stat %d, which is not a StatTypeDef row", mf.where, stat)
		}
		if err := w.exec(`INSERT INTO owner_stat_mod (owner_kind, owner_guid, ord, stat_mod_guid, stat_mod_name, stat_guid,
			stat_name, value_expr) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, kind, r.Guid, i, g, m.Name, stat, name, value); err != nil {
			return err
		}
	}
	return nil
}

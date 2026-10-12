// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// ruleEnv is the world the stamina formulas are run in: which effects the
// source carries (and how many stacks), whether it is moving or in combat, and
// whether it bought the skill the formulas ask about. Everything else the
// formulas call is a built-in of package aocexpr, or the run fails.
type ruleEnv struct {
	effects  map[int64]float64
	moving   bool
	inCombat bool
	skilled  bool
	// stats are the values GetStat answers by the stat's Guid; nil leaves GetStat to the host, which is to say
	// an unsupported function (the formulas of the stamina rules do not read stats).
	stats map[int64]float64
}

// [C.Y.B - CoA]

func (s ruleEnv) env() *aocexpr.Env {
	obj := func(kind string) aocexpr.Func {
		return func([]aocexpr.Value) (aocexpr.Value, error) { return aocexpr.Object{Kind: kind}, nil }
	}
	e := &aocexpr.Env{Funcs: map[string]aocexpr.Func{
		"GetSource":     obj("source"),
		"GetInstigator": obj("instigator"),
		"GetOwner":      obj("owner"),
		"IsMoving":      func([]aocexpr.Value) (aocexpr.Value, error) { return s.moving, nil },
		"IsInCombat":    func([]aocexpr.Value) (aocexpr.Value, error) { return s.inCombat, nil },
		"GetEffectCount": func(args []aocexpr.Value) (aocexpr.Value, error) {
			if len(args) != 2 {
				return nil, errors.New("takes an object and an effect")
			}
			ref, ok := args[1].(aocexpr.Ref)
			if !ok || ref.TypeID != content.TypeEffect {
				return nil, fmt.Errorf("the second argument is %v, not an effect reference", args[1])
			}
			return s.effects[ref.Guid], nil
		},
		"GetSkillPurchased": func(args []aocexpr.Value) (aocexpr.Value, error) {
			if len(args) != 3 {
				return nil, errors.New("takes an object, a skill tree and a skill")
			}
			return s.skilled, nil
		},
	}}
	if s.stats != nil {
		e.Funcs["GetStat"] = func(args []aocexpr.Value) (aocexpr.Value, error) {
			if len(args) != 2 {
				return nil, errors.New("takes an object and a stat")
			}
			ref, ok := args[1].(aocexpr.Ref)
			if !ok || ref.TypeID != content.TypeStatDef {
				return nil, fmt.Errorf("the second argument is %v, not a stat reference", args[1])
			}
			v, known := s.stats[ref.Guid]
			if !known {
				return nil, fmt.Errorf("stat %d has no value in this environment", ref.Guid)
			}
			return v, nil
		}
	}
	return e
}

// with returns the environment with one effect present.
func (s ruleEnv) with(effect int64) ruleEnv {
	e := map[int64]float64{effect: 1}
	for k, v := range s.effects {
		e[k] = v
	}
	s.effects = e
	return s
}

// ruleSet collects stamina_rule rows. The first problem (a key given twice)
// sticks in err, like fields: the builders put all their rules and check once.
type ruleSet struct {
	rows map[string][2]string // key -> value, source
	err  error
}

func (r *ruleSet) put(key, value, source string) {
	if _, dup := r.rows[key]; dup {
		if r.err == nil {
			r.err = fmt.Errorf("stamina rule %s is given twice", key)
		}
		return
	}
	r.rows[key] = [2]string{value, source}
}

func (r *ruleSet) putFloat(key string, v float64, source string) {
	r.put(key, strconv.FormatFloat(v, 'g', -1, 64), source)
}

// combatConfigSection is the section of DefaultCombat.ini that holds the combat
// settings, where Stats=(StatRegenTime=1.000000,...) is a property of
// GameSettings, on one very long line.
const combatConfigSection = "/Script/GameSystemsPlugin.CombatConfig"

// statRegenTimePattern finds Stats=(...StatRegenTime=n...) with StatRegenTime
// among the simple properties that open the struct (before any nested one).
var statRegenTimePattern = regexp.MustCompile(`\bStats=\((?:[A-Za-z0-9_]+=[^(),]*,)*StatRegenTime=([0-9]+(?:\.[0-9]+)?)`)

// statRegenTime reads StatRegenTime from the client's Config directory, or
// returns the documented default when there is no such file. Only a
// GameSettings line of the section [combatConfigSection] counts: the same name
// elsewhere in the file (another section, AccountSettings) is not the setting.
func statRegenTime(configDir string) (seconds float64, source string, err error) {
	def := fmt.Sprintf("the default %v s (spec §6: DefaultCombat.ini Stats.StatRegenTime); ", content.DefaultStatRegenTime)
	if configDir == "" {
		return content.DefaultStatRegenTime, def + "no client Config directory was given", nil
	}
	path := filepath.Join(configDir, "DefaultCombat.ini")
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return content.DefaultStatRegenTime, def + path + " does not exist", nil
	}
	if err != nil {
		return 0, "", err
	}
	const key = "GameSettings"
	section := ""
	for _, line := range strings.Split(strings.TrimPrefix(string(data), "\ufeff"), "\n") {
		line = strings.TrimRight(line, "\r")
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			section = line[1 : len(line)-1]
			continue
		}
		if section != combatConfigSection || !strings.HasPrefix(line, key+"=") {
			continue
		}
		m := statRegenTimePattern.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		v, err := strconv.ParseFloat(m[1], 64)
		if err != nil {
			return 0, "", fmt.Errorf("%s: StatRegenTime %q: %w", path, m[1], err)
		}
		return v, fmt.Sprintf("DefaultCombat.ini [%s] %s.Stats.StatRegenTime", section, key), nil
	}
	return 0, "", fmt.Errorf("%s has no StatRegenTime in the %s property of [%s]", path, key, combatConfigSection)
}

// C O A and C.Y.B

// buildStaminaRules fills stamina_rule and stamina_regen_disable and returns
// the number of rules. Every number comes from the design database rows and is
// computed by running the row's own formula; the one fact the data does not
// hold, how often a held Sprint charges its cost, is recorded as unknown.
func buildStaminaRules(w *writer, opt *Options, statRows []designdb.Row, lists map[string]*initList, sk *skillSet) (int, error) {
	rs := &ruleSet{rows: map[string][2]string{}}
	stat := func(guid int64) (*designdb.Row, error) {
		for i := range statRows {
			if statRows[i].Guid == guid {
				return &statRows[i], nil
			}
		}
		return nil, fmt.Errorf("no StatTypeDef row %d", guid)
	}

	// Stamina and MaxStamina: the stat rows and Player_Base's starting values.
	base := lists["Player_Base"]
	if base == nil || base.Guid != content.GuidPlayerBase {
		return 0, fmt.Errorf("no StatInitializerList Player_Base (%d)", content.GuidPlayerBase)
	}
	level1 := aocexpr.CharacterEnv(1, nil)
	for _, s := range []struct {
		key  string
		guid int64
	}{{"stamina", content.GuidStamina}, {"max_stamina", content.GuidMaxStamina}} {
		row, err := stat(s.guid)
		if err != nil {
			return 0, err
		}
		f := newFields("StatTypeDef "+row.Name, row.Fields)
		rep := int(f.int("Replication"))
		audience, field := content.Audience(rep)
		src := fmt.Sprintf("StatTypeDef %s (%d)", row.Name, row.Guid)
		rs.put(s.key+".stat_guid", strconv.FormatInt(s.guid, 10), src)
		rs.put(s.key+".min_expr", f.str("ValueMin"), src)
		rs.put(s.key+".max_expr", f.str("ValueMax"), src)
		rs.put(s.key+".regen_rate_expr", f.str("RegenRate"), src)
		rs.put(s.key+".on_spawn", f.str("OnSpawn"), src)
		rs.put(s.key+".on_death", f.str("OnDeath"), src)
		rs.put(s.key+".on_revive", f.str("OnRevive"), src)
		rs.put(s.key+".audience", audience, src+", Replication "+strconv.Itoa(rep))
		if field != "" {
			rs.put(s.key+".rep_field", field, src+", Replication "+strconv.Itoa(rep)+" (the capture)")
		}
		if f.err != nil {
			return 0, f.err
		}
		e, err := base.entry(s.guid)
		if err != nil {
			return 0, err
		}
		v, err := aocexpr.EvalFloat(e.Expression, level1)
		if err != nil {
			return 0, fmt.Errorf("Player_Base %s: %w", row.Name, err)
		}
		rs.put(s.key+".initial_expr", e.Expression, "StatInitializerList Player_Base ("+strconv.FormatInt(base.Guid, 10)+")")
		rs.putFloat(s.key+".initial", v, "StatInitializerList Player_Base, the formula run")
	}

	// StaminaRegeneration: the formula, its two outcomes, and the effects that
	// switch regeneration off.
	regenSrc := fmt.Sprintf("StatTypeDef StaminaRegeneration (%d), base", content.GuidStaminaRegeneration)
	rs.put(content.RuleRegenBaseExpr, sk.regenExpr, regenSrc)
	regenProg, err := aocexpr.Parse(sk.regenExpr)
	if err != nil {
		return 0, err
	}
	enabled, err := regenProg.EvalFloat(ruleEnv{}.env())
	if err != nil {
		return 0, fmt.Errorf("StaminaRegeneration with no effects: %w", err)
	}
	rs.putFloat(content.RuleRegenRateEnabled, enabled, regenSrc+", the formula run with no effect on the character")

	type disable struct {
		guid                 int64
		standing, moving     float64
		disablesS, disablesM bool
	}
	var disables []disable
	seen := map[int64]bool{}
	rateDisabled, haveDisabled := 0.0, false
	for _, g := range sk.regenEffects {
		if seen[g] { // an effect the formula names twice is one row
			continue
		}
		seen[g] = true
		d := disable{guid: g}
		if d.standing, err = regenProg.EvalFloat(ruleEnv{}.with(g).env()); err != nil {
			return 0, err
		}
		if d.moving, err = regenProg.EvalFloat(ruleEnv{moving: true}.with(g).env()); err != nil {
			return 0, err
		}
		d.disablesS, d.disablesM = d.standing != enabled, d.moving != enabled
		for _, v := range []float64{d.standing, d.moving} {
			if v == enabled {
				continue
			}
			if haveDisabled && v != rateDisabled {
				return 0, fmt.Errorf("StaminaRegeneration gives %v and %v with effects present, expected one disabled value", rateDisabled, v)
			}
			rateDisabled, haveDisabled = v, true
		}
		disables = append(disables, d)
	}
	if !haveDisabled {
		return 0, errors.New("no effect of the StaminaRegeneration formula changes its value")
	}
	rs.putFloat(content.RuleRegenRateDisabled, rateDisabled, regenSrc+", the formula run with a disabling effect present")
	sk.disables = map[int64]regenDisable{}
	for i, d := range disables {
		name := sk.names[d.guid]
		sk.disables[d.guid] = regenDisable{standing: d.disablesS, moving: d.disablesM}
		if err := w.exec(`INSERT INTO stamina_regen_disable (ord, effect_guid, effect_name, disables_standing, disables_moving)
			VALUES (?, ?, ?, ?, ?)`, i, d.guid, name, b2i(d.disablesS), b2i(d.disablesM)); err != nil {
			return 0, err
		}
	}

	regenTime, regenTimeSrc, err := statRegenTime(opt.Config)
	if err != nil {
		return 0, err
	}
	rs.putFloat(content.RuleStatRegenTime, regenTime, regenTimeSrc)

	// Sprint.
	if err := sprintRules(rs, sk); err != nil {
		return 0, err
	}
	if err := dodgeRules(rs, sk); err != nil {
		return 0, err
	}
	if rs.err != nil {
		return 0, rs.err
	}

	keys := make([]string, 0, len(rs.rows))
	for k := range rs.rows {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if err := w.exec(`INSERT INTO stamina_rule (key, value, source) VALUES (?, ?, ?)`, k, rs.rows[k][0], rs.rows[k][1]); err != nil {
			return 0, err
		}
	}
	return len(keys), nil
}

// C.O.A : C | Y | B

// staminaCost returns the cost group of an ability that spends Stamina: its
// enabling condition, whether it triggers the cooldown and the amount.
type staminaCost struct {
	Enabled          string
	TriggersCooldown bool
	Expression       string
}

func abilityStaminaCost(a *designdb.Row) (staminaCost, error) {
	found, err := staminaCosts(a)
	if err != nil {
		return staminaCost{}, err
	}
	if len(found) != 1 {
		return staminaCost{}, fmt.Errorf("Ability %s (%d) has %d stamina costs, want 1", a.Name, a.Guid, len(found))
	}
	return found[0], nil
}

// staminaCosts returns every cost group of an ability that spends Stamina.
func staminaCosts(a *designdb.Row) ([]staminaCost, error) {
	f := newFields(fmt.Sprintf("Ability %s (%d)", a.Name, a.Guid), a.Fields)
	var found []staminaCost
	for ci, cv := range f.list("Costs") {
		cs, ok := cv.(designdb.Struct)
		if !ok {
			return nil, fmt.Errorf("%s: Costs[%d] is %T", f.where, ci, cv)
		}
		cf := newFields(fmt.Sprintf("%s: Costs[%d]", f.where, ci), cs)
		for si, sv := range cf.list("StatCosts") {
			ss, ok := sv.(designdb.Struct)
			if !ok {
				return nil, fmt.Errorf("%s: StatCosts[%d] is %T", cf.where, si, sv)
			}
			sf := newFields(fmt.Sprintf("%s: StatCosts[%d]", cf.where, si), ss)
			g, _ := sf.id("Stat")
			expr := sf.str("Value")
			if sf.err != nil {
				return nil, sf.err
			}
			if g == content.GuidStamina {
				found = append(found, staminaCost{cf.str("Enabled"), cf.boolean("bTriggersCooldown"), expr})
			}
		}
		if cf.err != nil {
			return nil, cf.err
		}
	}
	if f.err != nil {
		return nil, f.err
	}
	return found, nil
}

// ~ C | Y | B - C O A ~

func sprintRules(rs *ruleSet, sk *skillSet) error {
	a := sk.abilities[content.GuidSprint]
	src := fmt.Sprintf("Ability %s (%d)", a.Name, a.Guid)
	f := newFields(src, a.Fields)
	cost, err := abilityStaminaCost(a)
	if err != nil {
		return err
	}
	cooldown, activation, held := f.str("Cooldown"), f.str("ActivationRequirements"), f.str("HeldAbilityBehavior")
	_, effects := f.keyedIds("EffectsIds")
	if f.err != nil {
		return f.err
	}
	rs.put("sprint.ability_guid", strconv.FormatInt(a.Guid, 10), src)
	rs.put("sprint.activation_requirements", activation, src)
	rs.put("sprint.held_behavior", held, src)
	rs.put("sprint.cost_enabled_expr", cost.Enabled, src+", Costs, the group that spends Stamina")
	rs.put("sprint.cost_expr", cost.Expression, src+", Costs, the group that spends Stamina")
	rs.put("sprint.cooldown_expr", cooldown, src)
	rs.put("sprint.triggers_cooldown", strconv.Itoa(b2i(cost.TriggersCooldown)), src)
	prog, err := aocexpr.Parse(cost.Expression)
	if err != nil {
		return fmt.Errorf("%s cost: %w", src, err)
	}
	for _, c := range []struct {
		key      string
		inCombat bool
		what     string
	}{{content.RuleSprintCostOutOfCombat, false, " out of combat"}, {content.RuleSprintCostInCombat, true, " in combat"}} {
		v, err := prog.EvalFloat(ruleEnv{inCombat: c.inCombat}.env())
		if err != nil {
			return fmt.Errorf("%s cost: %w", src, err)
		}
		rs.putFloat(c.key, v, src+", the cost formula run"+c.what)
	}
	cd, err := aocexpr.EvalFloat(cooldown, ruleEnv{}.env())
	if err != nil {
		return fmt.Errorf("%s cooldown: %w", src, err)
	}
	rs.putFloat("sprint.cooldown", cd, src+", the cooldown formula run")
	// How often a held Sprint charges its cost is in no row the Sprint reaches. The one cadence the data has is the
	// effect General_Sprint_StaminaDrain, which no record references: a tick every 0.25 s whose hit takes the same
	// Stamina as the Sprint's cost. That is the interval (spec section 7); it is used only when the drain's own numbers
	// agree with the cost row, so a client update that changes either stops the build.
	drain, err := sprintDrain(sk, cost.Expression)
	if err != nil {
		return err
	}
	rs.putFloat(content.RuleSprintChargeInterval, drain.interval,
		fmt.Sprintf("Effect %s (%d) TickTimer, in seconds: no record references that effect, so it is the Sprint's drain by decision (spec section 7), not by data; its tick hit takes %v Stamina while the owner moves (%v in combat), the Sprint cost row's amount", drain.name, drain.guid, drain.perCharge, drain.perChargeInCombat))
	// The Sprint effect itself ticks once a second, but nothing says that tick is a charge.
	if len(effects) != 1 {
		return fmt.Errorf("%s lists %d effects, want 1", src, len(effects))
	}
	e := sk.effects[effects[0]]
	if e == nil {
		return fmt.Errorf("%s: effect %d is not in the effect set", src, effects[0])
	}
	ef := newFields(fmt.Sprintf("Effect %s (%d)", e.Name, e.Guid), e.Fields)
	tick := ef.float("TickTimer")
	if ef.err != nil {
		return ef.err
	}
	rs.put("sprint.effect_guid", strconv.FormatInt(e.Guid, 10), src+", EffectsIds")
	rs.putFloat("sprint.effect_tick_timer", tick, fmt.Sprintf("Effect %s (%d) TickTimer, in seconds; that a tick charges the cost is not established", e.Name, e.Guid))
	return nil
}

// drainEffect is what sprintDrain found.
type drainEffect struct {
	guid                         int64
	name                         string
	interval                     float64 // the effect's TickTimer
	perCharge, perChargeInCombat float64 // the tick hit's stat mod negated: moving, out of combat and in combat
}

// -- C.O.A & C Y B --

// sprintDrain reads the effect General_Sprint_StaminaDrain (content.GuidSprintStaminaDrain) and checks it against the
// Sprint's cost formula: the effect is infinite with a tick timer, one of its tick hits has one stat mod on Stamina, and
// that mod, run with the owner moving, takes exactly what the cost formula charges, out of combat and in combat. Any other
// shape is an error: the interval is only as good as the claim that this effect is the Sprint's drain.
func sprintDrain(sk *skillSet, costExpr string) (drainEffect, error) {
	e := sk.effects[content.GuidSprintStaminaDrain]
	if e == nil {
		return drainEffect{}, fmt.Errorf("effect %d (General_Sprint_StaminaDrain) is not in the effect set", content.GuidSprintStaminaDrain)
	}
	where := fmt.Sprintf("Effect %s (%d)", e.Name, e.Guid)
	f := newFields(where, e.Fields)
	interval, infinite := f.float("TickTimer"), f.boolean("bInfiniteDuration")
	tickHits := f.ids("TickHitsIds")
	if f.err != nil {
		return drainEffect{}, f.err
	}
	if !infinite || interval <= 0 {
		return drainEffect{}, fmt.Errorf("%s is infinite=%v with TickTimer %v, want an infinite effect with a tick", where, infinite, interval)
	}
	costProg, err := aocexpr.Parse(costExpr)
	if err != nil {
		return drainEffect{}, err
	}
	d := drainEffect{guid: e.Guid, name: e.Name, interval: interval}
	found := 0
	for _, g := range tickHits {
		h := sk.hits[g]
		if h == nil {
			continue
		}
		hf := newFields(fmt.Sprintf("AbilityHit %s (%d)", h.Name, h.Guid), h.Fields)
		mods := hf.ids("StatModsIds")
		if hf.err != nil {
			return drainEffect{}, hf.err
		}
		for _, mg := range mods {
			m := sk.mods[mg]
			if m == nil {
				return drainEffect{}, fmt.Errorf("%s: StatModsIds names StatMod %d, which is not in the set", hf.where, mg)
			}
			mf := newFields(fmt.Sprintf("StatMod %s (%d)", m.Name, m.Guid), m.Fields)
			stat, _ := mf.id("StatRefId")
			value := mf.str("Value")
			if mf.err != nil {
				return drainEffect{}, mf.err
			}
			if stat != content.GuidStamina {
				continue
			}
			prog, err := aocexpr.Parse(value)
			if err != nil {
				return drainEffect{}, fmt.Errorf("%s: %w", mf.where, err)
			}
			found++
			for _, c := range []struct {
				inCombat bool
				out      *float64
			}{{false, &d.perCharge}, {true, &d.perChargeInCombat}} {
				v, err := prog.EvalFloat(ruleEnv{moving: true, inCombat: c.inCombat}.env())
				if err != nil {
					return drainEffect{}, fmt.Errorf("%s: %w", mf.where, err)
				}
				want, err := costProg.EvalFloat(ruleEnv{inCombat: c.inCombat}.env())
				if err != nil {
					return drainEffect{}, err
				}
				if v != -want {
					return drainEffect{}, fmt.Errorf("%s takes %v Stamina per tick (in combat: %v) but the Sprint cost row charges %v: the drain effect is not the Sprint's", mf.where, -v, c.inCombat, want)
				}
				*c.out = want
			}
			// While the owner stands still the drain is 0: a charge is paid only for movement.
			idle, err := prog.EvalFloat(ruleEnv{}.env())
			if err != nil {
				return drainEffect{}, fmt.Errorf("%s: %w", mf.where, err)
			}
			if idle != 0 {
				return drainEffect{}, fmt.Errorf("%s takes %v Stamina per tick while the owner stands still: the Sprint costs nothing then", mf.where, -idle)
			}
		}
	}
	if found != 1 {
		return drainEffect{}, fmt.Errorf("%s has %d tick-hit stat mods on Stamina, want 1", where, found)
	}
	return d, nil
}

func dodgeRules(rs *ruleSet, sk *skillSet) error {
	a := sk.abilities[content.GuidDodge]
	src := fmt.Sprintf("Ability %s (%d)", a.Name, a.Guid)
	f := newFields(src, a.Fields)
	cost, err := abilityStaminaCost(a)
	if err != nil {
		return err
	}
	cooldown, disableDuring := f.str("Cooldown"), f.boolean("bDisableDuringCooldown")
	if f.err != nil {
		return f.err
	}
	rs.put("dodge.ability_guid", strconv.FormatInt(a.Guid, 10), src)
	rs.put("dodge.cost_expr", cost.Expression, src+", Costs, the group that spends Stamina")
	rs.put("dodge.cost_enabled_expr", cost.Enabled, src+", Costs")
	rs.put("dodge.cooldown_expr", cooldown, src)
	rs.put("dodge.triggers_cooldown", strconv.Itoa(b2i(cost.TriggersCooldown)), src+", Costs: the cost group's bTriggersCooldown")
	rs.put("dodge.disable_during_cooldown", strconv.Itoa(b2i(disableDuring)), src)

	// The cooldown names the one effect that, with the skill, shortens the
	// cooldown and cheapens the dodge.
	cdProg, err := aocexpr.Parse(cooldown)
	if err != nil {
		return fmt.Errorf("%s cooldown: %w", src, err)
	}
	refs := cdProg.Refs()
	var skillEffect int64
	for _, r := range refs {
		if r.TypeID == content.TypeEffect {
			if skillEffect != 0 && skillEffect != r.Guid {
				return fmt.Errorf("%s cooldown names two effects", src)
			}
			skillEffect = r.Guid
		}
	}
	if skillEffect == 0 {
		return fmt.Errorf("%s cooldown names no effect", src)
	}
	rs.put("dodge.skill_effect_guid", strconv.FormatInt(skillEffect, 10), src+", Cooldown")
	rs.put("dodge.skill_effect_name", sk.names[skillEffect], src+", Cooldown")

	costProg, err := aocexpr.Parse(cost.Expression)
	if err != nil {
		return fmt.Errorf("%s cost: %w", src, err)
	}
	for _, c := range []struct {
		costKey, cdKey string
		env            ruleEnv
		what           string
	}{
		{content.RuleDodgeCostBase, content.RuleDodgeCooldownBase, ruleEnv{}, "no effect on the character and the skill not bought"},
		{content.RuleDodgeCostSkilled, content.RuleDodgeCooldownSkilled, ruleEnv{skilled: true}.with(skillEffect), "the skill bought and its effect on the character"},
	} {
		cv, err := costProg.EvalFloat(c.env.env())
		if err != nil {
			return fmt.Errorf("%s cost: %w", src, err)
		}
		rs.putFloat(c.costKey, cv, src+", the cost formula run with "+c.what)
		dv, err := cdProg.EvalFloat(c.env.env())
		if err != nil {
			return fmt.Errorf("%s cooldown: %w", src, err)
		}
		rs.putFloat(c.cdKey, dv, src+", the cooldown formula run with "+c.what)
	}
	return nil
}

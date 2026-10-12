// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// The mounts (milestone "mounts", stage 1; contentc version 11): see package content, mounts.go, for what each table holds.
//
//   mount          every ItemRecord named Mount_* that carries an Item.Mount.Ground, .Flying or .Aquatic tag: its Blueprint and activation and the numbers its
//                  text names (the speed percentages, the Maximum Health percentage, the level, a mule's crate capacity)
//   mount_stat     the Mount_Base StatInitializerList run at the owner's levels 1 to 50: the entries read the owner's level (GetCharacterLevel(GetOwner())), so a
//                  mount of a level 25 player has 4,875 MaxHealth (the Mount_Base_HP curve) before its item's factor
//   mount_ability  the abilities with bUsableWhileMounted or bAutoDismount, and the roles the mount flow names
//   mount_effect   Road_T0..T4_Speed_Mount, the dash (MountAbility_Sprint), the effects with bForceDismount and the crate debuff

var (
	mountPctRE    = regexp.MustCompile(`(\d+)%\s+(Ground Speed|Flying Speed|Water Speed|Maximum Health)`)
	mountLevelRE  = regexp.MustCompile(`(?m)^Level (\d+)\b`)
	mountCrateRE  = regexp.MustCompile(`Crate Carrying Capacity:\s*(\d+)`)
	mountPackage  = regexp.MustCompile(`^/Game/GameBlueprints/Mounts/([A-Za-z0-9_]+)\.([A-Za-z0-9_]+)_C$`)
	mountRoadName = regexp.MustCompile(`^Road_T([0-4])_Speed_Mount$`)
)

// mountKinds are the item tags that make an item a mount, and the kind each gives.
var mountKinds = map[string]string{
	"Item.Mount.Ground":  content.MountGround,
	"Item.Mount.Flying":  content.MountFlying,
	"Item.Mount.Aquatic": content.MountAquatic,
}

// mountText is what an item's text names.
type mountText struct {
	level, ground, flying, water, health, crate int
	mule                                        bool
}

// parseMountText reads the numbers out of a mount item's description ("Flying Mount\r\nLevel 25\r\n\r\n165% Ground Speed\r\n215% Flying Speed\r\n500% Maximum Health"):
// a percentage of each kind (0 when the text names none; health 100 then), the required level, a mule and its crate capacity.
func parseMountText(s string) (mountText, error) {
	t := mountText{health: 100}
	s = strings.ReplaceAll(s, "\r\n", "\n")
	for _, m := range mountPctRE.FindAllStringSubmatch(s, -1) {
		n, err := strconv.Atoi(m[1])
		if err != nil {
			return t, err
		}
		switch m[2] {
		case "Ground Speed":
			t.ground = n
		case "Flying Speed":
			t.flying = n
		case "Water Speed":
			t.water = n
		case "Maximum Health":
			t.health = n
		}
	}
	if m := mountLevelRE.FindStringSubmatch(s); m != nil {
		t.level, _ = strconv.Atoi(m[1])
	}
	if m := mountCrateRE.FindStringSubmatch(s); m != nil {
		t.crate, _ = strconv.Atoi(m[1])
	}
	t.mule = strings.HasPrefix(s, "Ground Mule")
	return t, nil
}

// C | O | A captures, C | Y | B code

// optBool reads a bool the record may not have (a synthetic row) as false.
func optBool(f *fields, name string) bool {
	if _, ok := f.get(name); !ok {
		return false
	}
	return f.boolean(name)
}

// mountStatCtx is the stat context a Mount_Base entry runs in: its owner at one level, the stat's running total, the equations by Guid.
type mountStatCtx struct {
	aocexpr.NoState
	level     float64
	running   float64
	equations map[int64]string
	curves    aocexpr.CurveSource
	depth     int
}

func (c *mountStatCtx) Stat(aocexpr.Ref) (float64, error) {
	return 0, fmt.Errorf("a stat read (GetStat) is not modelled for a mount's list")
}
func (c *mountStatCtx) InitRunningTotal() (float64, error) { return c.running, nil }
func (c *mountStatCtx) Formula(aocexpr.Ref) (float64, error) {
	return 0, fmt.Errorf("EvalFormula is not modelled for a mount's list")
}
func (c *mountStatCtx) Term(aocexpr.Ref) (float64, error) {
	return 0, fmt.Errorf("GetTerm is not modelled for a mount's list")
}
func (c *mountStatCtx) CharacterLevel() float64 { return c.level }

func (c *mountStatCtx) Equation(ref aocexpr.Ref) (float64, error) {
	text, ok := c.equations[ref.Guid]
	if !ok {
		return 0, fmt.Errorf("equation %d is no StatEquationType row", ref.Guid)
	}
	if c.depth++; c.depth > 8 {
		return 0, fmt.Errorf("equation %d nests too deep", ref.Guid)
	}
	defer func() { c.depth-- }()
	prog, err := aocexpr.Parse(text)
	if err != nil {
		return 0, fmt.Errorf("equation %d: %w", ref.Guid, err)
	}
	return prog.EvalFloat(aocexpr.StatEnv(c, c.curves))
}

// buildMounts fills mount, mount_stat, mount_ability and mount_effect.
func buildMounts(w *writer, src *source, opt *Options, meta map[string]string) error {
	archetypes := byGuid(src.rows["ItemActivationArchetypeRecordBase"])

	// mount
	var items []designdb.Row
	for _, r := range src.rows["ItemRecordBase"] {
		if strings.HasPrefix(r.Name, "Mount_") {
			items = append(items, r)
		}
	}
	sort.Slice(items, func(i, j int) bool { return items[i].Name < items[j].Name })
	counts := map[string]int{}
	n := 0
	for _, r := range items {
		f := newFields(fmt.Sprintf("Item %s (%d)", r.Name, r.Guid), r.Fields)
		if _, ok := f.get("GameplayTags"); !ok {
			continue
		}
		kind := ""
		for _, tag := range f.tags("GameplayTags") {
			if k, ok := mountKinds[tag]; ok {
				kind = k
			}
		}
		if kind == "" {
			continue
		}
		path := f.str("SummonNotifyClassPath")
		desc := f.text("Description")
		tradeable := f.boolean("Tradeable")
		arch, _ := f.id("ActivationArchetypeId")
		cast, _ := f.id("ActivationCastId")
		if f.err != nil {
			return f.err
		}
		bp := mountPackage.FindStringSubmatch(path)
		if bp == nil {
			return fmt.Errorf("%s: SummonNotifyClassPath %q is not /Game/GameBlueprints/Mounts/<Package>.<Class>_C", f.where, path)
		}
		activation := ""
		if arch != 0 {
			a, ok := archetypes[arch]
			if !ok {
				return fmt.Errorf("%s: activation archetype %d is no ItemActivationArchetype row", f.where, arch)
			}
			af := newFields(fmt.Sprintf("ItemActivationArchetype %s (%d)", a.Name, a.Guid), a.Fields)
			activation = enumValue(af.str("ActivationAction"))
			if af.err != nil {
				return af.err
			}
		}
		t, err := parseMountText(desc)
		if err != nil {
			return fmt.Errorf("%s: %w", f.where, err)
		}
		if err := w.exec(`INSERT INTO mount (item_guid, item_name, kind, blueprint_path, blueprint, activation, cast_ability_guid, level, ground_pct,
			flying_pct, water_pct, health_pct, mule, crate_capacity, tradeable, description) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, kind, path, bp[1], activation, cast, t.level, t.ground, t.flying, t.water, t.health, b2i(t.mule), t.crate, b2i(tradeable), desc); err != nil {
			return err
		}
		counts[kind]++
		n++
	}

	// mount_stat: the Mount_Base list at the owner's levels
	statLevels, err := buildMountStats(w, src)
	if err != nil {
		return err
	}

	// mount_ability
	abilities := 0
	for _, r := range src.rows["AoCAbilityRecordBase"] {
		f := newFields(fmt.Sprintf("Ability %s (%d)", r.Name, r.Guid), r.Fields)
		usable, auto := optBool(f, "bUsableWhileMounted"), optBool(f, "bAutoDismount")
		if f.err != nil {
			return f.err
		}
		role := ""
		switch r.Guid {
		case content.GuidSummonMount:
			role = content.MountRoleSummon
		case content.GuidMountAbilitySprint:
			role = content.MountRoleDash
		case content.GuidDismountElevated:
			role = content.MountRoleDismountE
		case content.GuidDismountGrounded:
			role = content.MountRoleDismountG
		}
		if !usable && !auto && role == "" {
			continue
		}
		if err := w.exec(`INSERT INTO mount_ability (ability_guid, name, role, usable_while_mounted, auto_dismount) VALUES (?, ?, ?, ?, ?)`,
			r.Guid, r.Name, role, b2i(usable), b2i(auto)); err != nil {
			return err
		}
		abilities++
	}

	// mount_effect
	mods := byGuid(src.rows["StatModRecordBase"])
	effects := 0
	for _, r := range src.rows["EffectRecordBase"] {
		f := newFields(fmt.Sprintf("Effect %s (%d)", r.Name, r.Guid), r.Fields)
		force := optBool(f, "bForceDismount")
		kind, tier := "", -1
		switch m := mountRoadName.FindStringSubmatch(r.Name); {
		case m != nil:
			kind = content.MountEffectRoad
			tier, _ = strconv.Atoi(m[1])
		case r.Guid == content.GuidMountAbilitySprint || r.Name == "MountAbility_Sprint":
			kind = content.MountEffectDash
		case force:
			kind = content.MountEffectForceDismount
		case r.Name == "Mount_Speed_CrateCarrying_Debuff":
			kind = content.MountEffectCrate
		}
		if kind == "" {
			continue
		}
		infinite := optBool(f, "bInfiniteDuration")
		duration := 0.0
		if v, ok := f.get("EffectDuration"); ok {
			if s, isStr := v.(string); isStr && !infinite {
				duration, _ = strconv.ParseFloat(strings.TrimSpace(s), 64)
			}
		}
		var stat int64
		value, relative := 0.0, false
		if _, ok := f.get("StatModsIds"); ok {
			ids := f.ids("StatModsIds")
			if f.err != nil {
				return f.err
			}
			if len(ids) > 0 {
				m, ok := mods[ids[0]]
				if !ok {
					return fmt.Errorf("%s: StatMod %d is no row", f.where, ids[0])
				}
				mf := newFields(fmt.Sprintf("StatMod %s (%d)", m.Name, m.Guid), m.Fields)
				stat, _ = mf.id("StatRefId")
				text := mf.str("Value")
				if mf.err != nil {
					return mf.err
				}
				if v, err := strconv.ParseFloat(strings.TrimSpace(text), 64); err == nil {
					value = v
				} else if strings.Contains(text, "GetStat(") {
					// the dash: GetTerm(share) * GetStat(GetSource(), MoveSpeedMult): the term's value is the share of the stat it adds
					terms := mf.list("ValueInputTerms")
					if mf.err != nil {
						return mf.err
					}
					if len(terms) != 1 {
						return fmt.Errorf("%s: %q has %d input terms, want 1", mf.where, text, len(terms))
					}
					ts, ok := terms[0].(designdb.Struct)
					if !ok {
						return fmt.Errorf("%s: an input term is %T", mf.where, terms[0])
					}
					tf := newFields(mf.where+": term", ts)
					share, perr := strconv.ParseFloat(strings.TrimSpace(tf.str("Value")), 64)
					if tf.err != nil || perr != nil {
						return fmt.Errorf("%s: the term of %q is not a number (%v %v)", mf.where, text, tf.err, perr)
					}
					value, relative = share, true
				} else {
					return fmt.Errorf("%s: the value %q is neither a number nor a share of a stat", mf.where, text)
				}
			}
		}
		if err := w.exec(`INSERT INTO mount_effect (effect_guid, name, kind, tier, stat_guid, value, relative, infinite, duration, force_dismount)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, kind, tier, stat, value, b2i(relative), b2i(infinite), duration, b2i(force)); err != nil {
			return err
		}
		effects++
	}

	opt.logf("mount: %d items (%d ground, %d flying, %d aquatic); mount_stat: %d rows; mount_ability: %d; mount_effect: %d",
		n, counts[content.MountGround], counts[content.MountFlying], counts[content.MountAquatic], statLevels, abilities, effects)
	meta["typed.mount"] = strconv.Itoa(n)
	meta["typed.mount_stat"] = strconv.Itoa(statLevels)
	meta["typed.mount_ability"] = strconv.Itoa(abilities)
	meta["typed.mount_effect"] = strconv.Itoa(effects)
	return nil
}

// buildMountStats runs the Mount_Base list (when the data has it) for an owner of each level 1 to 50 and writes what every entry gives.
func buildMountStats(w *writer, src *source) (int, error) {
	var list *designdb.Row
	for i := range src.rows["StatInitializerListRecordBase"] {
		if r := &src.rows["StatInitializerListRecordBase"][i]; r.Name == "Mount_Base" {
			list = r
		}
	}
	if list == nil {
		return 0, nil
	}
	if list.Guid != content.GuidMountBase {
		return 0, fmt.Errorf("StatInitializerList Mount_Base is %d, content names it %d", list.Guid, content.GuidMountBase)
	}
	equations := map[int64]string{}
	for _, r := range src.rows["StatEquationTypeRecordBase"] {
		f := newFields(fmt.Sprintf("StatEquationType %s (%d)", r.Name, r.Guid), r.Fields)
		equations[r.Guid] = f.str("Equation")
		if f.err != nil {
			return 0, f.err
		}
	}
	f := newFields(fmt.Sprintf("StatInitializerList %s (%d)", list.Name, list.Guid), list.Fields)
	entries := f.list("StatIDs")
	if f.err != nil {
		return 0, f.err
	}
	type entry struct {
		stat int64
		name string
		prog *aocexpr.Program
	}
	var es []entry
	for i, ev := range entries {
		s, ok := ev.(designdb.Struct)
		if !ok {
			return 0, fmt.Errorf("%s: StatIDs[%d] is %T", f.where, i, ev)
		}
		ef := newFields(fmt.Sprintf("%s: StatIDs[%d]", f.where, i), s)
		guid, name := ef.id("StatId")
		text := ef.str("ValueExpression")
		if ef.err != nil {
			return 0, ef.err
		}
		if strings.TrimSpace(text) == "" {
			continue
		}
		prog, err := aocexpr.Parse(text)
		if err != nil {
			return 0, fmt.Errorf("%s: StatIDs[%d]: %w", f.where, i, err)
		}
		es = append(es, entry{guid, name, prog})
	}
	statNames := src.statNames
	rows := 0
	for level := content.MinLevel; level <= content.MaxLevel; level++ {
		ctx := &mountStatCtx{level: float64(level), equations: equations, curves: src.curves}
		env := aocexpr.StatEnv(ctx, src.curves)
		running := map[int64]float64{}
		for _, e := range es {
			ctx.running = running[e.stat]
			v, err := e.prog.EvalFloat(env)
			if err != nil {
				return 0, fmt.Errorf("Mount_Base %s at owner level %d: %w", statNames[e.stat], level, err)
			}
			running[e.stat] = v
		}
		// each stat's final value once (an entry may follow another of the same stat), in the stat's guid order
		stats := make([]int64, 0, len(running))
		for g := range running {
			stats = append(stats, g)
		}
		sort.Slice(stats, func(i, j int) bool { return stats[i] < stats[j] })
		for _, g := range stats {
			if err := w.exec(`INSERT INTO mount_stat (level, stat_guid, stat_name, value) VALUES (?, ?, ?, ?)`, level, g, statNames[g], running[g]); err != nil {
				return 0, err
			}
			rows++
		}
	}
	return rows, nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// The ability tables (plan D2, contentc version 6): every ability of every class and what it reaches, as plain data and as
// expressions (package content, abilities.go, has the tables). The expressions are written as text; addStatProgram compiles them into
// the stat program's bundle afterwards and fills ab_expr.program.
//
// What is in them: the abilities are the ranks of the skill trees (plan D1's skill_rank: there is one table of ranks, and a class's abilities are those of its tree). Each ability's closure
// is what it reaches by itself: its own effects, its hits (directly, through its projectiles, through its lingering zones), and
// from there the effects the hits apply, the hits those effects fire in each phase, the projectiles and zones a hit fires or spawns.
// The effects the ranks themselves grant (passives) and their hits are in too. Reactions, cooldown modifiers and the like are stored
// as guid lists (the extra columns) and not followed: a part-2 runtime leaves them inert.

//go:embed montage_ids.tsv
var montageIDs string

// abilityNames lists the abilities of the retail captures that no skill tree grants (plan D2, stage 2): the weapon combos, the general abilities
// (Guard) and the consumables' abilities. They are seeds of the closure like the ranks' abilities, found by name.
//
//go:embed ability_names.tsv
var abilityNames string

// seedByName reads ability_names.tsv and adds every ability row of each name to the seed; a name no row has is an error (the list is stale).
func seedByName(abilities map[int64]*designdb.Row, seed map[int64]bool) (int, error) {
	byName := map[string][]int64{}
	for g, r := range abilities {
		byName[r.Name] = append(byName[r.Name], g)
	}
	n := 0
	for i, line := range strings.Split(abilityNames, "\n") {
		line = strings.TrimRight(line, "\r")
		if line == "" || line[0] == '#' {
			continue
		}
		f := strings.Split(line, "\t")
		if len(f) != 2 {
			return 0, fmt.Errorf("ability_names.tsv line %d: %d columns, want 2", i+1, len(f))
		}
		guids := byName[f[0]]
		if len(guids) == 0 {
			return 0, fmt.Errorf("ability_names.tsv line %d: no Ability row named %q", i+1, f[0])
		}
		for _, g := range guids {
			if !seed[g] {
				seed[g] = true
				n++
			}
		}
	}
	return n, nil
}

// -- CoA . C:Y:B --

// exprSet collects expressions once each (the same text and terms share an id) in the order they are met.
type exprSet struct {
	w    *writer
	ids  map[string]int64
	next int64
	// named is the record guid of each name by kind ("Skill", "SkillTree", "Effect", "Ability"), 0 for a name two rows share: a reference
	// written $Skill:Bard_Passive_QuickWit_Jest$ is rewritten to the number form before the expression is stored and compiled (the compiler
	// takes a reference as its type and guid: a named one would compile as the null reference and the expression could never be true).
	named map[string]map[string]int64
}

var commentOnly = regexp.MustCompile(`(?s)//[^\n]*|/\*.*?\*/`)

// namedRef matches a comment (left as it is) or a reference by name.
var namedRef = regexp.MustCompile(`(?s)//[^\n]*|/\*.*?\*/|\$(SkillTree|Skill|Effect|Ability|F):([^$#][^$]*)\$`)

// refTypes is the type id each named kind stands for.
var refTypes = map[string]int64{"SkillTree": content.TypeSkillTree, "Skill": content.TypeSkill, "Effect": content.TypeEffect, "Ability": content.TypeAbility, "F": 4547065559311335368} // F: a StatFormulaType, named in a reward table ($F:Recipe_Gear_Tier1$)

// recordByName matches a comment (left as it is) or GetAbilityRecordByName("Name"): the call is rewritten to the ability's reference.
var recordByName = regexp.MustCompile(`(?s)//[^\n]*|/\*.*?\*/|GetAbilityRecordByName\(\s*"([^"]+)"\s*\)`)

// resolve rewrites the named references of text (outside comments) to $#<type>:<guid>$; a name that is no record, or two, stays as written.
// GetAbilityRecordByName("Name") becomes the ability's reference the same way (the compiler takes a reference, not a call that looks one up).
func (e *exprSet) resolve(text string) string {
	if e.named == nil {
		return text
	}
	if strings.Contains(text, "GetAbilityRecordByName") {
		text = recordByName.ReplaceAllStringFunc(text, func(m string) string {
			sub := recordByName.FindStringSubmatch(m)
			if len(sub) < 2 || sub[1] == "" {
				return m // a comment
			}
			guid := e.named["Ability"][sub[1]]
			if guid == 0 {
				return m
			}
			return fmt.Sprintf("$#%d:%d$", refTypes["Ability"], guid)
		})
	}
	if !strings.Contains(text, "$") {
		return text
	}
	var out strings.Builder
	last := 0
	for _, m := range namedRef.FindAllStringSubmatchIndex(text, -1) {
		if m[2] < 0 {
			continue // a comment
		}
		kind, name := text[m[2]:m[3]], strings.TrimSpace(text[m[4]:m[5]])
		guid := e.named[kind][name]
		if guid == 0 {
			continue
		}
		out.WriteString(text[last:m[0]])
		fmt.Fprintf(&out, "$#%d:%d$", refTypes[kind], guid)
		last = m[1]
	}
	out.WriteString(text[last:])
	return out.String()
}

// namedRecords is the guid of every skill tree, skill, effect and ability by name ($Skill:Name$ in an expression); a name two rows share is 0.
func namedRecords(src *source) map[string]map[string]int64 {
	out := map[string]map[string]int64{}
	for kind, schema := range map[string]string{"SkillTree": "SkillTreeRecordBase", "Skill": "SkillRecordBase", "Effect": "EffectRecordBase", "Ability": "AoCAbilityRecordBase", "F": "StatFormulaTypeRecordBase"} {
		m := map[string]int64{}
		for _, r := range src.rows[schema] {
			if _, dup := m[r.Name]; dup {
				m[r.Name] = 0
				continue
			}
			m[r.Name] = r.Guid
		}
		out[kind] = m
	}
	return out
}

// id is the ab_expr id of the text with its terms, 0 for a text with nothing in it.
func (e *exprSet) id(text string, terms map[int64]string) (int64, error) {
	if strings.TrimSpace(commentOnly.ReplaceAllString(text, "")) == "" {
		return 0, nil
	}
	text = e.resolve(text)
	if len(terms) > 0 {
		resolved := make(map[int64]string, len(terms))
		for k, v := range terms {
			resolved[k] = e.resolve(v)
		}
		terms = resolved
	}
	tj := "{}"
	if len(terms) > 0 {
		m := make(map[string]string, len(terms))
		for k, v := range terms {
			m[strconv.FormatInt(k, 10)] = v
		}
		b, err := json.Marshal(m) // sorted by key
		if err != nil {
			return 0, err
		}
		tj = string(b)
	}
	key := text + "\x00" + tj
	if id, ok := e.ids[key]; ok {
		return id, nil
	}
	e.next++
	if err := e.w.exec(`INSERT INTO ab_expr (id, text, terms, program, uses_target, error) VALUES (?, ?, ?, -1, 0, '')`, e.next, text, tj); err != nil {
		return 0, err
	}
	e.ids[key] = e.next
	return e.next, nil
}

// abBuilder is the state of one buildAbilities.
type abBuilder struct {
	w         *writer
	src       *source
	statNames map[int64]string
	exprs     *exprSet

	abilities, effects, hits, mods, projectiles, zones map[int64]*designdb.Row
	targetingNames                                     map[int64]string
}

func indexRows(rows []designdb.Row) map[int64]*designdb.Row {
	m := make(map[int64]*designdb.Row, len(rows))
	for i := range rows {
		m[rows[i].Guid] = &rows[i]
	}
	return m
}

// montageNetGuids reads montage_ids.tsv: the NetGUID of a montage by ability name and AnimationIndex, where a capture names exactly one.
func montageNetGuids() (map[string]map[int]uint64, error) {
	out := map[string]map[int]uint64{}
	for i, line := range strings.Split(montageIDs, "\n") {
		line = strings.TrimRight(line, "\r")
		if line == "" || line[0] == '#' || strings.HasPrefix(line, "ability\t") {
			continue
		}
		f := strings.Split(line, "\t")
		if len(f) != 5 {
			return nil, fmt.Errorf("montage_ids.tsv line %d: %d columns, want 5", i+1, len(f))
		}
		idx, e1 := strconv.Atoi(f[1])
		guid, e2 := strconv.ParseUint(f[2], 10, 64)
		distinct, e3 := strconv.Atoi(f[4])
		if e1 != nil || e2 != nil || e3 != nil {
			return nil, fmt.Errorf("montage_ids.tsv line %d: bad number", i+1)
		}
		if distinct != 1 {
			continue // two captures name two ids for the same index: the data does not have it
		}
		if out[f[0]] == nil {
			out[f[0]] = map[int]uint64{}
		}
		out[f[0]][idx] = guid
	}
	return out, nil
}

// CoA.C_Y_B

// refGuids reads a list of Id structs, or a map of keys to Id structs (the design data uses either for the same kind of reference), into
// guids (zeros dropped).
func refGuids(f *fields, name string) []int64 {
	v, ok := f.get(name)
	if !ok {
		f.fail(name, "array or map of Id structs", v, ok)
		return nil
	}
	var items []designdb.Value
	switch x := v.(type) {
	case []designdb.Value:
		items = x
	case designdb.Pairs:
		for _, p := range x {
			items = append(items, p.Value)
		}
	default:
		f.fail(name, "array or map of Id structs", v, true)
		return nil
	}
	var out []int64
	for i, it := range items {
		s, ok := it.(designdb.Struct)
		if !ok {
			f.fail(name, "Id structs", it, true)
			return nil
		}
		g, _ := idOf(f, fmt.Sprintf("%s: %s[%d]", f.where, name, i), s)
		if g != 0 {
			out = append(out, g)
		}
	}
	return out
}

// jsonOf marshals v (nil is "{}"... never: callers pass a map or a slice).
func jsonOf(v any) string {
	b, _ := json.Marshal(v)
	return string(b)
}

// buildAbilities fills the ab_* tables (and meta's ab.* counts).
func buildAbilities(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range []string{"ProjectileRecordBase", "LingeringEffectRecordBase", "AbilityTargetingRecordBase", "StatTypeDefRecordBase"} {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	statNames := map[int64]string{}
	for _, r := range src.rows["StatTypeDefRecordBase"] {
		statNames[r.Guid] = r.Name
	}
	b := &abBuilder{
		w: w, src: src, statNames: statNames,
		exprs:       &exprSet{w: w, ids: map[string]int64{}, named: namedRecords(src)},
		abilities:   indexRows(src.rows["AoCAbilityRecordBase"]),
		effects:     indexRows(src.rows["EffectRecordBase"]),
		hits:        indexRows(src.rows["AbilityHitRecordBase"]),
		mods:        indexRows(src.rows["StatModRecordBase"]),
		projectiles: indexRows(src.rows["ProjectileRecordBase"]),
		zones:       indexRows(src.rows["LingeringEffectRecordBase"]),
	}

	// ---- the ranks of every skill tree (plan D1's skill_rank: the one table of ranks); the abilities and effects they grant seed the closure ----
	abilitySeed, effectSeed := map[int64]bool{}, map[int64]bool{}
	rankRows, err := w.tx.Query(`SELECT DISTINCT ability_guid, effect_guid FROM skill_rank`)
	if err != nil {
		return fmt.Errorf("read skill_rank (buildTalents must run before buildAbilities): %w", err)
	}
	nranks := 0
	for rankRows.Next() {
		var ag, eg int64
		if err := rankRows.Scan(&ag, &eg); err != nil {
			rankRows.Close()
			return err
		}
		if b.abilities[ag] != nil {
			abilitySeed[ag] = true
		}
		if b.effects[eg] != nil {
			effectSeed[eg] = true
		}
		nranks++
	}
	if err := rankRows.Err(); err != nil {
		return err
	}
	rankRows.Close()
	// The abilities every character has and no tree grants (plan A7's Sprint, Dodge and Rest), and the Sprint's drain, which no record references.
	for _, g := range []int64{content.GuidSprint, content.GuidDodge, content.GuidRest} {
		if b.abilities[g] == nil {
			return fmt.Errorf("no Ability row %d", g)
		}
		abilitySeed[g] = true
	}
	if b.effects[content.GuidSprintStaminaDrain] == nil {
		return fmt.Errorf("no Effect row %d (the Sprint's stamina drain)", content.GuidSprintStaminaDrain)
	}
	effectSeed[content.GuidSprintStaminaDrain] = true
	// ... and the abilities of the retail captures by name (weapon combos, Guard, consumables)
	named, err := seedByName(b.abilities, abilitySeed)
	if err != nil {
		return err
	}
	opt.logf("ability_names.tsv: %d more abilities", named)
	opt.logf("skill_rank: %d distinct ability/effect pairs, %d abilities, %d effects granted by ranks (and the always-on ones)", nranks, len(abilitySeed), len(effectSeed))

	// ---- the closure ----
	abilitySet := map[int64]bool{}
	effectSet, hitSet, projSet, zoneSet := map[int64]bool{}, map[int64]bool{}, map[int64]bool{}, map[int64]bool{}
	var effQ, hitQ []int64
	addEffect := func(g int64) error {
		if g == 0 || effectSet[g] {
			return nil
		}
		if b.effects[g] == nil {
			return fmt.Errorf("no Effect row %d", g)
		}
		effectSet[g] = true
		effQ = append(effQ, g)
		return nil
	}
	addHit := func(g int64) error {
		if g == 0 || hitSet[g] {
			return nil
		}
		if b.hits[g] == nil {
			return fmt.Errorf("no AbilityHit row %d", g)
		}
		hitSet[g] = true
		hitQ = append(hitQ, g)
		return nil
	}
	addProj := func(g int64) ([]int64, error) {
		if g == 0 {
			return nil, nil
		}
		p := b.projectiles[g]
		if p == nil {
			return nil, fmt.Errorf("no Projectile row %d", g)
		}
		projSet[g] = true
		pf := newFields(fmt.Sprintf("Projectile %s (%d)", p.Name, g), p.Fields)
		hs := refGuids(pf, "HitsIds")
		return hs, pf.err
	}
	zoneLists := []string{"InitialHitsIds", "LingeringHitsIds", "OnBeginOverlapHitsIds", "OnEndOverlapHitsIds", "EndHitsIds"}
	addZone := func(g int64) (map[string][]int64, error) {
		if g == 0 {
			return nil, nil
		}
		z := b.zones[g]
		if z == nil {
			return nil, fmt.Errorf("no LingeringEffect row %d", g)
		}
		zoneSet[g] = true
		zf := newFields(fmt.Sprintf("LingeringEffect %s (%d)", z.Name, g), z.Fields)
		out := map[string][]int64{}
		for _, role := range zoneLists {
			out[role] = refGuids(zf, role)
		}
		return out, zf.err
	}
	for _, g := range sortedBools(abilitySeed) {
		abilitySet[g] = true
	}
	for _, g := range sortedBools(effectSeed) {
		if err := addEffect(g); err != nil {
			return err
		}
	}
	// the abilities' own: effects, hits (directly, projectiles, zones)
	for _, g := range sortedBools(abilitySet) {
		af := newFields(fmt.Sprintf("Ability %s (%d)", b.abilities[g].Name, g), b.abilities[g].Fields)
		_, effs := af.keyedIds("EffectsIds")
		_, hs := af.keyedIds("HitsIds")
		_, ps := af.keyedIds("ProjectilesIds")
		_, zs := af.keyedIds("LingeringEffectsIds")
		if af.err != nil {
			return af.err
		}
		for _, e := range effs {
			if err := addEffect(e); err != nil {
				return err
			}
		}
		for _, h := range hs {
			if err := addHit(h); err != nil {
				return err
			}
		}
		for _, p := range ps {
			phs, err := addProj(p)
			if err != nil {
				return err
			}
			for _, h := range phs {
				if err := addHit(h); err != nil {
					return err
				}
			}
		}
		for _, z := range zs {
			lists, err := addZone(z)
			if err != nil {
				return err
			}
			for _, role := range zoneLists {
				for _, h := range lists[role] {
					if err := addHit(h); err != nil {
						return err
					}
				}
			}
		}
	}
	// the worklist: effects fire hits, hits apply effects and fire projectiles and zones, which fire hits
	for len(effQ) > 0 || len(hitQ) > 0 {
		for len(effQ) > 0 {
			g := effQ[0]
			effQ = effQ[1:]
			ef := newFields(fmt.Sprintf("Effect %s (%d)", b.effects[g].Name, g), b.effects[g].Fields)
			for _, p := range effectPhases {
				for _, h := range refGuids(ef, p.field) {
					if err := addHit(h); err != nil {
						return err
					}
				}
			}
			if ef.err != nil {
				return ef.err
			}
		}
		for len(hitQ) > 0 {
			g := hitQ[0]
			hitQ = hitQ[1:]
			hf := newFields(fmt.Sprintf("AbilityHit %s (%d)", b.hits[g].Name, g), b.hits[g].Fields)
			for i, v := range hf.list("ApplyEffects") {
				s, ok := v.(designdb.Struct)
				if !ok {
					return fmt.Errorf("%s: ApplyEffects[%d] is %T", hf.where, i, v)
				}
				xf := newFields(fmt.Sprintf("%s: ApplyEffects[%d]", hf.where, i), s)
				e, _ := xf.id("EffectId")
				if xf.err != nil {
					return xf.err
				}
				if err := addEffect(e); err != nil {
					return err
				}
			}
			for i, v := range hf.list("EffectsAppliedToSelf") {
				s, ok := v.(designdb.Struct)
				if !ok {
					return fmt.Errorf("%s: EffectsAppliedToSelf[%d] is %T", hf.where, i, v)
				}
				xf := newFields(fmt.Sprintf("%s: EffectsAppliedToSelf[%d]", hf.where, i), s)
				e, _ := xf.id("Effect")
				if xf.err != nil {
					return xf.err
				}
				if err := addEffect(e); err != nil {
					return err
				}
			}
			for _, field := range []string{"ProjectilesToFire", "ProjectilesToSpawn"} {
				for i, v := range hf.list(field) {
					s, ok := v.(designdb.Struct)
					if !ok {
						return fmt.Errorf("%s: %s[%d] is %T", hf.where, field, i, v)
					}
					xf := newFields(fmt.Sprintf("%s: %s[%d]", hf.where, field, i), s)
					p, _ := xf.id("Projectile")
					if xf.err != nil {
						return xf.err
					}
					phs, err := addProj(p)
					if err != nil {
						return err
					}
					for _, h := range phs {
						if err := addHit(h); err != nil {
							return err
						}
					}
				}
			}
			for _, z := range refGuids(hf, "LingeringZonesIds") {
				lists, err := addZone(z)
				if err != nil {
					return err
				}
				for _, role := range zoneLists {
					for _, h := range lists[role] {
						if err := addHit(h); err != nil {
							return err
						}
					}
				}
			}
			if hf.err != nil {
				return hf.err
			}
		}
	}

	// ---- the rows, in guid order ----
	netGuids, err := montageNetGuids()
	if err != nil {
		return err
	}
	b.targetingNames = map[int64]string{}
	for _, r := range src.rows["AbilityTargetingRecordBase"] {
		b.targetingNames[r.Guid] = r.Name
	}
	for _, g := range sortedBools(abilitySet) {
		if err := b.writeAbility(b.abilities[g], netGuids); err != nil {
			return err
		}
	}
	for _, g := range sortedBools(hitSet) {
		if err := b.writeHit(b.hits[g]); err != nil {
			return err
		}
	}
	for _, g := range sortedBools(projSet) {
		if err := b.writeProjectile(b.projectiles[g]); err != nil {
			return err
		}
	}
	for _, g := range sortedBools(zoneSet) {
		if err := b.writeZone(b.zones[g], zoneLists); err != nil {
			return err
		}
	}
	for _, g := range sortedBools(effectSet) {
		if err := b.writeEffect(b.effects[g]); err != nil {
			return err
		}
	}
	opt.logf("ab_*: %d abilities, %d hits, %d projectiles, %d zones, %d effects, %d expressions", len(abilitySet), len(hitSet), len(projSet), len(zoneSet), len(effectSet), b.exprs.next)
	meta["ab.abilities"] = strconv.Itoa(len(abilitySet))
	meta["ab.hits"] = strconv.Itoa(len(hitSet))
	meta["ab.projectiles"] = strconv.Itoa(len(projSet))
	meta["ab.zones"] = strconv.Itoa(len(zoneSet))
	meta["ab.effects"] = strconv.Itoa(len(effectSet))
	meta["ab.exprs"] = strconv.FormatInt(b.exprs.next, 10)
	meta["ab.montage_ids"] = "montage_ids.tsv: retail captures, ability and AnimationIndex to the montage's NetGUID where exactly one id was seen"
	return b.buildLoot(opt, meta) // plan E1, stage 3, part 2: the reward tables (their expressions join the set)
}

func sortedBools(m map[int64]bool) []int64 {
	out := make([]int64, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

func (b *abBuilder) name(guid int64, rows map[int64]*designdb.Row) string {
	if r := rows[guid]; r != nil {
		return r.Name
	}
	return ""
}

var effectFilterRef = regexp.MustCompile(`^\s*GetEffect\(\)\s*==\s*\$#\d+:(\d+)\$\s*$`)

func (b *abBuilder) writeAbility(r *designdb.Row, netGuids map[string]map[int]uint64) error {
	f := newFields(fmt.Sprintf("Ability %s (%d)", r.Name, r.Guid), r.Fields)
	tags := f.tags("AbilityTags")
	display := f.text("AbilityName")
	held, input, targetReq, fallback := f.str("HeldAbilityBehavior"), f.str("AbilityActivationInput"), f.str("TargetActivationRequirement"), f.str("DefaultFallbackTargetType")
	mask, types := f.int("ApplicableTargets"), f.int("ApplicableTypes")
	weapon, sharedCD := f.str("WeaponRequirement"), f.str("SharedCooldownTag")
	disable, hold, holdTime := f.boolean("bDisableDuringCooldown"), f.boolean("bHoldAnimation"), f.float("MaximumHoldTime")
	air, swim, walk, mounted, hotbar := f.boolean("bUsableInAir"), f.boolean("bUsableWhileSwimming"), f.boolean("bUsableWhileWalking"), f.boolean("bUsableWhileMounted"), f.boolean("bUsableFromHotbar")
	areaRange, areaRadius, netSpeed := f.float("AreaTargetRange"), f.float("AreaTargetRadius"), f.float("NetworkingMaxSpeed")
	exprText := []struct{ key, text string }{
		{"cooldown", f.str("Cooldown")}, {"charges", f.str("CooldownCharges")}, {"cast", f.str("DynamicCastDuration")},
		{"requirement", f.str("ActivationRequirements")}, {"canbreak", f.str("CanBreakEffects")}, {"hasproc", f.str("HasProc")},
	}
	costs, montages := f.list("Costs"), f.list("AnimMontages")
	effKeys, effs := f.keyedIds("EffectsIds")
	hitKeys, hits := f.keyedIds("HitsIds")
	projKeys, projs := f.keyedIds("ProjectilesIds")
	zoneKeys, zones := f.keyedIds("LingeringEffectsIds")
	// LingeringEffectsIds: the zones the ability itself spawns when it is cast, in key order. A zone with no hit of its own (Bard_Dance_Mesmerizing's)
	// has no ab_ability_hit row, so this list is the only place the link from the ability to the zone is kept (the worldd runtime opens the zone's actor).
	zoneGuids := []int64{}
	for _, g := range zones {
		if g != 0 {
			zoneGuids = append(zoneGuids, g)
		}
	}
	extra := map[string][]int64{
		"ModifyCooldownsIds": refGuids(f, "ModifyCooldownsIds"), "ReactionsIds": refGuids(f, "ReactionsIds"), "TeleportRecordsIds": refGuids(f, "TeleportRecordsIds"),
		"LingeringEffectsIds": zoneGuids,
	}
	tgKeys, tgs := f.keyedIds("TargetingRecordsIds")
	if f.err != nil {
		return f.err
	}
	ids := map[string]int64{}
	for _, x := range exprText { // in this order: the ids are assigned as the expressions are met
		id, err := b.exprs.id(x.text, nil)
		if err != nil {
			return err
		}
		ids[x.key] = id
	}
	if err := b.w.exec(`INSERT INTO ab_ability (guid, name, display_name, tags, held_behavior, activation_input, target_requirement, fallback_target,
		applicable_targets, applicable_types, weapon_requirement, shared_cooldown_tag, disable_during_cooldown, max_hold_time, hold_animation,
		usable_in_air, usable_while_swimming, usable_while_walking, usable_while_mounted, usable_from_hotbar, area_range, area_radius,
		networking_max_speed, cooldown_expr, charges_expr, cast_expr, requirement_expr, can_break_expr, has_proc_expr, extra)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		r.Guid, r.Name, display, jsonOf(tags), held, input, targetReq, fallback, mask, types, weapon, sharedCD, b2i(disable), holdTime, b2i(hold),
		b2i(air), b2i(swim), b2i(walk), b2i(mounted), b2i(hotbar), areaRange, areaRadius, netSpeed,
		ids["cooldown"], ids["charges"], ids["cast"], ids["requirement"], ids["canbreak"], ids["hasproc"], jsonOf(extra)); err != nil {
		return err
	}
	for i, g := range tgs {
		name := ""
		if g != 0 {
			name = b.targetingName(g)
		}
		if err := b.w.exec(`INSERT INTO ab_targeting (ability_guid, key, targeting_guid, targeting_name) VALUES (?, ?, ?, ?)`, r.Guid, tgKeys[i], g, name); err != nil {
			return err
		}
	}
	// costs
	for ci, cv := range costs {
		cs, ok := cv.(designdb.Struct)
		if !ok {
			return fmt.Errorf("%s: Costs[%d] is %T", f.where, ci, cv)
		}
		cf := newFields(fmt.Sprintf("%s: Costs[%d]", f.where, ci), cs)
		enabled, charge, trig := cf.str("Enabled"), cf.str("ChargeCost"), cf.boolean("bTriggersCooldown")
		stats, effCosts := cf.list("StatCosts"), cf.list("EffectCosts")
		if cf.err != nil {
			return cf.err
		}
		enID, err := b.exprs.id(enabled, nil)
		if err != nil {
			return err
		}
		chID, err := b.exprs.id(charge, nil)
		if err != nil {
			return err
		}
		if len(stats) == 0 {
			if err := b.w.exec(`INSERT INTO ab_cost (ability_guid, cost_ord, stat_ord, enabled_expr, triggers_cooldown, charge_cost_expr, stat_guid, stat_name, amount_expr)
				VALUES (?, ?, -1, ?, ?, ?, 0, '', 0)`, r.Guid, ci, enID, b2i(trig), chID); err != nil {
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
			value := sf.str("Value")
			if sf.err != nil {
				return sf.err
			}
			if _, known := b.statNames[stat]; !known && stat != 0 { // a null reference is a cost with no stat
				return fmt.Errorf("%s: names stat %d, which is not a StatTypeDef row", sf.where, stat)
			}
			vID, err := b.exprs.id(value, nil)
			if err != nil {
				return err
			}
			if err := b.w.exec(`INSERT INTO ab_cost (ability_guid, cost_ord, stat_ord, enabled_expr, triggers_cooldown, charge_cost_expr, stat_guid, stat_name, amount_expr)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, ci, si, enID, b2i(trig), chID, stat, b.statNames[stat], vID); err != nil {
				return err
			}
		}
		for ei, ev := range effCosts {
			es, ok := ev.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: EffectCosts[%d] is %T", cf.where, ei, ev)
			}
			xf := newFields(fmt.Sprintf("%s: EffectCosts[%d]", cf.where, ei), es)
			filter, num := xf.str("EffectFilter"), xf.str("Num")
			if xf.err != nil {
				return xf.err
			}
			var eg int64
			if m := effectFilterRef.FindStringSubmatch(filter); m != nil {
				eg, _ = strconv.ParseInt(m[1], 10, 64)
			}
			nID, err := b.exprs.id(num, nil)
			if err != nil {
				return err
			}
			if err := b.w.exec(`INSERT INTO ab_effect_cost (ability_guid, cost_ord, ord, effect_guid, filter, num_expr) VALUES (?, ?, ?, ?, ?, ?)`,
				r.Guid, ci, ei, eg, filter, nID); err != nil {
				return err
			}
		}
	}
	// montages
	for mi, mv := range montages {
		ms, ok := mv.(designdb.Struct)
		if !ok {
			return fmt.Errorf("%s: AnimMontages[%d] is %T", f.where, mi, mv)
		}
		mf := newFields(fmt.Sprintf("%s: AnimMontages[%d]", f.where, mi), ms)
		pathVals := mf.list("AbilityAnimationSoftPaths")
		direction, expr, section := mf.str("AbilityDirection"), mf.str("AnimExpression"), mf.str("StartSectionName")
		hitKeysV, tkey := mf.list("HitKeys"), mf.int("TargetingKey")
		if mf.err != nil {
			return mf.err
		}
		var paths []string
		for _, p := range pathVals {
			s, ok := p.(string)
			if !ok {
				return fmt.Errorf("%s: a soft path is %T", mf.where, p)
			}
			paths = append(paths, s)
		}
		first := ""
		if len(paths) > 0 {
			first = paths[0]
		}
		var hk []int64
		for _, h := range hitKeysV {
			n, ok := h.(int64)
			if !ok {
				return fmt.Errorf("%s: a hit key is %T", mf.where, h)
			}
			hk = append(hk, n)
		}
		eID, err := b.exprs.id(expr, nil)
		if err != nil {
			return err
		}
		var net any
		if g, ok := netGuids[r.Name][mi]; ok {
			net = int64(g)
		}
		if err := b.w.exec(`INSERT INTO ab_montage (ability_guid, anim_index, soft_path, paths, direction, select_expr, start_section, hit_keys, targeting_key, net_guid)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, mi, first, jsonOf(append([]string{}, paths...)), direction, eID, section, jsonOf(append([]int64{}, hk...)), tkey, net); err != nil {
			return err
		}
	}
	for i, g := range effs {
		if g == 0 {
			continue
		}
		if err := b.w.exec(`INSERT INTO ab_ability_effect (ability_guid, key, effect_guid) VALUES (?, ?, ?)`, r.Guid, effKeys[i], g); err != nil {
			return err
		}
	}
	for i, g := range hits {
		if g == 0 {
			continue
		}
		if err := b.w.exec(`INSERT INTO ab_ability_hit (ability_guid, source, key, owner_guid, role, ord, hit_guid) VALUES (?, 'hit', ?, 0, '', 0, ?)`, r.Guid, hitKeys[i], g); err != nil {
			return err
		}
	}
	for i, g := range projs {
		if g == 0 {
			continue
		}
		p := b.projectiles[g]
		pf := newFields(fmt.Sprintf("Projectile %s (%d)", p.Name, g), p.Fields)
		hs := refGuids(pf, "HitsIds")
		if pf.err != nil {
			return pf.err
		}
		for hi, h := range hs {
			if err := b.w.exec(`INSERT INTO ab_ability_hit (ability_guid, source, key, owner_guid, role, ord, hit_guid) VALUES (?, 'projectile', ?, ?, 'HitsIds', ?, ?)`,
				r.Guid, projKeys[i], g, hi, h); err != nil {
				return err
			}
		}
	}
	for i, g := range zones {
		if g == 0 {
			continue
		}
		z := b.zones[g]
		zf := newFields(fmt.Sprintf("LingeringEffect %s (%d)", z.Name, g), z.Fields)
		for _, role := range []string{"InitialHitsIds", "LingeringHitsIds", "OnBeginOverlapHitsIds", "OnEndOverlapHitsIds", "EndHitsIds"} {
			for hi, h := range refGuids(zf, role) {
				if err := b.w.exec(`INSERT INTO ab_ability_hit (ability_guid, source, key, owner_guid, role, ord, hit_guid) VALUES (?, 'zone', ?, ?, ?, ?, ?)`,
					r.Guid, zoneKeys[i], g, role, hi, h); err != nil {
					return err
				}
			}
		}
		if zf.err != nil {
			return zf.err
		}
	}
	return nil
}

// targetingName is the name of a targeting record (they are in no table kept: the row name comes from the ability's own reference
// when it has one; the design data's Id struct carries "None" for most, so the guid is the key).
func (b *abBuilder) targetingName(guid int64) string {
	if n, ok := b.targetingNames[guid]; ok {
		return n
	}
	return ""
}

func (b *abBuilder) writeHit(r *designdb.Row) error {
	f := newFields(fmt.Sprintf("AbilityHit %s (%d)", r.Name, r.Guid), r.Fields)
	mask, types := f.int("ApplicableTargets"), f.int("ApplicableTypes")
	netType, fallback := f.str("NetworkType"), f.str("DefaultFallbackTargetType")
	req, delay, threat, maxTargets := f.str("ApplicationRequirements"), f.str("HitDelay"), f.str("ThreatToTarget"), f.str("MaxTargetsPerHit")
	applies, selfs := f.list("ApplyEffects"), f.list("EffectsAppliedToSelf")
	fires, spawns := f.list("ProjectilesToFire"), f.list("ProjectilesToSpawn")
	zones := refGuids(f, "LingeringZonesIds")
	teleport, _ := f.id("TeleportId")
	resurrect, _ := f.id("ResurrectId")
	// A hit with an effect to show (OnHitFXId or OnActivateFXId) is confirmed to the client even when it has no stat mod: the retail server sent the
	// confirmation of every such hit the client asked for (plan D2, stage 2: 89 of 89 requests of hits with a FX and no mod, 7 of 75 without one).
	hitFX, _ := f.id("OnHitFXId")
	activateFX, _ := f.id("OnActivateFXId")
	extra := map[string]any{
		"HitFX": hitFX != 0 || activateFX != 0, "MaxNumEffectsToRemove": f.int("MaxNumEffectsToRemove"),
		"ModifyCooldownsIds": refGuids(f, "ModifyCooldownsIds"), "ModifyEffectsIds": refGuids(f, "ModifyEffectsIds"),
		"EffectsToRemoveIds": refGuids(f, "EffectsToRemoveIds"), "ReactionsIds": refGuids(f, "ReactionsIds"),
		"TeleportId": teleport, "ResurrectId": resurrect,
		"LingeringZonesIds": zones, // the zones the hit spawns (also in ab_hit_zone)
	}
	// Plan E1, stage 3: what the damage rules read of a hit besides its mods: its event tags (the element, Melee, ComboStep.End ...), the rates that
	// override the stats' crit, block and miss chances (expressions, 0 for the default), what it forces and the life states it applies to.
	extra["EventTags"] = f.tags("EventTags")
	extra["ApplicableLifeStates"] = f.int("ApplicableLifeStates")
	for _, k := range []string{"bForceCantCrit", "bForceCantBlock", "bForceCantMiss", "bForceRollCrit", "bForceRollBlock", "bForceRollMiss", "bShouldUseItemTags"} {
		extra[k] = f.boolean(k)
	}
	if f.err != nil {
		return f.err
	}
	var ids [4]int64
	for i, t := range []string{req, delay, threat, maxTargets} {
		id, err := b.exprs.id(t, nil)
		if err != nil {
			return err
		}
		ids[i] = id
	}
	for _, k := range []string{"CritRate", "BlockRate", "MissRate"} {
		id, err := b.exprs.id(f.str(k), nil)
		if err != nil {
			return err
		}
		extra[k+"Expr"] = id
	}
	if err := b.w.exec(`INSERT INTO ab_hit (guid, name, net_type, applicable_targets, applicable_types, fallback_target, requirement_expr, delay_expr, threat_expr, max_targets_expr, extra)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, netType, mask, types, fallback, ids[0], ids[1], ids[2], ids[3], jsonOf(extra)); err != nil {
		return err
	}
	for i, v := range applies {
		s, ok := v.(designdb.Struct)
		if !ok {
			return fmt.Errorf("%s: ApplyEffects[%d] is %T", f.where, i, v)
		}
		xf := newFields(fmt.Sprintf("%s: ApplyEffects[%d]", f.where, i), s)
		g, _ := xf.id("EffectId")
		reqT, stackT, durT := xf.str("EffectApplicationRequirement"), xf.str("StackCount"), xf.str("EffectDuration")
		if xf.err != nil {
			return xf.err
		}
		if g == 0 {
			continue
		}
		var e [3]int64
		for k, t := range []string{reqT, stackT, durT} {
			id, err := b.exprs.id(t, nil)
			if err != nil {
				return err
			}
			e[k] = id
		}
		if err := b.w.exec(`INSERT INTO ab_hit_effect (hit_guid, kind, ord, effect_guid, requirement_expr, stack_count_expr, duration_expr, on_hit_behavior, max_applications)
			VALUES (?, 'apply', ?, ?, ?, ?, ?, '', 0)`, r.Guid, i, g, e[0], e[1], e[2]); err != nil {
			return err
		}
	}
	for i, v := range selfs {
		s, ok := v.(designdb.Struct)
		if !ok {
			return fmt.Errorf("%s: EffectsAppliedToSelf[%d] is %T", f.where, i, v)
		}
		xf := newFields(fmt.Sprintf("%s: EffectsAppliedToSelf[%d]", f.where, i), s)
		g, _ := xf.id("Effect")
		stackT, behavior, maxApps := xf.str("StackCount"), xf.str("OnHitBehavior"), xf.int("MaxNumApplications")
		if xf.err != nil {
			return xf.err
		}
		if g == 0 {
			continue
		}
		sid, err := b.exprs.id(stackT, nil)
		if err != nil {
			return err
		}
		if err := b.w.exec(`INSERT INTO ab_hit_effect (hit_guid, kind, ord, effect_guid, requirement_expr, stack_count_expr, duration_expr, on_hit_behavior, max_applications)
			VALUES (?, 'self', ?, ?, 0, ?, 0, ?, ?)`, r.Guid, i, g, sid, behavior, maxApps); err != nil {
			return err
		}
	}
	for _, k := range []struct {
		kind string
		list []designdb.Value
	}{{"fire", fires}, {"spawn", spawns}} {
		for i, v := range k.list {
			s, ok := v.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: projectile %s[%d] is %T", f.where, k.kind, i, v)
			}
			xf := newFields(fmt.Sprintf("%s: projectile %s[%d]", f.where, k.kind, i), s)
			g, _ := xf.id("Projectile")
			if xf.err != nil {
				return xf.err
			}
			if err := b.w.exec(`INSERT INTO ab_hit_projectile (hit_guid, kind, ord, projectile_guid) VALUES (?, ?, ?, ?)`, r.Guid, k.kind, i, g); err != nil {
				return err
			}
		}
	}
	for i, g := range zones {
		if err := b.w.exec(`INSERT INTO ab_hit_zone (hit_guid, ord, zone_guid) VALUES (?, ?, ?)`, r.Guid, i, g); err != nil {
			return err
		}
	}
	return b.writeMods("hit", r, f)
}

func (b *abBuilder) writeProjectile(r *designdb.Row) error {
	f := newFields(fmt.Sprintf("Projectile %s (%d)", r.Name, r.Guid), r.Fields)
	typ := f.str("ProjectileType")
	speed, rng, life := f.float("Speed"), f.float("Range"), f.float("LifeTime")
	maxHits, chain := f.int("MaxProjectileHits"), f.int("ChainHitCount")
	chainRadius, radius := f.float("ChainRadius"), f.float("SphereCollisionRadius")
	hits := refGuids(f, "HitsIds")
	if f.err != nil {
		return f.err
	}
	if err := b.w.exec(`INSERT INTO ab_projectile (guid, name, projectile_type, speed, range, life_time, max_hits, chain_hit_count, chain_radius, sphere_radius)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, typ, speed, rng, life, maxHits, chain, chainRadius, radius); err != nil {
		return err
	}
	for i, h := range hits {
		if err := b.w.exec(`INSERT INTO ab_projectile_hit (projectile_guid, ord, hit_guid) VALUES (?, ?, ?)`, r.Guid, i, h); err != nil {
			return err
		}
	}
	return nil
}

func (b *abBuilder) writeZone(r *designdb.Row, roles []string) error {
	f := newFields(fmt.Sprintf("LingeringEffect %s (%d)", r.Name, r.Guid), r.Fields)
	life, tick, delay, speed := f.float("LifeTime"), f.float("TickRate"), f.float("DelayTime"), f.float("Speed")
	global, charges := f.boolean("bUseGlobalTickRate"), f.int("NumberOfCharges")
	delayType := f.str("DelayType")
	expireHits, expireIf := f.str("ExpireAfterAppliedHits"), f.str("ExpireIfExpressionTrue")
	targeting, _ := f.id("LingeringHitTargetingInfoId")
	if f.err != nil {
		return f.err
	}
	h, err := b.exprs.id(expireHits, nil)
	if err != nil {
		return err
	}
	x, err := b.exprs.id(expireIf, nil)
	if err != nil {
		return err
	}
	if err := b.w.exec(`INSERT INTO ab_zone (guid, name, life_time, tick_rate, use_global_tick_rate, charges, delay_time, delay_type, speed, expire_after_hits_expr, expire_if_expr, targeting_guid)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, life, tick, b2i(global), charges, delay, delayType, speed, h, x, targeting); err != nil {
		return err
	}
	for _, role := range roles {
		for i, g := range refGuids(f, role) {
			if err := b.w.exec(`INSERT INTO ab_zone_hit (zone_guid, role, ord, hit_guid) VALUES (?, ?, ?, ?)`, r.Guid, role, i, g); err != nil {
				return err
			}
		}
	}
	return f.err
}

// == C.O.A | C_Y_B ==

func (b *abBuilder) writeEffect(r *designdb.Row) error {
	f := newFields(fmt.Sprintf("Effect %s (%d)", r.Name, r.Guid), r.Fields)
	tags, etags := f.tags("Tags"), f.tags("EffectTags")
	display, category := f.text("EffectName"), f.str("EffectCategory")
	infinite, tick, ignoreTick := f.boolean("bInfiniteDuration"), f.float("TickTimer"), f.boolean("bIgnoreInitialTick")
	stackType, refresh, limit := f.str("EffectStackType"), f.str("StackDurationRefresh"), f.int("GlobalStackLimit")
	persDeath, persRelog, dispel, significant := f.boolean("bPersistsThroughDeath"), f.boolean("bPersistsRelog"), f.boolean("bDispellable"), f.boolean("bSignificant")
	durT, reqT, inPlaceT := f.str("EffectDuration"), f.str("ApplicationRequirements"), f.str("InPlaceStackLimit")
	cc, _ := f.id("CrowdControlId")
	extra := map[string][]int64{"ReactionsIds": refGuids(f, "ReactionsIds"), "HitModsIds": refGuids(f, "HitModsIds"), "FragilityDefinitionsIds": refGuids(f, "FragilityDefinitionsIds")}
	if f.err != nil {
		return f.err
	}
	var ids [3]int64
	for i, t := range []string{durT, reqT, inPlaceT} {
		id, err := b.exprs.id(t, nil)
		if err != nil {
			return err
		}
		ids[i] = id
	}
	if err := b.w.exec(`INSERT INTO ab_effect (guid, name, display_name, category, tags, effect_tags, infinite_duration, duration_expr, tick_timer, ignore_initial_tick,
		stack_type, global_stack_limit, in_place_stack_limit_expr, stack_duration_refresh, persists_through_death, persists_relog, dispellable, significant,
		requirement_expr, crowd_control_guid, extra) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		r.Guid, r.Name, display, category, jsonOf(tags), jsonOf(etags), b2i(infinite), ids[0], tick, b2i(ignoreTick), stackType, limit, ids[2], refresh,
		b2i(persDeath), b2i(persRelog), b2i(dispel), b2i(significant), ids[1], cc, jsonOf(extra)); err != nil {
		return err
	}
	for _, p := range effectPhases {
		for i, g := range refGuids(f, p.field) {
			if err := b.w.exec(`INSERT INTO ab_effect_hit (effect_guid, phase, ord, hit_guid) VALUES (?, ?, ?, ?)`, r.Guid, p.phase, i, g); err != nil {
				return err
			}
		}
	}
	return b.writeMods("effect", r, f)
}

// writeMods writes the StatMod rows an effect or hit lists (StatModsIds).
func (b *abBuilder) writeMods(kind string, r *designdb.Row, f *fields) error {
	guids := f.ids("StatModsIds")
	if f.err != nil {
		return f.err
	}
	for i, g := range guids {
		if g == 0 {
			continue
		}
		m := b.mods[g]
		if m == nil {
			return fmt.Errorf("%s: StatModsIds names StatMod %d, which is not a row", f.where, g)
		}
		mf := newFields(fmt.Sprintf("StatMod %s (%d)", m.Name, g), m.Fields)
		stat, _ := mf.id("StatRefId")
		value := mf.str("Value")
		shield := mf.boolean("bUseShieldAbsorb")
		termVals := mf.list("ValueInputTerms")
		if mf.err != nil {
			return mf.err
		}
		if _, known := b.statNames[stat]; !known && stat != 0 {
			return fmt.Errorf("%s: names stat %d, which is not a StatTypeDef row", mf.where, stat)
		}
		terms := map[int64]string{}
		for ti, tv := range termVals {
			ts, ok := tv.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: ValueInputTerms[%d] is %T", mf.where, ti, tv)
			}
			tf := newFields(fmt.Sprintf("%s: ValueInputTerms[%d]", mf.where, ti), ts)
			tg, _ := tf.id("TermId")
			text := tf.str("Value")
			if tf.err != nil {
				return tf.err
			}
			terms[tg] = text
		}
		vID, err := b.exprs.id(value, terms)
		if err != nil {
			return err
		}
		if err := b.w.exec(`INSERT INTO ab_mod (owner_kind, owner_guid, ord, stat_mod_guid, stat_guid, stat_name, value_expr, shield_absorb) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			kind, r.Guid, i, g, stat, b.statNames[stat], vID, b2i(shield)); err != nil {
			return err
		}
	}
	return nil
}

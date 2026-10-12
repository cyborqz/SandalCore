// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	_ "embed"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// The combat configuration (plan E1, stage 3): the DamageProfile of DefaultCombat.ini's [/Script/GameSystemsPlugin.CombatConfig] GameSettings.Stats,
// the stats the game's damage formulas read, and the numbers of the combat rules. The client's Config directory is the source when given; without it the
// copy of the file embedded here is (the profile is design data the server cannot work without, unlike StatRegenTime's documented default).

//go:embed default_combat.ini
var defaultCombatINI string

// parenSpan is the text of the parenthesised group that opens at s[open] ("(" at open), without the parentheses; quoted text is skipped.
func parenSpan(s string, open int) (string, bool) {
	depth, quoted := 0, false
	for i := open; i < len(s); i++ {
		switch c := s[i]; {
		case c == '"':
			quoted = !quoted
		case quoted:
		case c == '(':
			depth++
		case c == ')':
			depth--
			if depth == 0 {
				return s[open+1 : i], true
			}
		}
	}
	return "", false
}

// group is the contents of the struct property name=(...) of s (the first one where name starts the text or follows "(" or ",").
func group(s, name string) (string, bool) {
	re := regexp.MustCompile(`(?:^|[(,])` + regexp.QuoteMeta(name) + `=\(`)
	loc := re.FindStringIndex(s)
	if loc == nil {
		return "", false
	}
	return parenSpan(s, loc[1]-1)
}

// guidOf is the Guid of the record reference name=(Guid=N,...) of s.
func guidOf(s, name string) (int64, bool) {
	re := regexp.MustCompile(`(?:^|[(,])` + regexp.QuoteMeta(name) + `=\(Guid=(-?[0-9]+)`)
	m := re.FindStringSubmatch(s)
	if m == nil {
		return 0, false
	}
	v, err := strconv.ParseInt(m[1], 10, 64)
	return v, err == nil
}

// (C_Y_B + CoA)

// numberOf is the number of the property name=N of s.
func numberOf(s, name string) (float64, bool) {
	re := regexp.MustCompile(`(?:^|[(,])` + regexp.QuoteMeta(name) + `=(-?[0-9]+(?:\.[0-9]+)?)`)
	m := re.FindStringSubmatch(s)
	if m == nil {
		return 0, false
	}
	v, err := strconv.ParseFloat(m[1], 64)
	return v, err == nil
}

var elementPattern = regexp.MustCompile(`\(\(TagName="([^"]+)"\), \(Damage=\(Guid=([0-9]+)\),DamageMult=\(Guid=([0-9]+)\),DamageAddMult=\(Guid=([0-9]+)\),Mitigation=\(Guid=([0-9]+)\)\)\)`)

// combatConfig is what the file holds.
type combatConfig struct {
	profile  map[string]int64
	elements []content.CombatElement
	numbers  map[string]float64
	source   string
}

// gameSettings is the GameSettings line of the CombatConfig section.
func gameSettings(text string) (string, bool) {
	section := ""
	for _, line := range strings.Split(strings.TrimPrefix(text, "\xef\xbb\xbf"), "\n") {
		line = strings.TrimRight(line, "\r")
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			section = line[1 : len(line)-1]
			continue
		}
		if section == combatConfigSection && strings.HasPrefix(line, "GameSettings=") {
			return line, true
		}
	}
	return "", false
}

// parseCombat reads the profile and the numbers out of DefaultCombat.ini's text.
func parseCombat(text, source string) (*combatConfig, error) {
	line, ok := gameSettings(text)
	if !ok {
		return nil, fmt.Errorf("%s has no GameSettings in [%s]", source, combatConfigSection)
	}
	stats, ok := group(line, "Stats")
	if !ok {
		return nil, fmt.Errorf("%s: GameSettings has no Stats", source)
	}
	prof, ok := group(stats, "DamageProfile")
	if !ok {
		return nil, fmt.Errorf("%s: Stats has no DamageProfile", source)
	}
	cc := &combatConfig{profile: map[string]int64{}, numbers: map[string]float64{}, source: source}
	put := func(key string, guid int64, ok bool) error {
		if !ok {
			return fmt.Errorf("%s: the DamageProfile has no %s", source, key)
		}
		cc.profile[key] = guid
		return nil
	}
	for _, k := range []string{"PowerMult", "PowerAddMult", "DamagePowerMult", "DamagePowerAddMult"} {
		g, ok := guidOf(prof, k)
		if err := put(k, g, ok); err != nil {
			return nil, err
		}
	}
	corr, _ := group(prof, "CorruptionProfile")
	g, gok := guidOf(corr, "CorruptionStat")
	if err := put("Corruption", g, gok); err != nil {
		return nil, err
	}
	for _, asp := range []string{"Physical", "Magical"} {
		body, ok := group(prof, asp+"Aspect")
		if !ok {
			return nil, fmt.Errorf("%s: the DamageProfile has no %sAspect", source, asp)
		}
		for _, k := range []string{"Damage", "DamageMult", "DamageAddMult", "Mitigation"} {
			g, ok := guidOf(body, k)
			if err := put(asp+"."+k, g, ok); err != nil {
				return nil, err
			}
		}
		for _, k := range []string{"Penetration", "CriticalPower", "CriticalMitigation", "BlockMitigation"} {
			g, ok := guidOf(prof, asp+k)
			if err := put(asp+"."+k, g, ok); err != nil {
				return nil, err
			}
		}
	}
	for _, m := range elementPattern.FindAllStringSubmatch(prof, -1) {
		var e content.CombatElement
		e.Tag = m[1]
		for i, dst := range []*int64{&e.Damage, &e.DamageMult, &e.DamageAddMult, &e.Mitigation} {
			v, err := strconv.ParseInt(m[2+i], 10, 64)
			if err != nil {
				return nil, fmt.Errorf("%s: element %s: %w", source, e.Tag, err)
			}
			*dst = v
		}
		cc.elements = append(cc.elements, e)
	}
	if len(cc.elements) == 0 {
		return nil, fmt.Errorf("%s: the DamageProfile has no elements", source)
	}
	sort.Slice(cc.elements, func(i, j int) bool { return cc.elements[i].Tag < cc.elements[j].Tag })

	for _, n := range []struct {
		key, name, scope string
	}{
		{content.CombatInCombatTimer, "InCombatTimer", "General"},
		{content.CombatOutOfRange, "OutOfRangeDistance", "General"},
		{content.CombatActivationLeeway, "ServerActivationDistanceLeeway", "Abilities"},
		{content.CombatProjectileHitDist, "HitDistance", "Projectiles"},
		{content.CombatThirdPartyHit, "ThirdPartyHitDistance", "Networking"},
		{content.CombatMaxHitsPerRPC, "MaxHitsPerRPC", "Networking"},
		{content.CombatStatRegenTime, "StatRegenTime", "Stats"},
		{content.CombatGlobalDamageMult, "GlobalDamageMultiplier", "Stats"},
		{content.CombatGlobalHealingMult, "GlobalHealingMultiplier", "Stats"},
	} {
		scope, ok := group(line, n.scope)
		if n.scope == "Stats" {
			scope, ok = stats, true
		}
		if !ok {
			return nil, fmt.Errorf("%s: GameSettings has no %s", source, n.scope)
		}
		v, ok := numberOf(scope, n.name)
		if !ok {
			return nil, fmt.Errorf("%s: %s.%s is missing", source, n.scope, n.name)
		}
		cc.numbers[n.key] = v
	}
	return cc, nil
}

// loadCombat reads DefaultCombat.ini from the Config directory, or the embedded copy when there is none.
func loadCombat(configDir string) (*combatConfig, error) {
	if configDir != "" {
		path := filepath.Join(configDir, "DefaultCombat.ini")
		data, err := os.ReadFile(path)
		switch {
		case err == nil:
			return parseCombat(string(data), "DefaultCombat.ini")
		case !errors.Is(err, os.ErrNotExist):
			return nil, err
		}
	}
	return parseCombat(defaultCombatINI, "DefaultCombat.ini (the copy built into contentc)")
}

// [C.Y.B - C-O-A]

// buildCombat fills combat_profile, combat_element and combat_config. Every stat the profile names must be a stat of the design data.
func buildCombat(w *writer, src *source, opt *Options, meta map[string]string) error {
	cc, err := loadCombat(opt.Config)
	if err != nil {
		return err
	}
	check := func(what string, guid int64) error {
		if src.statNames != nil && src.statNames[guid] == "" {
			return fmt.Errorf("%s: %s names stat %d, which the design data does not have", cc.source, what, guid)
		}
		return nil
	}
	keys := make([]string, 0, len(cc.profile))
	for k := range cc.profile {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if err := check("DamageProfile."+k, cc.profile[k]); err != nil {
			return err
		}
		if err := w.exec(`INSERT INTO combat_profile (key, stat_guid) VALUES (?, ?)`, k, cc.profile[k]); err != nil {
			return err
		}
	}
	for _, e := range cc.elements {
		for _, g := range []int64{e.Damage, e.DamageMult, e.DamageAddMult, e.Mitigation} {
			if err := check("the element "+e.Tag, g); err != nil {
				return err
			}
		}
		if err := w.exec(`INSERT INTO combat_element (tag, damage, damage_mult, damage_add_mult, mitigation) VALUES (?, ?, ?, ?, ?)`,
			e.Tag, e.Damage, e.DamageMult, e.DamageAddMult, e.Mitigation); err != nil {
			return err
		}
	}
	nkeys := make([]string, 0, len(cc.numbers))
	for k := range cc.numbers {
		nkeys = append(nkeys, k)
	}
	sort.Strings(nkeys)
	for _, k := range nkeys {
		if err := w.exec(`INSERT INTO combat_config (key, value, source) VALUES (?, ?, ?)`, k, cc.numbers[k], cc.source+" GameSettings"); err != nil {
			return err
		}
	}
	rels, err := src.need("CharacterRelationshipRecordBase")
	if err != nil {
		return err
	}
	for _, r := range rels {
		f := newFields(fmt.Sprintf("CharacterRelationship %s (%d)", r.Name, r.Guid), r.Fields)
		if err := w.exec(`INSERT INTO relationship (guid, name, permissions) VALUES (?, ?, ?)`, r.Guid, r.Name, f.int("CombatPermissions")); err != nil {
			return err
		}
		if f.err != nil {
			return f.err
		}
	}
	aligns, err := src.need("CharacterAlignmentRecordBase")
	if err != nil {
		return err
	}
	nrel := 0
	for _, r := range aligns {
		f := newFields(fmt.Sprintf("CharacterAlignment %s (%d)", r.Name, r.Guid), r.Fields)
		seen := map[int64]bool{}
		pairs, isPairs := f.s["RelationshipsIds"].(designdb.Pairs)
		if !isPairs {
			return fmt.Errorf("%s: RelationshipsIds is %T, want a map", f.where, f.s["RelationshipsIds"])
		}
		for i, p := range pairs {
			ks, ok1 := p.Key.(designdb.Struct)
			vs, ok2 := p.Value.(designdb.Struct)
			if !ok1 || !ok2 {
				return fmt.Errorf("%s: RelationshipsIds[%d] has no Key and Value ids", f.where, i)
			}
			other, _ := idOf(f, f.where+": RelationshipsIds Key", ks)
			rel, _ := idOf(f, f.where+": RelationshipsIds Value", vs)
			if f.err != nil {
				return f.err
			}
			if seen[other] {
				continue
			}
			seen[other] = true
			if err := w.exec(`INSERT INTO alignment_relation (alignment, other, relationship) VALUES (?, ?, ?)`, r.Guid, other, rel); err != nil {
				return err
			}
			nrel++
		}
		if f.err != nil {
			return f.err
		}
	}
	meta["typed.relationship"] = strconv.Itoa(len(rels))
	meta["typed.alignment_relation"] = strconv.Itoa(nrel)
	meta["typed.combat_profile"] = strconv.Itoa(len(keys))
	meta["typed.combat_element"] = strconv.Itoa(len(cc.elements))
	meta["typed.combat_config"] = strconv.Itoa(len(nkeys))
	return nil
}

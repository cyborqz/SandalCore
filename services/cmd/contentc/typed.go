// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"sort"
	"strconv"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// recordTypes are the record structs the typed tables read and the TypeIds
// content names for them; Build checks that the database agrees. In
// CacheDB.dbc they are the tables 0 (Ability, 2130 rows), 2 (AbilityHit, 3608),
// 3 (StatTypeDef, 732), 4 (Effect, 2794), 44 (CharacterClasses, 16), 296
// (StatCurve, 122), 303 (StatInitializerList, 145) and 307 (StatMod, 3133);
// Build finds them by record struct, not by position.
var recordTypes = map[string]uint64{
	"AoCAbilityRecordBase":          content.TypeAbility,
	"AbilityHitRecordBase":          content.TypeAbilityHit,
	"StatTypeDefRecordBase":         content.TypeStatDef,
	"EffectRecordBase":              content.TypeEffect,
	"CharacterClassesRecordBase":    content.TypeCharacterClasses,
	"StatCurveRecordBase":           content.TypeStatCurve,
	"StatInitializerListRecordBase": content.TypeStatInitializerList,
	"StatModRecordBase":             content.TypeStatMod,
}

// buildTyped fills the typed tables from the decoded rows.
func buildTyped(w *writer, src *source, opt *Options, meta map[string]string) error {
	for schema, want := range recordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
		if got := src.typeIDs[schema]; got != want {
			return fmt.Errorf("%s has TypeId %d in the database, content expects %d", schema, got, want)
		}
	}
	statRows := src.rows["StatTypeDefRecordBase"]
	statNames, err := buildStatDefs(w, statRows)
	if err != nil {
		return err
	}
	opt.logf("stat_def: %d stats", len(statRows))

	curves, err := buildCurves(w, src.rows["StatCurveRecordBase"], src.artisan, opt)
	if err != nil {
		return err
	}
	opt.logf("stat_curve: %d curves", len(src.rows["StatCurveRecordBase"]))
	src.curves, src.statNames = curves, statNames

	lists, err := buildStatInits(w, src.rows["StatInitializerListRecordBase"], statNames)
	if err != nil {
		return err
	}
	opt.logf("stat_init: %d lists", len(lists))

	n, err := buildClasses(w, src.rows["CharacterClassesRecordBase"], lists, curves, statNames)
	if err != nil {
		return err
	}
	opt.logf("class_stat_base: %d rows", n)

	sk, err := buildSkills(w, src, statNames)
	if err != nil {
		return err
	}
	opt.logf("ability: %d, effect: %d, hit: %d", len(sk.abilities), len(sk.effects), len(sk.hits))

	rules, err := buildStaminaRules(w, opt, statRows, lists, sk)
	if err != nil {
		return err
	}
	opt.logf("stamina_rule: %d rules", rules)

	abilities, effects, err := buildAbilityRuntime(w, sk, statNames)
	if err != nil {
		return err
	}
	opt.logf("ability_runtime: %d abilities, effect_runtime: %d effects", abilities, effects)
	meta["typed.stat_def"] = strconv.Itoa(len(statRows))
	meta["typed.class_stat_base"] = strconv.Itoa(n)
	return nil
}

// [CoA + C.Y.B]

// buildStatDefs fills stat_def and returns the stat names by Guid.
func buildStatDefs(w *writer, rows []designdb.Row) (map[int64]string, error) {
	names := make(map[int64]string, len(rows))
	for _, r := range rows {
		f := newFields(fmt.Sprintf("StatTypeDef %s (%d)", r.Name, r.Guid), r.Fields)
		rep := int(f.int("Replication"))
		audience, repField := content.Audience(rep)
		var repArg any
		if repField != "" {
			repArg = repField
		}
		args := []any{
			r.Guid, r.Name, f.str("GameplayType"), f.str("StatType"), rep, audience, repArg,
			f.str("base"), f.str("ValueMin"), f.str("ValueMax"), f.str("RegenRate"),
			f.str("OnSpawn"), f.str("OnDeath"), f.str("OnRevive"),
			b2i(f.boolean("bPersistValue")), b2i(f.boolean("bHidden")), b2i(f.boolean("bClampStatFinalValue")),
			b2i(f.boolean("bMultiplyStatMods")), b2i(f.boolean("bRegenWhileDead")), b2i(f.boolean("bRecalculateOnCombatState")),
		}
		if f.err != nil {
			return nil, f.err
		}
		if _, dup := names[r.Guid]; dup {
			return nil, fmt.Errorf("two StatTypeDef rows have Guid %d", r.Guid)
		}
		names[r.Guid] = r.Name
		if err := w.exec(`INSERT INTO stat_def (guid, name, gameplay_type, stat_type, replication, audience, rep_field,
			base_expr, min_expr, max_expr, regen_expr, on_spawn, on_death, on_revive, persist_value, hidden,
			clamp_final_value, multiply_mods, regen_while_dead, recalc_on_combat_state)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, args...); err != nil {
			return nil, err
		}
	}
	return names, nil
}

// buildCurves fills stat_curve_def and stat_curve and returns the curves with
// keys as a table EvalCurve can use.
//
// A curve that has no keys of its own and names a pak asset (ExternalCurve) that artisan_curves.tsv holds gets the asset's keys: the three profession level-up
// curves (Gathering stage 2).
func buildCurves(w *writer, rows []designdb.Row, artisan *artisanCurves, opt *Options) (*aocexpr.CurveMap, error) {
	cm := &aocexpr.CurveMap{TypeID: content.TypeStatCurve, Curves: map[int64]*aocexpr.Curve{}}
	for _, r := range rows {
		f := newFields(fmt.Sprintf("StatCurve %s (%d)", r.Name, r.Guid), r.Fields)
		curve := f.strukt("Curve")
		cf := newFields(f.where+": Curve", curve)
		external := cf.str("ExternalCurve")
		ed := newFields(f.where+": Curve.EditorCurveData", cf.strukt("EditorCurveData"))
		def := ed.float("DefaultValue")
		keys := ed.list("Keys")
		if err := firstErr(f, cf, ed); err != nil {
			return nil, err
		}
		if _, dup := cm.Curves[r.Guid]; dup {
			return nil, fmt.Errorf("two StatCurve rows have Guid %d", r.Guid)
		}
		var ks []aocexpr.Key
		for i, kv := range keys {
			ks2, ok := kv.(designdb.Struct)
			if !ok {
				return nil, fmt.Errorf("%s: key %d is %T", f.where, i, kv)
			}
			kf := newFields(fmt.Sprintf("%s: key %d", f.where, i), ks2)
			k := aocexpr.Key{Time: kf.float("Time"), Value: kf.float("Value"), Arrive: kf.optFloat("ArriveTangent"), Leave: kf.optFloat("LeaveTangent")}
			mode := kf.str("InterpMode")
			if kf.err != nil {
				return nil, kf.err
			}
			var err error
			if k.Interp, err = aocexpr.ParseInterp(mode); err != nil {
				return nil, fmt.Errorf("%s: key %d: %w", f.where, i, err)
			}
			ks = append(ks, k)
		}
		if len(ks) == 0 {
			if pak := artisan.keysFor(external); pak != nil {
				ks = append(ks, pak...)
				opt.logf("stat_curve: %s (%d) has no keys of its own; its pak asset %s gives %d", r.Name, r.Guid, packageOf(external), len(ks))
			}
		}
		if len(ks) > 0 {
			c, err := aocexpr.NewCurve(ks)
			if err != nil {
				return nil, fmt.Errorf("%s: %w", f.where, err)
			}
			cm.Curves[r.Guid] = c
			ks = c.Keys
		}
		if err := w.exec(`INSERT INTO stat_curve_def (guid, name, key_count, default_value, external_curve) VALUES (?, ?, ?, ?, ?)`,
			r.Guid, r.Name, len(ks), def, external); err != nil {
			return nil, err
		}
		for i, k := range ks {
			if err := w.exec(`INSERT INTO stat_curve (guid, name, ord, level, value, interp, arrive_tangent, leave_tangent) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, r.Name, i, k.Time, k.Value, k.Interp.String(), k.Arrive, k.Leave); err != nil {
				return nil, err
			}
		}
	}
	return cm, nil
}

// firstErr is the first error of the readers.
func firstErr(fs ...*fields) error {
	for _, f := range fs {
		if f.err != nil {
			return f.err
		}
	}
	return nil
}

// initEntry is a stat initializer: the stat and its formula.
type initEntry struct {
	StatGuid   int64
	StatName   string
	Expression string
}

// initList is a StatInitializerList.
type initList struct {
	Guid    int64
	Name    string
	Entries []initEntry
}

// (Cyb | C O A)

// buildStatInits fills stat_init and returns the lists by name.
func buildStatInits(w *writer, rows []designdb.Row, statNames map[int64]string) (map[string]*initList, error) {
	lists := make(map[string]*initList, len(rows))
	for _, r := range rows {
		f := newFields(fmt.Sprintf("StatInitializerList %s (%d)", r.Name, r.Guid), r.Fields)
		entries := f.list("StatIDs")
		if f.err != nil {
			return nil, f.err
		}
		if _, dup := lists[r.Name]; dup {
			return nil, fmt.Errorf("two StatInitializerList rows are called %s", r.Name)
		}
		l := &initList{Guid: r.Guid, Name: r.Name}
		lists[r.Name] = l
		for i, ev := range entries {
			es, ok := ev.(designdb.Struct)
			if !ok {
				return nil, fmt.Errorf("%s: StatIDs[%d] is %T", f.where, i, ev)
			}
			ef := newFields(fmt.Sprintf("%s: StatIDs[%d]", f.where, i), es)
			guid, _ := ef.id("StatId")
			expr := ef.str("ValueExpression")
			if ef.err != nil {
				return nil, ef.err
			}
			name := ""
			if guid != 0 {
				var known bool
				if name, known = statNames[guid]; !known {
					return nil, fmt.Errorf("%s: StatIDs[%d] names stat %d, which is not a StatTypeDef row", f.where, i, guid)
				}
			}
			l.Entries = append(l.Entries, initEntry{guid, name, expr})
			if err := w.exec(`INSERT INTO stat_init (list_guid, list_name, ord, stat_guid, stat_name, expression) VALUES (?, ?, ?, ?, ?, ?)`,
				r.Guid, r.Name, i, guid, name, expr); err != nil {
				return nil, err
			}
		}
	}
	return lists, nil
}

// entry returns the list's entry for a stat; it is an error unless exactly one.
func (l *initList) entry(stat int64) (initEntry, error) {
	var found []initEntry
	for _, e := range l.Entries {
		if e.StatGuid == stat {
			found = append(found, e)
		}
	}
	if len(found) != 1 {
		return initEntry{}, fmt.Errorf("list %s has %d entries for stat %d, want 1", l.Name, len(found), stat)
	}
	return found[0], nil
}

// C.Y.B captures, CoA code

// buildClasses fills class and class_stat_base. The eight player classes are the
// CharacterClasses rows named in content.PlayerClasses (the table also holds a
// "<Class>Stage" row for each). Each class's base attributes come from the
// formulas of the list "Player_Base_<Class>", evaluated for levels 1-50.
func buildClasses(w *writer, rows []designdb.Row, lists map[string]*initList, curves *aocexpr.CurveMap, statNames map[int64]string) (int, error) {
	byName := map[string]designdb.Row{}
	for _, r := range rows {
		byName[r.Name] = r
	}
	// The attribute stats are the ones content names; the database must agree.
	for _, a := range content.PrimaryAttributes {
		if statNames[a.Guid] != a.Name {
			return 0, fmt.Errorf("stat %d is %q in the database, content names it %s", a.Guid, statNames[a.Guid], a.Name)
		}
	}
	count := 0
	for _, name := range content.PlayerClasses {
		r, ok := byName[name]
		if !ok {
			return 0, fmt.Errorf("no CharacterClasses row named %s", name)
		}
		f := newFields(fmt.Sprintf("CharacterClasses %s (%d)", r.Name, r.Guid), r.Fields)
		enum := f.str("Class")
		levelGuid, levelName := f.id("LevelTypeId")
		treeGuid, treeName := f.id("ActiveSkillTreeId")
		melee := f.boolean("bPrefersMelee")
		if f.err != nil {
			return 0, f.err
		}
		if want := "ECharacterClass::" + name; enum != want {
			return 0, fmt.Errorf("%s: Class is %s, want %s", f.where, enum, want)
		}
		list := lists["Player_Base_"+name]
		if list == nil {
			return 0, fmt.Errorf("%s: no StatInitializerList Player_Base_%s", f.where, name)
		}
		if err := w.exec(`INSERT INTO class (guid, name, class_enum, archetype_id, level_type_guid, level_type_name,
			skill_tree_guid, skill_tree_name, prefers_melee, stat_init_list_guid, stat_init_list_name)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, name, enum, content.ArchetypeID(name), levelGuid, levelName, treeGuid, treeName, b2i(melee),
			list.Guid, list.Name); err != nil {
			return 0, err
		}

		for _, attr := range content.PrimaryAttributes {
			e, err := list.entry(attr.Guid)
			if err != nil {
				return 0, err
			}
			prog, err := aocexpr.Parse(e.Expression)
			if err != nil {
				return 0, fmt.Errorf("%s %s: %w", list.Name, attr.Name, err)
			}
			for level := content.MinLevel; level <= content.MaxLevel; level++ {
				v, err := prog.EvalFloat(aocexpr.CharacterEnv(level, curves))
				if err != nil {
					return 0, fmt.Errorf("%s %s at level %d: %w", list.Name, attr.Name, level, err)
				}
				single := aocexpr.CharacterEnv(level, curves)
				single.Single = true
				v32, err := prog.EvalFloat(single)
				if err != nil {
					return 0, fmt.Errorf("%s %s at level %d (float32): %w", list.Name, attr.Name, level, err)
				}
				if err := w.exec(`INSERT INTO class_stat_base (class_guid, class_name, level, stat_guid, stat_name, value, rounding_ambiguous)
					VALUES (?, ?, ?, ?, ?, ?, ?)`, r.Guid, name, level, attr.Guid, attr.Name, v, b2i(v != v32)); err != nil {
					return 0, err
				}
				count++
			}
		}
	}
	return count, nil
}

// sortedKeys returns the keys of m in order.
func sortedKeys[V any](m map[int64]V) []int64 {
	keys := make([]int64, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
	return keys
}

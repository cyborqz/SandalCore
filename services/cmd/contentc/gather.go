// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/designdb"
)

// Gathering (milestone "Gathering stage 2", part "Gathering data"; contentc version 13): see package content, gather.go, for what each table holds.
//
// The tables are typed from the design records, with the gathering equations (the duration and durability formulas of an interaction, the Quality formula
// of an ItemVariation, a Profession experience reward) added to the ability expression set, so the stat program compiles them with the host functions
// 180-184 (aocexpr/hostfn.go). The profession level-up curves are not in the design database: `mapc artisan-curves` reads them from the paks into
// artisan_curves.tsv, and this file puts them into stat_curve and leveling_curve.

// gatherSchemas are the record structs the gathering tables read (build.go's keep names them).
var gatherSchemas = []string{
	"GatherableActorDefRecordBase", "GatheringFoliageDataRecordBase", "PlayerInteractionDefRecordBase", "GatheringInteractionDataRecordBase",
	"GatheringDynamicDefRecordBase", "GatheringToolDataRecordBase", "CertificationLevelRecordBase", "CharacterProfessionsRecordBase", "ItemVariationRecordBase",
}

// artisanCurveFile is the name of mapc's output beside contentc's.
const artisanCurveFile = "artisan_curves.tsv"

// artisanCurves are the profession level-up curves read from artisan_curves.tsv: the keys of each pak asset, by its package path
// (/Game/DesignAssets/Artisanship/Gathering/GatheringLevelUpCurve), which is what a LevelUpCurve's ExternalCurve names before the dot.
type artisanCurves struct {
	Path, SHA256 string
	Curves       map[string][]aocexpr.Key
	Keys         int
}

// packageOf is the package path of an object path ("/Game/X/Y.Y" -> "/Game/X/Y").
func packageOf(objectPath string) string {
	if i := strings.IndexByte(objectPath, '.'); i >= 0 {
		return objectPath[:i]
	}
	return objectPath
}

// keysFor returns the keys of the pak asset an ExternalCurve names; nil when there is no such asset (or no file).
func (a *artisanCurves) keysFor(external string) []aocexpr.Key {
	if a == nil || external == "" {
		return nil
	}
	return a.Curves[packageOf(external)]
}

// C.Y.B x C.O.A

// readArtisanCurves reads mapc's artisan_curves.tsv. A file that is missing is not an error (the caller keeps the placeholder and says so loudly); a file
// that is damaged is.
func readArtisanCurves(path string) (*artisanCurves, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	sum := sha256.Sum256(raw)
	out := &artisanCurves{Path: path, SHA256: hex.EncodeToString(sum[:]), Curves: map[string][]aocexpr.Key{}}
	sc := bufio.NewScanner(strings.NewReader(string(raw)))
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	line := 0
	for sc.Scan() {
		line++
		text := strings.TrimRight(sc.Text(), "\r")
		if line == 1 {
			if want := "curve\tord\ttime\tvalue\tinterp\tarrive_tangent\tleave_tangent\tpre_extrap\tpost_extrap"; text != want {
				return nil, fmt.Errorf("%s: the header is %q, want %q", path, text, want)
			}
			continue
		}
		if text == "" {
			continue
		}
		f := strings.Split(text, "\t")
		if len(f) != 9 {
			return nil, fmt.Errorf("%s:%d: %d fields, want 9", path, line, len(f))
		}
		ord, err := strconv.Atoi(f[1])
		if err != nil || ord != len(out.Curves[f[0]]) {
			return nil, fmt.Errorf("%s:%d: key %q of %s is out of order (it is key %d)", path, line, f[1], f[0], len(out.Curves[f[0]]))
		}
		num := func(s string) (float64, error) {
			x, err := strconv.ParseFloat(s, 32)
			if err != nil {
				return 0, fmt.Errorf("%s:%d: %q is no float32", path, line, s)
			}
			return decimal32(float32(x)), nil // the decimal the pak's float32 reads back from, as every other curve of content.db
		}
		var k aocexpr.Key
		var errs [4]error
		k.Time, errs[0] = num(f[2])
		k.Value, errs[1] = num(f[3])
		k.Arrive, errs[2] = num(f[5])
		k.Leave, errs[3] = num(f[6])
		for _, e := range errs {
			if e != nil {
				return nil, e
			}
		}
		if k.Interp, err = aocexpr.ParseInterp(f[4]); err != nil {
			return nil, fmt.Errorf("%s:%d: %w", path, line, err)
		}
		if f[7] != "Constant" || f[8] != "Constant" {
			return nil, fmt.Errorf("%s:%d: %s extrapolates by %s/%s: stat_curve holds a constant before the first key and after the last", path, line, f[0], f[7], f[8])
		}
		out.Curves[f[0]] = append(out.Curves[f[0]], k)
		out.Keys++
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}
	if len(out.Curves) == 0 {
		return nil, fmt.Errorf("%s: no curves", path)
	}
	for name, ks := range out.Curves {
		if _, err := aocexpr.NewCurve(ks); err != nil {
			return nil, fmt.Errorf("%s: %s: %w", path, name, err)
		}
	}
	return out, nil
}

// names of the gathering stat curves the tables assert (the data holds their keys itself).
var gatherToolSlots = map[string]bool{"Pickaxe": true, "Axe": true, "Sickle": true, "Bow": true, "FishingRod": true}

// == C-Y-B | CoA ==

// buildGather writes the gathering tables. It runs at the end of buildAbilities because the formulas it keeps join the ability expression set (b.exprs),
// which the stat program compiles with the others.
func (b *abBuilder) buildGather(opt *Options, meta map[string]string) error {
	if len(b.src.rows["GatherableActorDefRecordBase"]) == 0 {
		return nil // a synthetic test source with no gathering records
	}
	for _, s := range gatherSchemas {
		if _, err := b.src.need(s); err != nil {
			return err
		}
	}
	rows := func(schema string) []designdb.Row { return b.src.rows[schema] }
	byGuid := func(schema string) map[int64]*designdb.Row { return indexRows(rows(schema)) }
	nexpr0 := b.exprs.next
	counts := map[string]int{}
	insert := func(table, query string, args ...any) error {
		if err := b.w.exec(query, args...); err != nil {
			return fmt.Errorf("%s: %w", table, err)
		}
		counts[table]++
		return nil
	}

	// --- the certification levels and the professions -------------------------------------------------------------------------------------------
	certTier := map[int64]int{}
	certs := append([]designdb.Row(nil), rows("CertificationLevelRecordBase")...)
	sort.Slice(certs, func(i, j int) bool { return certs[i].Guid < certs[j].Guid })
	for _, r := range certs {
		f := newFields(fmt.Sprintf("CertificationLevel %s (%d)", r.Name, r.Guid), r.Fields)
		tier, minL, maxL := f.int("CertificationTier"), f.int("MinLevel"), f.int("MaxLevel")
		has, allowed := f.boolean("HasMaximumAllowable"), f.int("MaximumAllowable")
		if f.err != nil {
			return f.err
		}
		if !has {
			allowed = 0
		}
		certTier[r.Guid] = int(tier)
		if err := insert("certification_level", `INSERT INTO certification_level (guid, name, tier, min_level, max_level, has_max_allowable, max_allowable) VALUES (?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, tier, minL, maxL, b2i(has), allowed); err != nil {
			return err
		}
	}
	professions := map[int64]string{}
	profs := append([]designdb.Row(nil), rows("CharacterProfessionsRecordBase")...)
	sort.Slice(profs, func(i, j int) bool { return profs[i].Guid < profs[j].Guid })
	for _, r := range profs {
		f := newFields(fmt.Sprintf("CharacterProfessions %s (%d)", r.Name, r.Guid), r.Fields)
		category := enumValue(f.str("Category"))
		level, _ := f.id("LevelTypeId")
		trees := f.ids("SkillTreeIds")
		if f.err != nil {
			return f.err
		}
		var tree int64
		if len(trees) > 0 {
			tree = trees[0]
		}
		professions[r.Guid] = r.Name
		if err := insert("profession", `INSERT INTO profession (guid, name, category, leveling_type_guid, skill_tree_guid) VALUES (?, ?, ?, ?, ?)`, r.Guid, r.Name, category, level, tree); err != nil {
			return err
		}
	}

	// --- the actions that promote or demote ---------------------------------------------------------------------------------------------------
	for _, r := range rows("ActionResponseRecordBase") {
		f := newFields(fmt.Sprintf("ActionResponse %s (%d)", r.Name, r.Guid), r.Fields)
		if f.str("ResponseType") != "EActionResponse::SetCertificationLevel" {
			continue
		}
		action := enumValue(f.str("CertificationAction"))
		prof, _ := f.id("ProfessionId")
		if f.err != nil {
			return f.err
		}
		if action != "Promote" && action != "Demote" {
			return fmt.Errorf("%s: CertificationAction %q is neither Promote nor Demote", f.where, action)
		}
		if err := insert("action_certification", `INSERT INTO action_certification (action_guid, name, profession_guid, action) VALUES (?, ?, ?, ?)`, r.Guid, r.Name, prof, action); err != nil {
			return err
		}
	}

	// --- the seasonal multipliers -----------------------------------------------------------------------------------------------------------
	dyn := append([]designdb.Row(nil), rows("GatheringDynamicDefRecordBase")...)
	sort.Slice(dyn, func(i, j int) bool { return dyn[i].Guid < dyn[j].Guid })
	for _, r := range dyn {
		f := newFields(fmt.Sprintf("GatheringDynamicDef %s (%d)", r.Name, r.Guid), r.Fields)
		for _, season := range []string{"Winter", "Spring", "Summer", "Fall"} {
			ord, _ := aocexpr.EnumOrdinal("ESeason", season)
			sf := newFields(f.where+": "+season, f.strukt(season))
			day, night := sf.number("DayMultBase"), sf.number("NightMultBase")
			if err := firstErr(f, sf); err != nil {
				return err
			}
			if err := insert("gather_dynamic", `INSERT INTO gather_dynamic (guid, name, season, day_mult, night_mult) VALUES (?, ?, ?, ?, ?)`, r.Guid, r.Name, ord, day, night); err != nil {
				return err
			}
		}
	}

	// --- the gatherable actor defs ------------------------------------------------------------------------------------------------------------
	foliage, interactions, gathers := byGuid("GatheringFoliageDataRecordBase"), byGuid("PlayerInteractionDefRecordBase"), byGuid("GatheringInteractionDataRecordBase")
	slots, err := b.w.slotNumbers()
	if err != nil {
		return err
	}
	defs := append([]designdb.Row(nil), rows("GatherableActorDefRecordBase")...)
	sort.Slice(defs, func(i, j int) bool { return defs[i].Guid < defs[j].Guid })
	unknownSlot := 0
	for _, r := range defs {
		f := newFields(fmt.Sprintf("GatherableActorDef %s (%d)", r.Name, r.Guid), r.Fields)
		foliageGuid, _ := f.id("FoliageActorDefId")
		interactionGuid, _ := f.id("InteractionDefId")
		override := f.number("OverrideRespawnDuration")
		if f.err != nil {
			return f.err
		}
		fr, ok := foliage[foliageGuid]
		if !ok {
			return fmt.Errorf("%s: FoliageActorDefId %d is no GatheringFoliageData", f.where, foliageGuid)
		}
		ff := newFields(f.where+": "+fr.Name, fr.Fields)
		empty := ff.number("EmptyDuration")
		dynamicGuid, _ := ff.id("DynamicEmptyDurationDefId")
		resource, _ := ff.id("GatheringResourceTypeId")
		state := enumValue(ff.str("GatherableState"))
		ir, ok := interactions[interactionGuid]
		if !ok {
			return fmt.Errorf("%s: InteractionDefId %d is no PlayerInteractionDef", f.where, interactionGuid)
		}
		pf := newFields(f.where+": "+ir.Name, ir.Fields)
		gatherGuid, _ := pf.id("GatheringInteractionDataId")
		radius, tolerance, discovery, channel := pf.number("InteractRadius"), pf.number("InteractTolerance"), pf.number("DiscoveryDistance"), pf.number("ChannelDuration")
		combat, single, los, cancel := pf.boolean("AllowInCombat"), pf.boolean("SingleUse"), pf.boolean("bRequireLOS"), pf.boolean("CancelChannelIfActivePredicateEverFails")
		if err := firstErr(ff, pf); err != nil {
			return err
		}
		gr, ok := gathers[gatherGuid]
		if !ok {
			return fmt.Errorf("%s: the interaction %s names GatheringInteractionData %d, which is no row", f.where, ir.Name, gatherGuid)
		}
		gf := newFields(f.where+": "+gr.Name, gr.Fields)
		profession, _ := gf.id("ProfessionId")
		cert, _ := gf.id("CertificationLevelId")
		base := gf.number("BaseGatheringDuration")
		reward, _ := gf.id("RewardId")
		toolType := enumValue(gf.str("ToolType"))
		durationExpr := b.exprField(gf, "GatheringDurationFormula")
		durabilityExpr := b.exprField(gf, "DurabilityLossFormula")
		if gf.err != nil {
			return gf.err
		}
		if _, ok := professions[profession]; !ok {
			return fmt.Errorf("%s: ProfessionId %d is no CharacterProfessions", gf.where, profession)
		}
		tier, ok := certTier[cert]
		if !ok {
			return fmt.Errorf("%s: CertificationLevelId %d is no CertificationLevel", gf.where, cert)
		}
		slot, ok := slots[toolType]
		if !ok {
			slot = -1 // EEquipSlot::cCount: an interaction with no tool type (one of the 236)
			unknownSlot++
		}
		if err := insert("gather_def", `INSERT INTO gather_def (guid, name, foliage_guid, empty_duration, override_respawn, dynamic_guid, resource_type_guid, gatherable_state,
			interaction_guid, interact_radius, interact_tolerance, discovery_distance, channel_duration, allow_in_combat, single_use, require_los, cancel_if_predicate_fails,
			gather_guid, profession_guid, tool_slot, cert_guid, cert_tier, base_duration, duration_expr, durability_expr, reward_guid)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, foliageGuid, empty, override, dynamicGuid, resource, state, interactionGuid, radius, tolerance, discovery, channel, b2i(combat), b2i(single), b2i(los), b2i(cancel),
			gatherGuid, profession, slot, cert, tier, base, durationExpr, durabilityExpr, reward); err != nil {
			return err
		}
	}
	if unknownSlot > 0 {
		opt.logf("gather_def: %d def(s) have an interaction whose tool type is no equip slot: tool_slot -1", unknownSlot)
	}

	// --- the tools ------------------------------------------------------------------------------------------------------------------------------
	tools := byGuid("GatheringToolDataRecordBase")
	items := append([]designdb.Row(nil), rows("ItemRecordBase")...)
	sort.Slice(items, func(i, j int) bool { return items[i].Guid < items[j].Guid })
	for _, r := range items {
		f := newFields(fmt.Sprintf("Item %s (%d)", r.Name, r.Guid), r.Fields)
		dataGuid, _ := f.id("GatheringToolDataId")
		if f.err != nil || dataGuid == 0 {
			continue // an item with no tool data (or a record without the field)
		}
		tr, ok := tools[dataGuid]
		if !ok {
			return fmt.Errorf("%s: GatheringToolDataId %d is no GatheringToolData", f.where, dataGuid)
		}
		tf := newFields(f.where+": "+tr.Name, tr.Fields)
		profession, _ := tf.id("ProfessionId")
		cert, _ := tf.id("CertificationLevelId")
		reqProf, _ := f.id("RequiredProfessionId")
		reqCert, _ := f.id("RequiredCertificationLevelId")
		eq := f.tags("EquipSlots")
		if err := firstErr(f, tf); err != nil {
			return err
		}
		tier, ok := certTier[cert]
		if !ok || len(eq) == 0 {
			return fmt.Errorf("%s: the tool data %s has certification %d (a level: %v) and the item %d equip slots", f.where, tr.Name, cert, ok, len(eq))
		}
		slot, ok := slots[enumValue(eq[0])]
		if !ok {
			return fmt.Errorf("%s: EquipSlots[0] %q is no equip slot", f.where, eq[0])
		}
		if err := insert("gather_tool", `INSERT INTO gather_tool (item_guid, item_name, tool_data_guid, tool_data_name, profession_guid, cert_guid, cert_tier, tool_slot, required_profession, required_cert)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, dataGuid, tr.Name, profession, cert, tier, slot, reqProf, reqCert); err != nil {
			return err
		}
	}

	// --- the item variations ------------------------------------------------------------------------------------------------------------------
	vars := append([]designdb.Row(nil), rows("ItemVariationRecordBase")...)
	sort.Slice(vars, func(i, j int) bool { return vars[i].Guid < vars[j].Guid })
	for _, r := range vars {
		f := newFields(fmt.Sprintf("ItemVariation %s (%d)", r.Name, r.Guid), r.Fields)
		quality, durability := b.exprField(f, "Quality"), b.exprField(f, "Durability")
		if f.err != nil {
			return f.err
		}
		if err := insert("item_variation", `INSERT INTO item_variation (guid, name, quality_expr, durability_expr) VALUES (?, ?, ?, ?)`, r.Guid, r.Name, quality, durability); err != nil {
			return err
		}
	}

	// --- Gathering stage 3, "Foliage gathering" (version 15): the foliage defs and states, every gathering interaction ------------------------------------------
	if err := b.buildFoliage(opt, meta, certTier, professions, slots); err != nil {
		return err
	}

	// --- the Profession experience rewards: their expression joins the set ------------------------------------------------------------------
	xp, err := b.w.professionXP()
	if err != nil {
		return err
	}
	for _, x := range xp {
		id, err := b.exprs.id(x.expression, nil)
		if err != nil {
			return err
		}
		if err := b.w.exec(`UPDATE reward_xp SET expr = ? WHERE table_guid = ? AND ord = ?`, id, x.table, x.ord); err != nil {
			return err
		}
		counts["reward_xp.expr"]++
	}

	for _, t := range []string{"gather_def", "gather_dynamic", "gather_tool", "item_variation", "profession", "certification_level", "action_certification", "reward_xp.expr"} {
		meta["typed."+t] = strconv.Itoa(counts[t])
	}
	meta["gather.exprs"] = strconv.FormatInt(b.exprs.next-nexpr0, 10)
	opt.logf("gather: %d defs, %d tools, %d item variations, %d professions, %d certification levels, %d certification actions, %d profession experience expressions, %d new expressions",
		counts["gather_def"], counts["gather_tool"], counts["item_variation"], counts["profession"], counts["certification_level"], counts["action_certification"], counts["reward_xp.expr"],
		b.exprs.next-nexpr0)
	return nil
}

// (C:Y:B : CoA)

// slotNumbers is the equip_slot table buildItems wrote into this transaction: the slot number of each EEquipSlot name.
func (w *writer) slotNumbers() (map[string]int64, error) {
	rows, err := w.tx.Query(`SELECT slot, name FROM equip_slot`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]int64{}
	for rows.Next() {
		var slot int64
		var name string
		if err := rows.Scan(&slot, &name); err != nil {
			return nil, err
		}
		out[name] = slot
	}
	return out, rows.Err()
}

// profXP is the Profession experience reward of a reward table that buildProgression wrote: the expression text (the GM level scrolls' `xp - cur + 1`).
type profXP struct {
	table, ord int64
	expression string
}

// professionXP lists the reward_xp rows of the experience type Profession, read in full before any statement runs on the transaction.
func (w *writer) professionXP() ([]profXP, error) {
	rows, err := w.tx.Query(`SELECT table_guid, ord, expression FROM reward_xp WHERE experience_type = 'Profession' ORDER BY table_guid, ord`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []profXP
	for rows.Next() {
		var x profXP
		if err := rows.Scan(&x.table, &x.ord, &x.expression); err != nil {
			return nil, err
		}
		out = append(out, x)
	}
	return out, rows.Err()
}

// loadArtisanCurves reads artisan_curves.tsv (Options.ArtisanCurves, else dist/content/artisan_curves.tsv of the checkout) into src.artisan. A default file that is
// not there is not an error: the placeholder curves of the design database stay, which is wrong for the professions' levels, so the build says so loudly and meta
// records it. A file that was asked for by name, or one that is damaged, is an error.
func loadArtisanCurves(src *source, opt *Options, meta map[string]string) error {
	path, asked := opt.ArtisanCurves, opt.ArtisanCurves != ""
	if !asked {
		path = filepath.Join(repoRoot(), "dist", "content", artisanCurveFile)
	}
	a, err := readArtisanCurves(path)
	switch {
	case err == nil:
		src.artisan = a
		meta["artisan_curves.status"] = "applied"
		meta["artisan_curves.path"] = a.Path
		meta["artisan_curves.sha256"] = a.SHA256
		meta["artisan_curves.curves"] = strconv.Itoa(len(a.Curves))
		meta["artisan_curves.keys"] = strconv.Itoa(a.Keys)
		opt.logf("artisan curves: %d curves, %d keys from %s", len(a.Curves), a.Keys, a.Path)
		return nil
	case errors.Is(err, os.ErrNotExist) && !asked:
		meta["artisan_curves.status"] = "missing"
		meta["artisan_curves.path"] = path
		opt.logf("WARNING: %s is not there: run `mapc artisan-curves`. The profession level-up curves stay the design database's PLACEHOLDER (0 XP at level 0, 50,000 at level 50), "+
			"the three StatCurves Gathering_/Processing_/Crafting_LevelUpCurve have no keys, and the GM level scrolls will not land on their levels", path)
		return nil
	}
	return err
}

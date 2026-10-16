// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"sort"
	"strconv"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// Foliage gathering (milestone "Gathering stage 3", part "Foliage gathering"; contentc version 15): see package content, foliage.go, for what each table holds.
//
// A tree or plant of the painted world is a GatheringFoliageData (how long an emptied instance stays empty, the state it is painted in) with a GatheringFoliageState for each of
// its states (how long it stays in the state, and which GatheringInteractionData harvests it there). Every gathering interaction is a row of gather_interaction, the foliage's, the
// nodes' (gather_def has the same fields for the 339 placed nodes), the Hunting corpses' (the 53 interactions of the Bow) and fishing's; the range of an interaction comes from the
// PlayerInteractionDef that names it, and gather_interaction_def keys those by their own guid (a hunting creature's asset names its Hunting_<X> def).

// foliageStateOrd are EFoliageState's values by ordinal (content.StateInfancy ...).
var foliageStateOrd = map[string]int{"Infancy": content.StateInfancy, "Juvenile": content.StateJuvenile, "Adult": content.StateAdult, "Empty": content.StateEmpty}

// buildFoliage writes the foliage tables. It runs at the end of buildGather, which hands it the maps it built (the certification tiers, the professions, the tool slots).
func (b *abBuilder) buildFoliage(opt *Options, meta map[string]string, certTier map[int64]int, professions map[int64]string, slots map[string]int64) error {
	for _, s := range []string{"GatheringFoliageStateRecordBase"} {
		if _, err := b.src.need(s); err != nil {
			return err
		}
	}
	rows := func(schema string) []designdb.Row {
		out := append([]designdb.Row(nil), b.src.rows[schema]...)
		sort.Slice(out, func(i, j int) bool { return out[i].Guid < out[j].Guid })
		return out
	}
	counts := map[string]int{}
	insert := func(table, query string, args ...any) error {
		if err := b.w.exec(query, args...); err != nil {
			return fmt.Errorf("%s: %w", table, err)
		}
		counts[table]++
		return nil
	}
	stateOrd := func(f *fields, name string) int {
		v := enumValue(f.str(name))
		ord, ok := foliageStateOrd[v]
		if !ok && f.err == nil {
			f.err = fmt.Errorf("%s: %s is %q, not an EFoliageState", f.where, name, v)
		}
		return ord
	}

	// --- the PlayerInteractionDefs that name a GatheringInteractionData -------------------------------------------------------------------------
	type pdef struct {
		guid              int64
		name              string
		gather            int64
		radius, tolerance float64
		combat            bool
	}
	pdefsOf := map[int64][]pdef{} // by the GatheringInteractionData they name, in guid order
	for _, r := range rows("PlayerInteractionDefRecordBase") {
		f := newFields(fmt.Sprintf("PlayerInteractionDef %s (%d)", r.Name, r.Guid), r.Fields)
		gather, _ := f.id("GatheringInteractionDataId")
		if f.err != nil {
			return f.err
		}
		if gather == 0 {
			continue
		}
		radius, tolerance, discovery, combat := f.number("InteractRadius"), f.number("InteractTolerance"), f.number("DiscoveryDistance"), f.boolean("AllowInCombat")
		if f.err != nil {
			return f.err
		}
		if err := insert("gather_interaction_def", `INSERT INTO gather_interaction_def (interaction_guid, name, gather_guid, interact_radius, interact_tolerance, discovery_distance, allow_in_combat)
			VALUES (?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, gather, radius, tolerance, discovery, b2i(combat)); err != nil {
			return err
		}
		pdefsOf[gather] = append(pdefsOf[gather], pdef{r.Guid, r.Name, gather, radius, tolerance, combat})
	}

	// --- the 236 gathering interactions -----------------------------------------------------------------------------------------------------------
	interactions := map[int64]bool{}
	for _, r := range rows("GatheringInteractionDataRecordBase") {
		f := newFields(fmt.Sprintf("GatheringInteractionData %s (%d)", r.Name, r.Guid), r.Fields)
		profession, _ := f.id("ProfessionId")
		cert, _ := f.id("CertificationLevelId")
		reward, _ := f.id("RewardId")
		base := f.number("BaseGatheringDuration")
		target := stateOrd(f, "TargetFoliageState")
		toolType := enumValue(f.str("ToolType"))
		durationExpr := b.exprField(f, "GatheringDurationFormula")
		durabilityExpr := b.exprField(f, "DurabilityLossFormula")
		if f.err != nil {
			return f.err
		}
		// (one record, GatheringInteractionData_<guid>, is an unfinished test row with no profession and no certification: 0 and tier 0)
		if _, ok := professions[profession]; !ok && profession != 0 {
			return fmt.Errorf("%s: ProfessionId %d is no CharacterProfessions", f.where, profession)
		}
		tier, ok := certTier[cert]
		if !ok && cert != 0 {
			return fmt.Errorf("%s: CertificationLevelId %d is no CertificationLevel", f.where, cert)
		}
		slot, ok := slots[toolType]
		if !ok {
			slot = -1 // EEquipSlot::cCount: an interaction with no tool type (one of the 236)
		}
		// the def that carries the range: the one named like the interaction, else the first by guid
		var def pdef
		combat := true
		for _, p := range pdefsOf[r.Guid] {
			if def.guid == 0 || p.name == r.Name && def.name != r.Name {
				def = p
			}
		}
		if def.guid != 0 {
			combat = def.combat
		}
		if err := insert("gather_interaction", `INSERT INTO gather_interaction (guid, name, target_state, profession_guid, cert_guid, cert_tier, tool_slot, base_duration, duration_expr,
			durability_expr, reward_guid, interaction_guid, interact_radius, interact_tolerance, allow_in_combat) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, target, profession, cert, tier, slot, base, durationExpr, durabilityExpr, reward, def.guid, def.radius, def.tolerance, b2i(combat)); err != nil {
			return err
		}
		interactions[r.Guid] = true
	}

	// --- the 743 foliage states ---------------------------------------------------------------------------------------------------------------------
	states := map[int64]bool{}
	for _, r := range rows("GatheringFoliageStateRecordBase") {
		f := newFields(fmt.Sprintf("GatheringFoliageState %s (%d)", r.Name, r.Guid), r.Fields)
		life := f.number("LifecycleDuration")
		dynamic, _ := f.id("DynamicLifecycleDefId")
		var harvests []int64
		for _, g := range f.ids("FoliageInteractionsIds") {
			if g != 0 { // (a state may list an empty reference: 319 of the 743 have no interaction, 221 of them an empty list and 98 one empty entry)
				harvests = append(harvests, g)
			}
		}
		distance, angle, radius := f.number("OverrideInteractionDistance"), f.number("OverrideInteractionAngle"), f.number("CollisionRadius")
		if f.err != nil {
			return f.err
		}
		var interaction int64
		switch len(harvests) {
		case 0:
		case 1:
			interaction = harvests[0]
			if !interactions[interaction] {
				return fmt.Errorf("%s: FoliageInteractionsIds[0] %d is no GatheringInteractionData", f.where, interaction)
			}
		default:
			return fmt.Errorf("%s: %d FoliageInteractionsIds (the model has one per state)", f.where, len(harvests))
		}
		if err := insert("foliage_state", `INSERT INTO foliage_state (guid, name, lifecycle_duration, dynamic_guid, interaction_guid, override_distance, override_angle, collision_radius)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, life, dynamic, interaction, distance, angle, radius); err != nil {
			return err
		}
		states[r.Guid] = true
	}

	// --- the 402 foliage defs -------------------------------------------------------------------------------------------------------------------------
	for _, r := range rows("GatheringFoliageDataRecordBase") {
		f := newFields(fmt.Sprintf("GatheringFoliageData %s (%d)", r.Name, r.Guid), r.Fields)
		empty := f.number("EmptyDuration")
		dynamic, _ := f.id("DynamicEmptyDurationDefId")
		resource, _ := f.id("GatheringResourceTypeId")
		paint, gatherable, lo, hi := stateOrd(f, "PaintState"), stateOrd(f, "GatherableState"), stateOrd(f, "MinState"), stateOrd(f, "MaxState")
		keys, guids := f.keyedIds("FoliageStateDataIds")
		if f.err != nil {
			return f.err
		}
		var slot [3]int64
		for i, k := range keys {
			if k < content.StateInfancy || k > content.StateAdult {
				return fmt.Errorf("%s: FoliageStateDataIds has the key %d (Infancy 0 .. Adult 2)", f.where, k)
			}
			if guids[i] != 0 && !states[guids[i]] {
				return fmt.Errorf("%s: the state %d is no GatheringFoliageState", f.where, guids[i])
			}
			if slot[k] != 0 {
				return fmt.Errorf("%s: FoliageStateDataIds names state %d twice", f.where, k)
			}
			slot[k] = guids[i]
		}
		if err := insert("foliage_def", `INSERT INTO foliage_def (guid, name, empty_duration, dynamic_guid, resource_type_guid, paint_state, gatherable_state, min_state, max_state,
			state0_guid, state1_guid, state2_guid) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, empty, dynamic, resource, paint, gatherable, lo, hi,
			slot[0], slot[1], slot[2]); err != nil {
			return err
		}
	}

	for _, t := range []string{"foliage_def", "foliage_state", "gather_interaction", "gather_interaction_def"} {
		meta["typed."+t] = strconv.Itoa(counts[t])
	}
	opt.logf("foliage: %d defs, %d states, %d gathering interactions, %d interaction defs that name one", counts["foliage_def"], counts["foliage_state"], counts["gather_interaction"],
		counts["gather_interaction_def"])
	return nil
}

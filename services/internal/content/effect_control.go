// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"errors"
	"fmt"
)

// The effect-control export (contentc version 39): what the effect engine reads of an effect record besides the columns ab_effect has, and the records an effect names
// to control its holder. The creature effects milestone (DoTs and stat debuffs, then slows, roots and stuns) reads all of it, so it is exported once.
//
//	ab_effect_control      one row per ab_effect row (an expression column is an ab_expr id, 0 for none):
//	                         compare_source         bCompareSourceWhenStacking: instances applied by different instigators stack apart (each instigator has its own bleed)
//	                         recalc_on_tick         bRecalculateHitOnTick: the effect's tick hits are worked out again from the instigator's stats at every tick
//	                         stacks_lost_on_elapse  bNumStacksLostOnDurationElapse: when the duration runs out the effect loses stacks_lost_expr stacks and starts again, until none are left
//	                         extended_max           ExtendedMaximum: the most seconds an ExtendDuration effect has left after an application, a plain number
//	                         extension_expr         ExtensionAmount: the seconds one more application adds to an ExtendDuration effect (0: none given)
//	ab_crowd_control       a CrowdControlRecordBase: the status bits, the snare strength, the animation, the priority, the abilities it disables, the push it gives (as text)
//	ab_ability_disable     an AbilityDisableRecordBase: the filter text that says which abilities a silence, a disarm or an exhaustion stops
//	ab_fragility           a FragileEffectDefRecordBase: when an effect breaks by itself (a sleep at the first damage, a root at a hit), as the data's expression texts
//	ab_effect_fragility    the fragility records of an effect, in the order it lists them
//
// The crowd-control, ability-disable and fragility tables hold the records the written effects name, in guid order.
const EffectControlDDL = `
CREATE TABLE ab_effect_control (
    effect_guid           INTEGER PRIMARY KEY,
    compare_source        INTEGER NOT NULL,
    recalc_on_tick        INTEGER NOT NULL,
    stacks_lost_on_elapse INTEGER NOT NULL,
    stacks_lost_expr      INTEGER NOT NULL,
    extended_max          REAL NOT NULL,
    extension_expr        INTEGER NOT NULL
);
CREATE TABLE ab_crowd_control (
    guid                  INTEGER PRIMARY KEY,
    name                  TEXT NOT NULL,
    applied_status        INTEGER NOT NULL,   -- AppliedStatus, a bit set: Root 1, no turning 2, abilities disabled 4, Snare 16, displaced 32 (Stun and Trip 7, Frozen 11, a push 36)
    snare_strength        REAL NOT NULL,      -- SnareStrength: the share of its speed a snared character loses (0.5: half speed); 0 when the record gives none
    animation_request     TEXT NOT NULL,      -- ECrowdControlAnimationRequest: None, Custom, Stunned, Tripped
    priority              INTEGER NOT NULL,   -- the stronger status plays its animation (Stun 200, Trip 300)
    disable_guid          INTEGER NOT NULL,   -- AbilitiesToDisableId: the ab_ability_disable row that says which abilities it stops (0: none)
    face_source           INTEGER NOT NULL,   -- bFaceSource
    ignore_non_combatant  INTEGER NOT NULL,   -- bIgnoreNonCombatantImmunity
    restart_montage       INTEGER NOT NULL,   -- bRestartMontageOnReapply
    override_client_move  INTEGER NOT NULL,   -- bOverrideClientAuthMovement
    push_parameterization TEXT NOT NULL,      -- EPushParameterization: Force, Velocity, Acceleration, Time
    push_magnitude        TEXT NOT NULL,      -- PushMagnitude, the data's expression text ('': none); pushes are not played yet
    push_duration         TEXT NOT NULL,      -- PushDuration, the same
    push_dir_x            REAL NOT NULL,
    push_dir_y            REAL NOT NULL,
    push_dir_z            REAL NOT NULL,
    push_arrival_distance REAL NOT NULL,
    end_push_at_arrival   INTEGER NOT NULL
);
CREATE TABLE ab_ability_disable (
    guid   INTEGER PRIMARY KEY,
    name   TEXT NOT NULL,
    filter TEXT NOT NULL                      -- Filter, the data's expression text: it is true for each ability the record leaves usable
);
CREATE TABLE ab_fragility (
    guid                  INTEGER PRIMARY KEY,
    name                  TEXT NOT NULL,
    fragility_type        TEXT NOT NULL,      -- EEffectFragilityType: FRAGILITY_Tick (checked every tick_frequency seconds) or FRAGILITY_Proc (at the events it is bound to)
    break_threshold       TEXT NOT NULL,      -- EffectBreakThreshold, expression text ('': none)
    accumulation_rate     TEXT NOT NULL,      -- EffectBreakThresholdAccumulationRate
    tick_frequency        REAL NOT NULL,      -- EvaluationTickFrequency, seconds
    break_immediately     TEXT NOT NULL,      -- ShouldBreakEffectImmediately: when true the effect ends at once
    evaluate_accumulation TEXT NOT NULL,      -- ShouldEvaluateThresholdAccumulation
    bindings              TEXT NOT NULL       -- EventBindings, a JSON object of the event kinds' bit sets (HitEvents 2: the holder is hit, 4: the holder hits)
);
CREATE TABLE ab_effect_fragility (
    effect_guid    INTEGER NOT NULL,
    ord            INTEGER NOT NULL,
    fragility_guid INTEGER NOT NULL,
    PRIMARY KEY (effect_guid, ord)
) WITHOUT ROWID;
`

// EffectControlTables are the tables EffectControlDDL creates, in the DDL's order.
var EffectControlTables = []string{"ab_effect_control", "ab_crowd_control", "ab_ability_disable", "ab_fragility", "ab_effect_fragility"}

// createEffectControl creates the effect-control tables.
func createEffectControl(db *sql.DB) error {
	if _, err := db.Exec(EffectControlDDL); err != nil {
		return fmt.Errorf("content: create the effect-control tables: %w", err)
	}
	return nil
}

// EffectControlRow is an ab_effect_control row.
type EffectControlRow struct {
	CompareSource      bool
	RecalcOnTick       bool
	StacksLostOnElapse bool
	StacksLostExpr     int64
	ExtendedMax        float64
	ExtensionExpr      int64
}

// EffectControl returns the effect-control row of the ab_effect row with the guid (the defaults for an effect that has none).
func (d *DB) EffectControl(guid int64) (EffectControlRow, error) {
	var r EffectControlRow
	var cmp, recalc, lost int
	err := d.sql.QueryRow(`SELECT COALESCE(c.compare_source, 0), COALESCE(c.recalc_on_tick, 0), COALESCE(c.stacks_lost_on_elapse, 0), COALESCE(c.stacks_lost_expr, 0),
		COALESCE(c.extended_max, 0), COALESCE(c.extension_expr, 0) FROM ab_effect e LEFT JOIN ab_effect_control c ON c.effect_guid = e.guid WHERE e.guid = ?`, guid).
		Scan(&cmp, &recalc, &lost, &r.StacksLostExpr, &r.ExtendedMax, &r.ExtensionExpr)
	if errors.Is(err, sql.ErrNoRows) {
		return EffectControlRow{}, fmt.Errorf("content: no ab_effect %d", guid)
	}
	if err != nil {
		return EffectControlRow{}, err
	}
	r.CompareSource, r.RecalcOnTick, r.StacksLostOnElapse = cmp != 0, recalc != 0, lost != 0
	return r, nil
}

// CrowdControlRow is an ab_crowd_control row.
type CrowdControlRow struct {
	Guid                 int64
	Name                 string
	AppliedStatus        int64
	SnareStrength        float64
	AnimationRequest     string
	Priority             int64
	DisableGuid          int64
	FaceSource           bool
	IgnoreNonCombatant   bool
	RestartMontage       bool
	OverrideClientMove   bool
	PushParameterization string
	PushMagnitude        string
	PushDuration         string
	PushDirX             float64
	PushDirY             float64
	PushDirZ             float64
	PushArrivalDistance  float64
	EndPushAtArrival     bool
}

// CrowdControl returns the ab_crowd_control row with the guid.
func (d *DB) CrowdControl(guid int64) (CrowdControlRow, error) {
	var r CrowdControlRow
	var face, ignore, restart, override, end int
	err := d.sql.QueryRow(`SELECT guid, name, applied_status, snare_strength, animation_request, priority, disable_guid, face_source, ignore_non_combatant, restart_montage,
		override_client_move, push_parameterization, push_magnitude, push_duration, push_dir_x, push_dir_y, push_dir_z, push_arrival_distance, end_push_at_arrival
		FROM ab_crowd_control WHERE guid = ?`, guid).
		Scan(&r.Guid, &r.Name, &r.AppliedStatus, &r.SnareStrength, &r.AnimationRequest, &r.Priority, &r.DisableGuid, &face, &ignore, &restart,
			&override, &r.PushParameterization, &r.PushMagnitude, &r.PushDuration, &r.PushDirX, &r.PushDirY, &r.PushDirZ, &r.PushArrivalDistance, &end)
	if errors.Is(err, sql.ErrNoRows) {
		return CrowdControlRow{}, fmt.Errorf("content: no ab_crowd_control %d", guid)
	}
	if err != nil {
		return CrowdControlRow{}, err
	}
	r.FaceSource, r.IgnoreNonCombatant, r.RestartMontage, r.OverrideClientMove, r.EndPushAtArrival = face != 0, ignore != 0, restart != 0, override != 0, end != 0
	return r, nil
}

// AbilityDisableRow is an ab_ability_disable row.
type AbilityDisableRow struct {
	Guid   int64
	Name   string
	Filter string
}

// AbilityDisable returns the ab_ability_disable row with the guid.
func (d *DB) AbilityDisable(guid int64) (AbilityDisableRow, error) {
	var r AbilityDisableRow
	err := d.sql.QueryRow(`SELECT guid, name, filter FROM ab_ability_disable WHERE guid = ?`, guid).Scan(&r.Guid, &r.Name, &r.Filter)
	if errors.Is(err, sql.ErrNoRows) {
		return AbilityDisableRow{}, fmt.Errorf("content: no ab_ability_disable %d", guid)
	}
	return r, err
}

// FragilityRow is an ab_fragility row.
type FragilityRow struct {
	Guid                 int64
	Name                 string
	FragilityType        string
	BreakThreshold       string
	AccumulationRate     string
	TickFrequency        float64
	BreakImmediately     string
	EvaluateAccumulation string
	Bindings             string // JSON
}

// Fragility returns the ab_fragility row with the guid.
func (d *DB) Fragility(guid int64) (FragilityRow, error) {
	var r FragilityRow
	err := d.sql.QueryRow(`SELECT guid, name, fragility_type, break_threshold, accumulation_rate, tick_frequency, break_immediately, evaluate_accumulation, bindings
		FROM ab_fragility WHERE guid = ?`, guid).
		Scan(&r.Guid, &r.Name, &r.FragilityType, &r.BreakThreshold, &r.AccumulationRate, &r.TickFrequency, &r.BreakImmediately, &r.EvaluateAccumulation, &r.Bindings)
	if errors.Is(err, sql.ErrNoRows) {
		return FragilityRow{}, fmt.Errorf("content: no ab_fragility %d", guid)
	}
	return r, err
}

// EffectFragilities returns the guids of the fragility records an effect lists, in its order.
func (d *DB) EffectFragilities(effect int64) ([]int64, error) {
	return d.int64s(`SELECT fragility_guid FROM ab_effect_fragility WHERE effect_guid = ? ORDER BY ord`, effect)
}

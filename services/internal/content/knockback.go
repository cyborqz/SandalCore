// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"errors"
	"fmt"
)

// The knock-back export (contentc version 42): what throws, pushes and pulls a creature. The creature crowd-control milestone reads all of it.
//
//	ab_hit_impulse   one row per hit that carries an impulse (the flag is set, or a magnitude is named): the velocity change a hit gives its target when it lands.
//	                 An expression column is an ab_expr id (0 for none). The direction is horizontal, in the frame the origin gives; the angle lifts it up from the ground.
//	ab_crowd_push    one row per written crowd-control record that pushes (the displaced bit is set): the push's magnitude as an expression, the origin it works from, and whether the
//	                 origin is taken once, when the effect goes on, or again at every step.
//	ab_teleport      one row per teleport record a written hit names (its TeleportId): who is moved, by which script, and whether the two swap places.
//
// The origin of an impulse or a push is an EAbilitySystemOrigin (where) and an EAbilitySystemOrientation (which way the frame faces), with a relative location in that frame.
const KnockbackDDL = `
CREATE TABLE ab_hit_impulse (
    hit_guid           INTEGER PRIMARY KEY,
    applies            INTEGER NOT NULL,   -- bAppliesImpulse
    velocity_change    INTEGER NOT NULL,   -- bApplyImpulseAsVelocityChange: the magnitude is a change of velocity (cm/s), not an impulse over the target's mass
    angle_expr         INTEGER NOT NULL,   -- ImpulseAngle: the degrees the direction is lifted up by (an ab_expr id, 0: none)
    magnitude_expr     INTEGER NOT NULL,   -- ImpulseMagnitude (an ab_expr id, 0: none)
    dir_x              REAL NOT NULL,      -- ImpulseDirection: the horizontal direction, in the origin's frame
    dir_y              REAL NOT NULL,
    origin_location    TEXT NOT NULL,      -- ImpulseOriginTransform.OriginLocation: EAbilitySystemOrigin (Owner, Instigator, Source, Target, HitLocation, World)
    origin_orientation TEXT NOT NULL,      -- ImpulseOriginTransform.OriginOrientation: EAbilitySystemOrientation (Owner, Instigator, Source, Target, TargetToOrigin, SourceToOriginZ ...)
    origin_rel_x       REAL NOT NULL,      -- ImpulseOriginTransform.RelativeLocation, in the origin's frame
    origin_rel_y       REAL NOT NULL,
    origin_rel_z       REAL NOT NULL,
    rotation_behavior  TEXT NOT NULL       -- ImpulseRotationBehavior: ECrowdControlRotationBehavior (Unaffected, FaceOrigin)
);
CREATE TABLE ab_crowd_push (
    cc_guid            INTEGER PRIMARY KEY,
    magnitude_expr     INTEGER NOT NULL,   -- PushMagnitude (an ab_expr id, 0: none)
    duration_expr      INTEGER NOT NULL,   -- PushDuration (an ab_expr id, 0: none)
    origin_location    TEXT NOT NULL,      -- OriginTransform.OriginLocation
    origin_orientation TEXT NOT NULL,      -- OriginTransform.OriginOrientation
    origin_rel_x       REAL NOT NULL,      -- OriginTransform.RelativeLocation
    origin_rel_y       REAL NOT NULL,
    origin_rel_z       REAL NOT NULL,
    cache_origin       INTEGER NOT NULL    -- bCacheInitialOriginTransfrom: the origin is worked out once, when the effect goes on
);
CREATE TABLE ab_teleport (
    guid               INTEGER PRIMARY KEY,
    name               TEXT NOT NULL,
    target             TEXT NOT NULL,      -- TeleportTarget: ECombatLabel (None, Owner, Instigator, Source, Target): the actor the place is worked out from
    script             TEXT NOT NULL,      -- TeleportScript: the soft path of the script class that works the place out
    swap_with_target   INTEGER NOT NULL    -- bSwapLocationWithTarget
);
`

// KnockbackTables are the tables KnockbackDDL creates, in the DDL's order.
var KnockbackTables = []string{"ab_hit_impulse", "ab_crowd_push", "ab_teleport"}

// createKnockback creates the knock-back tables.
func createKnockback(db *sql.DB) error {
	if _, err := db.Exec(KnockbackDDL); err != nil {
		return fmt.Errorf("content: create the knock-back tables: %w", err)
	}
	return nil
}

// HitImpulseRow is an ab_hit_impulse row.
type HitImpulseRow struct {
	HitGuid           int64
	Applies           bool
	VelocityChange    bool
	AngleExpr         int64
	MagnitudeExpr     int64
	DirX, DirY        float64
	OriginLocation    string
	OriginOrientation string
	OriginRelX        float64
	OriginRelY        float64
	OriginRelZ        float64
	RotationBehavior  string
}

// HitImpulse returns the ab_hit_impulse row of the hit with the guid; ok is false for a hit that carries none.
func (d *DB) HitImpulse(hit int64) (r HitImpulseRow, ok bool, err error) {
	var applies, velocity int
	err = d.sql.QueryRow(`SELECT hit_guid, applies, velocity_change, angle_expr, magnitude_expr, dir_x, dir_y, origin_location, origin_orientation, origin_rel_x, origin_rel_y, origin_rel_z,
		rotation_behavior FROM ab_hit_impulse WHERE hit_guid = ?`, hit).
		Scan(&r.HitGuid, &applies, &velocity, &r.AngleExpr, &r.MagnitudeExpr, &r.DirX, &r.DirY, &r.OriginLocation, &r.OriginOrientation, &r.OriginRelX, &r.OriginRelY, &r.OriginRelZ, &r.RotationBehavior)
	if errors.Is(err, sql.ErrNoRows) {
		return HitImpulseRow{}, false, nil
	}
	if err != nil {
		return HitImpulseRow{}, false, err
	}
	r.Applies, r.VelocityChange = applies != 0, velocity != 0
	return r, true, nil
}

// CrowdPushRow is an ab_crowd_push row.
type CrowdPushRow struct {
	CcGuid            int64
	MagnitudeExpr     int64
	DurationExpr      int64
	OriginLocation    string
	OriginOrientation string
	OriginRelX        float64
	OriginRelY        float64
	OriginRelZ        float64
	CacheOrigin       bool
}

// CrowdPush returns the ab_crowd_push row of the crowd-control record with the guid; ok is false for a record that does not push.
func (d *DB) CrowdPush(cc int64) (r CrowdPushRow, ok bool, err error) {
	var cache int
	err = d.sql.QueryRow(`SELECT cc_guid, magnitude_expr, duration_expr, origin_location, origin_orientation, origin_rel_x, origin_rel_y, origin_rel_z, cache_origin FROM ab_crowd_push WHERE cc_guid = ?`, cc).
		Scan(&r.CcGuid, &r.MagnitudeExpr, &r.DurationExpr, &r.OriginLocation, &r.OriginOrientation, &r.OriginRelX, &r.OriginRelY, &r.OriginRelZ, &cache)
	if errors.Is(err, sql.ErrNoRows) {
		return CrowdPushRow{}, false, nil
	}
	if err != nil {
		return CrowdPushRow{}, false, err
	}
	r.CacheOrigin = cache != 0
	return r, true, nil
}

// TeleportRow is an ab_teleport row.
type TeleportRow struct {
	Guid           int64
	Name           string
	Target         string
	Script         string
	SwapWithTarget bool
}

// Teleport returns the ab_teleport row with the guid.
func (d *DB) Teleport(guid int64) (TeleportRow, error) {
	var r TeleportRow
	var swap int
	err := d.sql.QueryRow(`SELECT guid, name, target, script, swap_with_target FROM ab_teleport WHERE guid = ?`, guid).Scan(&r.Guid, &r.Name, &r.Target, &r.Script, &swap)
	if errors.Is(err, sql.ErrNoRows) {
		return TeleportRow{}, fmt.Errorf("content: no ab_teleport %d", guid)
	}
	if err != nil {
		return TeleportRow{}, err
	}
	r.SwapWithTarget = swap != 0
	return r, nil
}

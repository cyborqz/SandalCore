// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"strconv"
	"strings"
)

// The knock-back export (contentc version 42; the tables are content.KnockbackDDL's): the velocity change a hit gives its target (ab_hit_impulse), the origin and the magnitude of the crowd-control
// records that push (ab_crowd_push), and the teleport records a hit names (ab_teleport).
//
// It runs after the effect-control export, so that its expressions take the ids after all the others and no existing expression moves.

// Enum defaults of a transform that names none: both enums start at Owner.
const (
	defaultOrigin      = "EAbilitySystemOrigin::Owner"
	defaultOrientation = "EAbilitySystemOrientation::Owner"
)

// origin is an OriginTransform struct as the tables keep it: where the frame is, which way it faces, and the relative location in it.
type origin struct {
	location, orientation string
	rx, ry, rz            float64
}

// readOrigin reads the transform struct `key` of a record (a record that has none gets the defaults), and refuses what the tables do not serve: a relative location given as an expression.
func readOrigin(f *fields, key string) (origin, error) {
	o := origin{location: defaultOrigin, orientation: defaultOrientation}
	if _, ok := f.get(key); !ok {
		return o, nil
	}
	tf := newFields(f.where+": "+key, f.strukt(key))
	if loc := tf.optStr("OriginLocation"); loc != "" {
		o.location = loc
	}
	if ori := tf.optStr("OriginOrientation"); ori != "" {
		o.orientation = ori
	}
	if tf.optBool("bUseRelativeLocationExpression") {
		return o, fmt.Errorf("%s: the relative location is an expression, which the export does not serve", tf.where)
	}
	if _, ok := tf.get("RelativeLocation"); ok {
		rf := newFields(tf.where+": RelativeLocation", tf.strukt("RelativeLocation"))
		o.rx, o.ry, o.rz = rf.optNumber("X"), rf.optNumber("Y"), rf.optNumber("Z")
		if rf.err != nil {
			return o, rf.err
		}
	}
	return o, tf.err
}

// writeKnockback fills ab_hit_impulse for the hits that carry an impulse, ab_crowd_push for the crowd-control records that push (the ones writeEffectControl wrote), and ab_teleport for the teleport
// records the hits name.
func (b *abBuilder) writeKnockback(opt *Options, meta map[string]string, hits []int64) error {
	impulses, teleports := 0, 0
	tele := indexRows(b.src.rows["TeleportRecordBase"])
	teleDone := map[int64]bool{}
	for _, g := range hits {
		r := b.hits[g]
		if r == nil {
			return fmt.Errorf("no AbilityHit row %d", g)
		}
		f := newFields(fmt.Sprintf("AbilityHit %s (%d)", r.Name, g), r.Fields)
		applies, change := f.optBool("bAppliesImpulse"), f.optBool("bApplyImpulseAsVelocityChange")
		angleText, magnitudeText := f.optStr("ImpulseAngle"), f.optStr("ImpulseMagnitude")
		var dirX, dirY float64
		if _, ok := f.get("ImpulseDirection"); ok {
			df := newFields(f.where+": ImpulseDirection", f.strukt("ImpulseDirection"))
			dirX, dirY = df.optNumber("X"), df.optNumber("Y")
			if df.err != nil {
				return df.err
			}
		}
		rotation := f.optStr("ImpulseRotationBehavior")
		teleport := optID(f, "TeleportId")
		if f.err != nil {
			return f.err
		}
		if applies || strings.TrimSpace(magnitudeText) != "" {
			o, err := readOrigin(f, "ImpulseOriginTransform")
			if err != nil {
				return err
			}
			var ids [2]int64
			for i, t := range []string{angleText, magnitudeText} {
				id, err := b.exprs.id(t, nil)
				if err != nil {
					return err
				}
				ids[i] = id
			}
			if rotation == "" {
				rotation = "ECrowdControlRotationBehavior::Unaffected"
			}
			if err := b.w.exec(`INSERT INTO ab_hit_impulse (hit_guid, applies, velocity_change, angle_expr, magnitude_expr, dir_x, dir_y, origin_location, origin_orientation, origin_rel_x, origin_rel_y, origin_rel_z,
				rotation_behavior) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, g, b2i(applies), b2i(change), ids[0], ids[1], dirX, dirY, o.location, o.orientation, o.rx, o.ry, o.rz, rotation); err != nil {
				return err
			}
			impulses++
		}
		if teleport != 0 && !teleDone[teleport] {
			row := tele[teleport]
			if row == nil {
				return fmt.Errorf("%s: no TeleportRecord row %d", f.where, teleport)
			}
			tf := newFields(fmt.Sprintf("TeleportRecord %s (%d)", row.Name, teleport), row.Fields)
			target, script, swap := tf.optStr("TeleportTarget"), tf.optStr("TeleportScript"), tf.optBool("bSwapLocationWithTarget")
			if tf.err != nil {
				return tf.err
			}
			if err := b.w.exec(`INSERT INTO ab_teleport (guid, name, target, script, swap_with_target) VALUES (?, ?, ?, ?, ?)`, teleport, row.Name, target, script, b2i(swap)); err != nil {
				return err
			}
			teleDone[teleport] = true
			teleports++
		}
	}
	// the pushes: every written crowd-control record that has the displaced bit
	ccs := indexRows(b.src.rows["CrowdControlRecordBase"])
	pushes := 0
	for _, g := range sortedBools(b.ccWritten) {
		row := ccs[g]
		if row == nil {
			return fmt.Errorf("no CrowdControl row %d", g)
		}
		f := newFields(fmt.Sprintf("CrowdControl %s (%d)", row.Name, g), row.Fields)
		status := f.int("AppliedStatus")
		if f.err != nil {
			return f.err
		}
		if status&32 == 0 {
			continue
		}
		magnitude, duration := f.str("PushMagnitude"), f.str("PushDuration")
		cache := f.optBool("bCacheInitialOriginTransfrom")
		o, err := readOrigin(f, "OriginTransform")
		if err != nil {
			return err
		}
		if f.err != nil {
			return f.err
		}
		var ids [2]int64
		for i, t := range []string{magnitude, duration} {
			id, err := b.exprs.id(t, nil)
			if err != nil {
				return err
			}
			ids[i] = id
		}
		if err := b.w.exec(`INSERT INTO ab_crowd_push (cc_guid, magnitude_expr, duration_expr, origin_location, origin_orientation, origin_rel_x, origin_rel_y, origin_rel_z, cache_origin)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`, g, ids[0], ids[1], o.location, o.orientation, o.rx, o.ry, o.rz, b2i(cache)); err != nil {
			return err
		}
		pushes++
	}
	meta["ab.hit_impulses"] = strconv.Itoa(impulses)
	meta["ab.crowd_pushes"] = strconv.Itoa(pushes)
	meta["ab.teleports"] = strconv.Itoa(teleports)
	opt.logf("ab_hit_impulse: %d hits with an impulse; ab_crowd_push: %d pushing crowd-control records; ab_teleport: %d teleport records; %d expressions in all", impulses, pushes, teleports, b.exprs.next)
	return nil
}

// effectRemovalTag is the tag of a hit's EffectRemovalFilter when the filter asks only EffectHasTag(GetEffect(), "<tag>"), "" for any other filter (a hit with such a filter takes off the effects
// that carry the tag).
func effectRemovalTag(filter string) string {
	var code strings.Builder
	for _, line := range strings.Split(filter, "\n") {
		if i := strings.Index(line, "//"); i >= 0 {
			line = line[:i]
		}
		code.WriteString(strings.Join(strings.Fields(line), ""))
	}
	const head, tail = `EffectHasTag(GetEffect(),"`, `")`
	s := code.String()
	if !strings.HasPrefix(s, head) || !strings.HasSuffix(s, tail) || len(s) <= len(head)+len(tail) {
		return ""
	}
	tag := s[len(head) : len(s)-len(tail)]
	if strings.ContainsAny(tag, `"()|&!=<>`) {
		return ""
	}
	return tag
}

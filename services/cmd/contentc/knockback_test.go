// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"strings"
	"testing"

	"aocrop/services/internal/designdb"
)

// The knock-back export (contentc version 42) over the synthetic Bard: the impulse a hit gives, the push a crowd-control record plays and the teleport a hit names, and the tag a hit's removal filter asks for.

const kbTeleport = abBase + 80

func setHitFields(t *testing.T, src *source, guid int64, f st) {
	t.Helper()
	for i := range src.rows["AbilityHitRecordBase"] {
		if r := &src.rows["AbilityHitRecordBase"][i]; r.Guid == guid {
			for k, v := range f {
				r.Fields[k] = v
			}
			return
		}
	}
	t.Fatalf("no hit row %d", guid)
}

func setCCFields(t *testing.T, src *source, guid int64, f st) {
	t.Helper()
	for i := range src.rows["CrowdControlRecordBase"] {
		if r := &src.rows["CrowdControlRecordBase"][i]; r.Guid == guid {
			for k, v := range f {
				r.Fields[k] = v
			}
			return
		}
	}
	t.Fatalf("no crowd-control row %d", guid)
}

func knockbackSource(t *testing.T) *source {
	src := effectControlSource(t)
	setHitFields(t, src, abHitTick, st{
		"bAppliesImpulse": true, "bApplyImpulseAsVelocityChange": true, "ImpulseAngle": "35",
		"ImpulseMagnitude": "1000 * SelectInt(GetEffectCount(GetTarget(), $#3377240744094973029:1$) > 0, 0, 1)",
		"ImpulseDirection": st{"X": float32(1)}, "ImpulseRotationBehavior": "ECrowdControlRotationBehavior::FaceOrigin",
		"ImpulseOriginTransform": st{
			"OriginLocation": "EAbilitySystemOrigin::Instigator", "OriginOrientation": "EAbilitySystemOrientation::TargetToOrigin", "RelativeLocation": st{"X": float32(0), "Y": float32(0), "Z": float32(-100)},
		},
	})
	setHitFields(t, src, abHitSelf, st{
		"EffectRemovalFilter": "EffectHasTag(GetEffect(), \"Debuff.Snared\")  // the snares",
		"TeleportId":          idv(kbTeleport, 6064634060465569792, "InFrontOfInstigator"),
	})
	src.rows["TeleportRecordBase"] = []designdb.Row{
		mkRow(kbTeleport, "InFrontOfInstigator", st{"TeleportTarget": "ECombatLabel::Instigator", "TeleportScript": "/Game/Scripts/BP_TeleportToInstigatorScript.BP_TeleportToInstigatorScript_C", "bSwapLocationWithTarget": false}),
	}
	setCCFields(t, src, ecCC, st{
		"AppliedStatus": int64(36), "PushParameterization": "EPushParameterization::Force", "PushDuration": "1", "PushArrivalDistance": float32(50), "bEndPushAtArrival": true,
		"PushMagnitude":                "const d = GetDistanceBetween(GetInstigator(),GetTarget());\r\nconst f = 1500000.0-d*d;\r\nSelectFloat(f > 0.0, f, 0.0)",
		"PushDirection":                st{"X": float32(1), "Y": float32(0), "Z": float32(0)},
		"bCacheInitialOriginTransfrom": true,
		"OriginTransform": st{
			"OriginLocation": "EAbilitySystemOrigin::Source", "OriginOrientation": "EAbilitySystemOrientation::Source", "RelativeLocation": st{"X": float32(0), "Y": float32(0), "Z": float32(-100)},
			"bUseRelativeLocationExpression": false,
		},
	})
	return src
}

func TestKnockbackExport(t *testing.T) {
	db, meta, err := buildAbilitySynthetic(t, knockbackSource(t))
	if err != nil {
		t.Fatal(err)
	}
	im, ok, err := db.HitImpulse(abHitTick)
	if err != nil || !ok {
		t.Fatalf("the impulse of Nimble_Tick_Hit = %+v, %v, %v", im, ok, err)
	}
	if !im.Applies || !im.VelocityChange || im.DirX != 1 || im.DirY != 0 || im.OriginLocation != "EAbilitySystemOrigin::Instigator" || im.OriginOrientation != "EAbilitySystemOrientation::TargetToOrigin" ||
		im.OriginRelZ != -100 || im.RotationBehavior != "ECrowdControlRotationBehavior::FaceOrigin" || im.AngleExpr == 0 || im.MagnitudeExpr == 0 {
		t.Errorf("impulse = %+v", im)
	}
	for _, c := range []struct {
		name string
		id   int64
		want string
	}{{"angle", im.AngleExpr, "35"}, {"magnitude", im.MagnitudeExpr, "1000 * SelectInt(GetEffectCount(GetTarget(), $#3377240744094973029:1$) > 0, 0, 1)"}} {
		e, err := db.Expr(c.id)
		if err != nil || e.Text != c.want {
			t.Errorf("the %s expression %d = %q, %v; want %q", c.name, c.id, e.Text, err, c.want)
		}
	}
	if _, ok, err := db.HitImpulse(abHitSelf); err != nil || ok {
		t.Errorf("a hit with no impulse has a row: %v, %v", ok, err)
	}

	push, ok, err := db.CrowdPush(ecCC)
	if err != nil || !ok {
		t.Fatalf("the push of Test_Snare = %+v, %v, %v", push, ok, err)
	}
	if push.OriginLocation != "EAbilitySystemOrigin::Source" || push.OriginOrientation != "EAbilitySystemOrientation::Source" || push.OriginRelZ != -100 || !push.CacheOrigin || push.MagnitudeExpr == 0 || push.DurationExpr == 0 {
		t.Errorf("push = %+v", push)
	}
	if e, err := db.Expr(push.MagnitudeExpr); err != nil || !strings.Contains(e.Text, "GetDistanceBetween(GetInstigator(),GetTarget())") {
		t.Errorf("the push's magnitude = %q, %v", e.Text, err)
	}
	if _, ok, err := db.CrowdPush(ecCCUnused); err != nil || ok {
		t.Errorf("a crowd-control record no written effect names has a push row: %v, %v", ok, err)
	}

	tp, err := db.Teleport(kbTeleport)
	if err != nil || tp.Name != "InFrontOfInstigator" || tp.Target != "ECombatLabel::Instigator" || !strings.HasSuffix(tp.Script, "BP_TeleportToInstigatorScript_C") || tp.SwapWithTarget {
		t.Errorf("teleport = %+v, %v", tp, err)
	}

	// the hit's removal filter asks for one tag: the export names it, beside the kind
	h, err := db.HitRow(abHitSelf)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(h.Extra, `"EffectRemoval":"other"`) || !strings.Contains(h.Extra, `"EffectRemovalTag":"Debuff.Snared"`) || !strings.Contains(h.Extra, `"TeleportId":`+"5000000000000000080") {
		t.Errorf("the hit's extra = %s", h.Extra)
	}
	if other, err := db.HitRow(abHitTick); err != nil || strings.Contains(other.Extra, "EffectRemovalTag") {
		t.Errorf("a hit with no tag filter names a tag: %s, %v", other.Extra, err)
	}
	for k, v := range map[string]string{"ab.hit_impulses": "1", "ab.crowd_pushes": "1", "ab.teleports": "1"} {
		if meta[k] != v {
			t.Errorf("meta %s = %q, want %q", k, meta[k], v)
		}
	}
}

func TestEffectRemovalTag(t *testing.T) {
	for _, c := range []struct{ filter, want string }{
		{`EffectHasTag(GetEffect(), "Debuff.Snared")`, "Debuff.Snared"},
		{"EffectHasTag( GetEffect() ,\r\n \"Debuff.Chilled\" ) // a comment", "Debuff.Chilled"},
		{`EffectHasTag(GetEffect(), "Debuff.Rooted") || EffectHasTag(GetEffect(), "Debuff.Frozen")`, ""},
		{`IsDebuff(GetEffect())`, ""},
		{`GetEffect() == $#3377240744094973029:1$`, ""},
		{`!EffectHasTag(GetEffect(), "Debuff.Snared")`, ""},
		{`EffectHasTag(GetEffect(), "")`, ""},
		{"", ""},
	} {
		if got := effectRemovalTag(c.filter); got != c.want {
			t.Errorf("effectRemovalTag(%q) = %q, want %q", c.filter, got, c.want)
		}
	}
}

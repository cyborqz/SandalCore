// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"strconv"

	"aocrop/services/internal/aocexpr"
)

// The functions the game's reactions use (contentc version 35). A reaction's trigger and the expressions of the hits it applies ask the event in progress:
// which ability an ability event is about (GetConsumedAbility, AbilityHasTag), which effect a filter is looking at (GetEffect, EffectHasTag), the name of
// the hit's record, the record type of a source (GetReactionTypeId ...), how long an effect lasts (CalcEffectDuration) and a few reads of actors. Each is a
// constant or a host call (aocexpr/hostfn.go has the numbers); the C++ engine answers the host calls from the event.

// etypeHitRecord marks the value GetHitRecord gives: RecordNameIs asks the host for the name of the hit's record.
const etypeHitRecord = "HitRecord"

// reactionCall compiles one of those functions; ok is false for any other name.
func (f *fnc) reactionCall(e aocexpr.Call, args []val) (v val, ok bool, err error) {
	switch e.Name {
	case "GetReactionTypeId", "GetEffectTypeId", "GetAbilityTypeId", "GetConsumedAbility", "AbilityHasTag", "RecordNameContains", "RecordNameIs",
		"GetEffect", "EffectHasTag", "CalcEffectDuration", "GetEffectRecordByName", "GetSelectedTarget", "GetHitTargetByIndex", "GetEffectInstigator",
		"HitHasHitMod", "GetBlackboardFloat", "GetStatBase":
	default:
		return val{}, false, nil
	}
	v, err = f.reactionRead(e, args)
	return v, true, err
}

func (f *fnc) reactionRead(e aocexpr.Call, args []val) (val, error) {
	count := func(n int) error { return argc(e, n) }
	want := func(i int, t vtype) error {
		if t == tNum {
			args[i] = f.loose(args[i])
		}
		if args[i].t != t {
			return errAt(e.Args[i].Pos(), "%s: argument %d is %v, not %v", e.Name, i+1, args[i].t, t)
		}
		return nil
	}
	roleArg := func(i int) val { return val{t: tNum, n: nConst{float64(args[i].role)}} }
	inAbility := func() error {
		if f.kind != unitExpr {
			return unsupportedAt(e.At, "function %q (a stat formula has no event)", e.Name)
		}
		return nil
	}
	switch e.Name {
	case "GetReactionTypeId", "GetEffectTypeId", "GetAbilityTypeId":
		if err := count(0); err != nil {
			return val{}, err
		}
		id := map[string]uint64{"GetReactionTypeId": aocexpr.TypeReaction, "GetEffectTypeId": aocexpr.TypeEffect, "GetAbilityTypeId": aocexpr.TypeAbility}[e.Name]
		return val{t: tNum, n: nConst{float64(id)}, float: true}, nil
	case "GetConsumedAbility", "GetEffect":
		if err := inAbility(); err != nil {
			return val{}, err
		}
		if err := count(0); err != nil {
			return val{}, err
		}
		if e.Name == "GetEffect" {
			return val{t: tRef, ref: aocexpr.EffectInFilterRef}, nil
		}
		return val{t: tRef, ref: aocexpr.ConsumedAbilityRef}, nil
	case "AbilityHasTag", "RecordNameContains":
		if err := count(2); err != nil {
			return val{}, err
		}
		if err := want(0, tRef); err != nil {
			return val{}, err
		}
		if err := want(1, tStr); err != nil {
			return val{}, err
		}
		if args[0].ref != aocexpr.ConsumedAbilityRef {
			return val{}, unsupportedAt(e.Args[0].Pos(), "%s of a record other than GetConsumedAbility()", e.Name)
		}
		fn := aocexpr.HostConsumedAbilityHasTag
		if e.Name == "RecordNameContains" {
			fn = aocexpr.HostConsumedAbilityNameHas
		}
		return f.hostCall(e, fn, args[1].str, tBool)
	case "RecordNameIs":
		if err := count(2); err != nil {
			return val{}, err
		}
		if err := want(0, tObj); err != nil {
			return val{}, err
		}
		if err := want(1, tStr); err != nil {
			return val{}, err
		}
		if args[0].etype != etypeHitRecord {
			return val{}, unsupportedAt(e.Args[0].Pos(), "%s of a record other than the hit's (GetHitRecord)", e.Name)
		}
		return f.hostCall(e, aocexpr.HostHitRecordNameIs, args[1].str, tBool)
	case "EffectHasTag":
		if err := count(2); err != nil {
			return val{}, err
		}
		if err := want(0, tRef); err != nil {
			return val{}, err
		}
		if err := want(1, tStr); err != nil {
			return val{}, err
		}
		if args[0].ref != aocexpr.EffectInFilterRef {
			return val{}, unsupportedAt(e.Args[0].Pos(), "%s of an effect other than GetEffect()", e.Name)
		}
		return f.hostCall(e, aocexpr.HostEffectHasTag, args[1].str, tBool)
	case "CalcEffectDuration":
		if err := count(1); err != nil {
			return val{}, err
		}
		if err := want(0, tRef); err != nil {
			return val{}, err
		}
		r := args[0].ref
		if r.Kind != "" || r.TypeID != aocexpr.TypeEffect {
			return val{}, unsupportedAt(e.Args[0].Pos(), "%s of %v (only an effect record, $#<TypeId>:<Guid>$)", e.Name, r)
		}
		return f.hostCall(e, aocexpr.HostEffectDuration, strconv.FormatInt(r.Guid, 10), tNum)
	case "GetEffectRecordByName":
		// contentc rewrites GetEffectRecordByName("Name") to the effect's reference before the expression is compiled; one that is left names no single effect
		return val{}, unsupportedAt(e.At, "%s of a name that is no single effect record", e.Name)
	case "GetSelectedTarget":
		// the caster's selected target: the one GetActiveTarget(caster, Harm) gives (the finisher procs that ask are on weapons, which harm)
		if err := inAbility(); err != nil {
			return val{}, err
		}
		if err := count(0); err != nil {
			return val{}, err
		}
		f.usesTarget = true
		return val{t: tObj, role: aocexpr.RoleActiveHarm}, nil
	case "GetHitTargetByIndex":
		if err := inAbility(); err != nil {
			return val{}, err
		}
		if err := count(2); err != nil {
			return val{}, err
		}
		if err := want(0, tObj); err != nil {
			return val{}, err
		}
		if err := want(1, tNum); err != nil {
			return val{}, err
		}
		if idx, isConst := args[1].constant(); !isConst || idx != 0 {
			return val{}, unsupportedAt(e.Args[1].Pos(), "%s of an index other than 0", e.Name)
		}
		f.usesTarget = true
		return val{t: tObj, role: aocexpr.RoleTarget}, nil
	case "GetEffectInstigator":
		if err := inAbility(); err != nil {
			return val{}, err
		}
		if err := count(0); err != nil {
			return val{}, err
		}
		return val{t: tObj, role: aocexpr.RoleEffectInstigator}, nil
	case "HitHasHitMod":
		if err := count(3); err != nil {
			return val{}, err
		}
		if err := want(0, tObj); err != nil {
			return val{}, err
		}
		if err := want(1, tObj); err != nil {
			return val{}, err
		}
		if err := want(2, tRef); err != nil {
			return val{}, err
		}
		return f.hostCall(e, aocexpr.HostHitHasHitMod, strconv.FormatInt(args[2].ref.Guid, 10), tBool, roleArg(1))
	case "GetBlackboardFloat":
		if err := count(2); err != nil {
			return val{}, err
		}
		if err := want(0, tObj); err != nil {
			return val{}, err
		}
		if err := want(1, tStr); err != nil {
			return val{}, err
		}
		return f.hostCall(e, aocexpr.HostBlackboardFloat, args[1].str, tNum, roleArg(0))
	case "GetStatBase":
		if err := count(2); err != nil {
			return val{}, err
		}
		if err := want(0, tObj); err != nil {
			return val{}, err
		}
		if err := want(1, tRef); err != nil {
			return val{}, err
		}
		guid, err := f.statRef(args[1].ref)
		if err != nil {
			return val{}, errAt(e.Args[1].Pos(), "%v", err)
		}
		return f.hostCall(e, aocexpr.HostStatBase, strconv.FormatInt(guid, 10), tNum, roleArg(0))
	}
	return val{}, unsupportedAt(e.At, "function %q", e.Name)
}

// markerEquality compiles GetConsumedAbility() == <ability> and GetEffect() == <effect> (and !=): the marker has no guid of its own, so the host is asked
// whether the ability of the event or the effect a filter looks at is that record.
func (f *fnc) markerEquality(e aocexpr.Binary, l, r val, want bool) (val, bool, error) {
	for _, m := range []struct {
		marker aocexpr.Ref
		fn     int
		typeID uint64
	}{
		{aocexpr.ConsumedAbilityRef, aocexpr.HostConsumedAbilityIs, aocexpr.TypeAbility},
		{aocexpr.EffectInFilterRef, aocexpr.HostEffectIs, aocexpr.TypeEffect},
	} {
		var other val
		switch {
		case l.ref == m.marker:
			other = r
		case r.ref == m.marker:
			other = l
		default:
			continue
		}
		if other.ref == m.marker {
			return val{t: tBool, n: nConst{b2f(want)}}, true, nil
		}
		if other.ref.Kind != "" || other.ref.TypeID != m.typeID {
			return val{}, true, errAt(e.At, "cannot compare a marker with %v", other.ref)
		}
		v, err := f.hostCall(aocexpr.Call{Name: "==", At: e.At}, m.fn, strconv.FormatInt(other.ref.Guid, 10), tBool)
		if err != nil {
			return val{}, true, err
		}
		if !want {
			v = val{t: tBool, n: nUn{OpNot, v.n}}
		}
		return v, true, nil
	}
	return val{}, false, nil
}

// hitRecordEquality compiles GetHitRecord(GetHitEvent()) == <hit> (and !=): the host is asked whether the record of the hit in progress is that hit.
func (f *fnc) hitRecordEquality(e aocexpr.Binary, l, r val, want bool) (val, bool, error) {
	obj, ref := l, r
	if l.t == tRef {
		obj, ref = r, l
	}
	if obj.etype != etypeHitRecord {
		return val{}, false, nil
	}
	if ref.ref.Kind != "" || ref.ref.TypeID != aocexpr.TypeHit {
		return val{}, true, errAt(e.At, "cannot compare the hit's record with %v", ref.ref)
	}
	v, err := f.hostCall(aocexpr.Call{Name: "==", At: e.At}, aocexpr.HostHitRecordIs, strconv.FormatInt(ref.ref.Guid, 10), tBool)
	if err != nil {
		return val{}, true, err
	}
	if !want {
		v = val{t: tBool, n: nUn{OpNot, v.n}}
	}
	return v, true, nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"fmt"
	"sort"
	"strings"
)

// Interp is how a curve goes from one key to the next (ERichCurveInterpMode).
type Interp uint8

const (
	InterpLinear   Interp = iota // RCIM_Linear
	InterpConstant               // RCIM_Constant: holds the key's value until the next key
	InterpCubic                  // RCIM_Cubic: a Hermite spline through the keys' tangents
)

func (i Interp) String() string {
	switch i {
	case InterpLinear:
		return "Linear"
	case InterpConstant:
		return "Constant"
	case InterpCubic:
		return "Cubic"
	}
	return fmt.Sprintf("Interp(%d)", uint8(i))
}

// ParseInterp reads the enum value the design database stores for a key's
// InterpMode ("ERichCurveInterpMode::RCIM_Linear", or the bare "RCIM_Linear"),
// or an Interp's own name ("Linear", as String gives it).
func ParseInterp(s string) (Interp, error) {
	switch strings.TrimPrefix(s[strings.LastIndex(s, ":")+1:], "RCIM_") {
	case "Linear":
		return InterpLinear, nil
	case "Constant":
		return InterpConstant, nil
	case "Cubic":
		return InterpCubic, nil
	}
	return 0, fmt.Errorf("unknown curve interpolation mode %q", s)
}

// ~ C:Y:B & CoA ~

// Key is a point of a curve: the value at Time (a level, for a StatCurve), and
// how the curve leaves it. The tangents are those of a cubic segment (the editor's own: an automatic tangent is stored computed).
type Key struct {
	Time, Value float64
	Interp      Interp
	Arrive      float64 // ArriveTangent: the slope the curve arrives with
	Leave       float64 // LeaveTangent: the slope it leaves with
}

// Curve is a float curve (the Curve of a StatCurve row: its EditorCurveData
// keys). Before the first key and after the last it holds their values (the
// engine's default extrapolation, RCCE_Constant: the file does not store the
// mode).
//
// At a key's own time the value is the key's, whatever the interpolation. In
// between, Linear and Constant segments are evaluated, and (plan E1, stage 3: the NPCs' power curves) a Cubic one as Unreal's rich curve
// does: a Bezier segment through the control points the two keys' tangents make (CubicSegment).
type Curve struct {
	Keys []Key // in increasing Time
}

// NewCurve returns a curve of the keys, which are sorted by Time. Two keys at
// one time are an error.
func NewCurve(keys []Key) (*Curve, error) {
	ks := append([]Key(nil), keys...)
	sort.SliceStable(ks, func(i, j int) bool { return ks[i].Time < ks[j].Time })
	for i := 1; i < len(ks); i++ {
		if ks[i].Time == ks[i-1].Time {
			return nil, fmt.Errorf("two keys at time %v", ks[i].Time)
		}
	}
	return &Curve{Keys: ks}, nil
}

// Eval returns the curve's value at t.
func (c *Curve) Eval(t float64) (float64, error) {
	ks := c.Keys
	switch {
	case len(ks) == 0:
		return 0, &Error{Msg: "the curve has no keys"}
	case t <= ks[0].Time:
		return ks[0].Value, nil
	case t >= ks[len(ks)-1].Time:
		return ks[len(ks)-1].Value, nil
	}
	// The segment [ks[i], ks[i+1]) that holds t.
	i := sort.Search(len(ks), func(i int) bool { return ks[i].Time > t }) - 1
	a, b := ks[i], ks[i+1]
	if t == a.Time {
		return a.Value, nil
	}
	switch a.Interp {
	case InterpConstant:
		return a.Value, nil
	case InterpLinear:
		return a.Value + (b.Value-a.Value)*(t-a.Time)/(b.Time-a.Time), nil
	case InterpCubic:
		return CubicSegment(a.Time, a.Value, a.Leave, b.Time, b.Value, b.Arrive, t), nil
	}
	return 0, &Error{Unsupported: true, Construct: fmt.Sprintf("%v interpolation between the keys at %v and %v", a.Interp, a.Time, b.Time)}
}

// CubicSegment is FRichCurve's cubic evaluation of the segment from (t0, v0) leaving with slope leave0 to (t1, v1) arriving with slope arrive1, at t:
// the Bezier through v0, v0 + leave0 * dt / 3, v1 - arrive1 * dt / 3 and v1 at alpha = (t - t0) / dt, as three levels of linear interpolation. Every
// operation is its own statement in double (the C++ engine's CubicSegment does the same, so the bits agree).
func CubicSegment(t0, v0, leave0, t1, v1, arrive1, t float64) float64 {
	diff := t1 - t0
	alpha := (t - t0) / diff
	third := 1.0 / 3.0
	d0 := leave0 * diff
	d1 := arrive1 * diff
	o0 := d0 * third
	o1 := d1 * third
	c0 := v0 + o0
	c1 := v1 - o1
	lerp := func(a, b, x float64) float64 {
		d := b - a
		m := x * d
		return a + m
	}
	p01 := lerp(v0, c0, alpha)
	p12 := lerp(c0, c1, alpha)
	p23 := lerp(c1, v1, alpha)
	p012 := lerp(p01, p12, alpha)
	p123 := lerp(p12, p23, alpha)
	return lerp(p012, p123, alpha)
}

// CurveSource resolves the reference an EvalCurve call names.
type CurveSource interface {
	Curve(ref Ref) (*Curve, error)
}

// CurveMap is a CurveSource over curves by Guid, all of one record type.
type CurveMap struct {
	TypeID uint64 // the table the references must name (the StatCurve table)
	Curves map[int64]*Curve
}

// Curve implements CurveSource.
func (m *CurveMap) Curve(ref Ref) (*Curve, error) {
	if ref.TypeID != m.TypeID {
		return nil, fmt.Errorf("the reference names type %d, the curves are type %d", ref.TypeID, m.TypeID)
	}
	c, ok := m.Curves[ref.Guid]
	if !ok {
		return nil, fmt.Errorf("no curve %d", ref.Guid)
	}
	return c, nil
}

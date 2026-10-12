// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"math"
	"strconv"
	"testing"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
)

// goldenValue is a class attribute at a level, computed from the golden numbers
// (golden_data_test.go) with plain arithmetic and no package aocexpr: the curve's
// value at that level times the formula's multiplier, rounded half up, in float64
// and in float32 (what a game built on Unreal's float would compute). onHalf says
// the product is within 1e-6 of a half.
func goldenValue(multiplier string, level int) (want64, want32 float64, onHalf bool) {
	mult, err := strconv.ParseFloat(multiplier, 64)
	if err != nil {
		panic(err)
	}
	v := goldenCurveValues[level-1]
	x := v * mult
	want64 = math.Floor(x + 0.5)
	x32 := float32(float32(v) * float32(mult)) // the conversion keeps a compiler from fusing the multiply and the add
	want32 = math.Floor(float64(x32 + 0.5))
	return want64, want32, math.Abs(x-math.Floor(x)-0.5) < 1e-6
}

// goldenCurveMap returns the golden curves as a table EvalCurve can use.
func goldenCurveMap(t *testing.T) *aocexpr.CurveMap {
	t.Helper()
	var keys []aocexpr.Key
	for i, v := range goldenCurveValues {
		keys = append(keys, aocexpr.Key{Time: float64(i + 1), Value: v, Interp: aocexpr.InterpLinear})
	}
	cm := &aocexpr.CurveMap{TypeID: content.TypeStatCurve, Curves: map[int64]*aocexpr.Curve{}}
	for _, c := range goldenCurves {
		curve, err := aocexpr.NewCurve(keys)
		if err != nil {
			t.Fatal(err)
		}
		cm.Curves[c.guid] = curve
	}
	return cm
}

// -- C_Y_B . C | O | A --

// TestGoldenClassAttributes runs the 48 class formulas of the real design
// database (golden_data_test.go) through package aocexpr at all 50 levels, in
// float64 and in float32, and compares every value with goldenValue. It needs
// none of the inputs: the numbers are in the test.
func TestGoldenClassAttributes(t *testing.T) {
	cm := goldenCurveMap(t)
	if len(goldenFormulas) != 48 || len(goldenCurves) != 6 || len(goldenCurveValues) != 50 {
		t.Fatalf("%d formulas, %d curves, %d values", len(goldenFormulas), len(goldenCurves), len(goldenCurveValues))
	}
	// Each class has the six attributes once.
	perClass := map[string]map[string]bool{}
	for _, f := range goldenFormulas {
		if perClass[f.class] == nil {
			perClass[f.class] = map[string]bool{}
		}
		perClass[f.class][f.stat] = true
	}
	if len(perClass) != len(content.PlayerClasses) {
		t.Errorf("%d classes in the golden formulas", len(perClass))
	}
	for class, stats := range perClass {
		if len(stats) != len(content.PrimaryAttributes) {
			t.Errorf("%s has %d attributes in the golden formulas", class, len(stats))
		}
	}

	halves, differing := 0, 0
	for _, f := range goldenFormulas {
		prog, err := aocexpr.Parse(formula(f.curve, f.multiplier))
		if err != nil {
			t.Fatalf("%s %s: %v", f.class, f.stat, err)
		}
		for level := 1; level <= 50; level++ {
			want64, want32, onHalf := goldenValue(f.multiplier, level)
			got64, err := prog.EvalFloat(aocexpr.CharacterEnv(level, cm))
			if err != nil || got64 != want64 {
				t.Errorf("%s %s level %d = %v, %v; golden %v", f.class, f.stat, level, got64, err, want64)
			}
			single := aocexpr.CharacterEnv(level, cm)
			single.Single = true
			got32, err := prog.EvalFloat(single)
			if err != nil || got32 != want32 {
				t.Errorf("%s %s level %d in float32 = %v, %v; golden %v", f.class, f.stat, level, got32, err, want32)
			}
			if onHalf {
				halves++
			}
			if want64 != want32 {
				differing++
				if level != 20 || want64 != 436 || want32 != 435 {
					t.Errorf("%s %s level %d: float64 %v, float32 %v; the survey found only 436 against 435 at level 20", f.class, f.stat, level, want64, want32)
				}
			}
		}
	}
	// 80 of the 2400 values sit on a half; for 7 of them float32 arithmetic
	// rounds the other way (335 * 1.3 at level 20): not proven either way.
	if halves != 80 || differing != 7 {
		t.Errorf("%d values on a half, %d where float32 differs; the survey found 80 and 7", halves, differing)
	}
	// The captured Bard at level 1 (see real_test.go): Strength 24, Dexterity 24,
	// Constitution 30, Mentality 30, Wisdom 36.
	for _, f := range goldenFormulas {
		want, ok := capturedBardLevel1[f.stat]
		if f.class != "Bard" || !ok {
			continue
		}
		if got, _, _ := goldenValue(f.multiplier, 1); got != want {
			t.Errorf("golden Bard %s at level 1 = %v, the capture has %v", f.stat, got, want)
		}
	}
}

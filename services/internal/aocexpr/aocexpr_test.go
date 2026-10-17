// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"errors"
	"math"
	"strings"
	"testing"
)

// The expression texts below are copied from CacheDB.dbc (decoded by package
// designdb); the database keeps CRLF line ends, which crlf restores.

func crlf(s string) string { return strings.ReplaceAll(s, "\n", "\r\n") }

const (
	typeStatCurve = 7037248826340794574
	typeEffect    = 3377240744094973029

	// Stat record Guids and curves (StatTypeDef table 3, StatCurve table 296).
	curveStrength     = 6064630328334811136 // Player_Base_Attrib_Strength
	curveConstitution = 6064630328054775808 // Player_Base_Attrib_Constitution
	curveIntelligence = 6064630328334811139 // Player_Base_Attrib_Intelligence
)

// Player_Base_Bard (StatInitializerList 6064630736345890821), Strength.
var bardStrength = crlf(`const v = EvalCurve($#7037248826340794574:6064630328334811136$, Float(GetCharacterLevel(GetOwner())));
Round(v * 0.8)`)

// Player_Base_Tank, Constitution.
var tankConstitution = crlf(`const v = EvalCurve($#7037248826340794574:6064630328054775808$, Float(GetCharacterLevel(GetOwner())));
Round(v * 1.3)`)

// Player_Base_Bard, Intelligence.
var bardIntelligence = crlf(`const v = EvalCurve($#7037248826340794574:6064630328334811139$, Float(GetCharacterLevel(GetOwner())));
Round(v * 1.3)`)

// StatTypeDef StaminaRegeneration (6064631663405498419), the base expression.
var staminaRegen = crlf(`const disableRegen = (GetEffectCount(GetSource(), $#3377240744094973029:6064631641826394197$) > 0 && IsMoving(GetSource())) ||
GetEffectCount(GetSource(), $#3377240744094973029:6064631667969556480$) > 0 ||
GetEffectCount(GetSource(), $#3377240744094973029:106223481149050$) > 0 ||
GetEffectCount(GetSource(), $#3377240744094973029:6064631799761208354$) > 0 ||
GetEffectCount(GetSource(), $#3377240744094973029:6064631668308443138$) > 0;

SelectFloat(disableRegen, 0.0, 5.0)`)

// Ability General_Sprint (6064631641648332884), Costs[0].StatCosts[0].Value.
var sprintCost = crlf(`var inCombat = IsInCombat(GetSource());
var Cost = 1.25;

if (inCombat)
{
set Cost = 2.5;
}

Cost`)

// Ability General_Dodge (107258680465016): the stamina cost and the cooldown.
var dodgeCost = crlf(`var snareStacks = GetEffectCount(GetInstigator(), $#3377240744094973029:6064629784692426711$);
var chilledStacks = GetEffectCount(GetInstigator(), $#3377240744094973029:6064628906043703296$);
var chilled = SelectInt(chilledStacks > 0, 2, 0);
var mult = 1 + (0.25 * Min((snareStacks + chilled),3));

SelectInt(GetEffectCount(GetInstigator(), $#3377240744094973029:6064632660379712165$) > 0 &&
GetSkillPurchased(GetOwner(),$#297887639323616934:6064630422842835238$,$#4146438674247114715:6064632892734570925$),
30,
50) * mult`)

var dodgeCooldown = crlf(`SelectFloat(GetEffectCount(GetInstigator(), $#3377240744094973029:6064632660379712165$) > 0 &&
GetSkillPurchased(GetOwner(),$#297887639323616934:6064630422842835238$,$#4146438674247114715:6064632892734570925$),
0.4,
0.65)`)

// Player_Base MaxHealth and NPC_Base_MonsterCoin Strength: outside the subset.
var (
	playerPhysicalSpeed = `GetStatInitRunningTotal()+EvalE($#7180619218012719533:6064634178656927909$)`
	monsterCoinStrength = crlf(`const numWeights = 8;
const weight = 0.125; //GetNpcStatDistribution(GetOwner(), EStatDistributionStat::Strength);
const totalStats = EvalCurve($#7037248826340794574:6064630328334811136$, Float(GetCharacterLevel(GetOwner())));
totalStats * weight`)
)

// -- Cyb x C.O.A --

// curves are the first levels of the real Strength, Constitution and
// Intelligence curves (Linear keys; Constitution 30, 68, 118, 152 at levels 1-4).
func curves(t *testing.T) *CurveMap {
	t.Helper()
	mk := func(vals ...float64) *Curve {
		var keys []Key
		for i, v := range vals {
			keys = append(keys, Key{Time: float64(i + 1), Value: v, Interp: InterpLinear})
		}
		c, err := NewCurve(keys)
		if err != nil {
			t.Fatal(err)
		}
		return c
	}
	return &CurveMap{TypeID: typeStatCurve, Curves: map[int64]*Curve{
		curveStrength:     mk(30, 50, 75, 100),
		curveConstitution: mk(30, 68, 118, 152),
		curveIntelligence: mk(30, 55, 81, 107),
	}}
}

func TestAttributeFormulas(t *testing.T) {
	cs := curves(t)
	for _, tt := range []struct {
		name  string
		src   string
		level int
		want  float64
	}{
		{"Bard Strength 1", bardStrength, 1, 24},
		{"Bard Strength 2", bardStrength, 2, 40},
		{"Bard Strength 3", bardStrength, 3, 60},
		{"Tank Constitution 1", tankConstitution, 1, 39},
		{"Tank Constitution 2", tankConstitution, 2, 88},  // 68 * 1.3 = 88.4
		{"Tank Constitution 3", tankConstitution, 3, 153}, // 118 * 1.3 = 153.4
		{"Bard Intelligence 1", bardIntelligence, 1, 39},
		{"Bard Intelligence 4", bardIntelligence, 4, 139}, // 107 * 1.3 = 139.1
	} {
		got, err := EvalFloat(tt.src, CharacterEnv(tt.level, cs))
		if err != nil || got != tt.want {
			t.Errorf("%s = %v, %v; want %v", tt.name, got, err, tt.want)
		}
	}
}

// C | Y | B / C.O.A

func TestRound(t *testing.T) {
	for _, tt := range []struct {
		src  string
		want float64
	}{
		{"Round(2.4)", 2}, {"Round(2.5)", 3}, {"Round(2.6)", 3}, {"Round(0.5)", 1}, {"Round(100)", 100},
		{"Round(30 * 0.8)", 24}, {"Round(30 * 1.3)", 39}, {"Round(15 * 1.1)", 17}, // 16.5 up
		// Halves go toward positive infinity (floor(x+0.5)): -2.5 is -2, not -3.
		{"Round(-2.4)", -2}, {"Round(-2.5)", -2}, {"Round(-2.6)", -3}, {"Round(-0.5)", 0}, {"Round(-0.4)", 0}, {"Round(-1.5)", -1},
	} {
		// Single mode gives the same integers (its argument is a float32 value,
		// exact here, and Round adds 0.5 in float64).
		for _, env := range []*Env{nil, {Single: true}} {
			if got, err := EvalFloat(tt.src, env); err != nil || got != tt.want {
				t.Errorf("%s (env %+v) = %v, %v; want %v", tt.src, env, got, err, tt.want)
			}
		}
	}
}

// stamina is the environment of the stamina formulas: the effects the source
// has (by Guid), whether it is moving and in combat, and the skills it bought.
type stamina struct {
	effects  map[int64]float64
	moving   bool
	inCombat bool
	skilled  bool
}

func (s stamina) env() *Env {
	obj := func(kind string) Func {
		return func([]Value) (Value, error) { return Object{Kind: kind}, nil }
	}
	return &Env{Funcs: map[string]Func{
		"GetSource":     obj("source"),
		"GetInstigator": obj("instigator"),
		"GetOwner":      obj("owner"),
		"IsMoving":      func([]Value) (Value, error) { return s.moving, nil },
		"IsInCombat":    func([]Value) (Value, error) { return s.inCombat, nil },
		"GetEffectCount": func(args []Value) (Value, error) {
			ref, ok := args[1].(Ref)
			if !ok || ref.TypeID != typeEffect {
				return nil, errors.New("want an effect reference")
			}
			return s.effects[ref.Guid], nil
		},
		"GetSkillPurchased": func([]Value) (Value, error) { return s.skilled, nil },
	}}
}

func TestStaminaRegeneration(t *testing.T) {
	const (
		sprint    = 6064631641826394197
		guard     = 6064631667969556480
		evasion   = 106223481149050
		guardStun = 6064631799761208354
		exertion  = 6064631668308443138
	)
	for _, tt := range []struct {
		name string
		s    stamina
		want float64
	}{
		{"nothing", stamina{}, 5},
		{"sprinting and moving", stamina{effects: map[int64]float64{sprint: 1}, moving: true}, 0},
		{"sprint effect but standing still", stamina{effects: map[int64]float64{sprint: 1}}, 5},
		{"moving without the effect", stamina{moving: true}, 5},
		{"guard", stamina{effects: map[int64]float64{guard: 1}}, 0},
		{"evading", stamina{effects: map[int64]float64{evasion: 1}}, 0},
		{"guard broken", stamina{effects: map[int64]float64{guardStun: 2}}, 0},
		{"exertion", stamina{effects: map[int64]float64{exertion: 1}}, 0},
	} {
		got, err := EvalFloat(staminaRegen, tt.s.env())
		if err != nil || got != tt.want {
			t.Errorf("%s: %v, %v; want %v", tt.name, got, err, tt.want)
		}
	}

	p, err := Parse(staminaRegen)
	if err != nil {
		t.Fatal(err)
	}
	var effects []int64
	for _, r := range p.Refs() {
		if r.TypeID != typeEffect {
			t.Errorf("reference %v is not an effect", r)
		}
		effects = append(effects, r.Guid)
	}
	if want := []int64{sprint, guard, evasion, guardStun, exertion}; len(effects) != len(want) {
		t.Errorf("Refs = %v, want %v", effects, want)
	} else {
		for i := range want {
			if effects[i] != want[i] {
				t.Errorf("Refs = %v, want %v", effects, want)
				break
			}
		}
	}
}

func TestSprintCost(t *testing.T) {
	for _, tt := range []struct {
		inCombat bool
		want     float64
	}{{false, 1.25}, {true, 2.5}} {
		got, err := EvalFloat(sprintCost, stamina{inCombat: tt.inCombat}.env())
		if err != nil || got != tt.want {
			t.Errorf("Sprint cost in combat=%v: %v, %v; want %v", tt.inCombat, got, err, tt.want)
		}
	}
}

func TestDodge(t *testing.T) {
	const (
		snared      = 6064629784692426711
		chilled     = 6064628906043703296
		dodgeEffect = 6064632660379712165
	)
	for _, tt := range []struct {
		name         string
		s            stamina
		cost, cooldn float64
	}{
		{"plain", stamina{}, 50, 0.65},
		{"skill without its effect", stamina{skilled: true}, 50, 0.65},
		{"skill and effect", stamina{skilled: true, effects: map[int64]float64{dodgeEffect: 1}}, 30, 0.4},
		{"snared once", stamina{effects: map[int64]float64{snared: 1}}, 62.5, 0.65},
		{"chilled counts as two snares", stamina{effects: map[int64]float64{chilled: 1}}, 75, 0.65},
		{"snares are capped at three", stamina{effects: map[int64]float64{snared: 5, chilled: 1}}, 87.5, 0.65},
		{"skilled and snared twice", stamina{skilled: true, effects: map[int64]float64{dodgeEffect: 1, snared: 2}}, 45, 0.4},
	} {
		cost, err := EvalFloat(dodgeCost, tt.s.env())
		if err != nil || cost != tt.cost {
			t.Errorf("%s: cost %v, %v; want %v", tt.name, cost, err, tt.cost)
		}
		cd, err := EvalFloat(dodgeCooldown, tt.s.env())
		if err != nil || cd != tt.cooldn {
			t.Errorf("%s: cooldown %v, %v; want %v", tt.name, cd, err, tt.cooldn)
		}
	}
}

// ~ C.O.A :: Cyb ~

// % is the remainder of whole numbers (plan E1, stage 3: the weapon combos' step counts), truncated as a C++ one is.
func TestModulo(t *testing.T) {
	for src, want := range map[string]float64{"7 % 3": 1, "6 % 3": 0, "-7 % 3": -1, "7 % -3": 1, "7.9 % 3.9": 1, "2 + 7 % 4 * 2": 8, "(2 + 7) % 4": 1} {
		got, err := EvalFloat(src, &Env{})
		if err != nil || got != want {
			t.Errorf("%s = %v, %v; want %v", src, got, err, want)
		}
	}
	if _, err := EvalFloat("5 % 0", &Env{}); err == nil || !strings.Contains(err.Error(), "modulo by zero") {
		t.Errorf("5 %% 0: %v", err)
	}
	if _, err := EvalFloat("5 % 0.5", &Env{}); err == nil {
		t.Errorf("5 %% 0.5 truncates to a zero modulus: %v", err)
	}
}

func TestUnsupportedNamesTheConstruct(t *testing.T) {
	cs := curves(t)
	for _, tt := range []struct {
		name, src, construct string
	}{
		{"a host function the environment lacks", playerPhysicalSpeed, `function "GetStatInitRunningTotal"`},
		{"EvalE", `EvalE($#7180619218012719533:6064634178656927909$)`, `function "EvalE"`},
		{"stat reference", `GetStat(GetOwner(), $Stat:MaxStamina$)`, `function "GetStat"`},
		{"string", `Foo("x")`, `function "Foo"`},
		{"enum", `Foo(EEquipSlot::Secondary)`, `function "Foo"`},
		{"a power operator", `5 ^ 2`, `character "^"`},
		{"hex", `0x10`, `number syntax "0x"`},
		{"exponent", `1e5`, `number syntax "1e"`},
		{"while", `while (true) { }`, `statement "while"`},
		{"EvalCurve without curves", `EvalCurve($#7037248826340794574:1$, 1)`, `EvalCurve (the environment has no curve source)`},
		{"integer division", `7 / 2`, divisionConstruct},
		{"a division chain of integers", `8 / 2 / 2`, divisionConstruct},
		{"a division of a negative integer", `-7 / 2`, divisionConstruct},
		{"a division by zero of integers", `7 / 0`, divisionConstruct},
		{"a division of Round's result", `Round(3.5) / 2`, divisionConstruct},
		{"a division of SelectInt", `SelectInt(true, 3, 4) / 2`, divisionConstruct},
		{"a division of Min of integers", `Min(3, 2) / 2`, divisionConstruct},
		{"a division of an integer sum", `(1 + 2) / 4`, divisionConstruct},
		{"a division inside a sum with a float", `1.0 + 2 / 4`, divisionConstruct},
		{"a division of a host function's result", `GetCharacterLevel(GetOwner()) / 2`, divisionConstruct},
		{"a division of an integer variable", `const x = 7; x / 2`, divisionConstruct},
		{"a division of a variable that was set to an integer", `var x = 7.0; set x = 3; x / 2`, divisionConstruct},
		{"a division of an unknown variable's call", `const n = GetCharacterLevel(GetOwner()); n / 2`, divisionConstruct},
	} {
		env := CharacterEnv(1, cs)
		if tt.name == "EvalCurve without curves" {
			env = &Env{}
		}
		_, err := EvalFloat(tt.src, env)
		got, ok := Unsupported(err)
		if !ok || got != tt.construct {
			t.Errorf("%s: Unsupported(%v) = %q, %v; want %q", tt.name, err, got, ok, tt.construct)
		}
		if err != nil && !strings.Contains(err.Error(), "unsupported "+tt.construct) {
			t.Errorf("%s: error %q does not name %s", tt.name, err, tt.construct)
		}
	}

	// A construct outside the subset is an error even where a silent 0 would
	// have hidden it: MonsterCoin's formula asks for a function the player
	// environment does not have, after a comment that mentions another.
	if _, err := EvalFloat(monsterCoinStrength, CharacterEnv(1, cs)); err != nil {
		t.Errorf("a comment naming a function is not a call: %v", err)
	}
	// A cubic segment is a Bezier through the control points the tangents make (plan E1, stage 3): flat tangents ease in and out (smoothstep),
	// a slope the keys' own tangents give shows in the middle, and the key's own value is exact.
	cubic, _ := NewCurve([]Key{{Time: 1, Value: 0, Interp: InterpCubic}, {Time: 3, Value: 9}})
	if v, err := cubic.Eval(2); err != nil || v != 4.5 {
		t.Errorf("a flat-tangent cubic at the middle = %v, %v, want 4.5", v, err)
	}
	if v, err := cubic.Eval(1); err != nil || v != 0 {
		t.Errorf("cubic at a key = %v, %v", v, err)
	}
	sloped, _ := NewCurve([]Key{{Time: 0, Value: 0, Interp: InterpCubic, Leave: 3}, {Time: 2, Value: 0, Arrive: -3}})
	if v, err := sloped.Eval(1); err != nil || v < 1.49 || v > 1.51 {
		t.Errorf("a cubic leaving with slope 3 and arriving with -3 over 2 units peaks at %v, %v (want 1.5)", v, err)
	}
}

// (C:Y:B + CoA)

// divisionConstruct is the construct a division with no operand proven to be a
// float is reported as.
const divisionConstruct = "division where no operand is proven to be a float (integer division is not modelled; write 2.0 or Float(x))"

// TestDivision: the engine's expressions are typed and its integer division
// would not give a float's result, which this evaluator (every number a
// float64) cannot tell apart, so `/` is evaluated only when an operand is
// provably a float and is otherwise reported as unsupported (see the cases of
// TestUnsupportedNamesTheConstruct).
func TestDivision(t *testing.T) {
	cs := curves(t)
	for _, tt := range []struct {
		src  string
		want float64
	}{
		{"7 / 2.0", 3.5},
		{"7.0 / 2", 3.5},
		{"7. / 2", 3.5},
		{"Float(7) / 2", 3.5},
		{"7 / Float(2)", 3.5},
		{"-7.0 / 2", -3.5},
		{"8.0 / 2 / 2", 2}, // (8.0 / 2) is a float
		{"(1.0 + 2) / 4", 0.75},
		{"(1 + 2.0) * 3 / 4", 2.25},
		{"SelectFloat(true, 1, 2) / 4", 0.25},
		{"Min(1.5, 2) / 2", 0.75},
		{"Max(2, 1.0) / 4", 0.5},
		{"EvalCurve($#7037248826340794574:6064630328334811136$, 2) / 2", 25},
		{"const x = 7.0; x / 2", 3.5},
		{"const f = Float(3); const y = f * 2; y / 4", 1.5},
		{"var x = 7.0; set x = x + 1; x / 2", 4},
		{"var x = 1.5; if (true) { set x = 2.5; } x / 2", 1.25},
		{"var x = 7.0; if (true) { var x = 1; set x = 2; } x / 2", 3.5}, // the block's own x
	} {
		if got, err := EvalFloat(tt.src, CharacterEnv(1, cs)); err != nil || got != tt.want {
			t.Errorf("%s = %v, %v; want %v", tt.src, got, err, tt.want)
		}
	}
	// The error says where, and a division by zero of floats is still an error of its own.
	_, err := EvalFloat("1 +\r\n  7 / 2", nil)
	var e *Error
	if !errors.As(err, &e) || e.Pos != (Pos{2, 5}) || !e.Unsupported {
		t.Errorf("position of %v = %v, want 2:5", err, e)
	}
	if _, err := EvalFloat("1.0 / 0", nil); err == nil || !strings.Contains(err.Error(), "division by zero") {
		t.Errorf("1.0 / 0: %v", err)
	}
}

func TestErrors(t *testing.T) {
	for _, tt := range []struct {
		name, src, want string
	}{
		{"empty", "", "empty expression"},
		{"blank", " \r\n // nothing\r\n", "empty expression"},
		{"ends in a statement", "const x = 1;", "needs a final expression"},
		{"missing paren", "Round(1", `expected ",", found the end of the text`},
		{"missing operand", "1 +", "expected a value, found the end of the text"},
		{"stray brace", "1 }", `unexpected "}" after the final expression`},
		{"two results", "1 2", "unexpected number 2 after the final expression"},
		{"unclosed comment", "1 /* x", "unterminated /* comment"},
		{"unterminated ref", "$#1:2", "unterminated $ reference"},
		{"bad ref", "$#a:b$", `bad reference "$#a:b$"`},
		{"unknown variable", "x + 1", `unknown variable "x"`},
		{"set undeclared", "set x = 1; 2", `set of undeclared variable "x"`},
		{"set const", "const x = 1; set x = 2; x", `set of const "x"`},
		{"redeclare", "var x = 1; var x = 2; x", `"x" is already declared`},
		{"division by zero", "1.0 / 0", "division by zero"},
		{"type: number and boolean", "1 + true", "+ needs a number, got the boolean true"},
		{"type: condition", "if (1) { }\n2", "the condition of an if is the number 1"},
		{"type: not", "!1", "! needs a boolean"},
		{"type: compare", "1 == true", "cannot compare"},
		{"arity", "Round(1, 2)", "Round takes 1 argument(s), got 2"},
		{"select condition", "SelectFloat(1, 2, 3)", "the condition is the number 1, not a boolean"},
		{"keyword as value", "1 + if", `"if" is a keyword`},
		{"value of an if branch", "if (true) { 1 }\n2", "a branch of an if has no value"},
	} {
		_, err := EvalFloat(tt.src, nil)
		if err == nil || !strings.Contains(err.Error(), tt.want) {
			t.Errorf("%s: %v; want an error containing %q", tt.name, err, tt.want)
		}
		if _, ok := Unsupported(err); ok {
			t.Errorf("%s: reported as unsupported: %v", tt.name, err)
		}
	}

	// Positions are line:column, 1-based.
	_, err := EvalFloat("1 +\r\n  2 * )", nil)
	var e *Error
	if !errors.As(err, &e) || e.Pos != (Pos{2, 7}) {
		t.Errorf("position of %v = %v, want 2:7", err, e)
	}
	// The value of a formula must be a number.
	if _, err := EvalFloat("1 < 2", nil); err == nil || !strings.Contains(err.Error(), "not a number") {
		t.Errorf("EvalFloat of a boolean: %v", err)
	}
}

func TestLanguage(t *testing.T) {
	for _, tt := range []struct {
		src  string
		want any
	}{
		{"1 + 2 * 3", 7.0},
		{"(1 + 2) * 3", 9.0},
		{"10 - 4 - 3", 3.0},
		{"8.0 / 2 / 2", 2.0},
		{"-2 * -3", 6.0},
		{"--4", 4.0},
		{".5 + 1.", 1.5},
		{"1 < 2 && 2 <= 2 && 3 > 2 && 3 >= 3 && 1 != 2 && 1 == 1", true},
		{"1 > 2 || 2 > 3", false},
		{"true || Nope()", true}, // short circuit: the call is never made
		{"false && Nope()", false},
		{"!false", true},
		{"true == true", true},
		// && binds tighter than ||, == tighter than && and looser than <.
		{"true || false && false", true},  // true || (false && false); (true || false) && false is false
		{"false && false || true", true},  // (false && false) || true; false && (false || true) is false
		{"1 < 2 == true", true},           // (1 < 2) == true
		{"1 < 2 == 2 < 3", true},          // (1 < 2) == (2 < 3)
		{"false == 1 > 2", true},          // false == (1 > 2)
		{"1 + 2 < 4 && 2 * 3 == 6", true}, // arithmetic binds tighter than comparisons
		{"/* a */ 1 // b", 1.0},
		{"var x = 1; if (x < 2) { set x = x + 10; } else { set x = 0; } x", 11.0},
		{"var x = 1; if (x > 2) { set x = 5; } else if (x == 1) { set x = 7; } else { set x = 0; } x", 7.0},
		{"var x = 1; if (true) { var x = 5; set x = 6; } x", 1.0}, // a block's own scope
		{"var x = 1; if (true) { set x = 6; } x", 6.0},            // set reaches outward
		{"const a = 2; const b = a * a; b + a;", 6.0},             // a trailing ';' is allowed
		{"Min(3, 2) + Max(3, 2)", 5.0},
		{"SelectInt(2 > 1, 10, 20)", 10.0},
		{"Float(7)", 7.0},
	} {
		got, err := mustParse(t, tt.src).Eval(&Env{})
		if err != nil || got != tt.want {
			t.Errorf("%s = %v, %v; want %v", tt.src, got, err, tt.want)
		}
	}
}

// C-Y-B x C.O.A

func mustParse(t *testing.T, src string) *Program {
	t.Helper()
	p, err := Parse(src)
	if err != nil {
		t.Fatalf("Parse(%q): %v", src, err)
	}
	return p
}

func TestHostFunctionErrors(t *testing.T) {
	env := &Env{Funcs: map[string]Func{
		"Boom": func([]Value) (Value, error) { return nil, errors.New("exploded") },
	}}
	_, err := EvalFloat("1 + Boom()", env)
	if err == nil || !strings.Contains(err.Error(), "Boom: exploded") {
		t.Errorf("host error = %v", err)
	}
	// The built-ins cannot be replaced by the host.
	env.Funcs["Round"] = func([]Value) (Value, error) { return 99.0, nil }
	if v, err := EvalFloat("Round(1.2)", env); err != nil || v != 1 {
		t.Errorf("Round with a host Round = %v, %v", v, err)
	}
	// GetCharacterLevel takes the owner and nothing else.
	if _, err := EvalFloat("GetCharacterLevel(5)", CharacterEnv(3, nil)); err == nil {
		t.Error("GetCharacterLevel(5) did not fail")
	}
	if v, err := EvalFloat("GetCharacterLevel(GetOwner())", CharacterEnv(3, nil)); err != nil || v != 3 {
		t.Errorf("GetCharacterLevel = %v, %v", v, err)
	}
}

func TestCurve(t *testing.T) {
	c, err := NewCurve([]Key{
		{Time: 3, Value: 30, Interp: InterpConstant},
		{Time: 1, Value: 10, Interp: InterpLinear},
		{Time: 2, Value: 20, Interp: InterpLinear},
		{Time: 5, Value: 40, Interp: InterpLinear},
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, tt := range []struct{ t, want float64 }{
		{-5, 10}, {1, 10}, {1.5, 15}, {2, 20}, {2.25, 22.5}, // linear segments
		{3, 30}, {4, 30}, {4.99, 30}, // a constant segment holds its key
		{5, 40}, {9, 40}, // constant extrapolation after the last key
	} {
		if got, err := c.Eval(tt.t); err != nil || math.Abs(got-tt.want) > 1e-12 {
			t.Errorf("Eval(%v) = %v, %v; want %v", tt.t, got, err, tt.want)
		}
	}
	if _, err := (&Curve{}).Eval(1); err == nil {
		t.Error("an empty curve evaluated")
	}
	if _, err := NewCurve([]Key{{Time: 1}, {Time: 1}}); err == nil {
		t.Error("two keys at one time accepted")
	}

	for in, want := range map[string]Interp{
		"ERichCurveInterpMode::RCIM_Linear":   InterpLinear,
		"ERichCurveInterpMode::RCIM_Constant": InterpConstant,
		"RCIM_Cubic":                          InterpCubic,
		"Linear":                              InterpLinear,
		"Constant":                            InterpConstant,
	} {
		if got, err := ParseInterp(in); err != nil || got != want {
			t.Errorf("ParseInterp(%q) = %v, %v", in, got, err)
		}
	}
	if _, err := ParseInterp("ERichCurveInterpMode::RCIM_None"); err == nil {
		t.Error("RCIM_None accepted")
	}
}

func TestCurveMap(t *testing.T) {
	cs := curves(t)
	if _, err := cs.Curve(Ref{TypeID: 5, Guid: curveStrength}); err == nil {
		t.Error("a reference of another type resolved")
	}
	if _, err := cs.Curve(Ref{TypeID: typeStatCurve, Guid: 1}); err == nil {
		t.Error("an unknown curve resolved")
	}
	// A bad reference inside a formula is reported with the call's position.
	_, err := EvalFloat("EvalCurve($#7037248826340794574:1$, 1)", CharacterEnv(1, cs))
	if err == nil || !strings.Contains(err.Error(), "no curve 1") {
		t.Errorf("EvalCurve of an unknown curve: %v", err)
	}
	_, err = EvalFloat("EvalCurve(1, 1)", CharacterEnv(1, cs))
	if err == nil || !strings.Contains(err.Error(), "not a $#<TypeId>:<Guid>$ reference") {
		t.Errorf("EvalCurve(1, 1): %v", err)
	}
}

func TestCurveNaNInputIsAnError(t *testing.T) {
	for _, keys := range [][]Key{
		{{Time: 1, Value: 10}},
		{{Time: 1, Value: 10}, {Time: 2, Value: 20}},
	} {
		c, err := NewCurve(keys)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := c.Eval(math.NaN()); err == nil {
			t.Errorf("a NaN lookup in a %d-key curve returned no error", len(keys))
		}
	}
}

func TestCurveRejectsNonFiniteKeys(t *testing.T) {
	for _, value := range []float64{math.NaN(), math.Inf(-1), math.Inf(1)} {
		for _, key := range []Key{
			{Time: value}, {Value: value}, {Arrive: value}, {Leave: value},
		} {
			if _, err := NewCurve([]Key{key}); err == nil {
				t.Errorf("non-finite key accepted: %+v", key)
			}
		}
	}
}

// TestSingle: float32 arithmetic gives another integer where a product sits on
// a half. 335 is the Constitution curve's value at level 20, 1.3 the Tank's
// multiplier; of the 2400 primary attribute values 80 are on a half, and for
// these seven (class and attribute pairs at level 20) float32 and float64 differ.
func TestSingle(t *testing.T) {
	for _, tt := range []struct {
		src         string
		f64, single float64
	}{
		{"Round(335 * 1.3)", 436, 435}, // 435.5 against 435.49997
		{"Round(215 * 1.3)", 280, 280}, // 279.5 in both
		{"Round(30 * 1.3)", 39, 39},
		{"Round(30 * 0.8)", 24, 24},
		{"1.3", 1.3, float64(float32(1.3))},
		{"1.0 / 3", 1.0 / 3, float64(float32(1.0 / 3))},
		{"-1.3 + 0", -1.3, -float64(float32(1.3))},
	} {
		if got, err := EvalFloat(tt.src, nil); err != nil || got != tt.f64 {
			t.Errorf("%s = %v, %v; want %v", tt.src, got, err, tt.f64)
		}
		if got, err := EvalFloat(tt.src, &Env{Single: true}); err != nil || got != tt.single {
			t.Errorf("%s in float32 = %v, %v; want %v", tt.src, got, err, tt.single)
		}
	}
	// A curve's value is rounded too.
	c, _ := NewCurve([]Key{{Time: 0, Value: 0, Interp: InterpLinear}, {Time: 3, Value: 1}})
	cs := &CurveMap{TypeID: 1, Curves: map[int64]*Curve{2: c}}
	got, err := EvalFloat("EvalCurve($#1:2$, 1)", &Env{Curves: cs, Single: true})
	if err != nil || got != float64(float32(1.0/3)) {
		t.Errorf("EvalCurve in float32 = %v, %v", got, err)
	}
}

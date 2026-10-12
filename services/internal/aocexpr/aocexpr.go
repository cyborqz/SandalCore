// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package aocexpr parses and evaluates AoCExpression text: the small scripting
// language the design database (CacheDB.dbc, read by package designdb) uses for
// every formula, such as a stat initializer's value, a stat's regeneration or
// an ability's cost. A formula is a few statements and a final expression:
//
//	const v = EvalCurve($#7037248826340794574:6064630328334811136$, Float(GetCharacterLevel(GetOwner())));
//	Round(v * 0.8)
//
// This is the Strength of a Bard, from Player_Base_Bard's initializer list
// (StatInitializerList row 6064630736345890821): v is the character level's
// value on the StatCurve row 6064630328334811136 (Player_Base_Attrib_Strength),
// scaled by 0.8 and rounded: 24 at level 1.
//
// # The supported subset
//
// The first content slice (plan C1) computes the six primary attributes of the
// eight classes from those rows and collects the stamina rules for plan A7 from
// the Sprint and Dodge abilities and the StaminaRegeneration stat. The subset is
// what those texts use, and nothing else; anything outside it is an *Error
// that names the construct, never a silent zero:
//
//	numbers           100  1.25  0.0001  (decimal; no exponent, no hex)
//	true, false
//	strings           "Item.Gear.Weapon.Shield"  'text'  (one line; backslash escapes a quote or a backslash)
//	enum values       EEquipSlot::Secondary  (an Enum; the host's functions take them)
//	references        $#<TypeId>:<Guid>$  or  $Kind:Name$  (a Ref)
//	variables         const NAME = expr;   var NAME = expr;   set NAME = expr;
//	                  (a block's own scope; set changes the nearest declaration)
//	conditionals      if (expr) { statements } [else { statements }]
//	operators         unary - !   * /   + -   < <= > >=   == !=   &&   ||   (in that order of binding;
//	                  / needs a float operand, see below)
//	parentheses       (expr)
//	calls             Name(args). Built in: Round, Float, Floor, Ceil, SelectFloat,
//	                  SelectInt, Min, Max, Lerp, Abs, Clamp, EvalCurve. Every other function is the host's:
//	                  Env.Funcs. A call to one the host did not supply is an
//	                  unsupported-function error.
//	comments          // to the end of the line, /* ... */
//
// The result is the last expression of the text.
//
// # Numbers, division and Round
//
// Every number is a float64. The engine's expressions are typed (a number with
// a decimal point and Float(...) are floats, 100 and SelectInt(...) integers,
// and SelectInt and SelectFloat are two functions), and the evaluator models
// the types in one place only, where they change an answer: division. An
// integer division would not give a float's result (in C++ it truncates), and
// the evaluator cannot tell which one a text means, so "/" is evaluated only
// when an operand is provably a float, and any other division is an
// unsupported-construct error, never a guess. Provably a float: a number with a
// decimal point (2.0, 1.), Float(...), SelectFloat(...), EvalCurve(...), Min or
// Max with a float argument, a host function the host names in Env.FloatFuncs
// (StatEnv names GetStat: a stat is a float), a variable that has only ever
// been given such a value, and + - * / and unary - with such an operand (a float
// and an integer make a float). Not proven: integers (7 / 2), Round, Floor,
// Ceil, SelectInt, and every other host function (the evaluator does not know
// what type the host's numbers have), so GetCharacterLevel(GetOwner()) / 2
// needs Float(...) around it. The class
// formulas and the stamina, Sprint and Dodge texts do not divide.
//
// Round rounds halves toward positive infinity: floor(x+0.5), which is what
// Unreal's FMath::RoundToInt computes. Round(2.5) is 3, Round(-2.5) is -2 and
// Round(-0.5) is 0 (the tests pin them). The engine's own rule for halves is not
// in the data, and it matters: of the 2400 values of the primary attributes (8
// classes, 6 attributes, 50 levels) 80 land exactly on a half. Together with
// the arithmetic's width (Env.Single) that leaves 7 of them (335 * 1.3 at level
// 20: 436 in float64, 435 in float32) not proven; contentc flags them in
// class_stat_base.rounding_ambiguous.
//
// In Single mode Round's argument is already a float32 value and Round still
// computes floor(x+0.5) in float64, which is exact for it; a float32
// RoundToInt adds 0.5f in float32, and the two differ only for an x of
// magnitude below 1 within an ulp under a half (0.49999997f + 0.5f is 1.0f).
// The class attributes are 24 and more, and contentc's tests compare all 2400
// of their values with an independent float32 computation.
//
// A formula's text is parsed by Parse; Program.Eval runs it against an Env: the
// host functions and the curve table. CharacterEnv is the environment of a
// character of a given level, which is what the stat initializers need.
package aocexpr

// (C-Y-B + C.O.A)

import (
	"errors"
	"fmt"
)

// Pos is a position in the text, 1-based.
type Pos struct{ Line, Col int }

func (p Pos) String() string { return fmt.Sprintf("%d:%d", p.Line, p.Col) }

// Error is a parse or evaluation error. When Unsupported is set the text is
// valid AoCExpression the subset does not cover, and Construct names what it
// does not cover (for example `function "GetStatInitRunningTotal"`).
type Error struct {
	Pos         Pos
	Construct   string
	Unsupported bool
	Msg         string
}

func (e *Error) Error() string {
	if e.Unsupported {
		return fmt.Sprintf("aocexpr: %v: unsupported %s", e.Pos, e.Construct)
	}
	return fmt.Sprintf("aocexpr: %v: %s", e.Pos, e.Msg)
}

// Unsupported returns the construct of an unsupported-construct error, and
// whether err is one.
func Unsupported(err error) (construct string, ok bool) {
	var e *Error
	if errors.As(err, &e) && e.Unsupported {
		return e.Construct, true
	}
	return "", false
}

func unsupported(pos Pos, format string, args ...any) *Error {
	return &Error{Pos: pos, Unsupported: true, Construct: fmt.Sprintf(format, args...)}
}

func syntax(pos Pos, format string, args ...any) *Error {
	return &Error{Pos: pos, Msg: fmt.Sprintf(format, args...)}
}

// Ref is a record reference literal: $#<TypeId>:<Guid>$, the record with Guid in
// the table whose directory entry has TypeId, or $Kind:Name$ (Kind and Name set,
// TypeID and Guid zero), a record named instead of numbered ($Stat:MaxStamina$).
// The host resolves a named one.
type Ref struct {
	TypeID uint64
	Guid   int64
	Kind   string
	Name   string
}

func (r Ref) String() string {
	if r.Kind != "" {
		return "$" + r.Kind + ":" + r.Name + "$"
	}
	return fmt.Sprintf("$#%d:%d$", r.TypeID, r.Guid)
}

// Enum is an enum literal, Type::Value (EEquipSlot::Secondary).
type Enum struct {
	Type, Value string
}

func (e Enum) String() string { return e.Type + "::" + e.Value }

// Object is a handle to a game object (GetOwner(), GetSource()). The host gives
// it meaning; the evaluator only passes it along. Role (hostfn.go) says which actor of a hit an object stands for: the caster
// (0, every object of a stat formula), the target of the hit, the caster's selected target, a pet's owner.
type Object struct {
	Kind string
	Role int
}

// Value is a value of the language: float64, bool, string, Enum, Ref or Object.
type Value = any

// Func is a host function.
type Func func(args []Value) (Value, error)

// Env is what a formula runs against.
type Env struct {
	// Funcs are the host's functions by name (GetOwner, GetStat, IsMoving, ...).
	// The built-ins (Round, Float, Floor, Ceil, SelectFloat, SelectInt, Min,
	// Max, EvalCurve) cannot be replaced.
	Funcs map[string]Func
	// FloatFuncs names the host functions whose result is a float (GetStat:
	// every stat is one), which is what the division rule needs to know: the
	// evaluator cannot tell it from the value, which is a float64 either way.
	// A function not named is not proven to be a float (GetCharacterLevel).
	FloatFuncs map[string]bool
	// Curves resolves the first argument of EvalCurve.
	Curves CurveSource
	// Single evaluates in float32, as a game built on Unreal's float would:
	// every number and every result of + - * / and of EvalCurve is rounded to
	// float32 (Round's argument is such a value; its result is an integer, exact
	// in float32). The default is float64. The two differ only where a
	// value is close to a rounding boundary: for the primary attributes, 7 of
	// the 2400 class_stat_base values (335 * 1.3 at level 20 is 435.5 in float64
	// and 435.49997 in float32). The files do not say which the engine does.
	Single bool
	// LooseBool lets a boolean stand where a number is wanted, as 1 or 0: in + - * / and the comparisons and as the argument of
	// a built-in that takes a number. The design data's ability expressions rely on it (EvalFormula(...) > 0.0 where the formula gives
	// a boolean); the stat formulas do not, and the default keeps them strict.
	LooseBool bool
	// SameActor says whether two objects of different roles are one actor (GetTarget() != GetInstigator()). Nil: they are (one character).
	SameActor func(a, b int) bool
}

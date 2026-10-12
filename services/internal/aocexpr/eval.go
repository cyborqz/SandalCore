// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"errors"
	"fmt"
	"math"
)

// scope holds the variables of a block; a nested block looks outward.
type scope struct {
	parent *scope
	vars   map[string]*variable
}

type variable struct {
	v     Value
	konst bool
	// float: every value the variable has had is provably a float (see isFloat).
	float bool
}

func (s *scope) lookup(name string) *variable {
	for ; s != nil; s = s.parent {
		if v, ok := s.vars[name]; ok {
			return v
		}
	}
	return nil
}

// Eval runs the formula and returns its value.
func (p *Program) Eval(env *Env) (Value, error) {
	if env == nil {
		env = &Env{}
	}
	ev := &evaluator{env: env}
	sc := &scope{vars: map[string]*variable{}}
	if err := ev.stmts(p.stmts, sc); err != nil {
		return nil, err
	}
	return ev.expr(p.result, sc)
}

// EvalFloat runs the formula; its value must be a number.
func (p *Program) EvalFloat(env *Env) (float64, error) {
	v, err := p.Eval(env)
	if err != nil {
		return 0, err
	}
	f, ok := v.(float64)
	if !ok {
		return 0, &Error{Pos: p.result.position(), Msg: fmt.Sprintf("the formula's value is %s, not a number", describeValue(v))}
	}
	return f, nil
}

// EvalFloat parses and runs a formula that gives a number.
func EvalFloat(src string, env *Env) (float64, error) {
	p, err := Parse(src)
	if err != nil {
		return 0, err
	}
	return p.EvalFloat(env)
}

type evaluator struct {
	env *Env
}

// num rounds an arithmetic result to float32 in Single mode.
func (ev *evaluator) num(f float64) float64 {
	if ev.env.Single {
		return float64(float32(f))
	}
	return f
}

// (C-O-A : Cyb)

// loose is a boolean as a number (1 or 0) when the environment allows it (Env.LooseBool), else the value itself.
func (ev *evaluator) loose(v Value) Value {
	if b, ok := v.(bool); ok && ev.env.LooseBool {
		if b {
			return 1.0
		}
		return 0.0
	}
	return v
}

func describeValue(v Value) string {
	switch v := v.(type) {
	case float64:
		return "the number " + fmt.Sprint(v)
	case bool:
		return fmt.Sprintf("the boolean %v", v)
	case Ref:
		return "a record reference"
	case Object:
		return "an object"
	case string:
		return fmt.Sprintf("the string %q", v)
	case Enum:
		return "the enum value " + v.String()
	}
	return fmt.Sprintf("%T", v)
}

func (ev *evaluator) stmts(list []stmt, sc *scope) error {
	for _, s := range list {
		switch s := s.(type) {
		case declStmt:
			v, err := ev.expr(s.x, sc)
			if err != nil {
				return err
			}
			if _, dup := sc.vars[s.name]; dup {
				return syntax(s.pos, "%q is already declared in this block", s.name)
			}
			sc.vars[s.name] = &variable{v: v, konst: s.konst, float: ev.isFloat(s.x, sc)}
		case setStmt:
			target := sc.lookup(s.name)
			if target == nil {
				return syntax(s.pos, "set of undeclared variable %q", s.name)
			}
			if target.konst {
				return syntax(s.pos, "set of const %q", s.name)
			}
			v, err := ev.expr(s.x, sc)
			if err != nil {
				return err
			}
			target.v = v
			target.float = target.float && ev.isFloat(s.x, sc)
		case ifStmt:
			c, err := ev.expr(s.cond, sc)
			if err != nil {
				return err
			}
			b, ok := c.(bool)
			if !ok {
				return syntax(s.cond.position(), "the condition of an if is %s, not a boolean", describeValue(c))
			}
			branch := s.els
			if b {
				branch = s.then
			}
			if err := ev.stmts(branch, &scope{parent: sc, vars: map[string]*variable{}}); err != nil {
				return err
			}
		case exprStmt:
			if _, err := ev.expr(s.x, sc); err != nil {
				return err
			}
		}
	}
	return nil
}

func (ev *evaluator) expr(e expr, sc *scope) (Value, error) {
	switch e := e.(type) {
	case numLit:
		return ev.num(e.v), nil
	case boolLit:
		return e.v, nil
	case refLit:
		return e.ref, nil
	case strLit:
		return e.v, nil
	case enumLit:
		return e.v, nil
	case varRef:
		v := sc.lookup(e.name)
		if v == nil {
			return nil, syntax(e.pos, "unknown variable %q", e.name)
		}
		return v.v, nil
	case unary:
		x, err := ev.expr(e.x, sc)
		if err != nil {
			return nil, err
		}
		switch e.op {
		case "-":
			f, err := number(x, e.pos, "unary -")
			return ev.num(-f), err
		default: // "!"
			b, ok := x.(bool)
			if !ok {
				return nil, syntax(e.pos, "! needs a boolean, got %s", describeValue(x))
			}
			return !b, nil
		}
	case binary:
		return ev.binary(e, sc)
	case call:
		return ev.call(e, sc)
	}
	return nil, syntax(e.position(), "internal error: unknown expression %T", e)
}

// == C | O | A | C | Y | B ==

// isFloat reports whether an expression is provably a float by its text (and
// the variables' history), which is what decides whether `/` can be evaluated:
//
//   - a number with a decimal point (2.0, 1.);
//   - Float(...), SelectFloat(...) and EvalCurve(...);
//   - Min or Max with a float argument;
//   - unary -, and + - * / with a float operand (a float and an integer make a float);
//   - a variable that has only ever been given such a value.
//
// Everything else is not proven: integer literals, Round, SelectInt, comparisons,
// and every host function (the evaluator does not know what type the host's
// numbers have).
func (ev *evaluator) isFloat(e expr, sc *scope) bool {
	switch e := e.(type) {
	case numLit:
		return e.float
	case varRef:
		v := sc.lookup(e.name)
		return v != nil && v.float
	case unary:
		return e.op == "-" && ev.isFloat(e.x, sc)
	case binary:
		switch e.op {
		case "+", "-", "*", "/":
			return ev.isFloat(e.l, sc) || ev.isFloat(e.r, sc)
		case "%":
			return false
		}
	case call:
		switch e.name {
		case "Float", "SelectFloat", "EvalCurve", "Lerp", "RandDoubleRange":
			return true
		case "Min", "Max", "Abs", "Clamp":
			for _, a := range e.args {
				if ev.isFloat(a, sc) {
					return true
				}
			}
		default:
			if _, builtin := builtins[e.name]; !builtin {
				return ev.env.FloatFuncs[e.name]
			}
		}
	}
	return false
}

func number(v Value, pos Pos, what string) (float64, error) {
	f, ok := v.(float64)
	if !ok {
		return 0, syntax(pos, "%s needs a number, got %s", what, describeValue(v))
	}
	return f, nil
}

func (ev *evaluator) binary(e binary, sc *scope) (Value, error) {
	l, err := ev.expr(e.l, sc)
	if err != nil {
		return nil, err
	}
	switch e.op {
	case "&&", "||":
		lb, ok := l.(bool)
		if !ok {
			return nil, syntax(e.pos, "%s needs booleans, got %s", e.op, describeValue(l))
		}
		if e.op == "&&" && !lb {
			return false, nil
		}
		if e.op == "||" && lb {
			return true, nil
		}
		r, err := ev.expr(e.r, sc)
		if err != nil {
			return nil, err
		}
		rb, ok := r.(bool)
		if !ok {
			return nil, syntax(e.pos, "%s needs booleans, got %s", e.op, describeValue(r))
		}
		return rb, nil
	}
	r, err := ev.expr(e.r, sc)
	if err != nil {
		return nil, err
	}
	l, r = ev.loose(l), ev.loose(r)
	if e.op == "==" || e.op == "!=" {
		eq, err := ev.equal(l, r, e.pos)
		return eq == (e.op == "=="), err
	}
	a, err := number(l, e.pos, e.op)
	if err != nil {
		return nil, err
	}
	b, err := number(r, e.pos, e.op)
	if err != nil {
		return nil, err
	}
	switch e.op {
	case "+":
		return ev.num(a + b), nil
	case "-":
		return ev.num(a - b), nil
	case "*":
		return ev.num(a * b), nil
	case "/":
		if !ev.isFloat(e.l, sc) && !ev.isFloat(e.r, sc) {
			return nil, unsupported(e.pos, "division where no operand is proven to be a float (integer division is not modelled; write 2.0 or Float(x))")
		}
		if b == 0 {
			return nil, syntax(e.pos, "division by zero")
		}
		return ev.num(a / b), nil
	case "%":
		// the remainder of whole numbers (a combo count): truncated, as the C++ operator is
		if int64(b) == 0 {
			return nil, syntax(e.pos, "modulo by zero")
		}
		return float64(int64(a) % int64(b)), nil
	case "<":
		return a < b, nil
	case "<=":
		return a <= b, nil
	case ">":
		return a > b, nil
	case ">=":
		return a >= b, nil
	}
	return nil, syntax(e.pos, "internal error: operator %s", e.op)
}

// equal is equal for the values of this evaluation: two objects of different roles are the same actor when the host says so (hostfn.go).
func (ev *evaluator) equal(a, b Value, pos Pos) (bool, error) {
	if x, ok := a.(Object); ok {
		if y, ok := b.(Object); ok && x.Role != y.Role && ev.env.SameActor != nil {
			return ev.env.SameActor(x.Role, y.Role), nil
		}
	}
	return equal(a, b, pos)
}

// C | Y | B with C.O.A

func equal(a, b Value, pos Pos) (bool, error) {
	switch a := a.(type) {
	case float64:
		if b, ok := b.(float64); ok {
			return a == b, nil
		}
	case bool:
		if b, ok := b.(bool); ok {
			return a == b, nil
		}
	case Ref:
		if b, ok := b.(Ref); ok {
			return a == b, nil
		}
	case string:
		if b, ok := b.(string); ok {
			return a == b, nil
		}
	case Enum:
		if b, ok := b.(Enum); ok {
			return a == b, nil
		}
	case Object:
		// The objects of a formula are one character (GetOwner, GetInstigator, GetSource, GetSelf, and an ability's GetTarget): equal.
		if _, ok := b.(Object); ok {
			return true, nil
		}
	}
	return false, syntax(pos, "cannot compare %s with %s", describeValue(a), describeValue(b))
}

func (ev *evaluator) call(e call, sc *scope) (Value, error) {
	// The built-ins evaluate their own arguments (SelectFloat evaluates both
	// branches, as the engine's function does).
	args := make([]Value, len(e.args))
	for i, a := range e.args {
		v, err := ev.expr(a, sc)
		if err != nil {
			return nil, err
		}
		args[i] = v
	}
	if f, ok := builtins[e.name]; ok {
		if e.name != "SelectFloat" && e.name != "SelectInt" {
			for i := range args {
				args[i] = ev.loose(args[i])
			}
		}
		v, err := f(ev, e, args)
		if err != nil {
			return nil, err
		}
		return v, nil
	}
	if f := ev.env.Funcs[e.name]; f != nil {
		v, err := f(args)
		if err != nil {
			return nil, &Error{Pos: e.pos, Msg: fmt.Sprintf("%s: %v", e.name, err)}
		}
		return v, nil
	}
	return nil, unsupported(e.pos, "function %q", e.name)
}

type builtin func(ev *evaluator, e call, args []Value) (Value, error)

var builtins = map[string]builtin{
	"Round": func(_ *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 1)
		if err != nil {
			return nil, err
		}
		return math.Floor(x[0] + 0.5), nil
	},
	"Float": func(_ *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 1)
		if err != nil {
			return nil, err
		}
		return x[0], nil
	},
	"Floor": func(_ *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 1)
		if err != nil {
			return nil, err
		}
		return math.Floor(x[0]), nil
	},
	"Ceil": func(_ *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 1)
		if err != nil {
			return nil, err
		}
		return math.Ceil(x[0]), nil
	},
	"Min": func(_ *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 2)
		if err != nil {
			return nil, err
		}
		return math.Min(x[0], x[1]), nil
	},
	"Max": func(_ *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 2)
		if err != nil {
			return nil, err
		}
		return math.Max(x[0], x[1]), nil
	},
	// Lerp(a, b, t) = a + t * (b - a): the three operations are the engine's (each one rounds in Single mode).
	"Lerp": func(ev *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 3)
		if err != nil {
			return nil, err
		}
		d := ev.num(x[1] - x[0])
		p := ev.num(x[2] * d)
		return ev.num(x[0] + p), nil
	},
	// RandDoubleRange(a, b) = Lerp(a, b, RandDouble()): the host's next random number (an ability expression's; a stat formula has none).
	"RandDoubleRange": func(ev *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 2)
		if err != nil {
			return nil, err
		}
		rnd, ok := ev.env.Funcs["RandDouble"]
		if !ok {
			return nil, fmt.Errorf("RandDoubleRange: this environment has no random source")
		}
		v, err := rnd(nil)
		if err != nil {
			return nil, err
		}
		t, _ := v.(float64)
		d := ev.num(x[1] - x[0])
		p := ev.num(t * d)
		return ev.num(x[0] + p), nil
	},
	// Clamp(x, lo, hi) is Min(Max(x, lo), hi), Unreal's order.
	"Clamp": func(_ *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 3)
		if err != nil {
			return nil, err
		}
		return math.Min(math.Max(x[0], x[1]), x[2]), nil
	},
	"Abs": func(_ *evaluator, e call, args []Value) (Value, error) {
		x, err := numArgs(e, args, 1)
		if err != nil {
			return nil, err
		}
		return math.Abs(x[0]), nil
	},
	"SelectFloat": selectValue,
	"SelectInt":   selectValue,
	"EvalCurve":   evalCurve,
}

// [CoA ~ C_Y_B]

// numArgs checks that a call has n arguments, all numbers.
func numArgs(e call, args []Value, n int) ([]float64, error) {
	if len(args) != n {
		return nil, syntax(e.pos, "%s takes %d argument(s), got %d", e.name, n, len(args))
	}
	out := make([]float64, n)
	for i, a := range args {
		f, ok := a.(float64)
		if !ok {
			return nil, syntax(e.args[i].position(), "%s: argument %d is %s, not a number", e.name, i+1, describeValue(a))
		}
		out[i] = f
	}
	return out, nil
}

// selectValue is SelectFloat(cond, ifTrue, ifFalse) and SelectInt (the same).
func selectValue(_ *evaluator, e call, args []Value) (Value, error) {
	if len(args) != 3 {
		return nil, syntax(e.pos, "%s takes 3 arguments, got %d", e.name, len(args))
	}
	c, ok := args[0].(bool)
	if !ok {
		return nil, syntax(e.args[0].position(), "%s: the condition is %s, not a boolean", e.name, describeValue(args[0]))
	}
	for i := 1; i <= 2; i++ {
		if _, ok := args[i].(float64); !ok {
			return nil, syntax(e.args[i].position(), "%s: argument %d is %s, not a number", e.name, i+1, describeValue(args[i]))
		}
	}
	if c {
		return args[1], nil
	}
	return args[2], nil
}

// evalCurve is EvalCurve($#<TypeId>:<Guid>$, x): the curve's value at x.
func evalCurve(ev *evaluator, e call, args []Value) (Value, error) {
	if len(args) != 2 {
		return nil, syntax(e.pos, "EvalCurve takes 2 arguments, got %d", len(args))
	}
	ref, ok := args[0].(Ref)
	if !ok {
		return nil, syntax(e.args[0].position(), "EvalCurve: the first argument is %s, not a $#<TypeId>:<Guid>$ reference", describeValue(args[0]))
	}
	x, ok := args[1].(float64)
	if !ok {
		return nil, syntax(e.args[1].position(), "EvalCurve: the second argument is %s, not a number", describeValue(args[1]))
	}
	if ev.env.Curves == nil {
		return nil, unsupported(e.pos, "EvalCurve (the environment has no curve source)")
	}
	c, err := ev.env.Curves.Curve(ref)
	if err != nil {
		return nil, &Error{Pos: e.pos, Msg: fmt.Sprintf("EvalCurve %v: %v", ref, err)}
	}
	v, err := c.Eval(x)
	v = ev.num(v)
	if err != nil {
		var ae *Error
		if errors.As(err, &ae) {
			return nil, &Error{Pos: e.pos, Unsupported: ae.Unsupported, Construct: ae.Construct, Msg: fmt.Sprintf("EvalCurve %v: %s", ref, ae.Msg)}
		}
		return nil, &Error{Pos: e.pos, Msg: fmt.Sprintf("EvalCurve %v: %v", ref, err)}
	}
	return v, nil
}

// CharacterEnv is the environment a stat initializer runs in for a character
// of the given level: GetOwner() is the character and GetCharacterLevel of it
// the level. Everything else a formula may call is the caller's to add to
// Funcs; the curves resolve EvalCurve.
func CharacterEnv(level int, curves CurveSource) *Env {
	owner := Object{Kind: "owner"}
	return &Env{
		Curves: curves,
		Funcs: map[string]Func{
			"GetOwner": func(args []Value) (Value, error) {
				if len(args) != 0 {
					return nil, fmt.Errorf("takes no arguments, got %d", len(args))
				}
				return owner, nil
			},
			"GetCharacterLevel": func(args []Value) (Value, error) {
				if len(args) != 1 || args[0] != Value(owner) {
					return nil, fmt.Errorf("takes the owner, got %v", args)
				}
				return float64(level), nil
			},
		},
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"strings"
	"sync"
)

// The syntax tree, exported for the programs that compile a formula instead of
// running it (package statprog). Tree gives a read-only copy of what Parse
// built; the evaluator keeps its own, unexported nodes.

// Expr is an expression node: Num, Bool, RefLit, Str, EnumLit, Var, Unary,
// Binary or Call.
type Expr interface {
	// Pos is where the node starts in the text.
	Pos() Pos
	isExpr()
}

// Stmt is a statement node: Decl, Set, If or ExprStmt.
type Stmt interface {
	Pos() Pos
	isStmt()
}

// -- C O A & Cyb --

type (
	// Num is a number literal. Float is set when the text has a decimal point
	// (2.0, 1.): the engine reads such a number as a float, 100 as an integer.
	Num struct {
		At    Pos
		V     float64
		Float bool
	}
	// Bool is true or false.
	Bool struct {
		At Pos
		V  bool
	}
	// RefLit is a record reference literal.
	RefLit struct {
		At  Pos
		Ref Ref
	}
	// Str is a string literal.
	Str struct {
		At Pos
		V  string
	}
	// EnumLit is an enum literal, Type::Value.
	EnumLit struct {
		At Pos
		V  Enum
	}
	// Var is a use of a variable.
	Var struct {
		At   Pos
		Name string
	}
	// Unary is - or ! applied to X.
	Unary struct {
		At Pos
		Op string
		X  Expr
	}
	// Binary is L Op R: * / + - < <= > >= == != && ||.
	Binary struct {
		At   Pos
		Op   string
		L, R Expr
	}
	// Call is Name(Args).
	Call struct {
		At   Pos
		Name string
		Args []Expr
	}
)

type (
	// Decl is const NAME = X; or var NAME = X;.
	Decl struct {
		At    Pos
		Name  string
		Const bool
		X     Expr
	}
	// Set is set NAME = X;.
	Set struct {
		At   Pos
		Name string
		X    Expr
	}
	// If is if (Cond) { Then } else { Else }; an else-if is an If in Else.
	If struct {
		At   Pos
		Cond Expr
		Then []Stmt
		Else []Stmt
	}
	// ExprStmt is an expression whose value is dropped (EXPR;).
	ExprStmt struct{ X Expr }
)

func (n Num) Pos() Pos      { return n.At }
func (n Bool) Pos() Pos     { return n.At }
func (n RefLit) Pos() Pos   { return n.At }
func (n Str) Pos() Pos      { return n.At }
func (n EnumLit) Pos() Pos  { return n.At }
func (n Var) Pos() Pos      { return n.At }
func (n Unary) Pos() Pos    { return n.At }
func (n Binary) Pos() Pos   { return n.At }
func (n Call) Pos() Pos     { return n.At }
func (n Decl) Pos() Pos     { return n.At }
func (n Set) Pos() Pos      { return n.At }
func (n If) Pos() Pos       { return n.At }
func (n ExprStmt) Pos() Pos { return n.X.Pos() }

// (CoA + C | Y | B)

func (Num) isExpr()      {}
func (Bool) isExpr()     {}
func (RefLit) isExpr()   {}
func (Str) isExpr()      {}
func (EnumLit) isExpr()  {}
func (Var) isExpr()      {}
func (Unary) isExpr()    {}
func (Binary) isExpr()   {}
func (Call) isExpr()     {}
func (Decl) isStmt()     {}
func (Set) isStmt()      {}
func (If) isStmt()       {}
func (ExprStmt) isStmt() {}

// Tree is a parsed formula: statements, then the expression whose value is the
// formula's.
type Tree struct {
	Stmts  []Stmt
	Result Expr
}

// Tree returns the syntax tree of the program.
func (p *Program) Tree() Tree {
	return Tree{Stmts: exportStmts(p.stmts), Result: exportExpr(p.result)}
}

func exportStmts(list []stmt) []Stmt {
	if len(list) == 0 {
		return nil
	}
	out := make([]Stmt, len(list))
	for i, s := range list {
		switch s := s.(type) {
		case declStmt:
			out[i] = Decl{At: s.pos, Name: s.name, Const: s.konst, X: exportExpr(s.x)}
		case setStmt:
			out[i] = Set{At: s.pos, Name: s.name, X: exportExpr(s.x)}
		case ifStmt:
			out[i] = If{At: s.pos, Cond: exportExpr(s.cond), Then: exportStmts(s.then), Else: exportStmts(s.els)}
		case exprStmt:
			out[i] = ExprStmt{X: exportExpr(s.x)}
		}
	}
	return out
}

func exportExpr(e expr) Expr {
	switch e := e.(type) {
	case numLit:
		return Num{At: e.pos, V: e.v, Float: e.float}
	case boolLit:
		return Bool{At: e.pos, V: e.v}
	case refLit:
		return RefLit{At: e.pos, Ref: e.ref}
	case strLit:
		return Str{At: e.pos, V: e.v}
	case enumLit:
		return EnumLit{At: e.pos, V: e.v}
	case varRef:
		return Var{At: e.pos, Name: e.name}
	case unary:
		return Unary{At: e.pos, Op: e.op, X: exportExpr(e.x)}
	case binary:
		return Binary{At: e.pos, Op: e.op, L: exportExpr(e.l), R: exportExpr(e.r)}
	case call:
		args := make([]Expr, len(e.args))
		for i, a := range e.args {
			args[i] = exportExpr(a)
		}
		return Call{At: e.pos, Name: e.name, Args: args}
	}
	panic("aocexpr: unknown expression node")
}

// Builtins are the names of the built-in functions (they are not the host's and
// cannot be replaced).
func Builtins() []string {
	return []string{"Round", "Float", "Floor", "Ceil", "Min", "Max", "Lerp", "RandDoubleRange", "Abs", "Clamp", "SelectFloat", "SelectInt", "EvalCurve"}
}

var (
	canonicalFuncs map[string]string
	canonicalOnce  sync.Once
)

// CanonicalFunc is the name of a function the package knows (a built-in, a stat function, an ability function) spelled as the package
// spells it: the design data writes some as selectInt and Evalcurve, and the engine does not tell the cases apart. Any other name is
// returned as it is.
func CanonicalFunc(name string) string {
	canonicalOnce.Do(func() {
		m := map[string]string{}
		for _, list := range [][]string{Builtins(), StatFuncNames(), AbilityFuncNames()} {
			for _, n := range list {
				m[strings.ToLower(n)] = n
			}
		}
		canonicalFuncs = m
	})
	if c, ok := canonicalFuncs[strings.ToLower(name)]; ok {
		return c
	}
	return name
}

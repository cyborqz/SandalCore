// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import "strings"

// The syntax tree.

type expr interface{ position() Pos }

type (
	numLit struct {
		pos Pos
		v   float64
		// float is set when the text has a decimal point (2.0, 1.): a number the
		// engine reads as a float. 100 is an integer there.
		float bool
	}
	boolLit struct {
		pos Pos
		v   bool
	}
	refLit struct {
		pos Pos
		ref Ref
	}
	strLit struct {
		pos Pos
		v   string
	}
	enumLit struct {
		pos Pos
		v   Enum
	}
	varRef struct {
		pos  Pos
		name string
	}
	unary struct {
		pos Pos
		op  string
		x   expr
	}
	binary struct {
		pos  Pos
		op   string
		l, r expr
	}
	call struct {
		pos  Pos
		name string
		args []expr
	}
)

func (e numLit) position() Pos  { return e.pos }
func (e boolLit) position() Pos { return e.pos }
func (e refLit) position() Pos  { return e.pos }
func (e strLit) position() Pos  { return e.pos }
func (e enumLit) position() Pos { return e.pos }
func (e varRef) position() Pos  { return e.pos }
func (e unary) position() Pos   { return e.pos }
func (e binary) position() Pos  { return e.pos }
func (e call) position() Pos    { return e.pos }

// (C O A : Cyb)

type stmt interface{ stmtPos() Pos }

type (
	declStmt struct {
		pos   Pos
		name  string
		konst bool
		x     expr
	}
	setStmt struct {
		pos  Pos
		name string
		x    expr
	}
	ifStmt struct {
		pos  Pos
		cond expr
		then []stmt
		els  []stmt
	}
	exprStmt struct{ x expr }
)

func (s declStmt) stmtPos() Pos { return s.pos }
func (s setStmt) stmtPos() Pos  { return s.pos }
func (s ifStmt) stmtPos() Pos   { return s.pos }
func (s exprStmt) stmtPos() Pos { return s.x.position() }

// Program is a parsed formula: statements, then the expression whose value is
// the formula's.
type Program struct {
	stmts  []stmt
	result expr
}

// Parse parses the text of a formula. Empty text (a stat initializer with no
// value) is an error: callers check for it.
func Parse(src string) (*Program, error) {
	p := &parser{lex: newLexer(src)}
	if err := p.advance(); err != nil {
		return nil, err
	}
	stmts, res, err := p.block(false)
	if err != nil {
		return nil, err
	}
	if res == nil && len(stmts) > 0 {
		// A final expression may carry its ';'.
		if last, ok := stmts[len(stmts)-1].(exprStmt); ok {
			stmts, res = stmts[:len(stmts)-1], last.x
		}
	}
	if res == nil {
		if len(stmts) == 0 {
			return nil, syntax(p.tok.pos, "empty expression")
		}
		return nil, syntax(p.tok.pos, "the text ends in a statement; it needs a final expression for its value")
	}
	return &Program{stmts: stmts, result: res}, nil
}

// Refs lists the reference literals ($#<TypeId>:<Guid>$) of the text, in the
// order they appear.
func (p *Program) Refs() []Ref {
	var out []Ref
	var walkExpr func(e expr)
	walkExpr = func(e expr) {
		switch e := e.(type) {
		case refLit:
			out = append(out, e.ref)
		case unary:
			walkExpr(e.x)
		case binary:
			walkExpr(e.l)
			walkExpr(e.r)
		case call:
			for _, a := range e.args {
				walkExpr(a)
			}
		}
	}
	var walkStmts func(list []stmt)
	walkStmts = func(list []stmt) {
		for _, s := range list {
			switch s := s.(type) {
			case declStmt:
				walkExpr(s.x)
			case setStmt:
				walkExpr(s.x)
			case ifStmt:
				walkExpr(s.cond)
				walkStmts(s.then)
				walkStmts(s.els)
			case exprStmt:
				walkExpr(s.x)
			}
		}
	}
	walkStmts(p.stmts)
	walkExpr(p.result)
	return out
}

type parser struct {
	lex   *lexer
	tok   token
	depth int
}

// maxNesting bounds how deep a text can nest (parentheses, unary operators, braces, else-if
// chains): the parser and every walk of the tree recurse once per level, and a text is
// not trusted to be shallow.
const maxNesting = 200

func (p *parser) enter() error {
	p.depth++
	if p.depth > maxNesting {
		return syntax(p.tok.pos, "nested more than %d levels deep", maxNesting)
	}
	return nil
}

func (p *parser) leave() { p.depth-- }

// C:Y:B.C | O | A

func (p *parser) advance() error {
	t, err := p.lex.next()
	if err != nil {
		return err
	}
	p.tok = t
	return nil
}

func (p *parser) isPunct(s string) bool { return p.tok.kind == tPunct && p.tok.text == s }

func (p *parser) expect(s string) error {
	if !p.isPunct(s) {
		return syntax(p.tok.pos, "expected %q, found %s", s, p.tok.describe())
	}
	return p.advance()
}

// block parses statements up to the end of the text (braced false) or a closing
// brace (braced true; the brace is consumed). A trailing expression statement
// is returned as the result, not as a statement.
func (p *parser) block(braced bool) (stmts []stmt, result expr, err error) {
	for {
		switch {
		case p.tok.kind == tEOF:
			if braced {
				return nil, nil, syntax(p.tok.pos, "expected \"}\", found the end of the text")
			}
			return stmts, result, nil
		case braced && p.isPunct("}"):
			return stmts, result, p.advance()
		}
		if result != nil {
			return nil, nil, syntax(p.tok.pos, "unexpected %s after the final expression", p.tok.describe())
		}
		s, e, err := p.statement()
		if err != nil {
			return nil, nil, err
		}
		if e != nil {
			result = e
		} else {
			stmts = append(stmts, s)
		}
	}
}

// statement parses one statement. An expression with no ';' after it is the
// block's result and comes back as e; with a ';' it is a statement whose value
// is dropped.
func (p *parser) statement() (s stmt, e expr, err error) {
	at := p.tok.pos
	if p.tok.kind == tIdent {
		switch p.tok.text {
		case "const", "var":
			konst := p.tok.text == "const"
			if err := p.advance(); err != nil {
				return nil, nil, err
			}
			name, x, err := p.assignment()
			if err != nil {
				return nil, nil, err
			}
			return declStmt{pos: at, name: strings.ToLower(name), konst: konst, x: x}, nil, nil
		case "set":
			if err := p.advance(); err != nil {
				return nil, nil, err
			}
			name, x, err := p.assignment()
			if err != nil {
				return nil, nil, err
			}
			return setStmt{pos: at, name: strings.ToLower(name), x: x}, nil, nil
		case "if":
			st, err := p.ifStatement()
			return st, nil, err
		case "return":
			// a final "return x;" is the block's value (the damage equations end so)
			if err := p.advance(); err != nil {
				return nil, nil, err
			}
			x, err := p.expr(0)
			if err != nil {
				return nil, nil, err
			}
			if p.isPunct(";") {
				if err := p.advance(); err != nil {
					return nil, nil, err
				}
			}
			return nil, x, nil
		case "else", "while", "for", "switch":
			return nil, nil, unsupported(at, "statement %q", p.tok.text)
		}
	}
	x, err := p.expr(0)
	if err != nil {
		return nil, nil, err
	}
	if p.isPunct(";") {
		return exprStmt{x: x}, nil, p.advance()
	}
	return nil, x, nil
}

// assignment parses NAME = expr ;
func (p *parser) assignment() (string, expr, error) {
	if p.tok.kind != tIdent {
		return "", nil, syntax(p.tok.pos, "expected a variable name, found %s", p.tok.describe())
	}
	name := p.tok.text
	if err := p.advance(); err != nil {
		return "", nil, err
	}
	if err := p.expect("="); err != nil {
		return "", nil, err
	}
	x, err := p.expr(0)
	if err != nil {
		return "", nil, err
	}
	return name, x, p.expect(";")
}

func (p *parser) ifStatement() (stmt, error) {
	if err := p.enter(); err != nil {
		return nil, err
	}
	defer p.leave()
	at := p.tok.pos
	if err := p.advance(); err != nil { // if
		return nil, err
	}
	if err := p.expect("("); err != nil {
		return nil, err
	}
	cond, err := p.expr(0)
	if err != nil {
		return nil, err
	}
	if err := p.expect(")"); err != nil {
		return nil, err
	}
	then, err := p.braces()
	if err != nil {
		return nil, err
	}
	st := ifStmt{pos: at, cond: cond, then: then}
	if p.tok.kind == tIdent && p.tok.text == "else" {
		if err := p.advance(); err != nil {
			return nil, err
		}
		if p.tok.kind == tIdent && p.tok.text == "if" {
			nested, err := p.ifStatement()
			if err != nil {
				return nil, err
			}
			st.els = []stmt{nested}
		} else if st.els, err = p.braces(); err != nil {
			return nil, err
		}
	}
	return st, nil
}

// built by CoA with C:Y:B

// braces parses { statements }. A branch is statements only: it has no value.
func (p *parser) braces() ([]stmt, error) {
	if err := p.enter(); err != nil {
		return nil, err
	}
	defer p.leave()
	if err := p.expect("{"); err != nil {
		return nil, err
	}
	stmts, res, err := p.block(true)
	if err != nil {
		return nil, err
	}
	if res != nil {
		return nil, syntax(res.position(), "a branch of an if has no value; end the expression with ';'")
	}
	return stmts, nil
}

// Binary operators by binding power, loosest first.
var binaryPower = map[string]int{
	"||": 1,
	"&&": 2,
	"==": 3, "!=": 3,
	"<": 4, "<=": 4, ">": 4, ">=": 4,
	"+": 5, "-": 5,
	"*": 6, "/": 6, "%": 6,
}

// expr parses an expression whose operators all bind tighter than min.
func (p *parser) expr(min int) (expr, error) {
	if err := p.enter(); err != nil {
		return nil, err
	}
	defer p.leave()
	left, err := p.unary()
	if err != nil {
		return nil, err
	}
	for p.tok.kind == tPunct {
		power, ok := binaryPower[p.tok.text]
		if !ok || power <= min {
			break
		}
		op, at := p.tok.text, p.tok.pos
		if err := p.advance(); err != nil {
			return nil, err
		}
		right, err := p.expr(power)
		if err != nil {
			return nil, err
		}
		left = binary{pos: at, op: op, l: left, r: right}
	}
	return left, nil
}

func (p *parser) unary() (expr, error) {
	if err := p.enter(); err != nil {
		return nil, err
	}
	defer p.leave()
	if p.tok.kind == tPunct && (p.tok.text == "-" || p.tok.text == "!") {
		op, at := p.tok.text, p.tok.pos
		if err := p.advance(); err != nil {
			return nil, err
		}
		x, err := p.unary()
		if err != nil {
			return nil, err
		}
		return unary{pos: at, op: op, x: x}, nil
	}
	return p.primary()
}

func (p *parser) primary() (expr, error) {
	t := p.tok
	switch t.kind {
	case tNum:
		return numLit{t.pos, t.num, strings.Contains(t.text, ".")}, p.advance()
	case tRef:
		return refLit{t.pos, t.ref}, p.advance()
	case tString:
		return strLit{t.pos, t.str}, p.advance()
	case tEnum:
		return enumLit{t.pos, t.enum}, p.advance()
	case tIdent:
		if err := p.advance(); err != nil {
			return nil, err
		}
		switch t.text {
		case "true", "false":
			return boolLit{t.pos, t.text == "true"}, nil
		case "const", "var", "set", "if", "else":
			return nil, syntax(t.pos, "%q is a keyword and cannot be used as a value", t.text)
		}
		if !p.isPunct("(") {
			return varRef{t.pos, strings.ToLower(t.text)}, nil // a variable's name is not case sensitive ("activeTarg" and "activetarg")
		}
		if err := p.advance(); err != nil { // (
			return nil, err
		}
		var args []expr
		for !p.isPunct(")") {
			if len(args) > 0 {
				if err := p.expect(","); err != nil {
					return nil, err
				}
			}
			a, err := p.expr(0)
			if err != nil {
				return nil, err
			}
			args = append(args, a)
		}
		return call{t.pos, CanonicalFunc(t.text), args}, p.advance()
	case tPunct:
		if t.text == "(" {
			if err := p.advance(); err != nil {
				return nil, err
			}
			x, err := p.expr(0)
			if err != nil {
				return nil, err
			}
			return x, p.expect(")")
		}
	}
	return nil, syntax(t.pos, "expected a value, found %s", t.describe())
}

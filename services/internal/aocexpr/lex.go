// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"strconv"
	"strings"
)

type tokKind uint8

const (
	tEOF tokKind = iota
	tNum
	tIdent
	tRef
	tPunct  // an operator or punctuation; text holds it
	tString // "text" or 'text'; str holds the characters
	tEnum   // Type::Value; enum holds both parts
)

type token struct {
	kind tokKind
	text string
	num  float64
	ref  Ref
	str  string
	enum Enum
	pos  Pos
}

func (t token) describe() string {
	switch t.kind {
	case tEOF:
		return "the end of the text"
	case tNum:
		return "number " + t.text
	case tRef:
		return "reference " + t.text
	case tString:
		return "string " + t.text
	case tEnum:
		return "enum value " + t.text
	}
	return strconv.Quote(t.text)
}

// lexer turns the text into tokens. CRLF and LF both end a line.
type lexer struct {
	src       string
	i         int
	line, col int
}

func newLexer(src string) *lexer { return &lexer{src: src, line: 1, col: 1} }

func (l *lexer) pos() Pos { return Pos{l.line, l.col} }

func (l *lexer) advance(n int) {
	for ; n > 0 && l.i < len(l.src); n-- {
		if l.src[l.i] == '\n' {
			l.line++
			l.col = 1
		} else {
			l.col++
		}
		l.i++
	}
}

// [C | O | A ~ C-Y-B]

func isDigit(c byte) bool { return c >= '0' && c <= '9' }
func isIdentStart(c byte) bool {
	return c == '_' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z'
}

// puncts are the operator and punctuation tokens the subset has, longest first.
var puncts = []string{
	"&&", "||", "==", "!=", "<=", ">=",
	"+", "-", "*", "/", "%", "<", ">", "!", "=", "(", ")", "{", "}", ",", ";",
}

// next returns the next token, skipping white space and comments.
func (l *lexer) next() (token, error) {
	for l.i < len(l.src) {
		c := l.src[l.i]
		switch {
		case c == ' ' || c == '\t' || c == '\r' || c == '\n':
			l.advance(1)
			continue
		case c == '/' && l.i+1 < len(l.src) && l.src[l.i+1] == '/':
			for l.i < len(l.src) && l.src[l.i] != '\n' {
				l.advance(1)
			}
			continue
		case c == '/' && l.i+1 < len(l.src) && l.src[l.i+1] == '*':
			at := l.pos()
			l.advance(2)
			for {
				if l.i+1 >= len(l.src) {
					return token{}, syntax(at, "unterminated /* comment")
				}
				if l.src[l.i] == '*' && l.src[l.i+1] == '/' {
					l.advance(2)
					break
				}
				l.advance(1)
			}
			continue
		}
		break
	}
	at := l.pos()
	if l.i >= len(l.src) {
		return token{kind: tEOF, pos: at}, nil
	}
	c := l.src[l.i]
	switch {
	case isDigit(c) || c == '.' && l.i+1 < len(l.src) && isDigit(l.src[l.i+1]):
		j := l.i
		for j < len(l.src) && isDigit(l.src[j]) {
			j++
		}
		if j < len(l.src) && l.src[j] == '.' {
			j++
			for j < len(l.src) && isDigit(l.src[j]) {
				j++
			}
		}
		if j < len(l.src) && isIdentStart(l.src[j]) { // 1e5, 0x10, 2f
			return token{}, unsupported(at, "number syntax %q", l.src[l.i:j+1])
		}
		text := l.src[l.i:j]
		v, err := strconv.ParseFloat(text, 64)
		if err != nil {
			return token{}, syntax(at, "bad number %q", text)
		}
		l.advance(j - l.i)
		return token{kind: tNum, text: text, num: v, pos: at}, nil
	case isIdentStart(c):
		j := l.i
		for j < len(l.src) && (isIdentStart(l.src[j]) || isDigit(l.src[j])) {
			j++
		}
		// Type::Value is an enum literal (EEquipSlot::Secondary); with more than
		// one "::" every part but the last belongs to the type.
		last := l.i
		for j+2 < len(l.src) && l.src[j] == ':' && l.src[j+1] == ':' && isIdentStart(l.src[j+2]) {
			last = j + 2
			j += 2
			for j < len(l.src) && (isIdentStart(l.src[j]) || isDigit(l.src[j])) {
				j++
			}
		}
		text := l.src[l.i:j]
		if last != l.i {
			e := Enum{Type: l.src[l.i : last-2], Value: l.src[last:j]}
			l.advance(j - l.i)
			return token{kind: tEnum, text: text, enum: e, pos: at}, nil
		}
		l.advance(j - l.i)
		return token{kind: tIdent, text: text, pos: at}, nil
	case c == '$':
		return l.ref(at)
	case c == '"' || c == '\'':
		return l.str(at)
	}
	for _, p := range puncts {
		if len(l.src)-l.i >= len(p) && l.src[l.i:l.i+len(p)] == p {
			l.advance(len(p))
			return token{kind: tPunct, text: p, pos: at}, nil
		}
	}
	return token{}, unsupported(at, "character %q", string(c))
}

// ref reads $#<TypeId>:<Guid>$.
func (l *lexer) ref(at Pos) (token, error) {
	rest := l.src[l.i:]
	end := -1
	for j := 1; j < len(rest) && j < 256; j++ {
		if rest[j] == '$' {
			end = j
			break
		}
	}
	if end < 0 {
		return token{}, syntax(at, "unterminated $ reference")
	}
	body := rest[1:end]
	if len(body) == 0 {
		return token{}, syntax(at, "empty $ reference")
	}
	if body[0] != '#' {
		// $Kind:Name$, a record named instead of numbered ($Stat:MaxStamina$).
		colon := strings.IndexByte(body, ':')
		if colon <= 0 || colon == len(body)-1 {
			return token{}, syntax(at, "reference %q is not $Kind:Name$ or $#<TypeId>:<Guid>$", rest[:end+1])
		}
		l.advance(end + 1)
		return token{kind: tRef, text: rest[:end+1], ref: Ref{Kind: body[:colon], Name: body[colon+1:]}, pos: at}, nil
	}
	colon := -1
	for j := 1; j < len(body); j++ {
		if body[j] == ':' {
			colon = j
			break
		}
	}
	if colon < 0 {
		return token{}, syntax(at, "reference %q has no ':'", rest[:end+1])
	}
	tid, err1 := strconv.ParseUint(body[1:colon], 10, 64)
	guid, err2 := strconv.ParseInt(body[colon+1:], 10, 64)
	if err1 != nil || err2 != nil {
		return token{}, syntax(at, "bad reference %q", rest[:end+1])
	}
	l.advance(end + 1)
	return token{kind: tRef, text: rest[:end+1], ref: Ref{TypeID: tid, Guid: guid}, pos: at}, nil
}

// CoA captures, C_Y_B code

// str reads a string literal: "text" or 'text' on one line, with the escapes
// backslash-backslash, backslash-quote and backslash-apostrophe (the design
// database's texts have no others).
func (l *lexer) str(at Pos) (token, error) {
	quote := l.src[l.i]
	var b strings.Builder
	j := l.i + 1
	for {
		if j >= len(l.src) || l.src[j] == '\n' || l.src[j] == '\r' {
			return token{}, syntax(at, "unterminated string literal")
		}
		c := l.src[j]
		if c == quote {
			j++
			break
		}
		if c == '\\' {
			if j+1 >= len(l.src) || (l.src[j+1] != '\\' && l.src[j+1] != '"' && l.src[j+1] != '\'') {
				return token{}, unsupported(at, "string escape in %q", l.src[l.i:min(j+2, len(l.src))])
			}
			b.WriteByte(l.src[j+1])
			j += 2
			continue
		}
		b.WriteByte(c)
		j++
	}
	text := l.src[l.i:j]
	l.advance(j - l.i)
	return token{kind: tString, text: text, str: b.String(), pos: at}, nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"strconv"
	"strings"

	"aocrop/services/internal/designdb"
)

// fields reads typed values out of a decoded row (designdb.Struct). The first
// problem (a missing field or one of another kind) sticks in err and every
// later read returns the zero value, so a builder reads all it needs and checks
// once; where names the row for the message.
type fields struct {
	where string
	s     designdb.Struct
	err   error
}

func newFields(where string, s designdb.Struct) *fields { return &fields{where: where, s: s} }

func (f *fields) fail(name string, want string, v designdb.Value, ok bool) {
	if f.err != nil {
		return
	}
	if !ok {
		f.err = fmt.Errorf("%s: no field %s", f.where, name)
		return
	}
	f.err = fmt.Errorf("%s: field %s is %T, want %s", f.where, name, v, want)
}

// ~ CoA :: C:Y:B ~

// get finds a field by name. Property names are FNames, which Unreal compares
// without regard to case, and the two mappings files spell some differently
// (the client build's reflection says StatTypeDef.Base, the old usmap base), so
// an exact match is tried first and a case-insensitive one after it.
func (f *fields) get(name string) (designdb.Value, bool) {
	if v, ok := f.s[name]; ok {
		return v, true
	}
	for k, v := range f.s {
		if strings.EqualFold(k, name) {
			return v, true
		}
	}
	return nil, false
}

// str reads a Name, Str, Enum or AoCExpression.
func (f *fields) str(name string) string {
	v, ok := f.get(name)
	s, isStr := v.(string)
	if !ok || !isStr {
		f.fail(name, "string", v, ok)
	}
	return s
}

func (f *fields) boolean(name string) bool {
	v, ok := f.get(name)
	b, isBool := v.(bool)
	if !ok || !isBool {
		f.fail(name, "bool", v, ok)
	}
	return b
}

// int reads any integer property.
func (f *fields) int(name string) int64 {
	v, ok := f.get(name)
	n, isInt := v.(int64)
	if !ok || !isInt {
		f.fail(name, "int64", v, ok)
	}
	return n
}

// float reads a Float property as the decimal the designer typed: the float32
// 0.001 is stored as 0.001, not as the double nearest the float32 value.
func (f *fields) float(name string) float64 {
	v, ok := f.get(name)
	switch x := v.(type) {
	case float32:
		return decimal32(x)
	case float64:
		return x
	}
	f.fail(name, "float", v, ok)
	return 0
}

// optFloat reads a Float property that a synthetic or an older record may not have: 0 then.
func (f *fields) optFloat(name string) float64 {
	if _, ok := f.get(name); !ok {
		return 0
	}
	return f.float(name)
}

// number reads a numeric property of either kind (an integer or a float) as a float64: the design data types a radius or a time as an int in
// one record and a float in another (plan E1's population records), and the typed table holds a REAL either way.
func (f *fields) number(name string) float64 {
	v, ok := f.get(name)
	switch x := v.(type) {
	case int64:
		return float64(x)
	case float32:
		return decimal32(x)
	case float64:
		return x
	}
	f.fail(name, "a number", v, ok)
	return 0
}

// decimal32 is the float64 nearest the shortest decimal that reads back as x.
func decimal32(x float32) float64 {
	d, err := strconv.ParseFloat(strconv.FormatFloat(float64(x), 'g', -1, 32), 64)
	if err != nil {
		return float64(x)
	}
	return d
}

func (f *fields) list(name string) []designdb.Value {
	v, ok := f.get(name)
	l, isList := v.([]designdb.Value)
	if !ok || !isList {
		f.fail(name, "array", v, ok)
	}
	return l
}

func (f *fields) strukt(name string) designdb.Struct {
	v, ok := f.get(name)
	s, isStruct := v.(designdb.Struct)
	if !ok || !isStruct {
		f.fail(name, "struct", v, ok)
	}
	return s
}

func (f *fields) pairs(name string) designdb.Pairs {
	v, ok := f.get(name)
	p, isPairs := v.(designdb.Pairs)
	if !ok || !isPairs {
		f.fail(name, "map", v, ok)
	}
	return p
}

// [C-O-A :: C.Y.B]

// text reads an FText as its source text, or its key when it has none (a
// string-table entry).
func (f *fields) text(name string) string {
	v, ok := f.get(name)
	t, isText := v.(designdb.Text)
	if !ok || !isText {
		f.fail(name, "text", v, ok)
		return ""
	}
	if t.Source != "" {
		return t.Source
	}
	return t.Key
}

// id reads an Id struct: the record Guid and the name the row carries ("None"
// when it has none).
func (f *fields) id(name string) (guid int64, rname string) {
	return idOf(f, f.where+": "+name, f.strukt(name))
}

// idOf reads Guid and Name out of an Id struct s.
func idOf(f *fields, where string, s designdb.Struct) (int64, string) {
	if f.err != nil {
		return 0, ""
	}
	g, gok := s["Guid"].(int64)
	n, nok := s["Name"].(string)
	if !gok || !nok {
		f.err = fmt.Errorf("%s: not an Id struct (%v)", where, s)
		return 0, ""
	}
	return g, n
}

// ids reads an array of Id structs into their Guids.
func (f *fields) ids(name string) []int64 {
	var out []int64
	for i, v := range f.list(name) {
		s, ok := v.(designdb.Struct)
		if !ok {
			f.fail(name, "array of Id structs", v, true)
			return nil
		}
		g, _ := idOf(f, fmt.Sprintf("%s: %s[%d]", f.where, name, i), s)
		out = append(out, g)
	}
	return out
}

// keyedIds reads a map of int keys to Id structs (an ability's EffectsIds and
// HitsIds) into the keys and Guids, in file order.
func (f *fields) keyedIds(name string) (keys []int64, guids []int64) {
	for i, p := range f.pairs(name) {
		k, ok := p.Key.(int64)
		s, sok := p.Value.(designdb.Struct)
		if !ok || !sok {
			f.fail(name, "map of int to Id struct", p, true)
			return nil, nil
		}
		g, _ := idOf(f, fmt.Sprintf("%s: %s[%d]", f.where, name, i), s)
		keys = append(keys, k)
		guids = append(guids, g)
	}
	return keys, guids
}

// tags reads tag names from an array of {TagName} structs or a
// GameplayTagContainer (an array of names); never nil.
func (f *fields) tags(name string) []string {
	out := []string{}
	for _, v := range f.list(name) {
		switch t := v.(type) {
		case string:
			out = append(out, t)
		case designdb.Struct:
			n, ok := t["TagName"].(string)
			if !ok {
				f.fail(name, "tag struct with TagName", v, true)
				return nil
			}
			out = append(out, n)
		default:
			f.fail(name, "array of tags", v, true)
			return nil
		}
	}
	return out
}

// b2i is a bool as SQLite stores it.
func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

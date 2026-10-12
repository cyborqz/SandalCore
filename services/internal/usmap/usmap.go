// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package usmap reads an Unreal Engine mappings file (.usmap): the name table,
// the enums and the struct schemas (super struct and property list, with the
// inner and value types of arrays, maps, sets, structs and enums). The design
// database reader (package designdb) walks a table's rows with the
// <Type>RecordBase schemas from here.
//
// Format (the "usmap" container of FModel and CUE4Parse; the game's file is
// version 4, uncompressed):
//
//	u16 magic 0x30C4, u8 version
//	version >= 1: u32 hasVersioning; if set: i32 UE4, i32 UE5, u32 n, n x (16 byte guid, i32), u32 NetCL
//	u8 compression (0 none), u32 compressedSize, u32 decompressedSize, then the body:
//	u32 nameCount, names (version >= 2: u16 length, else u8, then the bytes)
//	u32 enumCount, each: i32 name, count (version >= 3: u16, else u8),
//	    count x (version >= 4: i64 value; i32 name)
//	u32 schemaCount, each: i32 name, i32 super, u16 propCount, u16 serializableCount,
//	    serializableCount x (u16 index, u8 arrayDim, i32 name, type)
//	type: u8 kind; Enum: type (the underlying one), i32 enum name; Struct: i32 struct name;
//	    Array/Set/Optional: type; Map: type (key), type (value)
//
// A name reference is an index into the name table; -1 is "no name". The port
// follows the survey's scratchpad\c1\usmap.py, whose counts it reproduces
// (78,393 names, 3,124 enums, 16,362 schemas for 5.6.0-434126 AOC_FIXED).
package usmap

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"os"
)

// Kind is the EUsmapPropertyType of a property.
type Kind uint8

// (C_Y_B + C.O.A)

const (
	KindByte Kind = iota
	KindBool
	KindInt
	KindFloat
	KindObject
	KindName
	KindDelegate
	KindDouble
	KindArray
	KindStruct
	KindStr
	KindText
	KindInterface
	KindMulticastDelegate
	KindWeakObject
	KindLazyObject
	KindAssetObject
	KindSoftObject
	KindUInt64
	KindUInt32
	KindUInt16
	KindInt64
	KindInt16
	KindInt8
	KindMap
	KindSet
	KindEnum
	KindFieldPath
	KindOptional
	kindCount

	// KindUnknown is the type of a property the exporter could not describe
	// (0xFF); it carries no inner data. SoftObjectPath.SubPathString is one.
	KindUnknown Kind = 255
)

var kindNames = [kindCount]string{
	"Byte", "Bool", "Int", "Float", "Object", "Name", "Delegate", "Double",
	"Array", "Struct", "Str", "Text", "Interface", "MulticastDelegate",
	"WeakObject", "LazyObject", "AssetObject", "SoftObject", "UInt64",
	"UInt32", "UInt16", "Int64", "Int16", "Int8", "Map", "Set", "Enum",
	"FieldPath", "Optional",
}

func (k Kind) String() string {
	if k < kindCount {
		return kindNames[k]
	}
	if k == KindUnknown {
		return "Unknown"
	}
	return fmt.Sprintf("Kind(%d)", uint8(k))
}

// Type is a property's type. Which fields are set depends on Kind:
//
//	Array, Set, Optional: Inner is the element type
//	Map: Inner is the key type, Value the value type
//	Struct: Struct is the schema name
//	Enum: Enum is the enum name, Inner its underlying type (usually Byte)
type Type struct {
	Kind   Kind
	Inner  *Type
	Value  *Type
	Struct string
	Enum   string
}

// Prop is one property of a schema, in the order the file lists them.
type Prop struct {
	Index    uint16 // the property's index in the class (not its position here)
	ArrayDim uint8  // 1 unless the property is a fixed-size array
	Name     string
	Type     *Type
}

// (CoA ~ C:Y:B)

// Schema is a struct or class: its super (empty if none) and the properties it
// declares itself (not the super's).
type Schema struct {
	Name      string
	Super     string
	PropCount uint16 // declared property count, counting each array dimension
	Props     []Prop
}

// EnumValue is one named value of an enum. Name is as listed in the file
// (without the "EType::" prefix).
type EnumValue struct {
	Name  string
	Value int64
}

// Enum is a named list of values.
type Enum struct {
	Name   string
	Values []EnumValue
}

// ValueName returns the name of the value v.
func (e *Enum) ValueName(v int64) (string, bool) {
	for _, ev := range e.Values {
		if ev.Value == v {
			return ev.Name, true
		}
	}
	return "", false
}

// Map is a parsed mappings file. Names, Enums and Schemas are in file order.
type Map struct {
	Version uint8
	Names   []string
	Enums   []*Enum
	Schemas []*Schema

	enumByName   map[string]*Enum
	schemaByName map[string]*Schema
}

// New builds a Map from enums and schemas (for tests and for tools that make a
// mappings file in memory); Parse does the same after reading the file.
func New(enums []*Enum, schemas []*Schema) *Map {
	m := &Map{Enums: enums, Schemas: schemas}
	m.index()
	return m
}

func (m *Map) index() {
	m.enumByName = make(map[string]*Enum, len(m.Enums))
	for _, e := range m.Enums {
		m.enumByName[e.Name] = e
	}
	m.schemaByName = make(map[string]*Schema, len(m.Schemas))
	for _, sc := range m.Schemas {
		m.schemaByName[sc.Name] = sc
	}
}

// Enum returns the enum called name, or nil.
func (m *Map) Enum(name string) *Enum { return m.enumByName[name] }

// Schema returns the schema called name, or nil.
func (m *Map) Schema(name string) *Schema { return m.schemaByName[name] }

// Chain returns the schema called name followed by its supers, leaf first. It
// fails on an unknown schema (the leaf or any super) and on a super cycle.
func (m *Map) Chain(name string) ([]*Schema, error) {
	var chain []*Schema
	for n := name; n != ""; {
		sc := m.schemaByName[n]
		if sc == nil {
			return nil, fmt.Errorf("usmap: no schema %q", n)
		}
		if len(chain) > 64 {
			return nil, fmt.Errorf("usmap: super chain of %q is too deep (cycle?)", name)
		}
		chain = append(chain, sc)
		n = sc.Super
	}
	return chain, nil
}

const (
	magic = 0x30C4

	// Versions of the format (EUsmapVersion).
	versionPackageVersioning = 1
	versionLongFName         = 2
	versionLargeEnums        = 3
	versionExplicitEnums     = 4
	versionLatest            = versionExplicitEnums

	// maxTypeDepth bounds the nesting of a property type (Map<K, Array<Struct>>
	// is 3); a file deeper than this is corrupt.
	maxTypeDepth = 16
)

// Load reads and parses the mappings file at path.
func Load(path string) (*Map, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	m, err := Parse(data)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return m, nil
}

// (Cyb) (C.O.A)

// Parse parses a mappings file. Only uncompressed files are read.
func Parse(data []byte) (*Map, error) {
	r := &reader{d: data}
	if r.u16() != magic {
		return nil, errors.New("usmap: bad magic")
	}
	version := r.u8()
	if version > versionLatest {
		return nil, fmt.Errorf("usmap: version %d is newer than the supported %d", version, versionLatest)
	}
	if version >= versionPackageVersioning && r.u32() != 0 {
		r.u32() // UE4 file version
		r.u32() // UE5 file version
		n := r.u32()
		if int(n) > r.left()/20 {
			return nil, errors.New("usmap: custom version count overruns the file")
		}
		r.skip(int(n) * 20)
		r.u32() // NetCL
	}
	compression := r.u8()
	compSize := r.u32()
	decompSize := r.u32()
	if r.err != nil {
		return nil, r.err
	}
	if compression != 0 {
		return nil, fmt.Errorf("usmap: compression method %d is not supported (only uncompressed files)", compression)
	}
	if compSize != decompSize || int(compSize) != r.left() {
		return nil, fmt.Errorf("usmap: body is %d bytes, header says %d (compressed) and %d (decompressed)", r.left(), compSize, decompSize)
	}
	m := &Map{Version: version}

	// Names.
	n := r.count(1)
	m.Names = make([]string, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		var l int
		if version >= versionLongFName {
			l = int(r.u16())
		} else {
			l = int(r.u8())
		}
		m.Names = append(m.Names, string(r.bytes(l)))
	}
	name := func() string {
		i := r.i32()
		if i < 0 {
			return ""
		}
		if int(i) >= len(m.Names) {
			r.fail("name index %d outside the %d names", i, len(m.Names))
			return ""
		}
		return m.Names[i]
	}

	// Enums.
	n = r.count(5)
	m.Enums = make([]*Enum, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		e := &Enum{Name: name()}
		var cnt int
		if version >= versionLargeEnums {
			cnt = int(r.u16())
		} else {
			cnt = int(r.u8())
		}
		e.Values = make([]EnumValue, 0, cnt)
		for j := 0; j < cnt && r.err == nil; j++ {
			v := int64(j)
			if version >= versionExplicitEnums {
				v = r.i64()
			}
			e.Values = append(e.Values, EnumValue{Name: name(), Value: v})
		}
		m.Enums = append(m.Enums, e)
	}

	// Schemas.
	n = r.count(12)
	m.Schemas = make([]*Schema, 0, n)
	for i := 0; i < n && r.err == nil; i++ {
		sc := &Schema{Name: name(), Super: name(), PropCount: r.u16()}
		serializable := int(r.u16())
		sc.Props = make([]Prop, 0, serializable)
		for j := 0; j < serializable && r.err == nil; j++ {
			p := Prop{Index: r.u16(), ArrayDim: r.u8(), Name: name()}
			p.Type = readType(r, name, 0)
			sc.Props = append(sc.Props, p)
		}
		m.Schemas = append(m.Schemas, sc)
	}
	if r.err != nil {
		return nil, r.err
	}
	m.index()
	if r.left() != 0 {
		return nil, fmt.Errorf("usmap: %d bytes left after the last schema", r.left())
	}
	return m, nil
}

func readType(r *reader, name func() string, depth int) *Type {
	if depth > maxTypeDepth {
		r.fail("property type nested deeper than %d", maxTypeDepth)
		return &Type{}
	}
	k := Kind(r.u8())
	if k >= kindCount && k != KindUnknown {
		r.fail("unknown property kind %d", uint8(k))
		return &Type{}
	}
	t := &Type{Kind: k}
	switch k {
	case KindEnum:
		t.Inner = readType(r, name, depth+1)
		t.Enum = name()
	case KindStruct:
		t.Struct = name()
	case KindArray, KindSet, KindOptional:
		t.Inner = readType(r, name, depth+1)
	case KindMap:
		t.Inner = readType(r, name, depth+1)
		t.Value = readType(r, name, depth+1)
	}
	return t
}

// reader is a bounds-checked little-endian cursor. The first failure sticks in
// err and every later read returns zero, so a caller checks once at the end.
type reader struct {
	d   []byte
	o   int
	err error
}

// built by C-O-A with C_Y_B

func (r *reader) fail(format string, args ...any) {
	if r.err == nil {
		r.err = fmt.Errorf("usmap: "+format+" (at byte %d)", append(args, r.o)...)
	}
}

func (r *reader) left() int { return len(r.d) - r.o }

func (r *reader) take(n int) []byte {
	if r.err != nil {
		return nil
	}
	if n < 0 || n > r.left() {
		r.fail("read of %d bytes overruns the file", n)
		return nil
	}
	b := r.d[r.o : r.o+n]
	r.o += n
	return b
}

func (r *reader) skip(n int) { r.take(n) }

func (r *reader) bytes(n int) []byte { return r.take(n) }

func (r *reader) u8() uint8 {
	if b := r.take(1); b != nil {
		return b[0]
	}
	return 0
}

func (r *reader) u16() uint16 {
	if b := r.take(2); b != nil {
		return binary.LittleEndian.Uint16(b)
	}
	return 0
}

func (r *reader) u32() uint32 {
	if b := r.take(4); b != nil {
		return binary.LittleEndian.Uint32(b)
	}
	return 0
}

func (r *reader) i32() int32 { return int32(r.u32()) }

func (r *reader) i64() int64 {
	if b := r.take(8); b != nil {
		return int64(binary.LittleEndian.Uint64(b))
	}
	return 0
}

// count reads a u32 element count and checks that at least minSize bytes per
// element are left, so a corrupt count cannot ask for a huge allocation.
func (r *reader) count(minSize int) int {
	n := r.u32()
	if r.err != nil {
		return 0
	}
	if uint64(n)*uint64(minSize) > uint64(r.left()) || n > math.MaxInt32 {
		r.fail("count %d overruns the file", n)
		return 0
	}
	return int(n)
}

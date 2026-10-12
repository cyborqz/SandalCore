// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package usmap

import (
	"bytes"
	"encoding/binary"
	"os"
	"strings"
	"testing"
)

// builder writes a usmap file: names are collected on first use, and the body
// is appended in the order the format lists it.
type builder struct {
	version uint8
	names   []string
	index   map[string]int
	enums   bytes.Buffer
	nenums  uint32
	schemas bytes.Buffer
	nsch    uint32
}

func newBuilder(version uint8) *builder {
	return &builder{version: version, index: map[string]int{}}
}

func (b *builder) name(s string) int32 {
	if s == "" {
		return -1
	}
	i, ok := b.index[s]
	if !ok {
		i = len(b.names)
		b.names = append(b.names, s)
		b.index[s] = i
	}
	return int32(i)
}

func put(buf *bytes.Buffer, v any) { _ = binary.Write(buf, binary.LittleEndian, v) }

// enum adds an enum; values are (name, value) pairs flattened.
func (b *builder) enum(name string, values ...any) {
	b.nenums++
	put(&b.enums, b.name(name))
	n := len(values) / 2
	if b.version >= versionLargeEnums {
		put(&b.enums, uint16(n))
	} else {
		put(&b.enums, uint8(n))
	}
	for i := 0; i < n; i++ {
		if b.version >= versionExplicitEnums {
			put(&b.enums, values[2*i+1].(int64))
		}
		put(&b.enums, b.name(values[2*i].(string)))
	}
}

// [C-Y-B ~ C.O.A]

// ty is a property type in the builder's notation.
type ty struct {
	kind  Kind
	inner *ty
	value *ty
	strct string
	enum  string
}

func (b *builder) writeType(buf *bytes.Buffer, t ty) {
	buf.WriteByte(byte(t.kind))
	switch t.kind {
	case KindEnum:
		b.writeType(buf, *t.inner)
		put(buf, b.name(t.enum))
	case KindStruct:
		put(buf, b.name(t.strct))
	case KindArray, KindSet, KindOptional:
		b.writeType(buf, *t.inner)
	case KindMap:
		b.writeType(buf, *t.inner)
		b.writeType(buf, *t.value)
	}
}

type pdef struct {
	name string
	t    ty
}

func (b *builder) schema(name, super string, props ...pdef) {
	b.nsch++
	put(&b.schemas, b.name(name))
	put(&b.schemas, b.name(super))
	put(&b.schemas, uint16(len(props)))
	put(&b.schemas, uint16(len(props)))
	for i, p := range props {
		put(&b.schemas, uint16(i))
		b.schemas.WriteByte(1)
		put(&b.schemas, b.name(p.name))
		b.writeType(&b.schemas, p.t)
	}
}

func (b *builder) bytes() []byte {
	var body bytes.Buffer
	put(&body, uint32(len(b.names)))
	for _, n := range b.names {
		if b.version >= versionLongFName {
			put(&body, uint16(len(n)))
		} else {
			put(&body, uint8(len(n)))
		}
		body.WriteString(n)
	}
	put(&body, b.nenums)
	body.Write(b.enums.Bytes())
	put(&body, b.nsch)
	body.Write(b.schemas.Bytes())

	var out bytes.Buffer
	put(&out, uint16(magic))
	out.WriteByte(b.version)
	if b.version >= versionPackageVersioning {
		put(&out, uint32(0))
	}
	out.WriteByte(0) // no compression
	put(&out, uint32(body.Len()))
	put(&out, uint32(body.Len()))
	out.Write(body.Bytes())
	return out.Bytes()
}

var (
	tByte  = ty{kind: KindByte}
	tFloat = ty{kind: KindFloat}
	tInt64 = ty{kind: KindInt64}
)

func tStruct(s string) ty { return ty{kind: KindStruct, strct: s} }
func tArray(in ty) ty     { return ty{kind: KindArray, inner: &in} }
func tEnum(e string) ty   { return ty{kind: KindEnum, inner: &tByte, enum: e} }

// sample builds a small usmap with every type shape: an enum, a base and a
// derived struct, an array, a map, a set and an enum property.
func sample(version uint8) []byte {
	b := newBuilder(version)
	b.enum("EColor", "Red", int64(0), "Green", int64(1), "Blue", int64(5))
	b.schema("Base", "",
		pdef{"Guid", tInt64},
		pdef{"Name", ty{kind: KindName}})
	b.schema("Cost", "",
		pdef{"Amount", tFloat},
		pdef{"Stat", tStruct("Base")})
	mapT := ty{kind: KindMap, inner: &ty{kind: KindName}, value: &ty{kind: KindArray, inner: &tFloat}}
	setT := ty{kind: KindSet, inner: &tInt64}
	b.schema("Leaf", "Base",
		pdef{"Costs", tArray(tStruct("Cost"))},
		pdef{"Color", tEnum("EColor")},
		pdef{"Weights", mapT},
		pdef{"Ids", setT})
	return b.bytes()
}

// Cyb + C.O.A

func TestParseSynthetic(t *testing.T) {
	m, err := Parse(sample(versionLatest))
	if err != nil {
		t.Fatal(err)
	}
	if m.Version != versionLatest {
		t.Errorf("version = %d", m.Version)
	}
	if got, want := len(m.Enums), 1; got != want {
		t.Fatalf("enums = %d, want %d", got, want)
	}
	if got, want := len(m.Schemas), 3; got != want {
		t.Fatalf("schemas = %d, want %d", got, want)
	}
	e := m.Enum("EColor")
	if e == nil {
		t.Fatal("no EColor")
	}
	for _, c := range []struct {
		v    int64
		name string
		ok   bool
	}{{0, "Red", true}, {1, "Green", true}, {5, "Blue", true}, {2, "", false}} {
		name, ok := e.ValueName(c.v)
		if name != c.name || ok != c.ok {
			t.Errorf("ValueName(%d) = %q, %v; want %q, %v", c.v, name, ok, c.name, c.ok)
		}
	}

	leaf := m.Schema("Leaf")
	if leaf == nil || leaf.Super != "Base" || leaf.PropCount != 4 || len(leaf.Props) != 4 {
		t.Fatalf("Leaf = %+v", leaf)
	}
	costs := leaf.Props[0]
	if costs.Name != "Costs" || costs.ArrayDim != 1 || costs.Type.Kind != KindArray ||
		costs.Type.Inner.Kind != KindStruct || costs.Type.Inner.Struct != "Cost" {
		t.Errorf("Costs = %+v / %+v", costs, costs.Type)
	}
	color := leaf.Props[1].Type
	if color.Kind != KindEnum || color.Enum != "EColor" || color.Inner.Kind != KindByte {
		t.Errorf("Color = %+v", color)
	}
	weights := leaf.Props[2].Type
	if weights.Kind != KindMap || weights.Inner.Kind != KindName ||
		weights.Value.Kind != KindArray || weights.Value.Inner.Kind != KindFloat {
		t.Errorf("Weights = %+v", weights)
	}
	ids := leaf.Props[3].Type
	if ids.Kind != KindSet || ids.Inner.Kind != KindInt64 {
		t.Errorf("Ids = %+v", ids)
	}
	if m.Schema("Nope") != nil || m.Enum("Nope") != nil {
		t.Error("unknown names must give nil")
	}
}

// A property of unknown type (0xFF) has no inner data and does not derail the
// properties after it.
func TestParseUnknownKind(t *testing.T) {
	b := newBuilder(versionLatest)
	b.schema("S", "", pdef{"A", ty{kind: KindUnknown}}, pdef{"B", tFloat})
	m, err := Parse(b.bytes())
	if err != nil {
		t.Fatal(err)
	}
	s := m.Schema("S")
	if len(s.Props) != 2 || s.Props[0].Type.Kind != KindUnknown || s.Props[1].Type.Kind != KindFloat {
		t.Errorf("S = %+v", s.Props)
	}
}

func TestChain(t *testing.T) {
	m, err := Parse(sample(versionLatest))
	if err != nil {
		t.Fatal(err)
	}
	chain, err := m.Chain("Leaf")
	if err != nil {
		t.Fatal(err)
	}
	var names []string
	for _, sc := range chain {
		names = append(names, sc.Name)
	}
	if got := strings.Join(names, ","); got != "Leaf,Base" {
		t.Errorf("chain = %s, want Leaf,Base", got)
	}
	if _, err := m.Chain("Nope"); err == nil {
		t.Error("Chain of an unknown schema must fail")
	}

	// A super cycle must not loop forever.
	b := newBuilder(versionLatest)
	b.schema("A", "B")
	b.schema("B", "A")
	cyc, err := Parse(b.bytes())
	if err != nil {
		t.Fatal(err)
	}
	if _, err := cyc.Chain("A"); err == nil {
		t.Error("a super cycle must fail")
	}
}

// Older versions (no explicit enum values, u8 name lengths and enum counts)
// are read too; enum values are then the positions.
func TestParseOlderVersions(t *testing.T) {
	b := newBuilder(1)
	b.enum("EColor", "Red", int64(0), "Green", int64(0), "Blue", int64(0))
	b.schema("S", "", pdef{"X", tFloat})
	m, err := Parse(b.bytes())
	if err != nil {
		t.Fatal(err)
	}
	if name, ok := m.Enum("EColor").ValueName(2); !ok || name != "Blue" {
		t.Errorf("value 2 = %q, %v; want Blue", name, ok)
	}
	if m.Schema("S") == nil || m.Schema("S").Props[0].Type.Kind != KindFloat {
		t.Errorf("S = %+v", m.Schema("S"))
	}
}

func TestParseVersioningBlock(t *testing.T) {
	// hasVersioning set: the UE4/UE5 versions, one custom version and the
	// NetCL sit between the version byte and the compression byte.
	good := sample(versionLatest)
	var out bytes.Buffer
	out.Write(good[:3])
	put(&out, uint32(1)) // hasVersioning
	put(&out, uint32(522))
	put(&out, uint32(1017))
	put(&out, uint32(1))
	out.Write(make([]byte, 16)) // custom version guid
	put(&out, uint32(7))        // custom version number
	put(&out, uint32(12345))    // NetCL
	out.Write(good[7:])         // from the compression byte on
	m, err := Parse(out.Bytes())
	if err != nil {
		t.Fatal(err)
	}
	if len(m.Schemas) != 3 {
		t.Errorf("schemas = %d, want 3", len(m.Schemas))
	}
}

// [C-O-A x Cyb]

func TestParseErrors(t *testing.T) {
	good := sample(versionLatest)
	flip := func(i int, v byte) []byte {
		c := append([]byte(nil), good...)
		c[i] = v
		return c
	}
	cases := []struct {
		name string
		data []byte
		want string
	}{
		{"empty", nil, "magic"},
		{"bad magic", flip(0, 0x00), "magic"},
		{"newer version", flip(2, versionLatest+1), "newer"},
		{"compressed", flip(7, 1), "compression"},
		{"truncated", good[:len(good)-5], "body is"},
		{"trailing bytes", append(append([]byte(nil), good...), 0, 0), "body is"},
	}
	for _, c := range cases {
		_, err := Parse(c.data)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err = %v, want it to contain %q", c.name, err, c.want)
		}
	}

	// A property type of a kind the format does not have.
	b := newBuilder(versionLatest)
	b.schema("S", "", pdef{"X", ty{kind: Kind(200)}})
	if _, err := Parse(b.bytes()); err == nil || !strings.Contains(err.Error(), "unknown property kind") {
		t.Errorf("bad kind: err = %v", err)
	}

	// A name reference outside the table: the schema's own name index, which
	// sits 15 bytes into the body (names 4+3, enum count 4, schema count 4),
	// after the 16-byte header.
	b2 := newBuilder(versionLatest)
	b2.schema("S", "")
	data := b2.bytes()
	binary.LittleEndian.PutUint32(data[16+15:], 99)
	if _, err := Parse(data); err == nil || !strings.Contains(err.Error(), "name index") {
		t.Errorf("bad name index: err = %v", err)
	}
}

// Parsing must not allocate by an attacker-sized count: a table that claims
// four billion names in a few bytes fails instead.
func TestParseHugeCount(t *testing.T) {
	var body bytes.Buffer
	put(&body, uint32(0xFFFFFFFF))
	var out bytes.Buffer
	put(&out, uint16(magic))
	out.WriteByte(versionLatest)
	put(&out, uint32(0))
	out.WriteByte(0)
	put(&out, uint32(body.Len()))
	put(&out, uint32(body.Len()))
	out.Write(body.Bytes())
	if _, err := Parse(out.Bytes()); err == nil {
		t.Error("a huge name count must fail")
	}
}

func TestKindString(t *testing.T) {
	if KindUnknown.String() != "Unknown" {
		t.Errorf("KindUnknown = %s", KindUnknown)
	}
	if KindMap.String() != "Map" || KindOptional.String() != "Optional" || Kind(99).String() != "Kind(99)" {
		t.Errorf("Kind strings: %s %s %s", KindMap, KindOptional, Kind(99))
	}
}

// TestRealUsmap reads the mappings file AOC_USMAP points at: the game's old
// one (5.6.0-434126 AOC_FIXED, whose counts the survey found) or the one
// tools/ghidra/usmapgen.py writes from the client executable, which has
// AbilityHitRecordBase.bMayCorrupt and no Blueprint ("_C") classes. It checks
// the counts of whichever it is, and a struct whose layout the survey read off
// the file.
func TestRealUsmap(t *testing.T) {
	path := os.Getenv("AOC_USMAP")
	if path == "" {
		t.Skip("AOC_USMAP not set")
	}
	if _, err := os.Stat(path); err != nil {
		t.Skipf("AOC_USMAP: %v", err)
	}
	m, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if m.Version != 4 {
		t.Errorf("version = %d, want 4", m.Version)
	}
	generated := false
	for _, p := range m.Schema("AbilityHitRecordBase").Props {
		generated = generated || p.Name == "bMayCorrupt"
	}
	names, enums, schemas := 78393, 3124, 16362
	if generated {
		names, enums, schemas = 72456, 3122, 15419
	}
	t.Logf("%s: %d names, %d enums, %d schemas (generated from the client: %v)", path, len(m.Names), len(m.Enums), len(m.Schemas), generated)
	if len(m.Names) != names || len(m.Enums) != enums || len(m.Schemas) != schemas {
		t.Errorf("%d names, %d enums, %d schemas; want %d, %d, %d", len(m.Names), len(m.Enums), len(m.Schemas), names, enums, schemas)
	}
	// No duplicate names: the lookups see every schema and enum.
	if len(m.schemaByName) != len(m.Schemas) || len(m.enumByName) != len(m.Enums) {
		t.Errorf("duplicate names: %d schemas / %d enums by name", len(m.schemaByName), len(m.enumByName))
	}
	blueprints := 0
	for _, sc := range m.Schemas {
		if strings.HasSuffix(sc.Name, "_C") {
			blueprints++
		}
	}
	if generated && blueprints != 0 || !generated && blueprints != 908 {
		t.Errorf("%d Blueprint classes, generated %v", blueprints, generated)
	}
	// Every super is a schema of the file.
	for _, sc := range m.Schemas {
		if sc.Super != "" && m.Schema(sc.Super) == nil {
			t.Errorf("%s: super %s is not in the file", sc.Name, sc.Super)
		}
	}
	// A class chain through the intrinsic classes the generator lists by hand.
	if chain, err := m.Chain("BlueprintGeneratedClass"); err != nil || len(chain) != 5 || chain[4].Name != "Object" {
		t.Errorf("BlueprintGeneratedClass chain = %v, %v", chain, err)
	}

	// AbilityCost: Enabled, StatCosts[AbilityStatCost], EffectCosts[AbilityEffectCost],
	// ChargeCost, bTriggersCooldown.
	ac := m.Schema("AbilityCost")
	if ac == nil {
		t.Fatal("no AbilityCost")
	}
	type want struct {
		name string
		kind Kind
		elem string // struct name, of the property or of its array element
	}
	for i, w := range []want{
		{"Enabled", KindStruct, "AoCExpression"},
		{"StatCosts", KindArray, "AbilityStatCost"},
		{"EffectCosts", KindArray, "AbilityEffectCost"},
		{"ChargeCost", KindStruct, "AoCExpression"},
		{"bTriggersCooldown", KindBool, ""},
	} {
		if i >= len(ac.Props) {
			t.Fatalf("AbilityCost has %d props, want at least %d", len(ac.Props), i+1)
		}
		p := ac.Props[i]
		elem := p.Type.Struct
		if p.Type.Kind == KindArray {
			elem = p.Type.Inner.Struct
		}
		if p.Name != w.name || p.Type.Kind != w.kind || elem != w.elem {
			t.Errorf("AbilityCost.Props[%d] = %s %s %q, want %s %s %q", i, p.Name, p.Type.Kind, elem, w.name, w.kind, w.elem)
		}
	}
	if len(ac.Props) != 5 || ac.Super != "" {
		t.Errorf("AbilityCost: %d props, super %q; want 5, none", len(ac.Props), ac.Super)
	}
	// AbilityStatCost: Stat (a StatTypeDefId) and Value (an expression).
	asc := m.Schema("AbilityStatCost")
	if asc == nil || len(asc.Props) < 2 || asc.Props[0].Type.Struct != "StatTypeDefId" {
		t.Errorf("AbilityStatCost = %+v", asc)
	}

	// A record schema: leaf first, the design-data tail last.
	chain, err := m.Chain("StatTypeDefRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(chain) != 2 || chain[1].Name != "DesignDataRecordBase" {
		t.Fatalf("StatTypeDefRecordBase chain = %v", chain)
	}
	var tail []string
	for _, p := range chain[1].Props {
		tail = append(tail, p.Name)
	}
	if got, want := strings.Join(tail, ","), "Guid,ParentGuid,Name,bDevOnly,RecordFilename,bIsDirty"; got != want {
		t.Errorf("DesignDataRecordBase props = %s, want %s", got, want)
	}

	// An enum with explicit values.
	if v, ok := m.Enum("ERichCurveInterpMode").ValueName(2); !ok || v != "RCIM_Cubic" {
		t.Errorf("ERichCurveInterpMode[2] = %q, %v", v, ok)
	}
}

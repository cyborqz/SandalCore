// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package designdb

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"math"
	"reflect"
	"strings"
	"testing"

	"aocrop/services/internal/usmap"
)

// ---- a synthetic design database -------------------------------------------

// pool builds the string pool; offsets are relative to the pool's start.
type pool struct {
	buf  bytes.Buffer
	refs map[string]uint32
}

func newPool() *pool {
	p := &pool{refs: map[string]uint32{}}
	p.ref("") // the real pool starts with a few common strings
	p.ref("None")
	return p
}

func (p *pool) ref(s string) uint32 {
	if r, ok := p.refs[s]; ok {
		return r
	}
	r := uint32(p.buf.Len())
	_ = binary.Write(&p.buf, binary.LittleEndian, uint32(len(s)+1))
	p.buf.WriteString(s)
	p.buf.WriteByte(0)
	p.refs[s] = r
	return r
}

// -- C:Y:B x C O A --

// rowWriter writes a table's rows; row() pads to the next 8-aligned file
// offset first, as the format does.
type rowWriter struct {
	base int // file offset of the table's first byte
	buf  bytes.Buffer
	pool *pool
}

func (w *rowWriter) row() { w.rowAligned(8) }

// rowAligned pads to the next file offset that is a multiple of align.
func (w *rowWriter) rowAligned(align int) {
	for (w.base+w.buf.Len())%align != 0 {
		w.buf.WriteByte(0)
	}
}
func (w *rowWriter) u8(v uint8)    { w.buf.WriteByte(v) }
func (w *rowWriter) bool(v bool)   { w.buf.WriteByte(map[bool]byte{false: 0, true: 1}[v]) }
func (w *rowWriter) u16(v uint16)  { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) u32(v uint32)  { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) i32(v int32)   { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) i64(v int64)   { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) u64(v uint64)  { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) f32(v float32) { w.u32(math.Float32bits(v)) }
func (w *rowWriter) f64(v float64) { w.u64(math.Float64bits(v)) }
func (w *rowWriter) str(s string)  { w.u32(w.pool.ref(s)) } // a pool reference
func (w *rowWriter) fstr(s string) { // an inline FString
	if s == "" {
		w.i32(0)
		return
	}
	w.i32(int32(len(s) + 1))
	w.buf.WriteString(s)
	w.buf.WriteByte(0)
}

// tail writes the DesignDataRecordBase properties that end every row, and the
// 4 zero bytes a row with a parent ends with.
func (w *rowWriter) tail(guid, parent int64, name string, devOnly bool) {
	w.i64(guid)
	w.i64(parent)
	w.str(name)
	w.bool(devOnly)
	if parent != 0 {
		w.u32(0)
	}
}

type tableSpec struct {
	typeID uint64
	rows   uint32
	write  func(w *rowWriter)
}

// buildDBC assembles a database file from table specs.
func buildDBC(p *pool, specs ...tableSpec) []byte {
	dataStart := headerSize + dirEntrySize*len(specs)
	var tables [][]byte
	off := dataStart
	var dir bytes.Buffer
	for _, s := range specs {
		w := &rowWriter{base: off, pool: p}
		s.write(w)
		data := w.buf.Bytes()
		_ = binary.Write(&dir, binary.LittleEndian, s.typeID)
		_ = binary.Write(&dir, binary.LittleEndian, uint64(off))
		_ = binary.Write(&dir, binary.LittleEndian, uint64(len(data)))
		_ = binary.Write(&dir, binary.LittleEndian, s.rows)
		_ = binary.Write(&dir, binary.LittleEndian, crc32.ChecksumIEEE(data))
		tables = append(tables, data)
		off += len(data)
	}
	poolOff := (off + 7) &^ 7
	head := make([]byte, headerSize)
	copy(head, magic)
	binary.LittleEndian.PutUint64(head[0x08:], 1768873989)
	binary.LittleEndian.PutUint32(head[0x14:], uint32(len(specs)))
	binary.LittleEndian.PutUint64(head[0x28:], uint64(poolOff))
	binary.LittleEndian.PutUint32(head[0x30:], uint32(p.buf.Len()))
	binary.LittleEndian.PutUint32(head[0x34:], crc32.ChecksumIEEE(p.buf.Bytes()))
	binary.LittleEndian.PutUint32(head[0x38:], uint32(dataStart))

	out := append(head, dir.Bytes()...)
	for _, t := range tables {
		out = append(out, t...)
	}
	for len(out) < poolOff {
		out = append(out, 0)
	}
	out = append(out, p.buf.Bytes()...)
	binary.LittleEndian.PutUint32(out[0x04:], crc32.ChecksumIEEE(out[8:dataStart]))
	return out
}

// ~ Cyb - C.O.A ~

// ---- a synthetic mappings file ---------------------------------------------

func prop(name string, k usmap.Kind) usmap.Prop {
	return usmap.Prop{ArrayDim: 1, Name: name, Type: &usmap.Type{Kind: k}}
}

func structProp(name, st string) usmap.Prop {
	return usmap.Prop{ArrayDim: 1, Name: name, Type: &usmap.Type{Kind: usmap.KindStruct, Struct: st}}
}

func arrayOf(name string, in *usmap.Type) usmap.Prop {
	return usmap.Prop{ArrayDim: 1, Name: name, Type: &usmap.Type{Kind: usmap.KindArray, Inner: in}}
}

func tInt() *usmap.Type             { return &usmap.Type{Kind: usmap.KindInt} }
func tStructT(n string) *usmap.Type { return &usmap.Type{Kind: usmap.KindStruct, Struct: n} }

func enumProp(name, enum string) usmap.Prop {
	return usmap.Prop{ArrayDim: 1, Name: name, Type: &usmap.Type{Kind: usmap.KindEnum, Enum: enum, Inner: &usmap.Type{Kind: usmap.KindByte}}}
}

func schema(name, super string, props ...usmap.Prop) *usmap.Schema {
	return &usmap.Schema{Name: name, Super: super, PropCount: uint16(len(props)), Props: props}
}

// testMap describes records of the shapes the tests use.
func testMap() *usmap.Map {
	return usmap.New(
		[]*usmap.Enum{
			{Name: "ERichCurveInterpMode", Values: []usmap.EnumValue{{Name: "RCIM_Linear", Value: 0}, {Name: "RCIM_Constant", Value: 1}, {Name: "RCIM_Cubic", Value: 2}}},
		},
		[]*usmap.Schema{
			schema("DesignDataRecordBase", "",
				prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName),
				prop("bDevOnly", usmap.KindBool), prop("RecordFilename", usmap.KindStr), prop("bIsDirty", usmap.KindBool)),
			schema("DesignDataObjectId", "",
				prop("Guid", usmap.KindInt64), prop("TypeId", usmap.KindInt64), prop("Name", usmap.KindName)),
			schema("WidgetId", "DesignDataObjectId"),
			schema("AoCExpression", "", prop("Expression", usmap.KindStr)),
			schema("SoftObjectPath", "", structProp("AssetPath", "TopLevelAssetPath"), prop("SubPathString", usmap.KindUnknown)),
			schema("GameplayTag", "", prop("TagName", usmap.KindName)),
			schema("GameplayTagContainer", "", arrayOf("GameplayTags", tStructT("GameplayTag")), arrayOf("ParentTags", tStructT("GameplayTag"))),
			schema("RichCurveKey", "", enumProp("InterpMode", "ERichCurveInterpMode"), prop("Time", usmap.KindFloat)),
			schema("Sub", "", prop("A", usmap.KindInt), arrayOf("Keys", tStructT("RichCurveKey"))),

			// Shapes that fit one table each: Widget and Gadget are one Int
			// (so a table of either fits both), Holder carries a WidgetId.
			schema("WidgetRecordBase", "DesignDataRecordBase", prop("Count", usmap.KindInt)),
			schema("GadgetRecordBase", "DesignDataRecordBase", prop("Level", usmap.KindInt)),
			schema("HolderRecordBase", "DesignDataRecordBase", structProp("Target", "WidgetId")),

			// Every value kind.
			schema("KitchenRecordBase", "DesignDataRecordBase",
				prop("B", usmap.KindBool), prop("By", usmap.KindByte), prop("I8", usmap.KindInt8),
				prop("I16", usmap.KindInt16), prop("U16", usmap.KindUInt16), prop("I", usmap.KindInt),
				prop("U32", usmap.KindUInt32), prop("L", usmap.KindInt64), prop("U64", usmap.KindUInt64),
				prop("F", usmap.KindFloat), prop("D", usmap.KindDouble), prop("N", usmap.KindName),
				prop("S", usmap.KindStr), enumProp("E", "EColor"), prop("Tx", usmap.KindText),
				arrayOf("Arr", tInt()),
				usmap.Prop{ArrayDim: 1, Name: "M", Type: &usmap.Type{Kind: usmap.KindMap, Inner: &usmap.Type{Kind: usmap.KindName}, Value: &usmap.Type{Kind: usmap.KindFloat}}},
				usmap.Prop{ArrayDim: 1, Name: "Set", Type: &usmap.Type{Kind: usmap.KindSet, Inner: tInt()}},
				structProp("Sub", "Sub"), structProp("Ref", "WidgetId"), structProp("Expr", "AoCExpression"),
				structProp("Soft", "SoftObjectPath"), structProp("Tags", "GameplayTagContainer"),
				prop("Obj", usmap.KindObject), prop("SoftObj", usmap.KindSoftObject)),
			schema("NotARecord", "", prop("X", usmap.KindInt)),
		})
}

// ---- tests -----------------------------------------------------------------

// widgetTable writes n Widget rows (Count = row index + 1).
func widgetTable(typeID uint64, n int) tableSpec {
	return tableSpec{typeID, uint32(n), func(w *rowWriter) {
		for i := 0; i < n; i++ {
			w.row()
			w.i32(int32(i + 1))
			w.tail(int64(1000+i), 0, "w"+string(rune('a'+i)), false)
		}
	}}
}

// C.O.A / C-Y-B

func TestParseHeaderAndDirectory(t *testing.T) {
	p := newPool()
	data := buildDBC(p, widgetTable(111, 2), widgetTable(222, 3))
	db, err := Parse(data)
	if err != nil {
		t.Fatal(err)
	}
	h := db.Header()
	if h.Tables != 2 || h.Generated != 1768873989 || h.DataStart != headerSize+2*dirEntrySize ||
		h.PoolSize != uint32(p.buf.Len()) {
		t.Errorf("header = %+v", h)
	}
	if got := h.GeneratedAt().Format("2006-01-02 15:04:05"); got != "2026-01-20 01:53:09" {
		t.Errorf("GeneratedAt = %s", got)
	}
	tabs := db.Tables()
	if len(tabs) != 2 || tabs[0].TypeID != 111 || tabs[0].Rows != 2 || tabs[1].TypeID != 222 || tabs[1].Rows != 3 {
		t.Fatalf("tables = %+v", tabs)
	}
	if tabs[0].Offset != uint64(h.DataStart) || tabs[1].Offset != tabs[0].End() {
		t.Errorf("tables are not one run: %+v", tabs)
	}
	if ti, ok := db.TableByTypeID(222); !ok || ti.Index != 1 {
		t.Errorf("TableByTypeID(222) = %+v, %v", ti, ok)
	}
	if _, ok := db.TableByTypeID(999); ok {
		t.Error("TableByTypeID(999) found a table")
	}
	if _, ok := db.Table(2); ok {
		t.Error("Table(2) found a table")
	}
	if db.Size() != len(data) || len(db.TableBytes(1)) != int(tabs[1].Size) {
		t.Error("Size / TableBytes")
	}
	if err := db.Verify(); err != nil {
		t.Errorf("Verify: %v", err)
	}
}

func TestPoolStrings(t *testing.T) {
	p := newPool()
	hello := p.ref("héllo")
	bad := p.ref("x")
	db, err := Parse(buildDBC(p, widgetTable(1, 1)))
	if err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		ref  uint32
		want string
		ok   bool
	}{
		{0, "", true}, {p.ref("None"), "None", true}, {hello, "héllo", true},
		{0xFFFFFFFF, "", true}, {hello + 1, "", false}, {bad + 100000, "", false},
	} {
		got, ok := db.poolString(c.ref)
		if got != c.want || ok != c.ok {
			t.Errorf("poolString(%#x) = %q, %v; want %q, %v", c.ref, got, ok, c.want, c.ok)
		}
	}
}

func TestParseErrors(t *testing.T) {
	good := func() []byte { return buildDBC(newPool(), widgetTable(1, 2), widgetTable(2, 1)) }
	le := binary.LittleEndian
	cases := []struct {
		name   string
		mutate func(b []byte) []byte
		want   string
	}{
		{"short file", func(b []byte) []byte { return b[:10] }, "bad magic"},
		{"bad magic", func(b []byte) []byte { b[0] = 'X'; return b }, "bad magic"},
		{"table count too big", func(b []byte) []byte { le.PutUint32(b[0x14:], 100000); return b }, "do not fit"},
		{"data start", func(b []byte) []byte { le.PutUint32(b[0x38:], 0x100); return b }, "data starts at"},
		{"pool not at the end", func(b []byte) []byte { return append(b, 0, 0, 0) }, "does not end"},
		{"table gap", func(b []byte) []byte { le.PutUint64(b[headerSize+dirEntrySize+8:], 0x1000); return b }, "not the next run"},
		{"table past the pool", func(b []byte) []byte { le.PutUint64(b[headerSize+16:], 1<<30); return b }, "not the next run"},
		{"pool string overruns", func(b []byte) []byte {
			poolOff := le.Uint64(b[0x28:])
			le.PutUint32(b[poolOff:], 1<<30)
			return b
		}, "claims"},
		{"pool offset and size that wrap to the file length", func(b []byte) []byte {
			// off + size is 2^64 + len(b), which wraps to len(b); the last table is
			// stretched to end 5 bytes before off, so no other check notices.
			size := uint32(len(b)) + 8
			off := ^uint64(0) - 7
			le.PutUint64(b[0x28:], off)
			le.PutUint32(b[0x30:], size)
			last := headerSize + dirEntrySize
			le.PutUint64(b[last+16:], off-5-le.Uint64(b[last+8:]))
			return b
		}, "does not end"},
		{"pool string not terminated", func(b []byte) []byte {
			poolOff := le.Uint64(b[0x28:])
			b[poolOff+4] = 'x' // the first string is "" (length 1, just the NUL)
			return b
		}, "NUL"},
	}
	for _, c := range cases {
		_, err := Parse(c.mutate(good()))
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err = %v, want it to contain %q", c.name, err, c.want)
		}
	}

	// Verify finds a flipped byte in each part.
	for _, c := range []struct {
		name string
		at   func(db *DB) int
		want string
	}{
		{"header", func(db *DB) int { return 0x20 }, "header crc"},
		{"table", func(db *DB) int { return int(db.tables[1].Offset) + 2 }, "table 1 crc"},
		{"pool", func(db *DB) int { return int(db.hdr.PoolOffset) + 10 }, "pool crc"},
	} {
		data := good()
		db, err := Parse(data)
		if err != nil {
			t.Fatal(err)
		}
		data[c.at(db)] ^= 0x40
		db2, err := Parse(data)
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if err := db2.Verify(); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("Verify after corrupting the %s: %v, want %q", c.name, err, c.want)
		}
	}
}

// Cyb : C-O-A

// TestDecodeRejectsAnImpossibleRowCount: a directory entry whose row count
// cannot fit in the table's bytes (a row takes at least 21: Guid, ParentGuid,
// Name, bDevOnly) is refused when the file is opened, before a decoder sizes
// anything by it (0xFFFFFFFF rows made Decode ask for 400 GB).
func TestDecodeRejectsAnImpossibleRowCount(t *testing.T) {
	for _, rows := range []uint32{3, 1 << 20, 0xFFFFFFFF} {
		data := buildDBC(newPool(), widgetTable(1, 2)) // 2 rows, 2*32 bytes
		binary.LittleEndian.PutUint32(data[headerSize+24:], rows)
		db, err := Parse(data)
		if err == nil || !strings.Contains(err.Error(), "rows") {
			t.Errorf("%d rows in a table of 2: Parse = %v, want an error about the rows", rows, err)
			if err == nil {
				_, _ = db.Decoder(testMap()).Decode(0, "WidgetRecordBase")
			}
		}
	}
	// The rows that do fit are fine, and a table of no rows may have no bytes.
	data := buildDBC(newPool(), widgetTable(1, 2), tableSpec{2, 0, func(w *rowWriter) {}})
	if _, err := Parse(data); err != nil {
		t.Errorf("Parse = %v", err)
	}
}

func TestFindInt64(t *testing.T) {
	db, err := Parse(buildDBC(newPool(), widgetTable(1, 3), widgetTable(2, 1)))
	if err != nil {
		t.Fatal(err)
	}
	// Row i of table 0 has Guid 1000+i, 4 bytes into its 28-byte row.
	hits := db.FindInt64(0, 1001)
	if len(hits) != 1 {
		t.Fatalf("hits = %v", hits)
	}
	if got := db.data[hits[0] : hits[0]+8]; binary.LittleEndian.Uint64(got) != 1001 {
		t.Errorf("hit %#x does not hold 1001", hits[0])
	}
	if hits := db.FindInt64(1, 1001); len(hits) != 0 {
		t.Errorf("table 1 has no 1001 row, hits = %v", hits)
	}
	if hits := db.FindInt64(0, 77777); len(hits) != 0 {
		t.Errorf("hits = %v", hits)
	}
}

// kitchenTable is one row holding every value kind.
func kitchenTable(typeID uint64) tableSpec {
	return tableSpec{typeID, 1, func(w *rowWriter) {
		w.row()
		w.bool(true)  // B
		w.u8(200)     // By
		w.u8(0xFE)    // I8 = -2
		w.u16(0xFFFF) // I16 = -1
		w.u16(65535)  // U16
		w.i32(-7)     // I
		w.u32(4000000000)
		w.i64(-1 << 40)
		w.u64(math.MaxUint64)
		w.f32(1.5)
		w.f64(-2.25)
		w.str("a name")
		w.str("a string")
		w.str("EColor::Green") // an enum as a pool reference
		// Tx: base history, namespace, key and source inline.
		w.u32(0)
		w.u8(0)
		w.fstr("ns")
		w.fstr("key")
		w.fstr("Source text")
		// Arr
		w.u32(3)
		w.i32(10)
		w.i32(20)
		w.i32(30)
		// M: removed-count word, then count, then pairs.
		w.u32(0)
		w.u32(2)
		w.str("x")
		w.f32(1)
		w.str("y")
		w.f32(2)
		// Set: also an removed-count word.
		w.u32(0)
		w.u32(2)
		w.i32(5)
		w.i32(6)
		// Sub: A, Keys (byte enum, then time)
		w.i32(9)
		w.u32(2)
		w.u8(2)
		w.f32(0.5)
		w.u8(0)
		w.f32(1.5)
		// Ref: a WidgetId (Guid, TypeId, Name)
		w.i64(42)
		w.i64(777)
		w.str("None")
		// Expr, Soft
		w.str("1 + 2")
		w.str("/Game/Foo.Foo")
		// Tags: only GameplayTags is stored
		w.u32(2)
		w.str("tag.a")
		w.str("tag.b")
		// Obj, SoftObj
		w.str("/Game/Obj")
		w.str("/Game/Soft")
		w.tail(5000, 6000, "kitchen", true)
	}}
}

// (Cyb | C O A)

func TestDecodeEveryValueKind(t *testing.T) {
	db, err := Parse(buildDBC(newPool(), kitchenTable(9)))
	if err != nil {
		t.Fatal(err)
	}
	rows, err := db.Decoder(testMap()).Decode(0, "KitchenRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 1 {
		t.Fatalf("rows = %d", len(rows))
	}
	r := rows[0]
	if r.Guid != 5000 || r.ParentGuid != 6000 || r.Name != "kitchen" || !r.DevOnly {
		t.Errorf("row tail = %+v", r)
	}
	if r.Offset%8 != 0 {
		t.Errorf("row offset %#x is not 8-aligned", r.Offset)
	}
	for _, name := range []string{"Guid", "ParentGuid", "Name", "bDevOnly", "RecordFilename", "bIsDirty"} {
		if _, in := r.Fields[name]; in {
			t.Errorf("Fields holds %s", name)
		}
	}
	want := Struct{
		"B": true, "By": int64(200), "I8": int64(-2), "I16": int64(-1), "U16": int64(65535), "I": int64(-7),
		"U32": int64(4000000000), "L": int64(-1 << 40), "U64": uint64(math.MaxUint64),
		"F": float32(1.5), "D": float64(-2.25), "N": "a name", "S": "a string", "E": "EColor::Green",
		"Tx":  Text{Namespace: "ns", Key: "key", Source: "Source text"},
		"Arr": []Value{int64(10), int64(20), int64(30)},
		"M":   Pairs{{"x", float32(1)}, {"y", float32(2)}},
		"Set": []Value{int64(5), int64(6)},
		"Sub": Struct{"A": int64(9), "Keys": []Value{
			Struct{"InterpMode": "ERichCurveInterpMode::RCIM_Cubic", "Time": float32(0.5)},
			Struct{"InterpMode": "ERichCurveInterpMode::RCIM_Linear", "Time": float32(1.5)},
		}},
		"Ref":     Struct{"Guid": int64(42), "TypeId": int64(777), "Name": "None"},
		"Expr":    "1 + 2",
		"Soft":    "/Game/Foo.Foo",
		"Tags":    []Value{"tag.a", "tag.b"},
		"Obj":     "/Game/Obj",
		"SoftObj": "/Game/Soft",
	}
	if !reflect.DeepEqual(r.Fields, want) {
		for k, v := range want {
			if !reflect.DeepEqual(r.Fields[k], v) {
				t.Errorf("%s = %#v, want %#v", k, r.Fields[k], v)
			}
		}
	}
}

func TestDecodeTextHistories(t *testing.T) {
	// One Text property per record, one record per history type.
	m := usmap.New(nil, []*usmap.Schema{
		schema("DesignDataRecordBase", "",
			prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName),
			prop("bDevOnly", usmap.KindBool), prop("RecordFilename", usmap.KindStr), prop("bIsDirty", usmap.KindBool)),
		schema("TextRecordBase", "DesignDataRecordBase", prop("T", usmap.KindText)),
	})
	cases := []struct {
		name  string
		write func(w *rowWriter)
		want  Text
	}{
		{"none, no string", func(w *rowWriter) { w.u32(0); w.u8(0xFF); w.u32(0) }, Text{History: -1}},
		{"none, culture invariant", func(w *rowWriter) { w.u32(0); w.u8(0xFF); w.u32(1); w.fstr("hi") }, Text{History: -1, Source: "hi"}},
		{"string table entry", func(w *rowWriter) { w.u32(0); w.u8(11); w.str("tbl"); w.fstr("k") }, Text{History: 11, Table: "tbl", Key: "k"}},
		{"base, empty strings", func(w *rowWriter) { w.u32(0x20); w.u8(0); w.fstr(""); w.fstr("k"); w.fstr("") }, Text{Key: "k"}},
	}
	for _, c := range cases {
		spec := tableSpec{1, 1, func(w *rowWriter) {
			w.row()
			c.write(w)
			w.tail(1, 0, "t", false)
		}}
		db, err := Parse(buildDBC(newPool(), spec))
		if err != nil {
			t.Fatal(err)
		}
		rows, err := db.Decoder(m).Decode(0, "TextRecordBase")
		if err != nil {
			t.Errorf("%s: %v", c.name, err)
			continue
		}
		if got := rows[0].Fields["T"]; got != c.want {
			t.Errorf("%s: %#v, want %#v", c.name, got, c.want)
		}
	}
	// An FText history the format does not define here is an error, not a guess.
	spec := tableSpec{1, 1, func(w *rowWriter) { w.row(); w.u32(0); w.u8(7); w.tail(1, 0, "t", false) }}
	db, _ := Parse(buildDBC(newPool(), spec))
	if _, err := db.Decoder(m).Decode(0, "TextRecordBase"); err == nil || !strings.Contains(err.Error(), "history type 7") {
		t.Errorf("history 7: err = %v", err)
	}
}

func TestRowAlignmentAndSelfCheck(t *testing.T) {
	m := testMap()
	// Widget rows are 4 + 8 + 8 + 4 + 1 = 25 bytes, so each next row skips pad bytes;
	// the directory is 0x3C + 32 bytes, so the first row is not at an aligned
	// offset either.
	p := newPool()
	db, err := Parse(buildDBC(p, widgetTable(1, 4)))
	if err != nil {
		t.Fatal(err)
	}
	d := db.Decoder(m)
	rows, err := d.Decode(0, "WidgetRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 4 {
		t.Fatalf("rows = %d", len(rows))
	}
	for i, r := range rows {
		if r.Offset%8 != 0 {
			t.Errorf("row %d at %#x is not 8-aligned", i, r.Offset)
		}
		if r.Fields["Count"] != int64(i+1) || r.Guid != int64(1000+i) {
			t.Errorf("row %d = %+v", i, r)
		}
		if i > 0 && r.Offset-rows[i-1].Offset != 32 {
			t.Errorf("row %d starts %d bytes after the last", i, r.Offset-rows[i-1].Offset)
		}
	}
	if err := d.Check(0, "WidgetRecordBase"); err != nil {
		t.Errorf("Check: %v", err)
	}

	// Rows that end before the table does (the directory says 4 bytes more).
	short := buildDBC(newPool(), tableSpec{1, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i32(1)
			w.tail(1, 0, "a", false)
		}
		w.u32(0xdeadbeef)
	}})
	db, err = Parse(short)
	if err != nil {
		t.Fatal(err)
	}
	rows, err = db.Decoder(m).Decode(0, "WidgetRecordBase")
	var de *DecodeError
	if rows != nil || !asDecodeError(err, &de) {
		t.Fatalf("Decode = %v, %v; want no rows and a *DecodeError", rows, err)
	}
	if de.Row != 2 || de.Table != 0 || de.Schema != "WidgetRecordBase" || !strings.Contains(de.Msg, "4 bytes short") {
		t.Errorf("error = %+v", de)
	}
	if got := db.Tables()[0].End(); de.Offset != int64(got)-4 {
		t.Errorf("error offset %#x, want %#x", de.Offset, got-4)
	}
	// DecodePartial still hands over the two rows that read, with the error.
	part, err := db.Decoder(m).DecodePartial(0, "WidgetRecordBase")
	if err == nil || len(part) != 2 {
		t.Errorf("DecodePartial = %d rows, %v", len(part), err)
	}

	// The directory says 3 rows but only 2 are there, and 8 bytes after them (a
	// count the table's size allows: Parse refuses one it does not): the third
	// read runs past the table.
	over := buildDBC(newPool(), tableSpec{1, 3, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i32(1)
			w.tail(1, 0, "a", false)
		}
		w.u64(0)
	}})
	db, _ = Parse(over)
	_, err = db.Decoder(m).Decode(0, "WidgetRecordBase")
	if !asDecodeError(err, &de) || de.Row != 2 || !strings.Contains(de.Msg, "past the end") {
		t.Errorf("over-count: %v", err)
	}

	// Rows that go past the table's last byte: the last row is cut short.
	cut := buildDBC(newPool(), tableSpec{1, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i32(1)
			w.tail(1, 0, "a", false)
		}
		w.buf.Truncate(w.buf.Len() - 3)
	}})
	db, _ = Parse(cut)
	_, err = db.Decoder(m).Decode(0, "WidgetRecordBase")
	if !asDecodeError(err, &de) || de.Row != 1 {
		t.Errorf("cut row: %v", err)
	}
}

func asDecodeError(err error, target **DecodeError) bool {
	de, ok := err.(*DecodeError)
	if ok {
		*target = de
	}
	return ok
}

// (C O A) (C-Y-B)

func TestDecodeErrorsNameTheProperty(t *testing.T) {
	m := testMap()
	// A bad pool reference in the second tag of a tag container.
	spec := tableSpec{9, 1, func(w *rowWriter) {
		w.row()
		w.bool(false)
		w.u8(0)
		w.u8(0)
		w.u16(0)
		w.u16(0)
		w.i32(0)
		w.u32(0)
		w.i64(0)
		w.u64(0)
		w.f32(0)
		w.f64(0)
		w.str("")
		w.str("")
		w.str("")
		w.u32(0) // Tx: none, empty
		w.u8(0xFF)
		w.u32(0)
		w.u32(0) // Arr
		w.u32(0) // M removed
		w.u32(0) // M count
		w.u32(0) // Set removed
		w.u32(0) // Set count
		w.i32(0) // Sub.A
		w.u32(0) // Sub.Keys
		w.i64(0) // Ref
		w.i64(0)
		w.str("")
		w.str("") // Expr
		w.str("") // Soft
		w.u32(2)  // Tags: two tags, the second a reference into the middle of a string
		w.str("ok")
		w.u32(3)
		w.str("")
		w.str("")
		w.tail(1, 0, "x", false)
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	_, err = db.Decoder(m).Decode(0, "KitchenRecordBase")
	var de *DecodeError
	if !asDecodeError(err, &de) {
		t.Fatalf("err = %v", err)
	}
	if de.Path != "Tags[1]" || !strings.Contains(de.Msg, "does not point at a string") || de.Row != 0 {
		t.Errorf("error = %+v", de)
	}
	if !strings.Contains(err.Error(), "Tags[1]") {
		t.Errorf("Error() = %s", err)
	}
}

func TestStrictValues(t *testing.T) {
	m := testMap()
	// bool must be 0 or 1.
	spec := tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.i32(1)
		w.i64(1)
		w.i64(0)
		w.str("a")
		w.u8(2) // bDevOnly
	}}
	db, _ := Parse(buildDBC(newPool(), spec))
	if _, err := db.Decoder(m).Decode(0, "WidgetRecordBase"); err == nil || !strings.Contains(err.Error(), "bool byte 2") {
		t.Errorf("bool 2: %v", err)
	}
	// A map's leading count of keys to remove is read as the rule says (the
	// keys are read and dropped), so the pairs after it are still found.
	m2 := usmap.New(nil, []*usmap.Schema{
		schema("DesignDataRecordBase", "", prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName), prop("bDevOnly", usmap.KindBool)),
		schema("MapRecordBase", "DesignDataRecordBase", usmap.Prop{ArrayDim: 1, Name: "M", Type: &usmap.Type{Kind: usmap.KindMap, Inner: &usmap.Type{Kind: usmap.KindInt}, Value: &usmap.Type{Kind: usmap.KindInt}}}),
	})
	spec = tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.u32(1) // keys to remove
		w.i32(7) // ... the key
		w.u32(1) // pairs
		w.i32(5)
		w.i32(6)
		w.tail(1, 0, "m", false)
	}}
	db, _ = Parse(buildDBC(newPool(), spec))
	rows, err := db.Decoder(m2).Decode(0, "MapRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if want := (Pairs{{int64(5), int64(6)}}); !reflect.DeepEqual(rows[0].Fields["M"], want) {
		t.Errorf("M = %#v, want %#v", rows[0].Fields["M"], want)
	}
	// A removal count that cannot fit in the table fails before allocating.
	spec = tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.u32(1 << 30)
		w.buf.Write(make([]byte, 24)) // a row takes at least 21 bytes, or Parse refuses the table
	}}
	db, _ = Parse(buildDBC(newPool(), spec))
	if _, err := db.Decoder(m2).Decode(0, "MapRecordBase"); err == nil || !strings.Contains(err.Error(), "needs at least") {
		t.Errorf("huge removal count: %v", err)
	}
	// An array count that cannot fit in the table fails before allocating.
	spec = tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.u32(0)
		w.u32(1 << 30)
		w.buf.Write(make([]byte, 24))
	}}
	db, _ = Parse(buildDBC(newPool(), spec))
	if _, err := db.Decoder(m2).Decode(0, "MapRecordBase"); err == nil || !strings.Contains(err.Error(), "needs at least") {
		t.Errorf("huge count: %v", err)
	}
}

// [C | O | A + Cyb]

func TestPatches(t *testing.T) {
	// A record whose usmap lacks the byte between A and B.
	m := usmap.New(nil, []*usmap.Schema{
		schema("DesignDataRecordBase", "", prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName), prop("bDevOnly", usmap.KindBool)),
		schema("PatchedRecordBase", "DesignDataRecordBase", prop("A", usmap.KindInt), prop("B", usmap.KindInt)),
	})
	spec := tableSpec{1, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i32(int32(10 + i))
			w.bool(i == 1) // the property the mappings lack
			w.i32(int32(20 + i))
			w.tail(int64(i), 0, "p", false)
		}
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	patch := Patch{Struct: "PatchedRecordBase", After: "A", Prop: prop("Mid", usmap.KindBool), Why: "test"}

	if _, err := db.DecoderWith(m, Options{}).Decode(0, "PatchedRecordBase"); err == nil {
		t.Error("the unpatched mappings decoded the table")
	}
	d := db.DecoderWith(m, Options{Patches: []Patch{patch}})
	rows, err := d.Decode(0, "PatchedRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if got := rows[1].Fields; got["A"] != int64(11) || got["Mid"] != true || got["B"] != int64(21) {
		t.Errorf("row 1 = %v", got)
	}
	if got := d.Applied(); len(got) != 1 || got[0].Why != "test" {
		t.Errorf("Applied = %+v", got)
	}
	// The mappings passed in are not changed.
	if n := len(m.Schema("PatchedRecordBase").Props); n != 2 {
		t.Errorf("the original schema has %d props", n)
	}

	// A patch that does not fit is not applied: a struct that is not there, a
	// property that is, an anchor that is missing.
	for _, tt := range []struct {
		p      Patch
		status PatchStatus
		fails  bool // CheckPatches
	}{
		{Patch{Struct: "NoSuchRecordBase", After: "A", Prop: prop("Mid", usmap.KindBool), Why: "w"}, PatchNoStruct, true},
		{Patch{Struct: "PatchedRecordBase", After: "A", Prop: prop("B", usmap.KindBool)}, PatchPresent, false},
		{Patch{Struct: "PatchedRecordBase", After: "Z", Prop: prop("Mid", usmap.KindBool)}, PatchNoAnchor, true},
		{patch, PatchApplied, false},
	} {
		d := db.DecoderWith(m, Options{Patches: []Patch{tt.p}})
		res := d.PatchResults()
		if len(res) != 1 || res[0].Status != tt.status || res[0].Patch.Struct != tt.p.Struct {
			t.Errorf("patch %v: results %+v, want status %v", tt.p, res, tt.status)
		}
		if got := d.Applied(); (len(got) == 1) != (tt.status == PatchApplied) {
			t.Errorf("patch %v: Applied = %+v", tt.p, got)
		}
		err := d.CheckPatches()
		if tt.fails != (err != nil) || (err != nil && !strings.Contains(err.Error(), tt.p.String()+": "+tt.status.String())) {
			t.Errorf("patch %v: CheckPatches = %v, want an error naming the patch and %q: %v", tt.p, err, tt.status, tt.fails)
		}
	}
	if s := PatchNoAnchor.String(); s != "anchor missing" || PatchPresent.String() != "already present" ||
		PatchNoStruct.String() != "struct missing" || PatchApplied.String() != "applied" {
		t.Errorf("status names: %q ...", s)
	}
	if got := patch.String(); got != "PatchedRecordBase.Mid after A" {
		t.Errorf("Patch.String = %q", got)
	}
	// Two patches to one struct both apply, in order.
	two := db.DecoderWith(m, Options{Patches: []Patch{patch, {Struct: "PatchedRecordBase", After: "Mid", Prop: prop("Mid2", usmap.KindBool)}}})
	if got := two.Applied(); len(got) != 2 {
		t.Errorf("Applied = %+v", got)
	}
	if props := two.m.Schema("PatchedRecordBase").Props; len(props) != 4 || props[1].Name != "Mid" || props[2].Name != "Mid2" || props[2].Index != props[1].Index+1 {
		t.Errorf("props = %+v", props)
	}

	// The built-in patch list is the four properties the old usmap lacks, the
	// AbilityHit one first, and a copy: changing what Patches returns changes nothing.
	got := Patches()
	if len(got) != 4 || got[0].Struct != "AbilityHitRecordBase" || got[0].After != "ApplicableLifeStates" ||
		got[0].Prop.Name != "bMayCorrupt" || got[0].Prop.Type.Kind != usmap.KindBool {
		t.Errorf("Patches = %+v", got)
	}
	for _, p := range got {
		if p.Why == "" || p.Struct == "" || p.Prop.Name == "" || p.After == "" || p.Prop.ArrayDim != 1 || p.Prop.Type == nil {
			t.Errorf("patch %+v is incomplete", p)
		}
	}
	got[0].Struct = "Changed"
	if Patches()[0].Struct != "AbilityHitRecordBase" {
		t.Error("Patches returns the list itself")
	}
}

// [C_Y_B ~ CoA]

func TestDecodeRejectsWhatIsNotARecord(t *testing.T) {
	db, _ := Parse(buildDBC(newPool(), widgetTable(1, 1)))
	d := db.Decoder(testMap())
	for _, c := range []struct{ schema, want string }{
		{"NotARecord", "not a record schema"},
		{"DesignDataRecordBase", "not a record schema"},
		{"NoSuchRecordBase", "no schema"},
	} {
		if _, err := d.Decode(0, c.schema); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("Decode as %s: %v, want %q", c.schema, err, c.want)
		}
	}
	if _, err := d.Decode(5, "WidgetRecordBase"); err == nil || !strings.Contains(err.Error(), "no table 5") {
		t.Errorf("Decode of table 5: %v", err)
	}
	if err := d.Check(-1, "WidgetRecordBase"); err == nil {
		t.Error("Check of table -1 succeeded")
	}
}

func TestClassify(t *testing.T) {
	const (
		holderType = 10
		widgetType = 20
		gadgetType = 30
		stray      = 40
	)
	// Holder rows carry a WidgetId whose TypeId is the Widget table's.
	holder := tableSpec{holderType, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i64(int64(i)) // Target.Guid
			w.i64(widgetType)
			w.str("None")
			w.tail(int64(i), 0, "h", false)
		}
	}}
	widgets := widgetTable(widgetType, 3) // fits WidgetRecordBase and GadgetRecordBase
	gadgets := widgetTable(gadgetType, 2) // fits both too, and nothing names it
	// A table of garbage no struct walks.
	garbage := tableSpec{stray, 1, func(w *rowWriter) {
		w.row()
		for i := 0; i < 4; i++ {
			w.u64(0xdeadbeefdeadbeef)
		}
	}}

	db, err := Parse(buildDBC(newPool(), holder, widgets, gadgets, garbage))
	if err != nil {
		t.Fatal(err)
	}
	cl := db.Decoder(testMap()).Classify()
	if len(cl) != 4 {
		t.Fatalf("classifications = %d", len(cl))
	}
	// Holder: only HolderRecordBase walks it.
	if c := cl[0]; !reflect.DeepEqual(c.Candidates, []string{"HolderRecordBase"}) || c.Schema != "HolderRecordBase" || !c.Decodes || c.Rows != 2 {
		t.Errorf("holder = %+v", c)
	}
	// Widget: two candidates, settled by the WidgetId in the Holder rows.
	if c := cl[1]; !reflect.DeepEqual(c.Candidates, []string{"GadgetRecordBase", "WidgetRecordBase"}) || c.Schema != "WidgetRecordBase" || !c.Decodes {
		t.Errorf("widgets = %+v", c)
	}
	// Gadget table: the same two candidates, and no Id struct names its TypeId.
	if c := cl[2]; len(c.Candidates) != 2 || c.Schema != "" || c.Decodes {
		t.Errorf("gadgets = %+v", c)
	}
	// Garbage: nothing walks it.
	if c := cl[3]; len(c.Candidates) != 0 || c.Schema != "" || c.Decodes {
		t.Errorf("garbage = %+v", c)
	}
	if got := db.Decoder(testMap()).Candidates(1); !reflect.DeepEqual(got, []string{"GadgetRecordBase", "WidgetRecordBase"}) {
		t.Errorf("Candidates(1) = %v", got)
	}
	if got := db.Decoder(testMap()).Candidates(9); got != nil {
		t.Errorf("Candidates(9) = %v", got)
	}

	// KnownTypes names a table nothing else does, and a table no struct walks
	// keeps the struct its TypeId is known by, undecoded.
	known := map[uint64]string{stray: "KitchenRecordBase", gadgetType: "GadgetRecordBase"}
	cl = db.DecoderWith(testMap(), Options{KnownTypes: known}).Classify()
	if c := cl[3]; c.Schema != "KitchenRecordBase" || c.Decodes || c.Basis != BasisTypeID {
		t.Errorf("garbage with a known type = %+v", c)
	}
	if c := cl[2]; c.Schema != "GadgetRecordBase" || !c.Decodes || c.Basis != BasisTypeID {
		t.Errorf("gadgets with a known type = %+v", c)
	}
}

// oneTableMap is testMap with record structs that tell tables apart: Thing
// (three ints, a shape no other struct fits), Box, and Ref, whose rows hold a
// ThingId and a BoxId and so teach the TypeIds of the Thing and Box tables.
func oneTableMap() *usmap.Map {
	m := testMap()
	return usmap.New(m.Enums, append(append([]*usmap.Schema{}, m.Schemas...),
		schema("ThingId", "DesignDataObjectId"),
		schema("BoxId", "DesignDataObjectId"),
		schema("ThingRecordBase", "DesignDataRecordBase", prop("A", usmap.KindInt), prop("B", usmap.KindInt), prop("C", usmap.KindInt)),
		schema("BoxRecordBase", "DesignDataRecordBase", prop("A", usmap.KindInt), prop("B", usmap.KindInt), prop("C", usmap.KindInt), prop("D", usmap.KindInt)),
		schema("RefRecordBase", "DesignDataRecordBase", structProp("Thing", "ThingId"), structProp("Box", "BoxId")),
	))
}

// thingTable writes n Thing rows named prefix_0, prefix_1, ...
func thingTable(typeID uint64, n int, prefix string) tableSpec {
	return tableSpec{typeID, uint32(n), func(w *rowWriter) {
		for i := 0; i < n; i++ {
			w.row()
			w.i32(1)
			w.i32(2)
			w.i32(3)
			w.tail(int64(2000+i), 0, prefix+"_"+string(rune('0'+i)), false)
		}
	}}
}

// C | O | A x C_Y_B

// namedWidgetTable writes n rows that fit Widget and Gadget (one int), named
// prefix_0, prefix_1, ...
func namedWidgetTable(typeID uint64, n int, prefix string) tableSpec {
	return tableSpec{typeID, uint32(n), func(w *rowWriter) {
		for i := 0; i < n; i++ {
			w.row()
			w.i32(int32(i + 1))
			w.tail(int64(3000+i), 0, prefix+"_"+string(rune('0'+i)), false)
		}
	}}
}

// refTable writes one Ref row that names the Thing table by thing and the Box
// table by box.
func refTable(typeID, thing, box uint64) tableSpec {
	return tableSpec{typeID, 1, func(w *rowWriter) {
		w.row()
		w.i64(1)
		w.i64(int64(thing))
		w.str("None")
		w.i64(2)
		w.i64(int64(box))
		w.str("None")
		w.tail(4000, 0, "ref", false)
	}}
}

// TestClassifyOneTablePerSchema: a struct that fits a table is not necessarily
// the table's (24 fishing rows walk as StatLevelGains), so a struct is given to
// one table only, chosen by what names its TypeId and then by the rows' names.
func TestClassifyOneTablePerSchema(t *testing.T) {
	const undecided = ""
	for _, tt := range []struct {
		name   string
		specs  []tableSpec
		schema []string // per table
		basis  []string // per table
	}{
		{
			name: "the table an Id struct names wins, the other is undecided",
			specs: []tableSpec{
				thingTable(100, 2, "x"), thingTable(101, 2, "y"),
				refTable(102, 101, 999),
			},
			schema: []string{undecided, "ThingRecordBase", "RefRecordBase"},
			basis:  []string{"", BasisSingleFit, BasisSingleFit},
		},
		{
			name: "the Id struct beats the rows' names",
			specs: []tableSpec{
				thingTable(100, 2, "Thing"), thingTable(101, 2, "Other"),
				refTable(102, 101, 999),
			},
			schema: []string{undecided, "ThingRecordBase", "RefRecordBase"},
			basis:  []string{"", BasisSingleFit, BasisSingleFit},
		},
		{
			name: "without an Id struct, the table whose rows are named for the struct wins",
			specs: []tableSpec{
				thingTable(100, 2, "Other"), thingTable(101, 2, "Thing"),
			},
			schema: []string{undecided, "ThingRecordBase"},
			basis:  []string{"", BasisSingleFit},
		},
		{
			name: "with neither, neither is settled",
			specs: []tableSpec{
				thingTable(100, 2, "a"), thingTable(101, 2, "b"),
			},
			schema: []string{undecided, undecided},
			basis:  []string{"", ""},
		},
		{
			name: "a name that is only partly the stem does not count",
			specs: []tableSpec{
				thingTable(100, 2, "Thing"), thingTable(101, 2, "Thingamajig"),
				thingTable(102, 1, "Thing"),
			},
			schema: []string{undecided, undecided, undecided}, // two tables carry the stem: a tie
			basis:  []string{"", "", ""},
		},
		{
			name: "a lone table is not settled when an Id struct gives its TypeId to another struct",
			specs: []tableSpec{
				thingTable(200, 2, "Thing"),
				refTable(201, 300, 200), // BoxId names table 200
			},
			schema: []string{undecided, "RefRecordBase"},
			basis:  []string{"", BasisSingleFit},
		},
		{
			name: "an Id struct that agrees changes nothing",
			specs: []tableSpec{
				thingTable(200, 2, "x"),
				refTable(201, 200, 300),
			},
			schema: []string{"ThingRecordBase", "RefRecordBase"},
			basis:  []string{BasisSingleFit, BasisSingleFit},
		},
		{
			name: "tables several structs fit are told apart by their rows' names",
			specs: []tableSpec{
				namedWidgetTable(100, 3, "Gadget"), namedWidgetTable(101, 2, "Widget"),
			},
			schema: []string{"GadgetRecordBase", "WidgetRecordBase"},
			basis:  []string{BasisNameStem, BasisNameStem},
		},
		{
			name: "two such tables named for one struct are a tie: neither is settled",
			specs: []tableSpec{
				namedWidgetTable(100, 3, "Gadget"), namedWidgetTable(101, 2, "Gadget"),
			},
			schema: []string{undecided, undecided},
			basis:  []string{"", ""},
		},
		{
			name: "a TypeId beats the rows' names",
			specs: []tableSpec{
				tableSpec{10, 1, func(w *rowWriter) { // a Holder row naming the Widget table by TypeId
					w.row()
					w.i64(1)
					w.i64(20)
					w.str("None")
					w.tail(1, 0, "h", false)
				}},
				namedWidgetTable(20, 2, "Gadget"),
			},
			schema: []string{"HolderRecordBase", "WidgetRecordBase"},
			basis:  []string{BasisSingleFit, BasisTypeID},
		},
	} {
		db, err := Parse(buildDBC(newPool(), tt.specs...))
		if err != nil {
			t.Fatalf("%s: %v", tt.name, err)
		}
		cl := db.Decoder(oneTableMap()).Classify()
		for i := range tt.specs {
			c := cl[i]
			if c.Schema != tt.schema[i] || c.Basis != tt.basis[i] || c.Decodes != (c.Schema != "") {
				t.Errorf("%s: table %d = %+v; want schema %q (%s)", tt.name, i, c, tt.schema[i], tt.basis[i])
			}
		}
		// Whatever the evidence, no struct is two tables' schema.
		seen := map[string]int{}
		for _, c := range cl {
			if c.Schema == "" {
				continue
			}
			if prev, dup := seen[c.Schema]; dup {
				t.Errorf("%s: tables %d and %d are both %s", tt.name, prev, c.Table, c.Schema)
			}
			seen[c.Schema] = c.Table
		}
	}
}

// C.Y.B - C O A

// TestKnownTypesIsACopy: the lists a Decoder starts from cannot be changed
// through the accessors, and the Decoder keeps its own copy of the options.
func TestKnownTypesIsACopy(t *testing.T) {
	// The record types the executable registers: 398, among them the ones the
	// first content slice reads, named by the direct child of DesignDataRecordBase.
	got := KnownTypes()
	for id, want := range map[uint64]string{
		7037248826340794574: "StatCurveRecordBase", 1841048597116038327: "AoCAbilityRecordBase",
		3672506661632452365: "AbilityHitRecordBase", 5365104778484440215: "StatTypeDefRecordBase",
		5024462864295026017: "DialogueNode", 2616523457551378613: "ItemRecordBase",
	} {
		if got[id] != want {
			t.Errorf("KnownTypes[%d] = %q, want %q", id, got[id], want)
		}
	}
	if len(got) != 398 {
		t.Errorf("KnownTypes has %d entries, want 398", len(got))
	}
	got[1] = "X"
	delete(got, 7037248826340794574)
	if again := KnownTypes(); len(again) != 398 || again[7037248826340794574] != "StatCurveRecordBase" {
		t.Errorf("KnownTypes after a change = %d entries, StatCurve %q", len(again), again[7037248826340794574])
	}
	// The facts read from the executable: the two DesignDataRecordBase properties
	// that are never stored, the four 16-byte-aligned record structs, and
	// RichCurveKey's native serializer.
	f := ExeFacts()
	if !f.NotStored["DesignDataRecordBase.RecordFilename"] || !f.NotStored["DesignDataRecordBase.bIsDirty"] ||
		f.RowAlign["StoryArcPhaseRecordBase"] != 16 || len(f.RowAlign) != 4 || !f.NativeSerialize["RichCurveKey"] {
		t.Errorf("ExeFacts = %+v", f)
	}

	db, err := Parse(buildDBC(newPool(), thingTable(5, 1, "Thing")))
	if err != nil {
		t.Fatal(err)
	}
	known := map[uint64]string{5: "KitchenRecordBase"}
	d := db.DecoderWith(oneTableMap(), Options{KnownTypes: known})
	known[5] = "BoxRecordBase" // after the fact
	// The table fits Thing alone, but the known type says it is Kitchen's: not Thing.
	if c := d.Classify()[0]; c.Schema != "" || c.Decodes {
		t.Errorf("table = %+v", c)
	}
}

// rulesMap is a mappings file for the rules read off the client build: a
// record struct with a leaf FXxxRecord below it, a struct whose C++ type
// serializes itself, an InstancedStruct and an Optional.
func rulesMap() *usmap.Map {
	return usmap.New(
		[]*usmap.Enum{{Name: "EMode", Values: []usmap.EnumValue{{Name: "Off", Value: 0}, {Name: "On", Value: 1}}}},
		[]*usmap.Schema{
			schema("DesignDataRecordBase", "",
				prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName),
				prop("bDevOnly", usmap.KindBool), prop("RecordFilename", usmap.KindStr), prop("bIsDirty", usmap.KindBool)),
			schema("InstancedStruct", ""),
			schema("Payload", "", prop("P", usmap.KindInt), prop("Q", usmap.KindBool)),
			schema("Native", "", enumProp("Mode", "EMode"), prop("V", usmap.KindFloat)),
			// The schema (ThingRecordBase) and the leaf the rows are (ThingRecord).
			schema("ThingRecordBase", "DesignDataRecordBase", prop("B", usmap.KindInt), prop("Skip", usmap.KindInt), structProp("N", "Native")),
			schema("ThingRecord", "ThingRecordBase", prop("L", usmap.KindBool)),
			schema("InstRecordBase", "DesignDataRecordBase", structProp("Inst", "InstancedStruct"),
				usmap.Prop{ArrayDim: 1, Name: "Opt", Type: &usmap.Type{Kind: usmap.KindOptional, Inner: tInt()}}),
			schema("WideRecordBase", "DesignDataRecordBase", prop("W", usmap.KindInt)),
		})
}

func rulesFacts() *Facts {
	return &Facts{
		NotStored:       map[string]bool{"ThingRecordBase.Skip": true, "DesignDataRecordBase.RecordFilename": true, "DesignDataRecordBase.bIsDirty": true},
		RowAlign:        map[string]int{"WideRecordBase": 16},
		NativeSerialize: map[string]bool{"Native": true},
	}
}

// [CoA ~ C | Y | B]

// TestLeafRecordAndFacts: a row is the leaf record struct (its own properties
// first), a property the facts say is not stored is not read, an enum in a
// natively serialized struct is one byte, and Classify names the schema, not
// the leaf.
func TestLeafRecordAndFacts(t *testing.T) {
	spec := tableSpec{1, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.bool(i == 1)      // ThingRecord.L
			w.i32(int32(7 + i)) // ThingRecordBase.B (Skip is not stored)
			w.u8(1)             // Native.Mode, one byte
			w.f32(2.5)          // Native.V
			w.tail(int64(i+1), 0, "thing", false)
		}
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	d := db.DecoderWith(rulesMap(), Options{Facts: rulesFacts()})
	rows, err := d.Decode(0, "ThingRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	want := Struct{"L": true, "B": int64(8), "N": Struct{"Mode": "EMode::On", "V": float32(2.5)}}
	if !reflect.DeepEqual(rows[1].Fields, want) {
		t.Errorf("row 1 = %#v, want %#v", rows[1].Fields, want)
	}
	cl := d.Classify()
	if c := cl[0]; c.Schema != "ThingRecordBase" || !c.Decodes || len(c.Candidates) != 1 {
		t.Errorf("classified as %+v", c)
	}
	if got := d.recordSchemas(); !reflect.DeepEqual(got, []string{"InstRecordBase", "ThingRecordBase", "WideRecordBase"}) {
		t.Errorf("recordSchemas = %v", got)
	}
	// Without the facts the walk reads Skip and the enum as a pool reference, and fails.
	if _, err := db.DecoderWith(rulesMap(), Options{Facts: &Facts{}}).Decode(0, "ThingRecordBase"); err == nil {
		t.Error("decoded without the facts")
	}
	// Two leaves below one schema cannot be walked.
	two := usmap.New(nil, append(rulesMap().Schemas, schema("ThingRecord2", "ThingRecordBase")))
	if _, err := db.DecoderWith(two, Options{Facts: rulesFacts()}).Decode(0, "ThingRecordBase"); err == nil || !strings.Contains(err.Error(), "derived structs") {
		t.Errorf("two leaves: %v", err)
	}
}

// TestInheritedRowTail: a row whose ParentGuid is not 0 ends with 4 bytes, 0;
// anything else there is an error.
func TestInheritedRowTail(t *testing.T) {
	spec := tableSpec{1, 3, func(w *rowWriter) {
		w.row()
		w.i32(1)
		w.tail(1, 0, "a", false) // no parent: no tail
		w.row()
		w.i32(2)
		w.tail(2, 1, "b", false) // a parent: the helper writes the 4 bytes
		w.row()
		w.i32(3)
		w.tail(3, 1, "c", false)
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	m := usmap.New(nil, append(rulesMap().Schemas, schema("PlainRecordBase", "DesignDataRecordBase", prop("W", usmap.KindInt))))
	rows, err := db.DecoderWith(m, Options{Facts: rulesFacts()}).Decode(0, "PlainRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 3 || rows[1].ParentGuid != 1 || rows[1].End-rows[1].Offset != 4+21+4 || rows[0].End-rows[0].Offset != 4+21 {
		t.Errorf("rows = %+v", rows)
	}
	bad := tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.i32(1)
		w.i64(1)
		w.i64(1)
		w.str("x")
		w.bool(false)
		w.u32(5) // a tail that is not 0
	}}
	db, _ = Parse(buildDBC(newPool(), bad))
	if _, err := db.DecoderWith(m, Options{Facts: rulesFacts()}).Decode(0, "PlainRecordBase"); err == nil || !strings.Contains(err.Error(), "inherited row's tail is 0x5") {
		t.Errorf("tail 5: %v", err)
	}
}

// TestRowAlign16: the rows of a record struct the facts give an alignment of
// 16 start on 16-byte boundaries.
func TestRowAlign16(t *testing.T) {
	spec := tableSpec{1, 3, func(w *rowWriter) {
		for i := 0; i < 3; i++ {
			w.rowAligned(16)
			w.i32(int32(i))
			w.tail(int64(i+1), 0, "w", false) // 25 bytes a row: the next starts 7 bytes later than an 8-aligned walk expects
		}
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	rows, err := db.DecoderWith(rulesMap(), Options{Facts: rulesFacts()}).Decode(0, "WideRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	for i, r := range rows {
		if r.Offset%16 != 0 || r.Fields["W"] != int64(i) {
			t.Errorf("row %d at %#x = %v", i, r.Offset, r.Fields)
		}
	}
	if _, err := db.DecoderWith(rulesMap(), Options{Facts: &Facts{}}).Decode(0, "WideRecordBase"); err == nil {
		t.Error("an 8-aligned walk decoded 16-aligned rows")
	}
}

// TestInstancedStructAndOptional: an InstancedStruct is its path, a size and
// the struct's properties; an Optional a set flag and the value.
func TestInstancedStructAndOptional(t *testing.T) {
	inst := func(w *rowWriter, path string, size int32, body func()) {
		w.fstr(path)
		w.i32(size)
		if body != nil {
			body()
		}
	}
	spec := tableSpec{1, 2, func(w *rowWriter) {
		w.row()
		inst(w, "/Script/Mod.Payload", 5, func() { w.i32(42); w.bool(true) })
		w.u32(1)
		w.i32(9)
		w.tail(1, 0, "a", false)
		w.row()
		inst(w, "", 0, nil)
		w.u32(0)
		w.tail(2, 0, "b", false)
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	rows, err := db.DecoderWith(rulesMap(), Options{Facts: rulesFacts()}).Decode(0, "InstRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	want0 := Struct{"Inst": Struct{"Type": "/Script/Mod.Payload", "Value": Struct{"P": int64(42), "Q": true}}, "Opt": int64(9)}
	want1 := Struct{"Inst": nil, "Opt": nil}
	if !reflect.DeepEqual(rows[0].Fields, want0) || !reflect.DeepEqual(rows[1].Fields, want1) {
		t.Errorf("rows = %#v / %#v", rows[0].Fields, rows[1].Fields)
	}
	for _, c := range []struct {
		name  string
		write func(w *rowWriter)
		want  string
	}{
		{"size mismatch", func(w *rowWriter) {
			inst(w, "/Script/Mod.Payload", 9, func() { w.i32(1); w.bool(false); w.u32(0) })
			w.u32(0)
		}, "take 5 bytes, the size says 9"},
		{"unknown struct", func(w *rowWriter) { inst(w, "/Script/Mod.Nope", 0, nil); w.u32(0) }, "no struct Nope"},
		{"data without a struct", func(w *rowWriter) { inst(w, "", 4, func() { w.i32(0) }); w.u32(0) }, "no struct with 4 bytes"},
		{"optional flag", func(w *rowWriter) { inst(w, "", 0, nil); w.u32(2) }, "optional set flag 2"},
	} {
		spec := tableSpec{1, 1, func(w *rowWriter) { w.row(); c.write(w); w.tail(1, 0, "x", false) }}
		db, _ := Parse(buildDBC(newPool(), spec))
		if _, err := db.DecoderWith(rulesMap(), Options{Facts: rulesFacts()}).Decode(0, "InstRecordBase"); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want %q", c.name, err, c.want)
		}
	}
}

func TestClassifyIgnoresEmptyTables(t *testing.T) {
	empty := tableSpec{7, 0, func(w *rowWriter) {}}
	db, err := Parse(buildDBC(newPool(), empty))
	if err != nil {
		t.Fatal(err)
	}
	cl := db.Decoder(testMap()).Classify()
	if len(cl[0].Candidates) != 0 || cl[0].Schema != "" {
		t.Errorf("empty table = %+v", cl[0])
	}
	// An empty table decodes to no rows.
	rows, err := db.Decoder(testMap()).Decode(0, "WidgetRecordBase")
	if err != nil || len(rows) != 0 {
		t.Errorf("Decode of an empty table = %v, %v", rows, err)
	}
}

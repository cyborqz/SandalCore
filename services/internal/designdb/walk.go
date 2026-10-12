// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package designdb

import (
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"strconv"
	"strings"
	"sync"
	"unicode/utf8"

	"aocrop/services/internal/usmap"
)

// Value is a decoded property: one of
//
//	bool                  Bool
//	int64                 Byte, Int8, Int16, Int, Int64, UInt16, UInt32
//	uint64                UInt64
//	float32, float64      Float, Double
//	string                Name, Str, Enum ("EType::Value"), Object and SoftObject (a path),
//	                      and three structs flattened to their one string: an AoCExpression
//	                      (its text), a SoftObjectPath or SoftClassPath (its path)
//	Text                  Text (FText)
//	[]Value               Array, Set, and a GameplayTagContainer (the tag names)
//	Pairs                 Map
//	Struct                any other struct, by property name
//
// A Guid is an int64; an Id struct (Guid, TypeId, Name) is a Struct.
type Value = any

// Struct is a decoded struct: property name to value.
type Struct map[string]Value

// Text is a decoded FText. History is the FText history type the file holds:
// 0 (base) has Namespace, Key and Source; 11 (string table entry) has Table
// (the table id) and Key; -1 (none) has only Source, the culture-invariant
// string, when it has one.
type Text struct {
	History   int8
	Namespace string
	Key       string
	Source    string
	Table     string
}

// Pairs is a decoded Map, entries in file order.
type Pairs []Pair

// Pair is one entry of a Map.
type Pair struct {
	Key   Value
	Value Value
}

// Row is a decoded record.
type Row struct {
	Offset     int64 // file offset the row starts at
	End        int64 // file offset just past the row's last byte (the padding before the next row is not counted)
	Guid       int64
	ParentGuid int64
	Name       string
	DevOnly    bool
	// Fields holds the <Type>RecordBase properties by name. The four
	// DesignDataRecordBase properties above are not repeated in it; RecordFilename
	// and bIsDirty are not stored in the file.
	Fields Struct
}

// built by CoA with C | Y | B

// DecodeError says where a table's walk failed. A table that fails is not
// decoded: Decoder.Decode returns no rows with it.
type DecodeError struct {
	Table  int
	Schema string
	// Row is the index of the row being read, or the row count when the rows
	// all read but did not end on the table's last byte.
	Row int
	// RowOffset is the file offset the row started at (or, for the end check,
	// where the rows ended); Offset is the file offset the walk went wrong at.
	RowOffset int64
	Offset    int64
	// Path is the property path inside the row, e.g. "StatCosts[1].Stat".
	Path string
	Msg  string
}

func (e *DecodeError) Error() string {
	s := fmt.Sprintf("designdb: table %d as %s: row %d (starts at %#x), byte %#x", e.Table, e.Schema, e.Row, e.RowOffset, e.Offset)
	if e.Path != "" {
		s += ", " + e.Path
	}
	return s + ": " + e.Msg
}

// Decoder walks tables with the record structs of a mappings file. It is safe
// for concurrent use; its caches fill as it goes.
//
// A table is a run of rows, written by UStruct::SerializeBin over a persistent
// binary archive whose FName, FString-less object and enum writes are pool
// references. Each row is the table's record type FXxxRecord: the properties
// of that leaf struct, then those of the XxxRecordBase it derives from (the
// schema, which names the table), then DesignDataRecordBase's: Guid i64,
// ParentGuid i64, Name, bDevOnly. A struct's properties come in the mappings'
// order, own first, then the super's. Properties the archive does not
// serialize are not stored: those marked Transient, Deprecated or
// SkipSerialization (Facts.NotStored, read from the executable; RecordFilename
// and bIsDirty are two). A row starts at a file offset aligned to the record
// type's C++ alignment, 8 or 16 (Facts.RowAlign; the directory's offsets are
// not aligned: the padding before the first row counts as the row's own), and
// a row whose ParentGuid is not 0 ends with 4 more bytes, 0 in every such row
// of the file (what they mean is not known). A property is:
//
//	Bool, Byte, Int8  1 byte (a bool must be 0 or 1)       Int16, UInt16  2 bytes
//	Int, UInt32, Float  4 bytes                               Int64, UInt64, Double  8 bytes
//	Name, Str, Enum   u32 pool reference; an Enum's string is "EType::Value"
//	                  (also when its underlying type is an Int)
//	Object, SoftObject, SoftObjectPath, SoftClassPath, AoCExpression
//	                  u32 pool reference (the path, the text)
//	Text              u32 flags, i8 history; 0: three inline FStrings (namespace, key, source);
//	                  11: u32 pool reference (the table id), an FString (the key);
//	                  -1: u32 flag, then an FString if set
//	Array             u32 count, the elements
//	Map, Set          u32 count of keys to remove and those keys, u32 count, the pairs or elements
//	                  (UE's FMapProperty / FSetProperty::SerializeItem; the first count is 0 in the file)
//	Optional          u32 set flag (0 or 1), then the value if set
//	GameplayTagContainer  its GameplayTags array only (ParentTags is Transient)
//	InstancedStruct   an inline FString with the struct's path ("/Script/Pkg.Name", "" for none),
//	                  an i32 size, and that struct's properties in exactly size bytes
//	Struct            its properties, in place; in a struct whose C++ type serializes
//	                  itself (Facts.NativeSerialize: RichCurveKey, the InterpCurve points, ...)
//	                  an enum is one byte
//
// where a pool reference is a u32 offset of a string in the pool (0xFFFFFFFF
// is ""), an inline FString is an i32 length counting the NUL and the bytes,
// and the other kinds are little-endian. Every read must stay inside the
// table, and a pool reference must point at the start of a pool string, which
// is what makes a walk of the wrong struct fail within a few bytes.
//
// The rules were found on the tables themselves and each is checked by the
// exact end-of-table test (see the package documentation for what they take).
type Decoder struct {
	db      *DB
	m       *usmap.Map // the mappings with the patches applied
	results []PatchResult
	known   map[uint64]string // record types by TypeId, for Classify
	facts   *Facts
	// children lists, by struct, the structs of the mappings whose super it is:
	// the leaf record structs below a schema.
	children map[string][]string

	mu    sync.Mutex
	plans map[planKey]*plan
	sizes map[string]int
}

// Facts are what the walk needs to know about the client build besides its
// mappings file, which cannot carry them: property flags, struct alignment and
// struct serializers. tools/ghidra/usmapgen.py --go reads them from the
// executable's reflection data into exe_gen.go (ExeFacts). A missing entry
// means 8 for RowAlign and false for the sets.
type Facts struct {
	// NotStored names, as "Struct.Property", the properties the database does
	// not store: those marked Transient, Deprecated or SkipSerialization.
	NotStored map[string]bool
	// RowAlign is the alignment of a record struct's rows (by the schema's name)
	// when it is not 8.
	RowAlign map[string]int
	// NativeSerialize lists the structs whose C++ type serializes itself; an
	// enum inside one is a byte, not a pool reference.
	NativeSerialize map[string]bool
}

// ExeFacts returns the facts read from the client executable (exe_gen.go).
func ExeFacts() *Facts {
	return &Facts{NotStored: exeNotStored, RowAlign: exeRowAlign, NativeSerialize: exeNativeSerialize}
}

// Options are what a Decoder takes besides the database and the mappings.
type Options struct {
	// Patches are changes to the mappings (see Patches).
	Patches []Patch
	// KnownTypes are record types by TypeId (see KnownTypes).
	KnownTypes map[uint64]string
	// Facts are the client build's facts; nil means ExeFacts().
	Facts *Facts
}

// Decoder returns a decoder for db's tables, using m's structs with the
// standard Patches applied, the standard KnownTypes and ExeFacts.
func (db *DB) Decoder(m *usmap.Map) *Decoder {
	return db.DecoderWith(m, Options{Patches: Patches(), KnownTypes: KnownTypes()})
}

// (C-O-A ~ Cyb)

// DecoderWith is Decoder with explicit options (the zero Options applies no
// patch and knows no type, to see what the unpatched mappings do).
func (db *DB) DecoderWith(m *usmap.Map, opt Options) *Decoder {
	facts := opt.Facts
	if facts == nil {
		facts = ExeFacts()
	}
	pm, results := applyPatches(m, opt.Patches)
	known := make(map[uint64]string, len(opt.KnownTypes))
	for id, s := range opt.KnownTypes {
		known[id] = s
	}
	children := map[string][]string{}
	for _, sc := range pm.Schemas {
		if sc.Super != "" {
			children[sc.Super] = append(children[sc.Super], sc.Name)
		}
	}
	return &Decoder{db: db, m: pm, results: results, known: known, facts: facts, children: children,
		plans: map[planKey]*plan{}, sizes: map[string]int{}}
}

// Applied lists the patches that changed a struct of the mappings (a patch
// whose struct is missing, or that already has the property, is not applied).
func (d *Decoder) Applied() []Patch {
	var out []Patch
	for _, r := range d.results {
		if r.Status == PatchApplied {
			out = append(out, r.Patch)
		}
	}
	return out
}

// PatchResults says what became of every patch, in the order they were given.
func (d *Decoder) PatchResults() []PatchResult { return append([]PatchResult(nil), d.results...) }

// CheckPatches is nil when every patch either applied or found its property
// already in the mappings; otherwise the error names the first patch that fit
// nothing (its struct or anchor is missing), which means the mappings are not
// the ones the patch was written against.
func (d *Decoder) CheckPatches() error {
	for _, r := range d.results {
		if r.Status == PatchNoStruct || r.Status == PatchNoAnchor {
			return fmt.Errorf("designdb: mappings patch %s: %s (%s)", r.Patch, r.Status, r.Patch.Why)
		}
	}
	return nil
}

const (
	recordRoot = "DesignDataRecordBase"
	idRoot     = "DesignDataObjectId"
	// instancedStruct is UE's FInstancedStruct: a struct chosen at runtime,
	// serialized with its path (see Decoder).
	instancedStruct = "InstancedStruct"
)

// planKey separates a record walk (the leaf record struct's properties first,
// the inherited row's tail) from a plain struct walk.
type planKey struct {
	name   string
	record bool
}

// step is one property to read.
type step struct {
	name     string
	t        *usmap.Type
	byteEnum bool // an enum stored as one byte
}

// plan is a struct's properties in file order: the struct's own first, then
// each super's (leaf to root), the order the survey proved on the tables. A
// record plan starts with the leaf record struct(s) derived from the schema.
type plan struct {
	name   string
	steps  []step
	record bool
	// isID is set for the Id structs (root DesignDataObjectId): Guid, TypeId, Name.
	isID bool
}

// leaves returns the structs derived from name, leaf first (FXxxRecord below
// XxxRecordBase). A schema with two derived structs cannot be walked: the
// error says so.
func (d *Decoder) leaves(name string) ([]*usmap.Schema, error) {
	var out []*usmap.Schema
	for n := name; ; {
		kids := d.children[n]
		if len(kids) == 0 {
			break
		}
		if len(kids) > 1 {
			return nil, fmt.Errorf("%s has %d derived structs (%s, ...): which one the rows are is not known", n, len(kids), kids[0])
		}
		n = kids[0]
		out = append([]*usmap.Schema{d.m.Schema(n)}, out...)
		if len(out) > 64 {
			return nil, fmt.Errorf("the structs derived from %s nest deeper than 64", name)
		}
	}
	return out, nil
}

func (d *Decoder) plan(name string, record bool) (*plan, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	key := planKey{name, record}
	if p := d.plans[key]; p != nil {
		return p, nil
	}
	chain, err := d.m.Chain(name)
	if err != nil {
		return nil, err
	}
	if record {
		leaves, err := d.leaves(name)
		if err != nil {
			return nil, err
		}
		chain = append(leaves, chain...)
	}
	p := &plan{name: name, record: record}
	seen := map[string]string{}
	for _, sc := range chain {
		for _, pr := range sc.Props {
			if d.facts.NotStored[sc.Name+"."+pr.Name] {
				continue
			}
			if pr.ArrayDim != 1 {
				return nil, fmt.Errorf("%s.%s is a fixed array of %d (not supported)", sc.Name, pr.Name, pr.ArrayDim)
			}
			if from, dup := seen[pr.Name]; dup {
				return nil, fmt.Errorf("%s.%s repeats %s.%s", sc.Name, pr.Name, from, pr.Name)
			}
			seen[pr.Name] = sc.Name
			p.steps = append(p.steps, step{name: pr.Name, t: pr.Type, byteEnum: d.facts.NativeSerialize[sc.Name]})
		}
	}
	root := chain[len(chain)-1].Name
	p.isID = root == idRoot && name != idRoot
	d.plans[key] = p
	return p, nil
}

// C:Y:B x CoA

// minSize is the fewest bytes a value of type t takes, used to reject an
// array count that cannot fit in the rest of the table.
func (d *Decoder) minSize(t *usmap.Type) int {
	switch t.Kind {
	case usmap.KindBool, usmap.KindByte, usmap.KindInt8:
		return 1
	case usmap.KindInt16, usmap.KindUInt16:
		return 2
	case usmap.KindInt, usmap.KindFloat, usmap.KindUInt32, usmap.KindName, usmap.KindStr,
		usmap.KindEnum, usmap.KindArray, usmap.KindUnknown, usmap.KindObject, usmap.KindSoftObject:
		return 4
	case usmap.KindMap, usmap.KindSet:
		return 8 // the removal count and the count
	case usmap.KindInt64, usmap.KindUInt64, usmap.KindDouble:
		return 8
	case usmap.KindText:
		return 5
	case usmap.KindOptional:
		return 4
	case usmap.KindStruct:
		switch t.Struct {
		case "AoCExpression", "SoftObjectPath", "SoftClassPath", "GameplayTagContainer":
			return 4
		case instancedStruct:
			return 8
		}
		d.mu.Lock()
		n, ok := d.sizes[t.Struct]
		d.mu.Unlock()
		if ok {
			return n
		}
		chain, err := d.m.Chain(t.Struct)
		if err != nil {
			return 0
		}
		n = 0
		d.mu.Lock()
		d.sizes[t.Struct] = 0 // a by-value cycle (there is none) counts as empty
		d.mu.Unlock()
		for _, sc := range chain {
			for _, p := range sc.Props {
				if d.facts.NotStored[sc.Name+"."+p.Name] {
					continue
				}
				if p.Type.Kind == usmap.KindEnum && d.facts.NativeSerialize[sc.Name] {
					n++
					continue
				}
				n += d.minSize(p.Type)
			}
		}
		d.mu.Lock()
		d.sizes[t.Struct] = n
		d.mu.Unlock()
		return n
	}
	return 0
}

// failure is what the walker panics with; walk recovers it into a DecodeError.
type failure struct {
	off int
	msg string
}

// pathElem is a property name, or an index into an array or map.
type pathElem struct {
	name string
	idx  int
}

// walker reads one table. With build false it only checks the bytes (no
// values are made), which is how Classify tries candidate structs.
type walker struct {
	d     *Decoder
	db    *DB
	data  []byte
	o     int
	end   int
	build bool
	strs  map[uint32]string
	path  []pathElem

	// ids records the TypeId each Id struct was seen with.
	ids map[string]uint64

	// trace, when set, receives one line per value read from row traceFrom on
	// (Decoder.Trace); row is the row being read.
	trace     io.Writer
	traceFrom int
	row       int
}

func (w *walker) tracing() bool { return w.trace != nil && w.row >= w.traceFrom }

// traced writes a trace line for the value read at offset at.
func (w *walker) traced(at int, what string, v Value) {
	s := fmt.Sprintf("%v", v)
	if len(s) > 80 {
		s = s[:77] + "..."
	}
	fmt.Fprintf(w.trace, "%#x row %d %s: %s = %s\n", at, w.row, w.pathString(), what, s)
}

func (w *walker) fail(format string, args ...any) {
	panic(&failure{off: w.o, msg: fmt.Sprintf(format, args...)})
}

func (w *walker) need(n int) {
	if n < 0 || w.o+n > w.end {
		w.fail("a %d-byte read runs past the end of the table (%#x)", n, w.end)
	}
}

func (w *walker) u8() uint8 {
	w.need(1)
	v := w.data[w.o]
	w.o++
	return v
}

func (w *walker) u32() uint32 {
	w.need(4)
	v := binary.LittleEndian.Uint32(w.data[w.o:])
	w.o += 4
	return v
}

func (w *walker) u64() uint64 {
	w.need(8)
	v := binary.LittleEndian.Uint64(w.data[w.o:])
	w.o += 8
	return v
}

func (w *walker) u16() uint16 {
	w.need(2)
	v := binary.LittleEndian.Uint16(w.data[w.o:])
	w.o += 2
	return v
}

// pool reads a pool reference. In build mode it returns the string.
func (w *walker) pool() string {
	at := w.o
	ref := w.u32()
	if !w.db.validPoolRef(ref) {
		w.o = at
		w.fail("pool reference %#x does not point at a string", ref)
	}
	if !w.build {
		return ""
	}
	if s, ok := w.strs[ref]; ok {
		return s
	}
	s, _ := w.db.poolString(ref)
	if !utf8.ValidString(s) {
		s = strings.ToValidUTF8(s, "\uFFFD")
	}
	w.strs[ref] = s
	return s
}

// fstring reads an inline FString: i32 length counting the NUL, the bytes. In
// check mode it returns "" (the string is not made).
func (w *walker) fstring() string { return w.fstringIf(w.build) }

// [C.Y.B :: CoA]

// fstringIf is fstring, making the string when want is set.
func (w *walker) fstringIf(want bool) string {
	at := w.o
	n := int32(w.u32())
	if n == 0 {
		return ""
	}
	if n < 0 {
		w.o = at
		w.fail("FString with negative length %d (UTF-16 is not supported)", n)
	}
	w.need(int(n))
	b := w.data[w.o : w.o+int(n)]
	if b[n-1] != 0 {
		w.o = at
		w.fail("FString of %d bytes is not NUL-terminated", n)
	}
	w.o += int(n)
	if !want {
		return ""
	}
	s := string(b[:n-1])
	if !utf8.ValidString(s) {
		s = strings.ToValidUTF8(s, "\uFFFD")
	}
	return s
}

// instanced reads an FInstancedStruct as FInstancedStruct::Serialize writes it
// through this archive: the struct's path as an inline FString ("" when none
// is set), an i32 size, and the struct's own serialization in exactly size
// bytes. The struct is the path's last segment ("/Script/Pkg.Name" is Name),
// walked by its mappings. The value is a Struct of Type (the path) and Value,
// or nil when none is set.
func (w *walker) instanced() Value {
	path := w.fstringIf(true)
	at := w.o
	size := int32(w.u32())
	if size < 0 {
		w.o = at
		w.fail("InstancedStruct size %d", size)
	}
	if path == "" {
		if size != 0 {
			w.o = at
			w.fail("InstancedStruct of no struct with %d bytes of data", size)
		}
		return nil
	}
	name := path[strings.LastIndexAny(path, "./:")+1:]
	if w.d.m.Schema(name) == nil {
		w.o = at
		w.fail("InstancedStruct of %s: the mappings have no struct %s", path, name)
	}
	start := w.o
	w.push("(" + name + ")")
	v := w.readStruct(name)
	w.pop()
	if got := w.o - start; got != int(size) {
		w.o = start
		w.fail("InstancedStruct of %s: its properties take %d bytes, the size says %d", path, got, size)
	}
	if !w.build {
		return nil
	}
	return Struct{"Type": path, "Value": v}
}

func (w *walker) text() Value {
	w.u32() // flags
	switch h := int8(w.u8()); h {
	case 0: // base: namespace, key, source
		ns, key, src := w.fstring(), w.fstring(), w.fstring()
		return Text{Namespace: ns, Key: key, Source: src}
	case 11: // string table entry: the table's name (a pool reference), the key
		table := w.pool()
		return Text{History: 11, Table: table, Key: w.fstring()}
	case -1: // none: a culture-invariant string, if flagged
		t := Text{History: -1}
		if w.u32() != 0 {
			t.Source = w.fstring()
		}
		return t
	default:
		w.o--
		w.fail("FText history type %d is not supported", h)
		return nil
	}
}

// removed reads the first part of a TMap or TSet property as Unreal serializes
// it (FMapProperty / FSetProperty::SerializeItem): an int32 count of keys (or
// elements) to remove from the property's default, then those keys. They are
// read as the rule says and dropped: a database row has no default to remove
// them from, and every one of the file's maps and sets has a count of 0.
func (w *walker) removed(key *usmap.Type) {
	n := w.count(w.d.minSize(key))
	for i := 0; i < n; i++ {
		w.pushIdx(i)
		w.readValue(key, false)
		w.pop()
	}
}

// ~ C.Y.B - C-O-A ~

// count reads an array or map element count, where each element takes at
// least elemSize bytes; a count that cannot fit in the rest of the table fails.
func (w *walker) count(elemSize int) int {
	at := w.o
	n := w.u32()
	if need := uint64(n) * uint64(elemSize); need > uint64(w.end-w.o) {
		left := w.end - w.o
		w.o = at
		w.fail("count %d needs at least %d bytes, %d are left", n, need, left)
	}
	if w.tracing() {
		w.traced(at, "count", int64(n))
	}
	return int(n)
}

func (w *walker) push(name string)   { w.path = append(w.path, pathElem{name: name, idx: -1}) }
func (w *walker) pushIdx(i int)      { w.path = append(w.path, pathElem{idx: i}) }
func (w *walker) pop()               { w.path = w.path[:len(w.path)-1] }
func (w *walker) pathString() string { return formatPath(w.path) }

func formatPath(p []pathElem) string {
	var b strings.Builder
	for _, e := range p {
		if e.idx >= 0 {
			b.WriteString("[" + strconv.Itoa(e.idx) + "]")
			continue
		}
		if b.Len() > 0 {
			b.WriteByte('.')
		}
		b.WriteString(e.name)
	}
	return b.String()
}

// readValue reads one value of type t. In build mode it returns it; otherwise
// it only moves the cursor (and returns nil).
func (w *walker) readValue(t *usmap.Type, byteEnum bool) Value {
	switch t.Kind {
	case usmap.KindBool:
		b := w.u8()
		if b > 1 {
			w.o--
			w.fail("bool byte %d", b)
		}
		return b == 1
	case usmap.KindByte:
		return int64(w.u8())
	case usmap.KindInt8:
		return int64(int8(w.u8()))
	case usmap.KindInt16:
		return int64(int16(w.u16()))
	case usmap.KindUInt16:
		return int64(w.u16())
	case usmap.KindInt:
		return int64(int32(w.u32()))
	case usmap.KindUInt32:
		return int64(w.u32())
	case usmap.KindInt64:
		return int64(w.u64())
	case usmap.KindUInt64:
		return w.u64()
	case usmap.KindFloat:
		return math.Float32frombits(w.u32())
	case usmap.KindDouble:
		return math.Float64frombits(w.u64())
	case usmap.KindName, usmap.KindStr, usmap.KindUnknown, usmap.KindSoftObject, usmap.KindObject:
		return w.pool()
	case usmap.KindEnum:
		if !byteEnum {
			return w.pool()
		}
		v := int64(w.u8())
		if e := w.d.m.Enum(t.Enum); e != nil {
			if name, ok := e.ValueName(v); ok {
				return t.Enum + "::" + name
			}
		}
		return v
	case usmap.KindText:
		return w.text()
	case usmap.KindOptional:
		at := w.o
		switch set := w.u32(); set {
		case 0:
			return nil
		case 1:
			return w.readValue(t.Inner, false)
		default:
			w.o = at
			w.fail("optional set flag %d", set)
		}
	case usmap.KindArray, usmap.KindSet:
		if t.Kind == usmap.KindSet {
			w.removed(t.Inner)
		}
		n := w.count(w.d.minSize(t.Inner))
		var out []Value
		if w.build {
			out = make([]Value, 0, n)
		}
		for i := 0; i < n; i++ {
			w.pushIdx(i)
			v := w.readValue(t.Inner, false)
			w.pop()
			if w.build {
				out = append(out, v)
			}
		}
		return out
	case usmap.KindMap:
		w.removed(t.Inner)
		n := w.count(w.d.minSize(t.Inner) + w.d.minSize(t.Value))
		var out Pairs
		if w.build {
			out = make(Pairs, 0, n)
		}
		for i := 0; i < n; i++ {
			w.pushIdx(i)
			w.push("key")
			k := w.readValue(t.Inner, false)
			w.pop()
			w.push("value")
			v := w.readValue(t.Value, false)
			w.pop()
			w.pop()
			if w.build {
				out = append(out, Pair{k, v})
			}
		}
		return out
	case usmap.KindStruct:
		return w.readStruct(t.Struct)
	}
	w.fail("property type %s is not supported", t.Kind)
	return nil
}

// built by CoA with C_Y_B

// readStruct reads a struct value. Four structs are flattened (see Value) and
// an InstancedStruct is read as its runtime struct.
func (w *walker) readStruct(name string) Value {
	switch name {
	case "AoCExpression", "SoftObjectPath", "SoftClassPath":
		return w.pool()
	case instancedStruct:
		return w.instanced()
	case "GameplayTagContainer":
		n := w.count(4) // each GameplayTag is one pool reference
		var out []Value
		if w.build {
			out = make([]Value, 0, n)
		}
		for i := 0; i < n; i++ {
			w.pushIdx(i)
			s := w.pool() // GameplayTag is one Name
			w.pop()
			if w.build {
				out = append(out, s)
			}
		}
		return out
	}
	p, err := w.d.plan(name, false)
	if err != nil {
		w.fail("%v", err)
	}
	return w.readPlan(p)
}

func (w *walker) readPlan(p *plan) Value {
	var out Struct
	if w.build {
		out = make(Struct, len(p.steps))
	}
	inherits := false
	for _, s := range p.steps {
		w.push(s.name)
		at := w.o
		var v Value
		switch {
		case p.isID && s.name == "TypeId" && s.t.Kind == usmap.KindInt64:
			tid := w.u64()
			if tid != 0 {
				w.ids[p.name] = tid
			}
			v = int64(tid)
		case p.record && s.name == "ParentGuid" && s.t.Kind == usmap.KindInt64:
			pg := w.u64()
			inherits = pg != 0
			v = int64(pg)
		default:
			v = w.readValue(s.t, s.byteEnum)
		}
		if w.tracing() && s.t.Kind != usmap.KindStruct {
			w.traced(at, s.t.Kind.String(), v)
		}
		w.pop()
		if w.build {
			out[s.name] = v
		}
	}
	if inherits {
		// A row with a parent ends with 4 more bytes; they are 0 in every such
		// row of the file, and nothing else is known about them.
		at := w.o
		if tail := w.u32(); tail != 0 {
			w.o = at
			w.fail("the inherited row's tail is %#x; only 0 is known", tail)
		}
		if w.tracing() {
			w.traced(at, "inherited row tail", int64(0))
		}
	}
	if !w.build {
		return nil
	}
	return out
}

// recordProps are the DesignDataRecordBase properties every row ends with.
var recordProps = []struct {
	name string
	kind usmap.Kind
}{
	{"Guid", usmap.KindInt64}, {"ParentGuid", usmap.KindInt64}, {"Name", usmap.KindName}, {"bDevOnly", usmap.KindBool},
}

// recordPlan returns the plan of a record schema after checking that it is
// one: its chain ends in DesignDataRecordBase, which has the four properties a
// row ends with.
func (d *Decoder) recordPlan(schema string) (*plan, error) {
	chain, err := d.m.Chain(schema)
	if err != nil {
		return nil, err
	}
	if len(chain) < 2 || chain[len(chain)-1].Name != recordRoot {
		return nil, fmt.Errorf("designdb: %s is not a record schema (its chain does not end in %s)", schema, recordRoot)
	}
	root := chain[len(chain)-1]
	for _, want := range recordProps {
		found := false
		for _, p := range root.Props {
			found = found || p.Name == want.name && p.Type.Kind == want.kind
		}
		if !found {
			return nil, fmt.Errorf("designdb: %s has no %s %s", recordRoot, want.kind, want.name)
		}
	}
	return d.plan(schema, true)
}

// CoA / C Y B

// walk decodes table t's rows as plan p. It returns the rows read before any
// failure (all of them when err is nil). In non-build mode no rows are made.
func (d *Decoder) walk(t TableInfo, schema string, p *plan, build bool, ids map[string]uint64) (rows []Row, err error) {
	return d.walkTraced(t, schema, p, build, ids, nil, 0)
}

// WalkFrom reads up to rows rows of table i as schema starting at file offset
// offset (inside the table; the rows need not end on the table's last byte) and
// returns the rows read before any failure. It is for investigating a table
// the walk fails in: what follows the failure, row by row.
func (d *Decoder) WalkFrom(table int, schema string, offset uint64, rows uint32) ([]Row, error) {
	t, ok := d.db.Table(table)
	if !ok {
		return nil, fmt.Errorf("designdb: no table %d", table)
	}
	if offset < t.Offset || offset > t.End() {
		return nil, fmt.Errorf("designdb: offset %#x is outside table %d", offset, table)
	}
	p, err := d.recordPlan(schema)
	if err != nil {
		return nil, err
	}
	part := TableInfo{Index: t.Index, TypeID: t.TypeID, Offset: offset, Size: t.End() - offset, Rows: rows}
	got, err := d.walkTraced(part, schema, p, true, nil, nil, 0)
	if de, ok := err.(*DecodeError); ok && de.Row == int(rows) && de.Path == "" {
		return got, nil // the rows read; they were not meant to end on the table's last byte
	}
	return got, err
}

// Trace writes, for every value the walk of table i as schema reads from row
// fromRow on, a line "offset row path: kind = value" to w, until the walk ends
// or fails (the error is returned). With offset not 0 the walk starts at that
// file offset inside the table instead of its first byte (see WalkFrom). It is
// for finding where a struct and a table disagree.
func (d *Decoder) Trace(table int, schema string, offset uint64, fromRow int, w io.Writer) error {
	t, ok := d.db.Table(table)
	if !ok {
		return fmt.Errorf("designdb: no table %d", table)
	}
	if offset != 0 {
		if offset < t.Offset || offset > t.End() {
			return fmt.Errorf("designdb: offset %#x is outside table %d", offset, table)
		}
		t = TableInfo{Index: t.Index, TypeID: t.TypeID, Offset: offset, Size: t.End() - offset, Rows: t.Rows}
	}
	p, err := d.recordPlan(schema)
	if err != nil {
		return err
	}
	_, err = d.walkTraced(t, schema, p, true, nil, w, fromRow)
	return err
}

func (d *Decoder) walkTraced(t TableInfo, schema string, p *plan, build bool, ids map[string]uint64, trace io.Writer, traceFrom int) (rows []Row, err error) {
	w := &walker{
		d: d, db: d.db, data: d.db.data,
		o: int(t.Offset), end: int(t.End()),
		build: build, ids: ids,
		trace: trace, traceFrom: traceFrom,
	}
	if build {
		w.strs = map[uint32]string{}
		rows = make([]Row, 0, t.Rows)
	}
	if w.ids == nil {
		w.ids = map[string]uint64{}
	}
	align := 8
	if a := d.facts.RowAlign[schema]; a > 8 {
		align = a
	}
	row, rowStart := 0, w.o
	defer func() {
		r := recover()
		if r == nil {
			return
		}
		f, ok := r.(*failure)
		if !ok {
			panic(r)
		}
		err = &DecodeError{
			Table: t.Index, Schema: schema, Row: row,
			RowOffset: int64(rowStart), Offset: int64(f.off),
			Path: w.pathString(), Msg: f.msg,
		}
	}()
	for ; row < int(t.Rows); row++ {
		w.o = (w.o + align - 1) &^ (align - 1) // a row starts aligned to its record type
		rowStart = w.o
		w.row = row
		w.path = w.path[:0]
		if w.tracing() {
			fmt.Fprintf(w.trace, "%#x row %d starts\n", w.o, row)
		}
		v := w.readPlan(p)
		if build {
			rows = append(rows, makeRow(int64(rowStart), int64(w.o), v.(Struct)))
		}
	}
	w.path = w.path[:0]
	if w.o != w.end {
		rowStart = w.o
		diff := w.end - w.o
		how := fmt.Sprintf("%d bytes short of", diff)
		if diff < 0 {
			how = fmt.Sprintf("%d bytes past", -diff)
		}
		panic(&failure{off: w.o, msg: fmt.Sprintf("the %d rows end at %#x, %s the table end %#x", t.Rows, w.o, how, w.end)})
	}
	return rows, nil
}

// [Cyb :: C.O.A]

func makeRow(off, end int64, s Struct) Row {
	r := Row{Offset: off, End: end, Fields: s}
	r.Guid, _ = s["Guid"].(int64)
	r.ParentGuid, _ = s["ParentGuid"].(int64)
	r.Name, _ = s["Name"].(string)
	r.DevOnly, _ = s["bDevOnly"].(bool)
	delete(s, "Guid")
	delete(s, "ParentGuid")
	delete(s, "Name")
	delete(s, "bDevOnly")
	return r
}

// Decode reads every row of table i as the record struct schema (for example
// "StatTypeDefRecordBase"). It is all or nothing: the rows must end exactly on
// the table's last byte, and when they do not, or any value does not fit, the
// result is a *DecodeError and no rows.
func (d *Decoder) Decode(table int, schema string) ([]Row, error) {
	rows, err := d.DecodePartial(table, schema)
	if err != nil {
		return nil, err
	}
	return rows, nil
}

// DecodePartial is Decode for diagnosis: on failure it also returns the rows
// that were read completely before the byte the walk failed at. Such rows are
// not a decoded table; do not use them as one.
func (d *Decoder) DecodePartial(table int, schema string) ([]Row, error) {
	t, ok := d.db.Table(table)
	if !ok {
		return nil, fmt.Errorf("designdb: no table %d", table)
	}
	p, err := d.recordPlan(schema)
	if err != nil {
		return nil, err
	}
	return d.walk(t, schema, p, true, nil)
}

// Check walks table i as the record struct schema without making values: nil
// when every row reads and the rows end exactly on the table's last byte.
func (d *Decoder) Check(table int, schema string) error {
	t, ok := d.db.Table(table)
	if !ok {
		return fmt.Errorf("designdb: no table %d", table)
	}
	p, err := d.recordPlan(schema)
	if err != nil {
		return err
	}
	_, err = d.walk(t, schema, p, false, nil)
	return err
}

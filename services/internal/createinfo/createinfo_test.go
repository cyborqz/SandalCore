// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package createinfo

import (
	"bytes"
	"encoding/hex"
	"math"
	"os"
	"strings"
	"testing"

	"google.golang.org/protobuf/encoding/protowire"
)

// entry is a map<string,string> entry: key at field 1, value at field 2.
func entry(key, val string) []byte {
	var b []byte
	b = protowire.AppendTag(b, 1, protowire.BytesType)
	b = protowire.AppendString(b, key)
	b = protowire.AppendTag(b, 2, protowire.BytesType)
	return protowire.AppendString(b, val)
}

// wrapper holds map entries at field 2, as the client's field 1 does.
func wrapper(entries ...[]byte) []byte {
	var b []byte
	for _, e := range entries {
		b = protowire.AppendTag(b, 2, protowire.BytesType)
		b = protowire.AppendBytes(b, e)
	}
	return b
}

// varintField is one varint field of a payload.
func varintField(num protowire.Number, v uint64) []byte {
	return protowire.AppendVarint(protowire.AppendTag(nil, num, protowire.VarintType), v)
}

func cat(parts ...[]byte) []byte { return bytes.Join(parts, nil) }

// payload is what the client sends: field 1 wraps the customization, fields 2,
// 3 and 4 are the gender, race and class (a zero is left out: proto3 does not
// write it) and field 5 the starting location.
func payload(wrap []byte, gender, race, class uint64) []byte {
	parts := [][]byte{protowire.AppendBytes(protowire.AppendTag(nil, 1, protowire.BytesType), wrap)}
	for i, v := range []uint64{gender, race, class} {
		if v != 0 {
			parts = append(parts, varintField(protowire.Number(2+i), v))
		}
	}
	return cat(append(parts, protowire.AppendString(protowire.AppendTag(nil, 5, protowire.BytesType), "Riverlands"))...)
}

// A payload of the real client, a Kaelar male Bard (plan B1): its field 4 is 2
// and it has neither field 2 nor field 3, because Male and Kaelar are 0. Until
// B1 the services read that 2 as the race.
func TestParseTheRealClientsPayload(t *testing.T) {
	raw, err := os.ReadFile("testdata/client_create_info.hex")
	if err != nil {
		t.Fatal(err)
	}
	in, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		t.Fatal(err)
	}
	got := Parse(in)
	if got.Gender != 0 || got.Race != 0 || got.Class != 2 {
		t.Errorf("gender %d race %d class %d, want 0 0 2 (Male, Kaelar, Bard)", got.Gender, got.Race, got.Class)
	}
	for key, want := range map[string]string{"race": "Kaelar", "gender": "Male", "class": "Bard"} {
		if v := JSONString(got.Appearance, key); v != want {
			t.Errorf("customization %s = %q, want %q", key, v, want)
		}
	}
	if len(got.Customization) == 0 || got.Appearance == "" {
		t.Errorf("no customization: %d bytes, %d characters of JSON", len(got.Customization), len(got.Appearance))
	}
}

// [C-Y-B x CoA]

// The three fields and the customization each have their own place.
func TestParseTheThreeFields(t *testing.T) {
	const js = `{"class":"Tank","gender":"Female","race":"Dunir"}`
	w := wrapper(entry("CustomizationDataKey", js))
	got := Parse(payload(w, 1, 6, 7))
	if got.Gender != 1 || got.Race != 6 || got.Class != 7 || got.Appearance != js || !bytes.Equal(got.Customization, w) {
		t.Errorf("Parse = %+v", got)
	}
	// Zeros are absent from the wire and parse as zero.
	if got := Parse(payload(w, 0, 0, 2)); got.Gender != 0 || got.Race != 0 || got.Class != 2 {
		t.Errorf("Parse = %+v, want gender 0 race 0 class 2", got)
	}
}

func TestParseNothing(t *testing.T) {
	for _, in := range [][]byte{nil, {}} {
		if got := Parse(in); got.Gender != 0 || got.Race != 0 || got.Class != 0 || got.Customization != nil || got.Appearance != "" {
			t.Errorf("Parse(%x) = %+v, want zero", in, got)
		}
	}
}

// The customization is the first field 1, even when its JSON is unusable;
// the raw wrapper is kept either way, because the client gets it back as is.
func TestCustomizationIsTheFirstFieldOne(t *testing.T) {
	first := wrapper(entry("CustomizationDataKey", "not json"))
	second := wrapper(entry("CustomizationDataKey", `{"a":1}`))
	var in []byte
	for _, w := range [][]byte{first, second} {
		in = protowire.AppendTag(in, 1, protowire.BytesType)
		in = protowire.AppendBytes(in, w)
	}
	got := Parse(in)
	if !bytes.Equal(got.Customization, first) || got.Appearance != "" {
		t.Errorf("Parse = %+v, want the first wrapper and no appearance", got)
	}
}

func TestJSONString(t *testing.T) {
	const js = `{"class":"Mage","level":3,"gender":null}`
	for _, tc := range []struct{ object, key, want string }{
		{js, "class", "Mage"},
		{js, "level", ""},  // not a string
		{js, "gender", ""}, // null
		{js, "race", ""},   // missing
		{"not json", "class", ""},
		{`["class"]`, "class", ""},
		{"", "class", ""},
	} {
		if got := JSONString(tc.object, tc.key); got != tc.want {
			t.Errorf("JSONString(%q, %q) = %q, want %q", tc.object, tc.key, got, tc.want)
		}
	}
}

// The first varint of each of the fields 2, 3 and 4 counts, and it must fit an
// int32.
func TestParseIds(t *testing.T) {
	for _, num := range []protowire.Number{2, 3, 4} {
		field := func(in Info) int32 { return [...]int32{2: in.Gender, 3: in.Race, 4: in.Class}[num] }
		for _, tc := range []struct {
			name string
			in   []byte
			want int32
		}{
			{"first of two wins", cat(varintField(num, 7), varintField(num, 8)), 7},
			{"int32 max fits", varintField(num, math.MaxInt32), math.MaxInt32},
			{"int32 max+1 is invalid", varintField(num, math.MaxInt32+1), Invalid},
			{"a negative enum (ten bytes) is invalid", varintField(num, math.MaxUint64), Invalid},
			{"a later valid field is not looked at after an oversized one", cat(varintField(num, math.MaxInt32+1), varintField(num, 7)), Invalid},
			{"the wrong wire type is skipped", cat(protowire.AppendString(protowire.AppendTag(nil, num, protowire.BytesType), "x"), varintField(num, 5)), 5},
			{"absent is 0", nil, 0},
		} {
			if got := field(Parse(tc.in)); got != tc.want {
				t.Errorf("field %d, %s: got %d, want %d", num, tc.name, got, tc.want)
			}
		}
	}
	// Each field is independent of the others.
	if got := Parse(cat(varintField(4, 3), varintField(2, 1))); got.Gender != 1 || got.Race != 0 || got.Class != 3 {
		t.Errorf("Parse = %+v", got)
	}
}

// What was decoded before bad bytes is kept, and a bad tag ends the walk: it
// does not resynchronise on later bytes.
func TestParseBadBytesEndTheWalk(t *testing.T) {
	w := wrapper(entry("CustomizationDataKey", `{"a":1}`))
	field1 := protowire.AppendBytes(protowire.AppendTag(nil, 1, protowire.BytesType), w)
	for _, tc := range []struct {
		name string
		in   []byte
		race int32
		cust []byte
	}{
		{"race, then a field 1 cut short", cat(varintField(3, 7), []byte{0x0a, 0x05, 'a'}), 7, nil},
		{"field 1, then a race varint cut short", cat(field1, []byte{0x18, 0x80}), 0, w},
		{"race, then a bad tag", cat(varintField(3, 7), []byte{0x80}), 7, nil},
		{"field 1, then a bad tag", cat(field1, []byte{0x80}), 0, w},
		{"field 1, then a skipped field cut short", cat(field1, []byte{0x2a, 0x09, 'x'}, varintField(3, 7)), 0, w},
		{"field number 0 stops the walk", cat([]byte{0x00}, field1, varintField(3, 7)), 0, nil},
	} {
		got := Parse(tc.in)
		if got.Race != tc.race || !bytes.Equal(got.Customization, tc.cust) {
			t.Errorf("%s: Parse = %+v, want race %d customization %x", tc.name, got, tc.race, tc.cust)
		}
	}
}

// JSON keys are case-sensitive (encoding/json into a struct would not be).
func TestJSONStringKeyIsCaseSensitive(t *testing.T) {
	if got := JSONString(`{"Class":"Mage"}`, "class"); got != "" {
		t.Errorf("JSONString matched %q case-insensitively", got)
	}
}

// An empty first field 1 is still the first: the customization is empty, and
// a later field 1 is not used instead (the legacy server does the same).
func TestParseEmptyFirstField1(t *testing.T) {
	w := wrapper(entry("CustomizationDataKey", `{"a":1}`))
	in := cat(protowire.AppendTag(nil, 1, protowire.BytesType), []byte{0}, payload(w, 0, 7, 2))
	if got := Parse(in); len(got.Customization) != 0 || got.Appearance != "" || got.Race != 7 || got.Class != 2 {
		t.Errorf("Parse = %+v", got)
	}
}

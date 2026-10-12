// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package legacyimport

// DB-free tests for the edge branches of createinfo.Parse and of this
// package's Parse: the int32 race boundary, truncated payloads, malformed wrappers and map entries, error
// indexes, exact-case gender names and ids passed through unchanged. They use
// the helpers in legacyimport_test.go (mapEntry, customization, raceField, cat,
// looks, parseOne).

import (
	"encoding/hex"
	"math"
	"strings"
	"testing"

	"google.golang.org/protobuf/encoding/protowire"
)

const custKey = "CustomizationDataKey"

func strField(num protowire.Number, s string) []byte {
	return protowire.AppendString(protowire.AppendTag(nil, num, protowire.BytesType), s)
}

func varintField(num protowire.Number, v uint64) []byte {
	return protowire.AppendVarint(protowire.AppendTag(nil, num, protowire.VarintType), v)
}

// entryOf is one field 2 (a map entry) of a wrapper, from raw entry bytes.
func entryOf(e []byte) []byte {
	return protowire.AppendBytes(protowire.AppendTag(nil, 2, protowire.BytesType), e)
}

// withWrapper is a field 1 whose wrapper holds exactly the given raw bytes.
func withWrapper(w []byte) []byte {
	return protowire.AppendBytes(protowire.AppendTag(nil, 1, protowire.BytesType), w)
}

func appearanceOf(t *testing.T, info []byte) string {
	t.Helper()
	return parseOne(t, map[string]any{"create_info_hex": hex.EncodeToString(info)}).AppearanceJSON
}

func TestParseRaceAtTheInt32Boundary(t *testing.T) {
	for name, tc := range map[string]struct {
		v    uint64
		want int32
	}{
		"MaxInt32 fits":          {math.MaxInt32, math.MaxInt32},
		"MaxInt32+1 is invalid":  {math.MaxInt32 + 1, -1},
		"MaxUint64 is invalid":   {math.MaxUint64, -1},
		"a small value is taken": {17, 17},
	} {
		c := parseOne(t, map[string]any{"create_info_hex": hex.EncodeToString(raceField(tc.v))})
		if c.RaceID != tc.want {
			t.Errorf("%s: race %d, want %d", name, c.RaceID, tc.want)
		}
	}
}

func TestParseKeepsAppearanceWhenTheTailIsTruncated(t *testing.T) {
	cust := customization(mapEntry(custKey, looks))
	for name, tail := range map[string][]byte{
		"a race varint that never ends":  {0x20, 0x80},
		"a fixed32 with one byte":        {0x2d, 0x01},
		"a tag that never ends":          {0x80},
		"a bytes field longer than data": {0x32, 0x7f, 0x01},
	} {
		c := parseOne(t, map[string]any{"create_info_hex": hex.EncodeToString(cat(cust, tail))})
		if c.AppearanceJSON != looks || c.RaceID != 0 || c.ClassID != 2 { // the class is the customization's Bard
			t.Errorf("%s: race %d class %d appearance %q", name, c.RaceID, c.ClassID, c.AppearanceJSON)
		}
	}
}

// C | O | A : C-Y-B

func TestParseWrapperFieldsOtherThanEntriesAreSkipped(t *testing.T) {
	entry := entryOf(mapEntry(custKey, looks))
	for name, w := range map[string][]byte{
		"a varint field before the entry": cat(varintField(3, 7), entry),
		"a bytes field before the entry":  cat(strField(5, "noise"), entry),
		"unknown fields around the entry": cat(varintField(3, 7), entry, strField(5, "noise")),
	} {
		if got := appearanceOf(t, withWrapper(w)); got != looks {
			t.Errorf("%s: appearance %q", name, got)
		}
	}
}

func TestParseMalformedWrapperGivesNoAppearance(t *testing.T) {
	for name, w := range map[string][]byte{
		"an entry longer than the wrapper":        {0x12, 0x7f, 0x01},
		"a truncated field after a foreign entry": cat(entryOf(mapEntry("Other", looks)), []byte{0x2d, 0x01}),
		"a tag that never ends":                   {0x80},
		"a truncated field and no entries":        {0x2d, 0x01},
	} {
		if got := appearanceOf(t, withWrapper(w)); got != "" {
			t.Errorf("%s: appearance %q, want none", name, got)
		}
	}
}

func TestParseEntryReading(t *testing.T) {
	for name, tc := range map[string]struct {
		entry []byte
		want  string
	}{
		"a varint at field 1 is skipped, the key follows": {cat(varintField(1, 300), strField(1, custKey), strField(2, looks)), looks},
		"the last key wins":                            {cat(strField(1, "Other"), strField(1, custKey), strField(2, looks)), looks},
		"the last value wins":                          {cat(strField(1, custKey), strField(2, `{"x":1}`), strField(2, looks)), looks},
		"the value may come before the key":            {cat(strField(2, looks), strField(1, custKey)), looks},
		"an unknown field inside the entry is skipped": {cat(strField(1, custKey), varintField(3, 9), strField(2, looks)), looks},
		"a key without a value":                        {strField(1, custKey), ""},
		// A malformed entry is skipped whole, even if its key and value came
		// before the bad bytes.
		"garbage tag after key and value":      {cat(mapEntry(custKey, looks), []byte{0x80}), ""},
		"oversize field 2 after key and value": {cat(mapEntry(custKey, looks), []byte{0x12, 0x7f}), ""},
		"truncated field after key and value":  {cat(mapEntry(custKey, looks), []byte{0x2d, 0x01}), ""},
		"oversize key":                         {[]byte{0x0a, 0x7f, 0x01}, ""},
	} {
		if got := appearanceOf(t, customization(tc.entry)); got != tc.want {
			t.Errorf("%s: appearance %q, want %q", name, got, tc.want)
		}
	}
	// A malformed entry does not hide the entries after it.
	bad := cat(mapEntry(custKey, `{"a":1}`), []byte{0x80})
	if got := appearanceOf(t, customization(bad, mapEntry(custKey, looks))); got != looks {
		t.Errorf("entry after a malformed one: appearance %q", got)
	}
}

func TestParseErrorNamesTheRecordIndex(t *testing.T) {
	in := `[{"character_id":"00000000000000000000000000000001","character_name":"Alpha"},
	        {"character_id":"00000000000000000000000000000002","character_name":"Bravo","create_info_hex":"zz"}]`
	_, err := Parse(strings.NewReader(in), 1)
	if err == nil || !strings.Contains(err.Error(), "records[1] (Bravo)") {
		t.Errorf("error %v should name records[1] (Bravo)", err)
	}
}

func TestParseGenderNamesAreExactCase(t *testing.T) {
	// The enum names are exact: anything else is no claim about the gender, and
	// the payload's gender (0, Male) stands.
	for _, name := range []string{"male", "FEMALE", "MALE", "female", " Female", "Female "} {
		if c := parseOne(t, map[string]any{"gender_name": name}); c.GenderID != 0 {
			t.Errorf("gender_name %q: gender %d, want the payload's 0", name, c.GenderID)
		}
	}
}

func TestParsePassesIDAndNameThroughUnchanged(t *testing.T) {
	// Judging them is the store's job (it rejects an uppercase id and a
	// padded name as invalid); Parse must not quietly repair them.
	c := parseOne(t, map[string]any{"character_id": "ABCDEF00000000000000000000000001", "character_name": " Bob Smith "})
	if c.ID != "ABCDEF00000000000000000000000001" || c.Name != " Bob Smith " {
		t.Errorf("id %q name %q were changed", c.ID, c.Name)
	}
}

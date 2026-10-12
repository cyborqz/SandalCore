// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package legacyimport

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"reflect"
	"strings"
	"testing"
	"testing/iotest"

	"google.golang.org/protobuf/encoding/protowire"

	"aocrop/services/internal/character"
)

const sample = `[{
  "archetype_id": 17747,
  "character_id": "bbec5c86eb101fe5f38c87f0fa22e7d0",
  "character_name": "cyb",
  "class_name": "Bard",
  "create_info_hex": "0afc32",
  "created_at_ms": 1778257423202,
  "gender_id": 0,
  "gender_name": "Male",
  "level": 1,
  "race_id": 0,
  "race_name": "Kaelar",
  "world_id": "aoc-emu-useast2-local"
}]`

func TestParse(t *testing.T) {
	got, err := Parse(strings.NewReader(sample), 42)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 {
		t.Fatalf("got %d characters", len(got))
	}
	c := got[0]
	// sample's create_info_hex is only the truncated header of a real payload
	// (field 1, length 6524, no body), so it carries no ids: Kaelar (0) and Male
	// (0) are the zeros, which agree with race_name and gender_name, and the
	// class is the file's class_name, Bard (2).
	if c.ID != "bbec5c86eb101fe5f38c87f0fa22e7d0" || c.AccountID != 42 || c.Name != "cyb" ||
		c.ArchetypeID != 17747 || c.ClassName != "Bard" || c.RaceID != 0 || c.GenderID != 0 || c.ClassID != 2 ||
		c.Level != 1 || c.AppearanceJSON != "" || string(c.CreateInfo) != "\x0a\xfc\x32" {
		t.Errorf("parsed %+v", c)
	}
}

func TestParseRejectsBadHex(t *testing.T) {
	bad := strings.Replace(sample, `"0afc32"`, `"zz"`, 1)
	if _, err := Parse(strings.NewReader(bad), 1); err == nil {
		t.Error("want an error for bad create_info_hex")
	}
}

// [C Y B ~ CoA]

// Every mapped field carries a value that differs from the zero value and from
// every other numeric field of the same record, so a dropped, swapped or
// mis-tagged field cannot go unnoticed. race_id and gender_id are the legacy
// file's fields, which carry nothing and are ignored (as are race_name and
// gender_name when they are not enum names).
const multiSample = `[
 {"character_id":"00000000000000000000000000000001","character_name":"Alpha","class_name":"Mage",
  "archetype_id":17750,"race_id":3,"gender_id":1,"level":12,"create_info_hex":"0a01ff",
  "created_at_ms":1,"world_id":"w","race_name":"r","gender_name":"g"},
 {"character_id":"00000000000000000000000000000002","character_name":"Bravo","class_name":"Tank",
  "archetype_id":17754,"race_id":7,"gender_id":2,"level":30,"create_info_hex":"","created_at_ms":2},
 {"character_id":"00000000000000000000000000000003","character_name":"Charlie","class_name":"Rogue",
  "archetype_id":17752,"race_id":9,"gender_id":1,"level":2,"create_info_hex":"DEADbeef"}
]`

func TestParseMapsEveryFieldOfEveryRecord(t *testing.T) {
	got, err := Parse(strings.NewReader(multiSample), 99)
	if err != nil {
		t.Fatal(err)
	}
	want := []character.New{
		{ID: "00000000000000000000000000000001", AccountID: 99, Name: "Alpha", ClassID: 8,
			ArchetypeID: 17750, ClassName: "Mage", CreateInfo: []byte{0x0a, 0x01, 0xff}, Level: 12},
		{ID: "00000000000000000000000000000002", AccountID: 99, Name: "Bravo", ClassID: 7,
			ArchetypeID: 17754, ClassName: "Tank", CreateInfo: []byte{}, Level: 25}, // the file says 30: the game ends at 25
		{ID: "00000000000000000000000000000003", AccountID: 99, Name: "Charlie", ClassID: 5,
			ArchetypeID: 17752, ClassName: "Rogue", CreateInfo: []byte{0xde, 0xad, 0xbe, 0xef}, Level: 2},
	}
	if len(got) != len(want) {
		t.Fatalf("got %d characters, want %d", len(got), len(want))
	}
	for i := range want {
		// Empty and nil create_info are the same to the store; compare by content.
		if len(got[i].CreateInfo) == 0 && len(want[i].CreateInfo) == 0 {
			got[i].CreateInfo, want[i].CreateInfo = nil, nil
		}
		if !reflect.DeepEqual(got[i], want[i]) {
			t.Errorf("record %d:\n got  %+v\n want %+v", i, got[i], want[i])
		}
	}
}

func TestParseEmptyArrayIsNoCharacters(t *testing.T) {
	for _, in := range []string{"[]", " [ ] \r\n"} {
		got, err := Parse(strings.NewReader(in), 1)
		if err != nil || len(got) != 0 {
			t.Errorf("Parse(%q) = %d characters, %v; want none, nil", in, len(got), err)
		}
	}
}

func TestParseRejectsMalformedFiles(t *testing.T) {
	// The type-error records carry a valid id and name, so it is the bad field
	// that fails them, not the record validation.
	const idName = `"character_id":"00000000000000000000000000000001","character_name":"cyb",`
	for name, in := range map[string]string{
		"empty file":        "",
		"object not array":  `{"character_name":"cyb"}`,
		"string":            `"characters"`,
		"null":              `null`,
		"null with spaces":  " null \r\n",
		"array of numbers":  `[1]`,
		"truncated":         `[{"character_name":"cyb"`,
		"level as string":   `[{` + idName + `"level":"1"}]`,
		"level as fraction": `[{` + idName + `"level":1.5}]`,
		"archetype > int32": `[{` + idName + `"archetype_id":2147483648}]`,
	} {
		got, err := Parse(strings.NewReader(in), 1)
		if err == nil {
			t.Errorf("%s: no error", name)
			continue
		}
		if got != nil {
			t.Errorf("%s: returned %d characters alongside the error", name, len(got))
		}
		if !strings.HasPrefix(err.Error(), "characters.json: ") {
			t.Errorf("%s: error %q does not name the file", name, err)
		}
	}
}

const bom = "\xef\xbb\xbf"

// [CoA + C_Y_B]

func TestParseToleratesAByteOrderMark(t *testing.T) {
	got, err := Parse(strings.NewReader(bom+sample), 42)
	if err != nil || len(got) != 1 || got[0].Name != "cyb" {
		t.Fatalf("BOM then the sample: %d characters, %v", len(got), err)
	}
	if got, err := Parse(strings.NewReader(bom+"[]"), 1); err != nil || len(got) != 0 {
		t.Errorf("BOM then []: %d characters, %v", len(got), err)
	}
	// However the reader hands the bytes over, the BOM is found.
	if got, err := Parse(iotest.OneByteReader(strings.NewReader(bom+sample)), 42); err != nil || len(got) != 1 {
		t.Errorf("BOM, one byte at a time: %d characters, %v", len(got), err)
	}
	if got, err := Parse(iotest.OneByteReader(strings.NewReader(sample)), 42); err != nil || len(got) != 1 {
		t.Errorf("no BOM, one byte at a time: %d characters, %v", len(got), err)
	}
}

func TestParseOnlyAToleratesOneLeadingBOM(t *testing.T) {
	for name, in := range map[string]string{
		"two BOMs":              bom + bom + "[]",
		"BOM after whitespace":  " " + bom + "[]",
		"BOM after the array":   "[]" + bom,
		"BOM and nothing else":  bom,
		"a truncated BOM":       "\xef\xbb[]",
		"a byte shorter than 3": "[",
	} {
		got, err := Parse(strings.NewReader(in), 1)
		if err == nil || got != nil {
			t.Errorf("%s: %d characters, %v; want an error", name, len(got), err)
		}
	}
}

func TestParseRejectsContentAfterTheArray(t *testing.T) {
	for name, tail := range map[string]string{
		"a second array":        sample,
		"an empty second array": " []",
		"an object":             `{}`,
		"garbage":               `x`,
		"a stray bracket":       `]`,
		"a comma":               `,`,
		"null":                  `null`,
		"a number":              `1`,
		"junk after whitespace": "\r\n\r\n junk",
	} {
		got, err := Parse(strings.NewReader(sample+tail), 1)
		if err == nil {
			t.Errorf("%s: no error", name)
			continue
		}
		if got != nil {
			t.Errorf("%s: returned %d characters alongside the error", name, len(got))
		}
		if !strings.HasPrefix(err.Error(), "characters.json: ") {
			t.Errorf("%s: error %q does not name the file", name, err)
		}
	}
}

func TestParseAcceptsTrailingWhitespace(t *testing.T) {
	got, err := Parse(strings.NewReader(sample+" \r\n\t\n"), 1)
	if err != nil || len(got) != 1 {
		t.Errorf("%d characters, %v", len(got), err)
	}
}

func TestParseRejectsRecordsWithoutIDOrName(t *testing.T) {
	const good = `{"character_id":"00000000000000000000000000000001","character_name":"Alpha"}`
	for name, tc := range map[string]struct{ rec, field string }{
		"no id":      {`{"character_name":"Bravo"}`, "character_id"},
		"empty id":   {`{"character_id":"","character_name":"Bravo"}`, "character_id"},
		"null id":    {`{"character_id":null,"character_name":"Bravo"}`, "character_id"},
		"no name":    {`{"character_id":"00000000000000000000000000000002"}`, "character_name"},
		"empty name": {`{"character_id":"00000000000000000000000000000002","character_name":""}`, "character_name"},
		"both empty": {`{"character_id":"","character_name":""}`, "character_id"},
	} {
		// After a good record, the error names index 1 and returns nothing.
		got, err := Parse(strings.NewReader(`[`+good+`,`+tc.rec+`]`), 1)
		if err == nil {
			t.Errorf("%s: no error", name)
			continue
		}
		if got != nil {
			t.Errorf("%s: returned %d characters alongside the error", name, len(got))
		}
		if !strings.HasPrefix(err.Error(), "characters.json: ") || !strings.Contains(err.Error(), "records[1]") ||
			!strings.Contains(err.Error(), tc.field) {
			t.Errorf("%s: error %q should name the file, records[1] and %s", name, err, tc.field)
		}
		// As the first record it is index 0, and a good record after it does not rescue it.
		if _, err := Parse(strings.NewReader(`[`+tc.rec+`,`+good+`]`), 1); err == nil || !strings.Contains(err.Error(), "records[0]") {
			t.Errorf("%s first: error %v should name records[0]", name, err)
		}
	}
}

func TestParseKeepsTheReadErrorInTheChain(t *testing.T) {
	if _, err := Parse(strings.NewReader(""), 1); !errors.Is(err, io.EOF) {
		t.Errorf("empty file: %v, want io.EOF in the chain", err)
	}
	if _, err := Parse(strings.NewReader(`[{"character_name":"cyb"`), 1); !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Errorf("truncated file: %v, want io.ErrUnexpectedEOF in the chain", err)
	}
}

func TestParseBadCreateInfoIsFatalAndNamed(t *testing.T) {
	good := `{"character_id":"00000000000000000000000000000001","character_name":"Alpha","create_info_hex":"0a"}`
	for name, tc := range map[string]struct {
		hex   string
		check func(error) bool
	}{
		"invalid digit": {"zz", func(err error) bool { var e hex.InvalidByteError; return errors.As(err, &e) }},
		"odd length":    {"0af", func(err error) bool { return errors.Is(err, hex.ErrLength) }},
		"0x prefix":     {"0x0a", func(err error) bool { var e hex.InvalidByteError; return errors.As(err, &e) }},
	} {
		// The bad record comes second: an earlier good one must not mask it.
		in := `[` + good + `,{"character_id":"00000000000000000000000000000002","character_name":"Bravo","create_info_hex":"` + tc.hex + `"}]`
		got, err := Parse(strings.NewReader(in), 1)
		if err == nil {
			t.Errorf("%s: no error", name)
			continue
		}
		if got != nil {
			t.Errorf("%s: returned %d characters alongside the error", name, len(got))
		}
		if !strings.Contains(err.Error(), "Bravo") || !strings.Contains(err.Error(), "create_info_hex") {
			t.Errorf("%s: error %q should name the character and the field", name, err)
		}
		if !tc.check(err) {
			t.Errorf("%s: error %q does not wrap the hex error", name, err)
		}
	}
}

// (C.O.A) (C | Y | B)

func TestParseMissingOptionalFieldsAreZero(t *testing.T) {
	// The legacy loader defaults absent fields; a zero level becomes 1 in the store.
	got, err := Parse(strings.NewReader(`[{"character_id":"00000000000000000000000000000001","character_name":"cyb"}]`), 5)
	if err != nil || len(got) != 1 {
		t.Fatalf("%d characters, %v", len(got), err)
	}
	c := got[0]
	if c.ID != "00000000000000000000000000000001" || c.Name != "cyb" || c.AccountID != 5 ||
		c.Level != 0 || c.RaceID != 0 || c.GenderID != 0 || c.ClassID != 0 || c.ArchetypeID != 0 || c.ClassName != "" ||
		len(c.CreateInfo) != 0 {
		t.Errorf("parsed %+v", c)
	}
}

func TestParseKeepsDuplicatesForTheStoreToJudge(t *testing.T) {
	// Deduplication is the importer's job (id and name are unique in the store).
	in := `[{"character_id":"00000000000000000000000000000001","character_name":"Same"},
	        {"character_id":"00000000000000000000000000000001","character_name":"Same"}]`
	got, err := Parse(strings.NewReader(in), 1)
	if err != nil || len(got) != 2 {
		t.Fatalf("%d characters, %v", len(got), err)
	}
}

// Synthetic CreateCharacterInfo payloads, built the way the client encodes
// them: field 1 is a message whose repeated field 2 holds map entries
// (key = field 1, value = field 2); fields 2, 3 and 4 are the gender, race and
// class ids.

func mapEntry(key, val string) []byte {
	var e []byte
	e = protowire.AppendTag(e, 1, protowire.BytesType)
	e = protowire.AppendString(e, key)
	e = protowire.AppendTag(e, 2, protowire.BytesType)
	e = protowire.AppendString(e, val)
	return e
}

// customization is field 1: a wrapper message holding the given map entries.
func customization(entries ...[]byte) []byte {
	var w []byte
	for _, e := range entries {
		w = protowire.AppendTag(w, 2, protowire.BytesType)
		w = protowire.AppendBytes(w, e)
	}
	var b []byte
	b = protowire.AppendTag(b, 1, protowire.BytesType)
	return protowire.AppendBytes(b, w)
}

// idField is field num (2 gender, 3 race, 4 class) of the payload.
func idField(num protowire.Number, v uint64) []byte {
	return protowire.AppendVarint(protowire.AppendTag(nil, num, protowire.VarintType), v)
}

func genderField(v uint64) []byte { return idField(2, v) }
func raceField(v uint64) []byte   { return idField(3, v) }
func classField(v uint64) []byte  { return idField(4, v) }

func cat(parts ...[]byte) []byte { return bytes.Join(parts, nil) }

const looks = `{"presetGuid":"p-1","gender":"Male","race":"Kaelar","class":"Bard","hair":{"style":3,"color":[1,2,3]}}`

// parseOne parses a single record carrying the given fields on top of a valid
// id and name.
func parseOne(t *testing.T, fields map[string]any) character.New {
	t.Helper()
	rec := map[string]any{"character_id": "00000000000000000000000000000001", "character_name": "cyb"}
	for k, v := range fields {
		rec[k] = v
	}
	in, err := json.Marshal([]any{rec})
	if err != nil {
		t.Fatal(err)
	}
	got, err := Parse(bytes.NewReader(in), 7)
	if err != nil || len(got) != 1 {
		t.Fatalf("%d characters, %v", len(got), err)
	}
	return got[0]
}

func TestParseTakesRaceGenderClassAndAppearanceFromCreateInfo(t *testing.T) {
	// The shape of the real file: a Kaelar male Bard sends only the class, and
	// the writer left race_id and gender_id at 0.
	info := cat(customization(mapEntry("CustomizationDataKey", looks)), classField(2))
	c := parseOne(t, map[string]any{
		"race_id": 0, "gender_id": 0, "gender_name": "Male", "race_name": "Kaelar", "class_name": "Bard", "archetype_id": 17747,
		"create_info_hex": hex.EncodeToString(info),
	})
	if c.RaceID != 0 || c.GenderID != 0 || c.ClassID != 2 || c.ArchetypeID != 17747 || c.ClassName != "Bard" || c.AppearanceJSON != looks {
		t.Errorf("race %d gender %d class %d archetype %d %q appearance %q", c.RaceID, c.GenderID, c.ClassID, c.ArchetypeID, c.ClassName, c.AppearanceJSON)
	}
	if !bytes.Equal(c.CreateInfo, info) {
		t.Errorf("create_info must be kept whole")
	}

	// A Dunir (6) female (1) Mage (8): all three fields on the wire.
	const dunir = `{"presetGuid":"0","class":"Mage","gender":"Female","race":"Dunir"}`
	info = cat(customization(mapEntry("CustomizationDataKey", dunir)), genderField(1), raceField(6), classField(8))
	c = parseOne(t, map[string]any{
		"gender_name": "Female", "race_name": "Dunir", "class_name": "Mage", "archetype_id": 1,
		"create_info_hex": hex.EncodeToString(info),
	})
	if c.RaceID != 6 || c.GenderID != 1 || c.ClassID != 8 || c.ArchetypeID != 17750 || c.ClassName != "Mage" {
		t.Errorf("race %d gender %d class %d archetype %d %q", c.RaceID, c.GenderID, c.ClassID, c.ArchetypeID, c.ClassName)
	}
}

// The ids come from the payload alone (no customization here to check them
// against), and the class falls back to the file's name, then to the
// customization's.
func TestParseIds(t *testing.T) {
	cust := customization(mapEntry("CustomizationDataKey", `{"class":"Tank"}`))
	tooBig := cat(genderField(1<<31), raceField(1<<31), classField(1<<31)) // none fits an int32
	wrongType := protowire.AppendString(protowire.AppendTag(nil, 3, protowire.BytesType), "2")
	// A field 3 inside the customization wrapper is not the race.
	nested := customization(mapEntry("CustomizationDataKey", `{"class":"Tank"}`), raceField(5))
	// Truncated after a complete field 4: the walk gives up but keeps what it found.
	truncated := cat(classField(5), []byte{0x0a, 0x7f, 0x01})

	for name, tc := range map[string]struct {
		info                string
		file                map[string]any
		gender, race, class int32
	}{
		"the three fields":                     {hex.EncodeToString(cat(genderField(1), raceField(6), classField(8))), nil, 1, 6, 8},
		"in any order":                         {hex.EncodeToString(cat(classField(8), raceField(6), genderField(1))), nil, 1, 6, 8},
		"the first of each decides":            {hex.EncodeToString(cat(classField(2), classField(9), raceField(4), raceField(1))), nil, 0, 4, 2},
		"zeros are the defaults":               {"", nil, 0, 0, 0},
		"no class: the file's class_name":      {"", map[string]any{"class_name": "Mage"}, 0, 0, 8},
		"no class: the customization's":        {hex.EncodeToString(cust), nil, 0, 0, 7},
		"the file's name beats the JSON's":     {hex.EncodeToString(cust), map[string]any{"class_name": "Tank"}, 0, 0, 7},
		"an unknown class_name is no class":    {"", map[string]any{"class_name": "Paladin"}, 0, 0, 0},
		"varints out of int32 are invalid":     {hex.EncodeToString(tooBig), nil, -1, -1, -1},
		"a field of the wrong type is skipped": {hex.EncodeToString(cat(wrongType, raceField(5))), nil, 0, 5, 0},
		"a field nested in field 1 is no id":   {hex.EncodeToString(nested), nil, 0, 0, 7},
		"truncated after field 4 keeps it":     {hex.EncodeToString(truncated), nil, 0, 0, 5},
		"garbage is no ids":                    {"deadbeef", nil, 0, 0, 0},
		"field number 0 is no ids":             {"0001", nil, 0, 0, 0},
	} {
		fields := map[string]any{"create_info_hex": tc.info}
		for k, v := range tc.file {
			fields[k] = v
		}
		c := parseOne(t, fields)
		if c.GenderID != tc.gender || c.RaceID != tc.race || c.ClassID != tc.class {
			t.Errorf("%s: gender %d race %d class %d, want %d %d %d", name, c.GenderID, c.RaceID, c.ClassID, tc.gender, tc.race, tc.class)
		}
	}
}

// C.Y.B : CoA

// What the file and the customization say about the character must agree with
// the payload's fields: a disagreement is an error that names the record, and
// nothing is returned.
func TestParseRefusesARecordThatDisagreesWithItself(t *testing.T) {
	dunir := cat(customization(mapEntry("CustomizationDataKey", `{"class":"Mage","gender":"Female","race":"Dunir"}`)), genderField(1), raceField(6), classField(8))
	for name, tc := range map[string]struct {
		fields map[string]any
		want   string
	}{
		"the customization says Dunir, the race is Kaelar": {
			map[string]any{"create_info_hex": hex.EncodeToString(cat(customization(mapEntry("CustomizationDataKey", looks)), raceField(6), classField(2)))},
			`the customization says race "Kaelar" but the character's race is "Dunir"`},
		"the customization says Male, the gender is Female": {
			map[string]any{"create_info_hex": hex.EncodeToString(cat(customization(mapEntry("CustomizationDataKey", looks)), genderField(1), classField(2)))},
			`the customization says gender "Male" but the character's gender is "Female"`},
		"the customization says Bard, the class is Mage": {
			map[string]any{"create_info_hex": hex.EncodeToString(cat(customization(mapEntry("CustomizationDataKey", looks)), classField(8)))},
			`the customization says class "Bard" but the character's class is "Mage"`},
		"race_name Kaelar, the race is Dunir": {
			map[string]any{"race_name": "Kaelar", "create_info_hex": hex.EncodeToString(dunir)},
			`race_name "Kaelar", but create_info has race 6`},
		"gender_name Male, the gender is Female": {
			map[string]any{"gender_name": "Male", "create_info_hex": hex.EncodeToString(dunir)},
			`gender_name "Male", but create_info has gender 1`},
		"gender_name Female, and no payload": {
			map[string]any{"gender_name": "Female"},
			`gender_name "Female", but create_info has gender 0`},
		"class_name Bard, the class is Mage": {
			map[string]any{"class_name": "Bard", "create_info_hex": hex.EncodeToString(dunir)},
			`class name "Bard" is not "Mage"`},
	} {
		in, err := json.Marshal([]any{map[string]any{"character_id": "00000000000000000000000000000001", "character_name": "cyb"}, mergeRecord(tc.fields)})
		if err != nil {
			t.Fatal(err)
		}
		got, err := Parse(bytes.NewReader(in), 7)
		if err == nil || got != nil || !strings.Contains(err.Error(), tc.want) || !strings.Contains(err.Error(), "records[1] (Bad)") {
			t.Errorf("%s: %v, %v; want an error with %q naming records[1] (Bad)", name, got, err, tc.want)
		}
	}
	// Names that are not enum names are no claim.
	c := parseOne(t, map[string]any{"race_name": "r", "gender_name": "g", "class_name": "Paladin", "create_info_hex": hex.EncodeToString(dunir[:0])})
	if c.RaceID != 0 || c.GenderID != 0 {
		t.Errorf("race %d gender %d", c.RaceID, c.GenderID)
	}
}

// mergeRecord is a second record, named Bad, with the given fields.
func mergeRecord(fields map[string]any) map[string]any {
	rec := map[string]any{"character_id": "00000000000000000000000000000002", "character_name": "Bad"}
	for k, v := range fields {
		rec[k] = v
	}
	return rec
}

func TestParseAppearance(t *testing.T) {
	const key = "CustomizationDataKey"
	invalidUTF8 := `{"a":"` + "\xff" + `"}`
	for name, tc := range map[string]struct {
		info []byte
		want string
	}{
		"the object as stored, byte for byte": {customization(mapEntry(key, looks)), looks},
		"an empty object is an object":        {customization(mapEntry(key, `{}`)), `{}`},
		"found after other entries":           {customization(mapEntry("Other", `{"x":1}`), mapEntry(key, looks)), looks},
		"the first matching key decides":      {customization(mapEntry(key, looks), mapEntry(key, `{"x":1}`)), looks},
		"another key alone":                   {customization(mapEntry("Other", looks)), ""},
		"a key that merely contains it":       {customization(mapEntry(key+"2", looks)), ""},
		"an array":                            {customization(mapEntry(key, `[1,2]`)), ""},
		"null":                                {customization(mapEntry(key, `null`)), ""},
		"a string":                            {customization(mapEntry(key, `"hair"`)), ""},
		"a number":                            {customization(mapEntry(key, `12`)), ""},
		"not JSON":                            {customization(mapEntry(key, `{"hair":`)), ""},
		"empty value":                         {customization(mapEntry(key, ``)), ""},
		"invalid UTF-8":                       {customization(mapEntry(key, invalidUTF8)), ""},
		"no wrapper":                          {raceField(2), ""},
		"empty wrapper":                       {customization(), ""},
		"truncated inside the entry":          {[]byte{0x0a, 0x06, 0x12, 0x04, 0x0a, 0x04, 0x43, 0x75}, ""},
		"field 1 longer than the payload":     {[]byte{0x0a, 0x7f, 0x12}, ""},
		"empty create_info":                   {nil, ""},
	} {
		c := parseOne(t, map[string]any{"create_info_hex": hex.EncodeToString(tc.info)})
		if c.AppearanceJSON != tc.want {
			t.Errorf("%s: appearance %q, want %q", name, c.AppearanceJSON, tc.want)
		}
	}
}

// The first entry keyed CustomizationDataKey decides, whatever its value is:
// when that value is not a JSON object there is no appearance, and a later
// object-valued entry with the same key is not looked at. Real payloads carry
// a single entry, so this only pins the rule for malformed ones. (The client
// parses the wrapper as a protobuf map, where the last duplicate wins: decide
// deliberately if live character creation ever reuses createinfo.Parse.)
func TestParseTheFirstCustomizationEntryDecidesEvenIfItIsNotAnObject(t *testing.T) {
	const key = "CustomizationDataKey"
	invalidUTF8 := `{"a":"` + "\xff" + `"}`
	for name, first := range map[string]string{
		"null":           `null`,
		"an array":       `[1,2]`,
		"a string":       `"hair"`,
		"a number":       `12`,
		"not JSON":       `{"hair":`,
		"an empty value": ``,
		"invalid UTF-8":  invalidUTF8,
	} {
		info := customization(mapEntry(key, first), mapEntry(key, looks))
		c := parseOne(t, map[string]any{"create_info_hex": hex.EncodeToString(info)})
		if c.AppearanceJSON != "" {
			t.Errorf("first value %s: appearance %q from the later entry, want none", name, c.AppearanceJSON)
		}
	}
}

func TestParseOnlyTheFirstFieldOneIsTheCustomization(t *testing.T) {
	// The legacy emulator serves the first field 1 to the client; so do we.
	info := cat(customization(mapEntry("Other", `{"x":1}`)), customization(mapEntry("CustomizationDataKey", looks)))
	if c := parseOne(t, map[string]any{"create_info_hex": hex.EncodeToString(info)}); c.AppearanceJSON != "" {
		t.Errorf("appearance %q from a later field 1", c.AppearanceJSON)
	}
}

func TestParseKeepsWhatItFoundBeforeTheGarbage(t *testing.T) {
	info := cat(customization(mapEntry("CustomizationDataKey", looks)), []byte{0xde, 0xad, 0xbe, 0xef})
	c := parseOne(t, map[string]any{"create_info_hex": hex.EncodeToString(info)})
	if c.AppearanceJSON != looks || c.RaceID != 0 || c.ClassID != 2 { // the class is the customization's Bard
		t.Errorf("race %d class %d appearance %q", c.RaceID, c.ClassID, c.AppearanceJSON)
	}
}

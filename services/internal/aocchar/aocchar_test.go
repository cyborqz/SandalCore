// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocchar

import (
	"fmt"
	"strings"
	"testing"

	"google.golang.org/protobuf/encoding/protowire"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/createinfo"
)

const id = "0123456789abcdef0123456789abcdef"

// createInfo builds a CreateCharacterInfo payload the way the client does:
// field 1 wraps the customization map entry, fields 2, 3 and 4 are the gender,
// race and class (a zero is left out) and field 5 the starting location.
func createInfo(json string, gender, race, class uint64) []byte {
	var entry []byte
	entry = protowire.AppendTag(entry, 1, protowire.BytesType)
	entry = protowire.AppendString(entry, "CustomizationDataKey")
	entry = protowire.AppendTag(entry, 2, protowire.BytesType)
	entry = protowire.AppendString(entry, json)
	var wrapper []byte
	wrapper = protowire.AppendTag(wrapper, 2, protowire.BytesType)
	wrapper = protowire.AppendBytes(wrapper, entry)
	var b []byte
	b = protowire.AppendTag(b, 1, protowire.BytesType)
	b = protowire.AppendBytes(b, wrapper)
	for i, v := range []uint64{gender, race, class} {
		if v != 0 {
			b = protowire.AppendTag(b, protowire.Number(2+i), protowire.VarintType)
			b = protowire.AppendVarint(b, v)
		}
	}
	b = protowire.AppendTag(b, 5, protowire.BytesType)
	return protowire.AppendString(b, "Riverlands")
}

// render prints one level of a message as "num=value" in wire order: varints
// as numbers, length-delimited fields quoted, except the field numbers in
// nested, which are rendered one level down in braces.
func render(t *testing.T, b []byte, nested map[protowire.Number]bool) string {
	t.Helper()
	var parts []string
	for len(b) > 0 {
		num, typ, n := protowire.ConsumeTag(b)
		if n < 0 {
			t.Fatalf("bad tag in %x", b)
		}
		b = b[n:]
		switch typ {
		case protowire.VarintType:
			v, n := protowire.ConsumeVarint(b)
			if n < 0 {
				t.Fatal("bad varint")
			}
			parts = append(parts, fmt.Sprintf("%d=%d", num, v))
			b = b[n:]
		case protowire.BytesType:
			v, n := protowire.ConsumeBytes(b)
			if n < 0 {
				t.Fatal("bad length-delimited field")
			}
			if nested[num] {
				parts = append(parts, fmt.Sprintf("%d={%s}", num, render(t, v, nil)))
			} else {
				parts = append(parts, fmt.Sprintf("%d=%q", num, v))
			}
			b = b[n:]
		default:
			t.Fatalf("field %d has unexpected wire type %d", num, typ)
		}
	}
	return strings.Join(parts, " ")
}

// [C.Y.B :: CoA]

var recordNested = map[protowire.Number]bool{100: true, 101: true}

// A Dunir (6) female (1) Mage (8): gender at 4, race at 5, class at 6 of
// CharacterInformation, and the archetype id nowhere.
func TestRecordLayout(t *testing.T) {
	info := createInfo(`{"class":"Mage"}`, 1, 6, 8)
	c := &characterv1.Character{Id: id, Name: "Zed", RaceId: 6, GenderId: 1, ClassId: 8, ArchetypeId: 17750,
		Level: 3, CreatedAtMs: 1760000000123, CreateInfo: info}
	w := createinfo.Parse(info).Customization
	want := fmt.Sprintf(`2=3 5=%q 6="Zed" 7="world-1" 10=1760000000123 11=1760000000123 `+
		`100={1=%q 2=3 4=1 5=6 6=8 9=1 12=30 14=2} 101={1=%q}`, id, w, id)
	if got := render(t, Record(c, "world-1"), recordNested); got != want {
		t.Errorf("Record =\n %s\nwant\n %s", got, want)
	}
}

// The case that started plan B1: a Kaelar male Bard (the real client's cyb) has
// only the class on the wire, because Kaelar and Male are 0 and proto3 leaves a
// zero out. It used to be written as race 2.
func TestRecordOfAKaelarMaleBard(t *testing.T) {
	c := &characterv1.Character{Id: id, Name: "cyb", RaceId: 0, GenderId: 0, ClassId: 2, ArchetypeId: 17747, Level: 1}
	want := fmt.Sprintf(`2=1 5=%q 6="cyb" 7="w" 100={2=1 6=2 9=1 12=30 14=2} 101={1=%q}`, id, id)
	if got := render(t, Record(c, "w"), recordNested); got != want {
		t.Errorf("Record =\n %s\nwant\n %s", got, want)
	}
}

// No payload, no ids and no creation time: no customization field, no race, no
// gender, no class (a record whose class could not be derived), no fields 10
// and 11. The legacy encoders write nothing for a zero or empty value.
func TestRecordDefaults(t *testing.T) {
	c := &characterv1.Character{Id: id, Name: "Bob", Level: 1}
	want := fmt.Sprintf(`2=1 5=%q 6="Bob" 7="w" 100={2=1 9=1 12=30 14=2} 101={1=%q}`, id, id)
	if got := render(t, Record(c, "w"), recordNested); got != want {
		t.Errorf("Record =\n %s\nwant\n %s", got, want)
	}
}

// The create reply's character_info is the same message: the level is at 2 (it
// used to carry the gender there).
func TestCreateReplyInfoLayout(t *testing.T) {
	info := createInfo(`{"class":"Mage","gender":"Female"}`, 1, 6, 8)
	c := &characterv1.Character{Id: id, Level: 1, RaceId: 6, GenderId: 1, ClassId: 8, CreateInfo: info}
	want := fmt.Sprintf(`1=%q 2=1 4=1 5=6 6=8 9=1 14=2`, createinfo.Parse(info).Customization)
	if got := render(t, CreateReplyInfo(c), nil); got != want {
		t.Errorf("CreateReplyInfo = %s, want %s", got, want)
	}
	// Nothing known: no payload, no field 1.
	if got := render(t, CreateReplyInfo(&characterv1.Character{}), nil); got != "9=1 14=2" {
		t.Errorf("CreateReplyInfo(empty) = %s", got)
	}
}

// Every race, gender and class the client can create comes out at the fields of
// the client's descriptors, read back with an independent reader.
func TestEveryRaceGenderAndClassRoundTrips(t *testing.T) {
	for race := int32(0); race <= 7; race++ {
		for gender := int32(0); gender <= 1; gender++ {
			for class := int32(1); class <= 8; class++ {
				c := &characterv1.Character{Id: id, Name: "Zed", Level: 7, RaceId: race, GenderId: gender, ClassId: class}
				for name, info := range map[string][]byte{"Record": informationOf(t, Record(c, "w")), "CreateReplyInfo": CreateReplyInfo(c)} {
					got := fields(t, info)
					if got[2] != 7 || got[4] != uint64(gender) || got[5] != uint64(race) || got[6] != uint64(class) {
						t.Errorf("%s race %d gender %d class %d: level %d gender %d race %d class %d",
							name, race, gender, class, got[2], got[4], got[5], got[6])
					}
				}
			}
		}
	}
}

// informationOf is field 100 of a list record.
func informationOf(t *testing.T, record []byte) []byte {
	t.Helper()
	for b := record; len(b) > 0; {
		num, typ, n := protowire.ConsumeTag(b)
		b = b[n:]
		n = protowire.ConsumeFieldValue(num, typ, b)
		if num == 100 {
			v, _ := protowire.ConsumeBytes(b)
			return v
		}
		b = b[n:]
	}
	t.Fatal("no field 100")
	return nil
}

// [CoA ~ C:Y:B]

// fields is the varints of a message by field number.
func fields(t *testing.T, b []byte) map[protowire.Number]uint64 {
	t.Helper()
	out := map[protowire.Number]uint64{}
	for len(b) > 0 {
		num, typ, n := protowire.ConsumeTag(b)
		b = b[n:]
		if typ == protowire.VarintType {
			v, n := protowire.ConsumeVarint(b)
			out[num] = v
			b = b[n:]
			continue
		}
		b = b[protowire.ConsumeFieldValue(num, typ, b):]
	}
	return out
}

// The legacy server echoes the first field 1 of the payload, and omits it when
// that is empty; it does not look for a later, non-empty field 1.
func TestEmptyFirstField1IsNotEchoed(t *testing.T) {
	later := createInfo(`{"class":"Mage"}`, 0, 0, 8)
	info := append(protowire.AppendBytes(protowire.AppendTag(nil, 1, protowire.BytesType), nil), later...)
	c := &characterv1.Character{Id: id, Name: "Zed", Level: 1, ClassId: 8, CreateInfo: info}
	if got := render(t, CreateReplyInfo(c), nil); got != "2=1 6=8 9=1 14=2" {
		t.Errorf("CreateReplyInfo = %s", got)
	}
	want := fmt.Sprintf(`2=1 5=%q 6="Zed" 7="w" 100={2=1 6=8 9=1 12=30 14=2} 101={1=%q}`, id, id)
	if got := render(t, Record(c, "w"), recordNested); got != want {
		t.Errorf("Record = %s", got)
	}
}

// Boundaries of the "has a value" tests: the smallest real values still count.
func TestSmallestIdsStillCount(t *testing.T) {
	c := &characterv1.Character{Id: id, Name: "Zed", Level: 1, ArchetypeId: 1, RaceId: 1, GenderId: 1, ClassId: 1}
	want := fmt.Sprintf(`2=1 5=%q 6="Zed" 7="w" 100={2=1 4=1 5=1 6=1 9=1 12=30 14=2} 101={1=%q}`, id, id)
	if got := render(t, Record(c, "w"), recordNested); got != want {
		t.Errorf("Record = %s", got)
	}
	if got := render(t, CreateReplyInfo(c), nil); got != "2=1 4=1 5=1 6=1 9=1 14=2" {
		t.Errorf("CreateReplyInfo = %s", got)
	}
}

// A negative level, time or id (never stored, but the message allows it) must
// not wrap to a ten-byte varint: it counts as zero, which is omitted.
func TestNegativeValuesAreClamped(t *testing.T) {
	c := &characterv1.Character{Id: id, Name: "Zed", Level: -1, CreatedAtMs: -5, RaceId: -1, GenderId: -1, ClassId: -1}
	want := fmt.Sprintf(`5=%q 6="Zed" 7="w" 100={9=1 12=30 14=2} 101={1=%q}`, id, id)
	if got := render(t, Record(c, "w"), recordNested); got != want {
		t.Errorf("Record = %s", got)
	}
}

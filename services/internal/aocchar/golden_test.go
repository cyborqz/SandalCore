// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocchar

import (
	"encoding/hex"
	"fmt"
	"testing"

	characterv1 "aocrop/services/gen/aoc/character/v1"
)

// The golden bytes are assembled here from the wire format, field by field
// (tag byte = number<<3 | wire type), not taken from the encoder under test.
// CharacterInformation is laid out as the client's embedded protobuf
// descriptors have it: json_blob_map 1, level 2, gender 4, race 5,
// primary_class 6, skill_points 9, current_breathing_time 12, character_type 14
// (plan B1; the legacy server's bytes had the archetype id at 5 and the class
// at 6 and, in a create reply, the gender at 2).
const (
	idHex   = "3031323334353637383961626364656630313233343536373839616263646566" // the 32 ASCII digits of id
	created = "50fb80b3c19c33" + "58fb80b3c19c33"                                // fields 10 and 11: 1760000000123
	// The customization as the client sends it and the server echoes it back:
	// field 1, a map entry "CustomizationDataKey" -> the JSON.
	custMage = "0a5c125a0a14437573746f6d697a6174696f6e446174614b657912427b2270726573657447756964223a2230222c22636c617373223a224d616765222c2267656e646572223a2246656d616c65222c2272616365223a2244756e6972227d"
	custBard = "0a3b12390a14437573746f6d697a6174696f6e446174614b657912217b2270726573657447756964223a2230222c22636c617373223a2242617264227d"
	// field 5 of CreateCharacterInfo: "Riverlands"
	location = "2a0a52697665726c616e6473"
)

// record is a list record's golden hex: the outer fields, then field 100 (the
// information) and field 101 (the equipment: the character's id).
func record(level, name, world, createdFields, info string) string {
	return level + "2a20" + idHex + name + world + createdFields +
		"a206" + fmt.Sprintf("%02x", len(info)/2) + info + "aa0622" + "0a20" + idHex
}

// C O A - C.Y.B

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// A Dunir (6) female (1) Mage (8) whose payload carries all three ids.
func TestBytesMatchTheDescriptors(t *testing.T) {
	payload := mustHex(t, custMage+"1001"+"1806"+"2008"+location)
	c := &characterv1.Character{Id: id, Name: "Zed", RaceId: 6, GenderId: 1, ClassId: 8, ArchetypeId: 17750,
		Level: 3, CreatedAtMs: 1760000000123, CreateInfo: payload}

	wantRecord := record("1003", "3203"+"5a6564", "3a02"+"7731", created,
		custMage+"1003"+"2001"+"2806"+"3008"+"4801"+"601e"+"7002")
	if got := hex.EncodeToString(Record(c, "w1")); got != wantRecord {
		t.Errorf("Record = %s\nwant   %s", got, wantRecord)
	}
	wantReply := custMage + "1003" + "2001" + "2806" + "3008" + "4801" + "7002"
	if got := hex.EncodeToString(CreateReplyInfo(c)); got != wantReply {
		t.Errorf("CreateReplyInfo = %s\nwant   %s", got, wantReply)
	}
}

// Zero and empty values, which the encoders leave out: characters with no
// payload, no level, no creation time, no name and no world, and a Kaelar male
// Bard, whose race and gender are zeros.
func TestZeroValuesAreLeftOut(t *testing.T) {
	bard := mustHex(t, custBard+"2002"+location)
	for _, tc := range []struct {
		name, world string
		c           *characterv1.Character
		wantRecord  string
		wantReply   string
	}{
		{
			name:       "no payload, level 0 and no ids",
			world:      "w1",
			c:          &characterv1.Character{Id: id, Name: "Zed", Level: 0, CreatedAtMs: 1760000000123},
			wantRecord: record("", "3203"+"5a6564", "3a02"+"7731", created, "4801"+"601e"+"7002"),
			wantReply:  "4801" + "7002",
		},
		{
			name:  "a Kaelar male Bard, creation time 0",
			world: "w1",
			c:     &characterv1.Character{Id: id, Name: "Zed", Level: 1, ClassId: 2, ArchetypeId: 17747, CreatedAtMs: 0, CreateInfo: bard},
			wantRecord: record("1001", "3203"+"5a6564", "3a02"+"7731", "",
				custBard+"1001"+"3002"+"4801"+"601e"+"7002"),
			wantReply: custBard + "1001" + "3002" + "4801" + "7002",
		},
		{
			name:  "empty name",
			world: "w1",
			c:     &characterv1.Character{Id: id, Name: "", Level: 1, ClassId: 2, CreatedAtMs: 1760000000123, CreateInfo: bard},
			wantRecord: record("1001", "", "3a02"+"7731", created,
				custBard+"1001"+"3002"+"4801"+"601e"+"7002"),
			wantReply: custBard + "1001" + "3002" + "4801" + "7002",
		},
		{
			name:  "empty world",
			world: "",
			c:     &characterv1.Character{Id: id, Name: "Zed", Level: 1, ClassId: 2, CreatedAtMs: 1760000000123, CreateInfo: bard},
			wantRecord: record("1001", "3203"+"5a6564", "", created,
				custBard+"1001"+"3002"+"4801"+"601e"+"7002"),
			wantReply: custBard + "1001" + "3002" + "4801" + "7002",
		},
	} {
		if got := hex.EncodeToString(Record(tc.c, tc.world)); got != tc.wantRecord {
			t.Errorf("%s: Record = %s\nwant %s", tc.name, got, tc.wantRecord)
		}
		if got := hex.EncodeToString(CreateReplyInfo(tc.c)); got != tc.wantReply {
			t.Errorf("%s: CreateReplyInfo = %s\nwant %s", tc.name, got, tc.wantReply)
		}
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package aocchar encodes character records in the client's aoc_character
// wire format. There is no .proto for it in this repository: the layout is
// the one the legacy emulator sends (src/services/xclient/xclient_service.h,
// build_character_proto and build_create_reply_info), which the client
// accepts and renders in character select, with the field meanings of the
// client's own protobuf descriptors (embedded in the executable) where the
// legacy server's reading of a capture was wrong (plan B1).
//
// CharacterInformation, the message in field 100 of a list record and the
// character_info of a create reply: json_blob_map 1 (the customization),
// level 2, character_title 3, gender 4, race 5, primary_class 6,
// secondary_class 7, skill_points 9, current_breathing_time 12, pvp_flag 13,
// character_type 14. Gender, race and class are the client's enums
// (internal/chardef). The legacy server wrote the archetype record id at 5 and
// the class at 6, calling them "archetype" and "race", and a create reply's
// gender at 2, which is the level.
package aocchar

import (
	"google.golang.org/protobuf/encoding/protowire"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/createinfo"
)

// Record encodes c as one element of GetCharactersReply.characters_raw.
func Record(c *characterv1.Character, worldID string) []byte {
	level := uint64(max(c.GetLevel(), 0))
	created := uint64(max(c.GetCreatedAtMs(), 0))

	info := information(c) // field 100: the character's information
	info = appendVarint(info, 12, 30)
	info = appendVarint(info, 14, 2)

	var equipment []byte // field 101: equipped items, none yet
	equipment = appendBytes(equipment, 1, []byte(c.GetId()))

	var b []byte
	b = appendVarint(b, 2, level)
	b = appendBytes(b, 5, []byte(c.GetId()))
	b = appendBytes(b, 6, []byte(c.GetName()))
	b = appendBytes(b, 7, []byte(worldID))
	b = appendVarint(b, 10, created)
	b = appendVarint(b, 11, created)
	b = appendBytes(b, 100, info)
	b = appendBytes(b, 101, equipment)
	return b
}

// C:Y:B with C-O-A

// CreateReplyInfo encodes the character_info of a CreateCharacterReply: the
// same message as Record's field 100, without the breathing time the list
// carries.
func CreateReplyInfo(c *characterv1.Character) []byte {
	b := information(c)
	return appendVarint(b, 14, 2)
}

// information writes the customization, level, gender, race, class and the
// skill points of CharacterInformation, in field order, up to field 9. The
// enums are written as numbers: a zero (Male, Kaelar, no class) is left out,
// as proto3 does.
func information(c *characterv1.Character) []byte {
	var b []byte
	b = appendBytes(b, 1, createinfo.Parse(c.GetCreateInfo()).Customization)
	b = appendVarint(b, 2, uint64(max(c.GetLevel(), 0)))
	b = appendVarint(b, 4, uint64(max(c.GetGenderId(), 0)))
	b = appendVarint(b, 5, uint64(max(c.GetRaceId(), 0)))
	b = appendVarint(b, 6, uint64(max(c.GetClassId(), 0)))
	return appendVarint(b, 9, 1)
}

// appendVarint and appendBytes write nothing for a zero or empty value: the
// legacy encoders (pb_encode_uint64, pb_encode_string, pb_encode_bytes) omit
// them.
func appendVarint(b []byte, num protowire.Number, v uint64) []byte {
	if v == 0 {
		return b
	}
	b = protowire.AppendTag(b, num, protowire.VarintType)
	return protowire.AppendVarint(b, v)
}

func appendBytes(b []byte, num protowire.Number, v []byte) []byte {
	if len(v) == 0 {
		return b
	}
	b = protowire.AppendTag(b, num, protowire.BytesType)
	return protowire.AppendBytes(b, v)
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package createinfo reads the client's CreateCharacterInfo payload: the
// create_character_info_raw bytes of an ICS CreateCharacterRequest, which the
// character store keeps as create_info.
package createinfo

import (
	"encoding/json"
	"math"
	"unicode/utf8"

	"google.golang.org/protobuf/encoding/protowire"
)

// customizationKey is the map key under which the client stores the character
// customization JSON.
const customizationKey = "CustomizationDataKey"

// Invalid is what Gender, Race and Class hold when the payload's varint does
// not fit an int32. It is no enum value, so whoever checks the ids refuses it.
const Invalid int32 = -1

// Info is what a payload says, as far as it could be decoded.
//
// CreateCharacterInfo, from the client's embedded protobuf descriptors:
// json_blob_map 1, gender 2, race 3, primary_class 4, starting_location 5.
// proto3 leaves a zero out of the wire, so an absent field is 0: a Kaelar (0)
// male (0) Bard (2) sends field 4 alone.
type Info struct {
	// Gender is the first varint at field 2 (ECharacterGender, 0 Male when
	// absent), Race the first at field 3 (ECharacterRace, 0 Kaelar when
	// absent) and Class the first at field 4 (ECharacterClass: 1 Ranger,
	// 2 Bard ... 8 Mage; 0 when absent, which no real creation does). One
	// that does not fit an int32 is Invalid.
	Gender, Race, Class int32
	// Customization is the first field 1 (a message) as raw bytes, which the
	// legacy server echoes back to the client unchanged; nil when there is
	// none. It shares memory with the payload.
	Customization []byte
	// Appearance is the customization JSON inside Customization: the value of
	// its first map entry (field 2) keyed "CustomizationDataKey", if that
	// value is a UTF-8 JSON object, and "" otherwise. The first entry with
	// that key decides; a later one is not looked at. (Real payloads carry a
	// single entry. The client parses the wrapper as a protobuf map, where the
	// last duplicate wins, so this only pins the rule for malformed ones.)
	Appearance string
}

// (Cyb ~ C-O-A)

// Parse reads a payload. The walk is bounded by the payload; anything it
// cannot decode counts as absent, never as an error, and what was decoded
// before the bad bytes is kept (as the legacy decoder does). Of each of the
// fields 2, 3 and 4 the first varint counts; one of another wire type is
// skipped.
func Parse(info []byte) Info {
	var out Info
	var sawGender, sawRace, sawClass, sawCustomization bool
	for b := info; len(b) > 0 && !(sawGender && sawRace && sawClass && sawCustomization); {
		num, typ, n := protowire.ConsumeTag(b)
		if n < 0 {
			return out
		}
		b = b[n:]
		var dst *int32
		switch {
		case num == 2 && typ == protowire.VarintType && !sawGender:
			dst, sawGender = &out.Gender, true
		case num == 3 && typ == protowire.VarintType && !sawRace:
			dst, sawRace = &out.Race, true
		case num == 4 && typ == protowire.VarintType && !sawClass:
			dst, sawClass = &out.Class, true
		}
		switch {
		case dst != nil:
			v, n := protowire.ConsumeVarint(b)
			if n < 0 {
				return out
			}
			*dst = Invalid
			if v <= math.MaxInt32 {
				*dst = int32(v)
			}
			b = b[n:]
		case num == 1 && typ == protowire.BytesType && !sawCustomization:
			v, n := protowire.ConsumeBytes(b)
			if n < 0 {
				return out
			}
			sawCustomization = true
			out.Customization = v
			out.Appearance = customizationJSON(v)
			b = b[n:]
		default:
			n := protowire.ConsumeFieldValue(num, typ, b)
			if n < 0 {
				return out
			}
			b = b[n:]
		}
	}
	return out
}

// JSONString returns the string value of key in a JSON object, or "" when
// object is not a JSON object or key is missing or not a string. The client's
// customization JSON names the character's "class", "race" and "gender" this
// way.
func JSONString(object, key string) string {
	var m map[string]json.RawMessage
	if json.Unmarshal([]byte(object), &m) != nil {
		return ""
	}
	var s string
	if json.Unmarshal(m[key], &s) != nil {
		return ""
	}
	return s
}

// customizationJSON digs the JSON out of the field 1 wrapper: the value of the
// first map entry keyed customizationKey, if it is a JSON object. The first
// such entry decides; it does not fall through to a later one.
func customizationJSON(wrapper []byte) string {
	for b := wrapper; len(b) > 0; {
		num, typ, n := protowire.ConsumeTag(b)
		if n < 0 {
			return ""
		}
		b = b[n:]
		if num == 2 && typ == protowire.BytesType {
			entry, n := protowire.ConsumeBytes(b)
			if n < 0 {
				return ""
			}
			b = b[n:]
			if key, val := splitEntry(entry); string(key) == customizationKey {
				return jsonObject(val)
			}
			continue
		}
		n = protowire.ConsumeFieldValue(num, typ, b)
		if n < 0 {
			return ""
		}
		b = b[n:]
	}
	return ""
}

// [Cyb ~ C O A]

// splitEntry reads a map<string,string> entry: key at field 1, value at field 2
// (the last of each wins). A malformed entry yields a nil key, which matches no
// key the caller looks for.
func splitEntry(entry []byte) (key, val []byte) {
	for e := entry; len(e) > 0; {
		num, typ, n := protowire.ConsumeTag(e)
		if n < 0 {
			return nil, nil
		}
		e = e[n:]
		if typ == protowire.BytesType && (num == 1 || num == 2) {
			v, n := protowire.ConsumeBytes(e)
			if n < 0 {
				return nil, nil
			}
			e = e[n:]
			if num == 1 {
				key = v
			} else {
				val = v
			}
			continue
		}
		if n = protowire.ConsumeFieldValue(num, typ, e); n < 0 {
			return nil, nil
		}
		e = e[n:]
	}
	return key, val
}

// jsonObject returns v as a string if it is a JSON object ("null", arrays,
// scalars and invalid UTF-8 are not), and "" otherwise.
func jsonObject(v []byte) string {
	var obj map[string]json.RawMessage
	if utf8.Valid(v) && json.Unmarshal(v, &obj) == nil && obj != nil {
		return string(v)
	}
	return ""
}

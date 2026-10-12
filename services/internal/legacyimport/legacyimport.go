// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package legacyimport reads the legacy emulator's data/characters.json so
// existing characters can be carried into the character service.
package legacyimport

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"

	"aocrop/services/internal/character"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/createinfo"
)

// byteOrderMark is what some editors put at the start of a UTF-8 file.
var byteOrderMark = []byte{0xef, 0xbb, 0xbf}

// legacyCharacter is one record of characters.json. The legacy writer never
// filled race_id and gender_id (they are always 0 in the file), and its
// race_name and gender_name are the names from the customization JSON; none of
// the four carries anything the payload does not, so race_id and gender_id are
// not read at all and the names only check the payload (see Parse).
type legacyCharacter struct {
	ArchetypeID   int32  `json:"archetype_id"`
	CharacterID   string `json:"character_id"`
	CharacterName string `json:"character_name"`
	ClassName     string `json:"class_name"`
	CreateInfoHex string `json:"create_info_hex"`
	GenderName    string `json:"gender_name"`
	Level         int32  `json:"level"`
	RaceName      string `json:"race_name"`
}

// ~ C | Y | B :: CoA ~

// Parse turns the file's records into characters owned by accountID. Import
// sets the account itself, so a caller that does not know it yet (charimport
// parses before it connects to the database) may pass 0.
//
// The character's race, gender and class are the client's enums (plan B1:
// ECharacterRace, ECharacterGender, ECharacterClass, internal/chardef), read
// from the fields of the client's CreateCharacterInfo payload in create_info:
// gender at field 2, race at field 3 and class at field 4 (a zero is absent
// from the wire, so a Kaelar male sends neither of the first two). The file's
// own fields hold nothing the payload does not:
//
//   - the class falls back to class_name, then to the customization's "class",
//     when the payload has no field 4;
//   - appearance is the customization JSON inside create_info, if it is a JSON
//     object; otherwise AppearanceJSON stays empty (the store defaults it);
//   - the class name and the archetype id are the class's own when it is known,
//     and the file's otherwise;
//   - a name that disagrees with the payload (the customization JSON's race,
//     gender and class, the file's race_name, gender_name and class_name when
//     they are enum names) is an error: nothing is guessed.
//
// A record's level above the game's maximum (character.MaxLevel, 25) is imported as 25. CreateInfo keeps the client's bytes whole.
//
// Parse is strict, and fails before returning anything (so before any database
// write): the file must be one JSON array (a leading UTF-8 byte order mark and
// trailing whitespace are fine, anything else after the array is not; null is
// not an array, while [] is an array of no characters), and every record needs
// a character_id and a character_name and a create_info_hex that is hex, and
// agrees with itself.
func Parse(r io.Reader, accountID int64) ([]character.New, error) {
	br := bufio.NewReader(r)
	if head, _ := br.Peek(len(byteOrderMark)); bytes.Equal(head, byteOrderMark) {
		_, _ = br.Discard(len(byteOrderMark))
	}
	dec := json.NewDecoder(br)
	var records []legacyCharacter
	if err := dec.Decode(&records); err != nil {
		return nil, fmt.Errorf("characters.json: %w", err)
	}
	if records == nil { // decoding null leaves the slice nil; [] makes it empty
		return nil, fmt.Errorf("characters.json: expected a JSON array of characters, found null")
	}
	var extra json.RawMessage
	if err := dec.Decode(&extra); err != io.EOF {
		return nil, fmt.Errorf("characters.json: unexpected content after the array")
	}
	out := make([]character.New, 0, len(records))
	for i, rec := range records {
		if rec.CharacterID == "" {
			return nil, fmt.Errorf("characters.json: records[%d]: character_id is empty (character_name %q)", i, rec.CharacterName)
		}
		if rec.CharacterName == "" {
			return nil, fmt.Errorf("characters.json: records[%d]: character_name is empty (character_id %q)", i, rec.CharacterID)
		}
		info, err := hex.DecodeString(rec.CreateInfoHex)
		if err != nil {
			return nil, fmt.Errorf("characters.json: records[%d] (%s): create_info_hex: %w", i, rec.CharacterName, err)
		}
		f := createinfo.Parse(info)
		race, gender, class := f.Race, f.Gender, f.Class
		if class == 0 {
			for _, name := range []string{rec.ClassName, createinfo.JSONString(f.Appearance, "class")} {
				if c, ok := chardef.ClassByName(name); ok {
					class = c
					break
				}
			}
		}
		if err := agree(rec, race, gender, class, f.Appearance); err != nil {
			return nil, fmt.Errorf("characters.json: records[%d] (%s): %w", i, rec.CharacterName, err)
		}
		className, archetype := rec.ClassName, rec.ArchetypeID
		if name, ok := chardef.ClassName(class); ok {
			className, archetype = name, chardef.ArchetypeID(class)
		}
		out = append(out, character.New{
			ID: rec.CharacterID, AccountID: accountID, Name: rec.CharacterName,
			RaceID: race, GenderID: gender, ClassID: class, ArchetypeID: archetype,
			ClassName: className, AppearanceJSON: f.Appearance, CreateInfo: info, Level: min(rec.Level, character.MaxLevel),
		})
	}
	return out, nil
}

// agree checks the payload's race, gender and class against the names the
// record carries: the customization JSON's, and the file's own race_name and
// gender_name (a value that is not an enum name is not a claim).
func agree(rec legacyCharacter, race, gender, class int32, appearance string) error {
	if err := chardef.Agree(race, gender, class, rec.ClassName, appearance); err != nil {
		return err
	}
	if n, ok := chardef.RaceByName(rec.RaceName); ok && n != race {
		return fmt.Errorf("race_name %q, but create_info has race %d", rec.RaceName, race)
	}
	if n, ok := chardef.GenderByName(rec.GenderName); ok && n != gender {
		return fmt.Errorf("gender_name %q, but create_info has gender %d", rec.GenderName, gender)
	}
	return nil
}

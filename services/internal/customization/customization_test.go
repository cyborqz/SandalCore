// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package customization

import (
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/chardef"
	"aocrop/services/internal/content"
	"aocrop/services/internal/createinfo"
)

// ---- a small content.db, built here, that every run can use ------------------------------------------------------------

// Guids of the synthetic records: the options of race r and gender g start at
// optionBase(r, g), so every combination has its own.
func optionBase(race, gender int32) int64 { return 1_000_000 * int64(1+race*2+gender) }

// syntheticDB writes a content.db with the player appearance map and, for each
// of the 14 playable race and gender pairs, an appearance, a definition, an
// entitlement and a morph record, in the shape of the real data (field names
// and references as the real record.data has them).
func syntheticDB(t *testing.T) *content.DB {
	t.Helper()
	path := filepath.Join(t.TempDir(), "content.db")
	raw, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if err := content.Create(raw); err != nil {
		t.Fatal(err)
	}
	exec := func(query string, args ...any) {
		t.Helper()
		if _, err := raw.Exec(query, args...); err != nil {
			t.Fatalf("%s: %v", query, err)
		}
	}
	exec(`INSERT INTO meta (key, value) VALUES (?, ?), (?, ?)`,
		content.MetaSchemaVersion, fmt.Sprint(content.SchemaVersion), content.MetaRecordEncoding, content.EncodingJSON)
	put := func(typeID, guid int64, schema, name string, fields map[string]any) {
		t.Helper()
		data, err := json.Marshal(fields)
		if err != nil {
			t.Fatal(err)
		}
		exec(`INSERT INTO record VALUES (0, ?, ?, ?, 0, ?, 0, ?)`, typeID, schema, guid, name, data)
	}
	ref := func(typeID, guid int64) map[string]any {
		return map[string]any{"Guid": guid, "Name": "x", "TypeId": typeID}
	}
	refs := func(typeID int64, guids ...int64) []any {
		out := []any{}
		for _, g := range guids {
			out = append(out, ref(typeID, g))
		}
		return out
	}
	const (
		tApp, tDef, tEnt, tMorph, tOpt = 11, 12, 13, 14, 15
		none                           = 0
	)

	mapFields := map[string]any{}
	for race, rn := range []string{"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua", "Tulnar"} {
		for gender, gn := range []string{"Male", "Female"} {
			key := gn + rn + "Id"
			if race == 8 {
				mapFields[key] = ref(tApp, none) // Tulnar: no appearance
				continue
			}
			b := optionBase(int32(race), int32(gender))
			mapFields[key] = ref(tApp, b)
			horns := refs(tOpt)
			if race == 5 { // Pyrai: horns
				horns = refs(tOpt, b+60, b+61)
			}
			put(tApp, b, "RaceGenderAppearanceRecordBase", "App_"+gn+rn, map[string]any{
				"CustomizationDefId": ref(tDef, b),
				"SectionsValuesLimits": map[string]any{
					"MaxSectionsValues": []any{map[string]any{"Key": "FCharacterCustomizationBodySections::Chest", "Value": map[string]any{"X": 0.7, "Y": 0.7, "Z": 0.7}}},
					"MinSectionsValues": []any{map[string]any{"Key": "FCharacterCustomizationBodySections::Chest", "Value": map[string]any{"X": 0.1, "Y": 0.1, "Z": 0.1}}},
				},
			})
			put(tDef, b, "RaceGenderCustomizationDefRecordBase", rn+gn[:1], map[string]any{
				"CustomizationEntitlementId": ref(tEnt, b),
				"HeadHairOptionsIds":         refs(tOpt, b+1, b+2),
				"FacialLipHairOptionsIds":    refs(tOpt, b+3),
				"FacialChinHairOptionsIds":   refs(tOpt, b+4),
				"FacialCheekHairOptionsIds":  refs(tOpt, b+5),
				"FacialBrowHairOptionsIds":   refs(tOpt, b+6),
				"RacialHornOptionsIds":       horns,
				"SkinSetIds":                 refs(tOpt, b+7, b+8, b+9),
				"PresetsIds":                 refs(tOpt, b+50),
			})
			put(tEnt, b, "CharacterCustomizationEntitlementsRecordBase", rn, map[string]any{
				"IrisIDsIds":      refs(tOpt, b+10, b+11),
				"ScleraIDsIds":    refs(tOpt, b+12),
				"IrisColorIDsIds": refs(tOpt, b+13),
				"HairColorIDsIds": refs(tOpt, b+14, b+15),
				"NailColorIDsIds": refs(tOpt, b+16),
				"MorphsId":        ref(tMorph, b),
			})
			put(tMorph, b, "CharacterMorphRecordBase", "Basic", map[string]any{
				"Morph": []any{
					map[string]any{"Section": "x", "Morphs": []any{
						map[string]any{"MorphName": "Forehead_Round", "Min": -1.0, "Max": 1.0, "TwinMorphName": "None"},
						map[string]any{"MorphName": "Temples_Sharp_0", "Min": 0.0, "Max": 0.5, "TwinMorphName": "None"},
					}},
				},
			})
		}
	}
	put(content.TypeRaceGenderAppearanceMap, content.GuidPlayerAppearanceMap, "RaceGenderAppearanceMapRecordBase", "PlayerCharacter", mapFields)
	if err := raw.Close(); err != nil {
		t.Fatal(err)
	}
	db, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

// syntheticCustomization is a valid customization of the synthetic Kaelar male.
func syntheticCustomization() map[string]any {
	b := optionBase(0, 0)
	id := func(n int64) string { return fmt.Sprint(b + n) }
	return map[string]any{
		"presetGuid": "0", "randomSeed": 1454, "skinColorHue": 0.85, "skinColorPigmentation": 0.19, "skinSet": 1,
		"eyeColor": id(13), "eyeShape": id(11), "scleraShape": id(12), "eyebrows": id(6), "headHair": id(2),
		"headHairRootColor": id(14), "headHairTipColor": id(15), "headHairLength": 0.8, "headHairContrast": 0, "headHairGradient": -0.4,
		"facialHairLip": id(3), "facialHairChin": id(4), "facialHairCheek": id(5), "facialHairLipLength": 0.5,
		"facialHairRootColor": id(14), "facialHairTipColor": id(15), "facialHairContrast": 0.9, "facialHairGradient": 0.4,
		"racialHorns": fmt.Sprint(HornsNone), "racialHornsLength": 0, "nailColor": id(16), "nailOpacity": 0,
		"decalData": []any{}, "decalBlendGroups": []any{}, "bIsHelmetVisible": true, "bIsCapeVisible": true,
		"gender": "Male", "race": "Kaelar", "class": "Bard",
		"faceMorphWeightMaps": map[string]any{"forehead_Round": 0.42, "temples_Sharp_0": 0.5, "racial_3": 0.31, "snear_Left_0": 0.13},
		"sectionsValues": map[string]any{
			"none":  map[string]any{"x": 0.69, "y": 0.31, "z": 0.37},
			"chest": map[string]any{"x": 0.45, "y": 0.45, "z": 0.45},
			"hips":  map[string]any{"x": 0.49, "y": 0.49, "z": 0.49},
		},
	}
}

func text(t *testing.T, m map[string]any) string {
	t.Helper()
	b, err := json.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// ---- the checks, on a copy of the valid customization --------------------------------------------------------------------

// mutation changes a valid customization of a Kaelar male Bard; want is what the
// refusal's text must contain.
type mutation struct {
	name   string
	change func(m map[string]any)
	want   string
}

func set(key string, value any) func(map[string]any) {
	return func(m map[string]any) { m[key] = value }
}

func morph(name string, value any) func(map[string]any) {
	return func(m map[string]any) { m["faceMorphWeightMaps"].(map[string]any)[name] = value }
}

func section(name string, value any) func(map[string]any) {
	return func(m map[string]any) { m["sectionsValues"].(map[string]any)[name] = value }
}

// decal is a valid decal with one member changed.
func decal(key string, value any) map[string]any {
	d := map[string]any{"guid": "6064630961557340182", "strength": 1, "scaleU": 1, "scaleV": 1, "slotIndex": 1,
		"mergeSectionMaterialBase": "6064630961557340183", "offsetU": 0, "offsetV": 0, "angle": 0,
		"color": "6064630707193774081", "discoloration": 0}
	d[key] = value
	return d
}

// namedMorphs is n morphs m0, m1, ... of weight 0.1.
func namedMorphs(n int) map[string]any {
	morphs := map[string]any{}
	for i := range n {
		morphs[fmt.Sprintf("m%d", i)] = 0.1
	}
	return morphs
}

// ~ C | Y | B & CoA ~

var refused = []mutation{
	{"hair of nobody", set("headHair", "12345"), "headHair 12345 is not one of the"},
	{"hair id that is not a number", set("headHair", "abc"), "headHair is not a decimal id"},
	{"hair id that is a float", set("headHair", 1.5), "headHair is not a decimal id"},
	{"hair id that is negative", set("headHair", "-1"), "headHair is not a decimal id"},
	{"hair id past 64 bits", set("headHair", "99999999999999999999999"), "headHair is not a decimal id"},
	{"lip hair of nobody", set("facialHairLip", "12345"), "facialHairLip 12345"},
	{"chin hair of nobody", set("facialHairChin", "12345"), "facialHairChin 12345"},
	{"cheek hair of nobody", set("facialHairCheek", "12345"), "facialHairCheek 12345"},
	{"brows of nobody", set("eyebrows", "12345"), "eyebrows 12345"},
	{"horns of nobody", set("racialHorns", "12345"), "racialHorns 12345"},
	{"eye shape of nobody", set("eyeShape", "12345"), "eyeShape 12345"},
	{"sclera of nobody", set("scleraShape", "12345"), "scleraShape 12345"},
	{"eye colour of nobody", set("eyeColor", "12345"), "eyeColor 12345"},
	{"hair root colour of nobody", set("headHairRootColor", "12345"), "headHairRootColor 12345"},
	{"hair tip colour of nobody", set("headHairTipColor", "12345"), "headHairTipColor 12345"},
	{"facial hair root colour of nobody", set("facialHairRootColor", "12345"), "facialHairRootColor 12345"},
	{"facial hair tip colour of nobody", set("facialHairTipColor", "12345"), "facialHairTipColor 12345"},
	{"nail colour of nobody", set("nailColor", "12345"), "nailColor 12345"},
	{"a preset of nobody", set("presetGuid", "12345"), "presetGuid 12345"},
	{"a skin set past the last", set("skinSet", 3), "skinSet 3 is not one of the 3 skin sets"},
	{"a negative skin set", set("skinSet", -1), "skinSet -1"},
	{"a fractional skin set", set("skinSet", 1.5), "skinSet is not an integer"},
	{"a skin set that is a string", set("skinSet", "1"), "skinSet is not an integer"},
	{"hue past 1", set("skinColorHue", 1.5), "skinColorHue 1.5 is outside 0 to 1"},
	{"a negative length", set("headHairLength", -0.5), "headHairLength -0.5 is outside 0 to 1"},
	{"a gradient past 1", set("headHairGradient", 2), "headHairGradient 2 is outside -1 to 1"},
	{"opacity that is a string", set("nailOpacity", "x"), "nailOpacity is not a number"},
	{"a flag that is a number", set("bIsCapeVisible", 1), "bIsCapeVisible is not a boolean"},
	{"decals that are an object", set("decalData", map[string]any{}), "decalData is not an array"},
	{"a morph past 1", morph("temples_Sharp_0", 1.5), "face morph temples_Sharp_0 1.5 is outside -1 to 1"},
	{"a morph below -1", morph("temples_Sharp_0", -1.5), "face morph temples_Sharp_0 -1.5 is outside -1 to 1"},
	{"a morph the record lacks, past -1 to 1", morph("racial_3", 5), "face morph racial_3 5 is outside -1 to 1"},
	{"a morph that is a string", morph("forehead_Round", "x"), "face morph \"forehead_Round\" is not a name with a number"},
	{"a morph with a bad name", morph("a b", 0.5), "face morph \"a b\" is not a name with a number"},
	{"morphs that are a list", set("faceMorphWeightMaps", []any{}), "faceMorphWeightMaps is not an object"},
	{"a section past 1", section("chest", map[string]any{"x": 1.2, "y": 0.45, "z": 0.45}), "body section chest x 1.2 is outside 0 to 1"},
	{"a section below 0", section("chest", map[string]any{"x": 0.45, "y": -0.05, "z": 0.45}), "body section chest y -0.05 is outside 0 to 1"},
	{"a section without limits past 1", section("jaw", map[string]any{"x": 0.5, "y": 0.5, "z": 1.5}), "body section jaw z 1.5 is outside 0 to 1"},
	{"a section without a coordinate", section("jaw", map[string]any{"x": 0.5, "y": 0.5}), "body section jaw z is not a number"},
	{"a section that is a number", section("jaw", 0.5), "body section \"jaw\" is not a name with x, y and z"},
	// what worldd reads (customization_from_json): the checks that keep an accepted character joinable
	{"a null id", set("headHair", nil), "headHair is not a decimal id"},
	{"a null float member", set("headHairLength", nil), "headHairLength is not a number"},
	{"a null flag", set("bIsHelmetVisible", nil), "bIsHelmetVisible is not a boolean"},
	{"a fractional random seed", set("randomSeed", 1.5), "randomSeed is not a 32-bit integer"},
	{"a random seed past 32 bits", set("randomSeed", 1<<31), "randomSeed is not a 32-bit integer"},
	{"a random seed that is a string", set("randomSeed", "7"), "randomSeed is not a 32-bit integer"},
	{"a float member no float32 holds", set("eyelashLength", 1e39), "eyelashLength 1e+39 does not fit a 32-bit float"},
	{"a detail strength that is null", set("normalDetailStrength01", nil), "normalDetailStrength01 is not a number"},
	{"decals that are null", set("decalData", nil), "decalData is not an array"},
	{"blend groups that are null", set("decalBlendGroups", nil), "decalBlendGroups is not an array"},
	{"a decal that is a number", set("decalData", []any{1}), "decalData[0] is not an object or null"},
	{"a decal that is empty", set("decalData", []any{map[string]any{}}), "decalData[0].strength is missing"},
	{"a decal slot past a byte", set("decalData", []any{decal("slotIndex", 256)}), "decalData[0].slotIndex is not an integer of 0 to 255"},
	{"a decal slot that is a float", set("decalData", []any{decal("slotIndex", 1.5)}), "decalData[0].slotIndex is not an integer of 0 to 255"},
	{"a decal strength that is null", set("decalData", []any{decal("strength", nil)}), "decalData[0].strength is not a number"},
	{"a decal id that is no id", set("decalData", []any{decal("color", "abc")}), "decalData[0].color is not a decimal id"},
	{"a blend group that is empty", set("decalBlendGroups", []any{map[string]any{}}), "decalBlendGroups[0].intensity is missing"},
	{"a blend group intensity past float32", set("decalBlendGroups", []any{map[string]any{"decalAppearanceGuid": "1", "colorGuid": "2", "intensity": 1e39}}), "decalBlendGroups[0].intensity 1e+39 does not fit a 32-bit float"},
	{"morphs that are null", set("faceMorphWeightMaps", nil), "faceMorphWeightMaps is not an object"},
	{"a morph that is null", morph("forehead_Round", nil), `face morph "forehead_Round" is not a name with a number`},
	{"morphs that differ only in the first letter's case", func(m map[string]any) {
		morph("noseBridge", 0.1)(m)
		morph("NoseBridge", 0.2)(m)
	}, `face morphs "NoseBridge" and "noseBridge" are the same morph`},
	{"257 morphs", func(m map[string]any) { m["faceMorphWeightMaps"] = namedMorphs(257) }, "faceMorphWeightMaps has 257 morphs, more than 256"},
	{"sections that are null", set("sectionsValues", nil), "sectionsValues is not an object"},
	{"a section the enum does not have", section("foo", map[string]any{"x": 0.5, "y": 0.5, "z": 0.5}), `body section "foo" is not one of the 30 sections`},
	{"a section in capitals", section("Chest", map[string]any{"x": 0.5, "y": 0.5, "z": 0.5}), `body section "Chest" is not one of the 30 sections`},
	{"a section that is null", section("jaw", nil), `body section "jaw" is not a name with x, y and z`},
	{"a section coordinate that is null", section("jaw", map[string]any{"x": nil, "y": 0.5, "z": 0.5}), "body section jaw x is not a number"},
	{"257 sections", func(m map[string]any) {
		sections := map[string]any{}
		for i := range 257 {
			sections[fmt.Sprintf("s%d", i)] = map[string]any{"x": 0.5, "y": 0.5, "z": 0.5}
		}
		m["sectionsValues"] = sections
	}, "sectionsValues has 257 sections, more than 256"},
	{"101 members nothing reads", func(m map[string]any) {
		for i := range maxUnknownKeys + 1 {
			m[fmt.Sprintf("extra%d", i)] = 1
		}
	}, "101 members that are no part of the customization, more than 100"},
	{"a Dunir customization for a Kaelar", set("race", "Dunir"), `the customization says race "Dunir" but the character's race is "Kaelar"`},
	{"a female customization for a male", set("gender", "Female"), `the customization says gender "Female" but the character's gender is "Male"`},
	{"another class", set("class", "Mage"), `the customization says class "Mage" but the character's class is "Bard"`},
	{"a race that is not a name", set("race", 0), "the customization's race is not a name"},
	{"a race name that is not one", set("race", "Orc"), `the customization says race "Orc"`},
}

// TestChecksRefuse runs the refusals on the synthetic data: each changes one
// thing of a valid customization, which is refused with that thing named, and
// the customization without the change passes.
func TestChecksRefuse(t *testing.T) {
	rules, err := Load(syntheticDB(t))
	if err != nil {
		t.Fatal(err)
	}
	if err := rules.Check(New{Race: 0, Gender: 0, Class: chardef.ClassBard, ClassName: "Bard", AppearanceJSON: text(t, syntheticCustomization())}); err != nil {
		t.Fatalf("the valid customization is refused: %v", err)
	}
	for _, m := range refused {
		t.Run(m.name, func(t *testing.T) {
			c := syntheticCustomization()
			m.change(c)
			err := rules.Check(New{Race: 0, Gender: 0, Class: chardef.ClassBard, ClassName: "Bard", AppearanceJSON: text(t, c)})
			if err == nil || !strings.Contains(err.Error(), m.want) {
				t.Errorf("Check = %v, want an error with %q", err, m.want)
			}
		})
	}
}

func TestChecksOnTheCharacter(t *testing.T) {
	rules, err := Load(syntheticDB(t))
	if err != nil {
		t.Fatal(err)
	}
	js := text(t, syntheticCustomization())
	for _, tc := range []struct {
		name string
		c    New
		want string // "" for accepted
	}{
		{"a Kaelar male Bard", New{0, 0, 2, "Bard", js}, ""},
		{"a class name that is the class's", New{0, 0, 2, "", js}, ""},
		{"Tulnar", New{chardef.RaceTulnar, 0, 2, "Bard", js}, "Tulnar cannot be created"},
		{"race 9", New{9, 0, 2, "Bard", js}, "race 9 is not a playable race"},
		{"race -1", New{-1, 0, 2, "Bard", js}, "race -1 is not a playable race"},
		{"gender 2", New{0, 2, 2, "Bard", js}, "gender 2 is not 0 (Male) or 1 (Female)"},
		{"class 0", New{0, 0, 0, "Bard", js}, "class 0 is not a class"},
		{"class 9", New{0, 0, 9, "Bard", js}, "class 9 is not a class"},
		{"a class name that is not the class's", New{0, 0, 3, "Bard", strings.Replace(js, `"class":"Bard"`, `"class":"Cleric"`, 1)}, `class name "Bard" is not "Cleric"`},
		{"no customization", New{0, 0, 2, "Bard", ""}, "no customization"},
		{"customization that is not JSON", New{0, 0, 2, "Bard", "not json"}, "not a JSON object"},
		{"customization that is an array", New{0, 0, 2, "Bard", "[]"}, "not a JSON object"},
		{"customization that is null", New{0, 0, 2, "Bard", "null"}, "not a JSON object"},
		{"customization that is huge", New{0, 0, 2, "Bard", `{"a":"` + strings.Repeat("x", maxAppearanceBytes) + `"}`}, "more than"},
		{"an empty object", New{0, 0, 2, "Bard", "{}"}, ""}, // nothing chosen: every member is its default
	} {
		err := rules.Check(tc.c)
		switch {
		case tc.want == "" && err != nil:
			t.Errorf("%s: refused: %v", tc.name, err)
		case tc.want != "" && (err == nil || !strings.Contains(err.Error(), tc.want)):
			t.Errorf("%s: Check = %v, want an error with %q", tc.name, err, tc.want)
		}
	}
}

// [C:Y:B :: C-O-A]

// Each race and gender has its own options: the Kaelar male's hair is not the
// Dunir female's, and the horns of a Pyrai are not a Kaelar's.
func TestOptionsBelongToTheRaceAndGender(t *testing.T) {
	rules, err := Load(syntheticDB(t))
	if err != nil {
		t.Fatal(err)
	}
	for race := int32(0); race <= 7; race++ {
		for gender := int32(0); gender <= 1; gender++ {
			b := optionBase(race, gender)
			rn, _ := chardef.RaceName(race)
			gn, _ := chardef.GenderName(gender)
			c := syntheticCustomization()
			c["race"], c["gender"] = rn, gn
			// the options of this race and gender
			for key, n := range map[string]int64{"headHair": 2, "facialHairLip": 3, "facialHairChin": 4, "facialHairCheek": 5, "eyebrows": 6,
				"eyeColor": 13, "eyeShape": 11, "scleraShape": 12, "headHairRootColor": 14, "headHairTipColor": 15,
				"facialHairRootColor": 14, "facialHairTipColor": 15, "nailColor": 16} {
				c[key] = fmt.Sprint(b + n)
			}
			if race == 5 {
				c["racialHorns"] = fmt.Sprint(b + 61)
			}
			if err := rules.Check(New{race, gender, 2, "Bard", text(t, c)}); err != nil {
				t.Errorf("%s %s: %v", gn, rn, err)
			}
			// the options of another gender of the race, or another race
			other := optionBase((race+1)%8, gender)
			c["headHair"] = fmt.Sprint(other + 2)
			if err := rules.Check(New{race, gender, 2, "Bard", text(t, c)}); err == nil || !strings.Contains(err.Error(), "headHair") {
				t.Errorf("%s %s with another race's hair: Check = %v", gn, rn, err)
			}
		}
	}
	// Horns: only a Pyrai has them; "no horns" is valid for everyone.
	c := syntheticCustomization()
	c["racialHorns"] = fmt.Sprint(optionBase(5, 0) + 60)
	if err := rules.Check(New{0, 0, 2, "Bard", text(t, c)}); err == nil || !strings.Contains(err.Error(), "racialHorns") {
		t.Errorf("a Kaelar with Pyrai horns: Check = %v", err)
	}
	for _, none := range []any{"0", fmt.Sprint(HornsNone), 0} {
		c["racialHorns"] = none
		if err := rules.Check(New{0, 0, 2, "Bard", text(t, c)}); err != nil {
			t.Errorf("racialHorns %v: %v", none, err)
		}
	}
}

// Many problems are all named, up to eight.
func TestCheckNamesEveryProblem(t *testing.T) {
	rules, err := Load(syntheticDB(t))
	if err != nil {
		t.Fatal(err)
	}
	c := syntheticCustomization()
	for _, key := range []string{"headHair", "facialHairLip", "facialHairChin", "facialHairCheek", "eyebrows", "eyeShape", "scleraShape", "eyeColor", "nailColor", "presetGuid"} {
		c[key] = "12345"
	}
	err = rules.Check(New{0, 0, 2, "Bard", text(t, c)})
	if err == nil {
		t.Fatal("accepted")
	}
	msg := err.Error()
	if got := strings.Count(msg, "is not one of the"); got != maxProblems || !strings.Contains(msg, "(and 2 more)") || !strings.Contains(msg, "Kaelar") && !strings.Contains(msg, "customization for") {
		t.Errorf("%d problems named in %q", got, msg)
	}
}

// A content.db without the appearances cannot be used.
func TestLoadFailsWithoutTheAppearances(t *testing.T) {
	path := filepath.Join(t.TempDir(), "content.db")
	raw, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if err := content.Create(raw); err != nil {
		t.Fatal(err)
	}
	if _, err := raw.Exec(`INSERT INTO meta (key, value) VALUES (?, ?), (?, ?)`,
		content.MetaSchemaVersion, fmt.Sprint(content.SchemaVersion), content.MetaRecordEncoding, content.EncodingJSON); err != nil {
		t.Fatal(err)
	}
	raw.Close()
	db, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := Load(db); err == nil || !strings.Contains(err.Error(), "player appearance map") {
		t.Errorf("Load = %v", err)
	}
}

// ---- the real design data and the real client's customizations ------------------------------------------------------------

// realRules loads the rules from content.db at AOC_CONTENT_DB (scripts\services.ps1 test sets it when the file
// exists); the test is skipped without it.
func realRules(t *testing.T) *Rules {
	t.Helper()
	rules, err := Load(realDB(t))
	if err != nil {
		t.Fatal(err)
	}
	return rules
}

func realDB(t *testing.T) *content.DB {
	t.Helper()
	path := os.Getenv("AOC_CONTENT_DB")
	if path == "" {
		t.Skip("AOC_CONTENT_DB not set (the content.db contentc builds)")
	}
	db, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

// -- CoA . C.Y.B --

// What the research read in the design data (plan B1's evidence): 14 playable race and gender pairs, Tulnar
// with no appearance, horns for the Pyrai only, 48 presets for a Vek male and 42 for a Kaelar male, the Vek and
// Renkai on the morph set with the racial morphs.
func TestRealAppearances(t *testing.T) {
	db := realDB(t)
	get := func(race, gender string) *content.Appearance {
		t.Helper()
		a, err := db.PlayerAppearance(race, gender)
		if err != nil || a == nil {
			t.Fatalf("%s %s: %v %v", gender, race, a, err)
		}
		return a
	}
	if a, err := db.PlayerAppearance("Tulnar", "Male"); a != nil || err != nil {
		t.Errorf("Tulnar male: %v, %v; want none", a, err)
	}
	kae, vek, pyr := get("Kaelar", "Male"), get("Vek", "Male"), get("Pyrai", "Male")
	if kae.Name != "Kae_M" || kae.SkinSets != 3 || len(kae.Presets) != 42 || len(kae.Horns) != 0 || len(kae.Morphs) != 72 {
		t.Errorf("Kaelar male: %s, %d skin sets, %d presets, %d horns, %d morphs", kae.Name, kae.SkinSets, len(kae.Presets), len(kae.Horns), len(kae.Morphs))
	}
	if len(vek.Presets) != 48 || len(vek.Morphs) != 74 {
		t.Errorf("Vek male: %d presets, %d morphs", len(vek.Presets), len(vek.Morphs))
	}
	if len(pyr.Horns) != 3 || len(pyr.HairColor) != 27 {
		t.Errorf("Pyrai male: %d horns, %d hair colours", len(pyr.Horns), len(pyr.HairColor))
	}
	if r, ok := kae.Morphs["forehead_round"]; !ok || r.Min != -1 || r.Max != 1 {
		t.Errorf("Kaelar male's forehead_round = %v, %v", r, ok)
	}
	if kae.SectionMax["chest"] != (content.Vec3{0.7, 0.7, 0.7}) || kae.SectionMin["chest"] != (content.Vec3{0.1, 0.1, 0.1}) {
		t.Errorf("Kaelar male's chest limits: %v to %v", kae.SectionMin["chest"], kae.SectionMax["chest"])
	}
}

func TestTulnarHasNoAppearance(t *testing.T) {
	for _, gender := range []string{"Male", "Female"} {
		if a, err := syntheticDB(t).PlayerAppearance("Tulnar", gender); a != nil || err != nil {
			t.Errorf("Tulnar %s: %v, %v; want none", gender, a, err)
		}
	}
}

// retail reads the 125 customizations of the community's retail captures (the
// pawns of other players, in the client's JSON shape): 14 race and gender
// pairs, from 66 sessions.
func retail(t *testing.T) []map[string]any {
	t.Helper()
	raw, err := os.ReadFile("testdata/retail_customizations.json")
	if err != nil {
		t.Fatal(err)
	}
	var list []map[string]any
	if err := json.Unmarshal(raw, &list); err != nil {
		t.Fatal(err)
	}
	return list
}

func TestRetailCustomizationsPass(t *testing.T) {
	rules := realRules(t)
	list := retail(t)
	if len(list) != 125 {
		t.Fatalf("%d retail customizations, want 125", len(list))
	}
	pairs := map[string]int{}
	for i, c := range list {
		race, ok := chardef.RaceByName(c["race"].(string))
		gender, ok2 := chardef.GenderByName(c["gender"].(string))
		if !ok || !ok2 {
			t.Fatalf("customization %d: race %v gender %v", i, c["race"], c["gender"])
		}
		pairs[fmt.Sprint(c["gender"], " ", c["race"])]++
		if err := rules.Check(New{race, gender, chardef.ClassBard, "", text(t, c)}); err != nil {
			t.Errorf("retail customization %d (%s %s): %v", i, c["gender"], c["race"], err)
		}
	}
	if len(pairs) != 14 {
		t.Errorf("%d race and gender pairs, want 14: %v", len(pairs), pairs)
	}
}

// The real client's own creator output (cyb: 41 members, 84 face morphs, 30 body sections) passes, with every
// morph and section of it: the creator sends morphs that no record lists.
func TestTheRealClientsCustomizationPasses(t *testing.T) {
	rules := realRules(t)
	c := cyb(t)
	if err := rules.Check(New{0, 0, chardef.ClassBard, "Bard", c}); err != nil {
		t.Errorf("cyb: %v", err)
	}
}

// cyb reads the customization JSON of the real client's payload (createinfo's fixture: a Kaelar male Bard).
func cyb(t *testing.T) string {
	t.Helper()
	raw, err := os.ReadFile("../createinfo/testdata/client_create_info.hex")
	if err != nil {
		t.Fatal(err)
	}
	payload, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		t.Fatal(err)
	}
	js := createinfo.Parse(payload).Appearance
	if js == "" {
		t.Fatal("the payload has no customization")
	}
	return js
}

// The refusals again, on the real data: the real Kaelar male customization with one thing changed.
func TestRealDataRefuses(t *testing.T) {
	rules := realRules(t)
	for _, m := range refused {
		switch m.name {
		case "a morph the record lacks, past -1 to 1", "a morph above its slider", "a morph below its slider":
			continue // the synthetic morph records differ from the real ones: checked below
		}
		t.Run(m.name, func(t *testing.T) {
			var c map[string]any
			if err := json.Unmarshal([]byte(cyb(t)), &c); err != nil {
				t.Fatal(err)
			}
			m.change(c)
			err := rules.Check(New{0, 0, chardef.ClassBard, "Bard", text(t, c)})
			if err == nil || !strings.Contains(err.Error(), m.want) {
				t.Errorf("Check = %v, want an error with %q", err, m.want)
			}
		})
	}
	var c map[string]any
	if err := json.Unmarshal([]byte(cyb(t)), &c); err != nil {
		t.Fatal(err)
	}
	morph("racial_3", 5)(c) // in no morph record, so only -1 to 1
	if err := rules.Check(New{0, 0, chardef.ClassBard, "Bard", text(t, c)}); err == nil || !strings.Contains(err.Error(), "face morph racial_3 5 is outside -1 to 1") {
		t.Errorf("racial_3: %v", err)
	}
	morph("racial_3", 0.5)(c)
	morph("forehead_Round", 1.5)(c) // a Kaelar slider of -1 to 1
	if err := rules.Check(New{0, 0, chardef.ClassBard, "Bard", text(t, c)}); err == nil || !strings.Contains(err.Error(), "face morph forehead_Round 1.5 is outside -1 to 1") {
		t.Errorf("forehead_Round: %v", err)
	}
	// The races' section limits are not enforced: the creator's default Dunir male look has chest, abdomen and
	// lowerArms values of 0 and 0.18 against a limit of 0.2 to 0.7 (in game, 2026-10-02). Such values pass; only
	// 0 to 1 holds.
	morph("forehead_Round", 0.4)(c)
	section("chest", map[string]any{"x": 0.0, "y": 0.17884419113397598, "z": 0.0})(c)
	section("abdomen", map[string]any{"x": 0.0, "y": 0.5, "z": 0.0})(c)
	if err := rules.Check(New{0, 0, chardef.ClassBard, "Bard", text(t, c)}); err != nil {
		t.Errorf("sections of 0 and 0.18 (the default Dunir male look): %v", err)
	}
	section("chest", map[string]any{"x": 1.2, "y": 0.45, "z": 0.45})(c)
	if err := rules.Check(New{0, 0, chardef.ClassBard, "Bard", text(t, c)}); err == nil || !strings.Contains(err.Error(), "body section chest x 1.2 is outside 0 to 1") {
		t.Errorf("a chest past 1: %v", err)
	}
}

// [C | O | A ~ Cyb]

// A customization of one race is not another's: each retail customization is refused as the other gender or race.
func TestRetailCustomizationsAreNotInterchangeable(t *testing.T) {
	rules := realRules(t)
	refusedCount, total := 0, 0
	for _, c := range retail(t) {
		race, _ := chardef.RaceByName(c["race"].(string))
		gender, _ := chardef.GenderByName(c["gender"].(string))
		if c["headHair"] == nil || c["headHair"] == "0" {
			continue // a default look has nothing to compare
		}
		for _, other := range [][2]int32{{(race + 1) % 8, gender}, {race, 1 - gender}} {
			total++
			c2 := map[string]any{}
			for k, v := range c {
				c2[k] = v
			}
			delete(c2, "race")
			delete(c2, "gender")
			if rules.Check(New{other[0], other[1], chardef.ClassBard, "", text(t, c2)}) != nil {
				refusedCount++
			}
		}
	}
	// Races share some options (the Kaelar and the Vaelune have the same sclera), but the hair is the race's own.
	if total == 0 || refusedCount < total*9/10 {
		t.Errorf("only %d of %d customizations were refused as another race or gender", refusedCount, total)
	}
}

// The caps: 256 face morphs and 100 unknown members are accepted, one more is not (what the creator makes is far
// under them: 84 morphs, 30 sections, no unknown member).
func TestTheCapsOnMapsAndUnknownMembers(t *testing.T) {
	rules, err := Load(syntheticDB(t))
	if err != nil {
		t.Fatal(err)
	}
	check := func(c map[string]any) error {
		return rules.Check(New{Race: 0, Gender: 0, Class: chardef.ClassBard, ClassName: "Bard", AppearanceJSON: text(t, c)})
	}
	c := syntheticCustomization()
	c["faceMorphWeightMaps"] = namedMorphs(maxMapEntries)
	if err := check(c); err != nil {
		t.Errorf("%d morphs: %v", maxMapEntries, err)
	}
	c["faceMorphWeightMaps"] = namedMorphs(maxMapEntries + 1)
	if err := check(c); err == nil || !strings.Contains(err.Error(), "faceMorphWeightMaps has 257 morphs, more than 256") {
		t.Errorf("%d morphs: %v", maxMapEntries+1, err)
	}

	c = syntheticCustomization()
	for i := range maxUnknownKeys {
		c[fmt.Sprintf("extra%d", i)] = i
	}
	if err := check(c); err != nil {
		t.Errorf("%d unknown members: %v", maxUnknownKeys, err)
	}
	c["one more"] = 1
	if err := check(c); err == nil || !strings.Contains(err.Error(), "101 members that are no part of the customization") {
		t.Errorf("%d unknown members: %v", maxUnknownKeys+1, err)
	}
}

// ~ C Y B & C.O.A ~

// What scan says of a text: its size as the database stores it, and the first number past a double.
func TestScan(t *testing.T) {
	for _, tc := range []struct {
		js     string
		stored int
		huge   string
	}{
		{`{"a":1}`, 8, ""},                        // a space more after the colon
		{`{ "a" : 1 , "b" : [ 1 , 2 ] }`, 21, ""}, // no whitespace but a space after ':' and ','
		{`{"a": 1, "b": [1, 2]}`, 21, ""},         // as the database writes it
		{`"x\"y"`, 6, ""},                         // a string is counted as sent, escapes included
		{`1e5`, 3 + 5, ""},                        // 100000
		{`1.5e-7`, 6 + 7, ""},                     // 0.00000015
		{`-1.0000000116860974e-07`, 23 + 7, ""},   // the creator's own kind of number
		{`[1e400]`, 7 + 400, "1e400"},             // past a double
		{`[-1e400, 1e400]`, 15 + 800, "-1e400"},
		{`1` + strings.Repeat("0", 399), 400, "1" + strings.Repeat("0", 23) + "..."},
		{`1` + strings.Repeat("0", 299), 300, ""},
		{`1.7976931348623157e308`, 22 + 308, ""}, // the largest double
		{`1e-400`, 6 + 400, ""},                  // below the smallest: zero, which is finite
	} {
		stored, huge := scan(tc.js)
		if stored != tc.stored || huge != tc.huge {
			t.Errorf("scan(%.40s) = %d, %q; want %d, %q", tc.js, stored, huge, tc.stored, tc.huge)
		}
	}
}

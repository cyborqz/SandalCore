// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package customization checks a new character's race, gender, class and
// customization against what the design database allows (plan B1): the
// character service refuses a creation that the client's own character creator
// could not have produced.
//
// The options come from content.db (internal/content, PlayerAppearance): for
// the character's race and gender the RaceGenderCustomizationDef lists the
// hair, facial hair, brow and horn options, the skin sets and the presets, and
// its CharacterCustomizationEntitlements the iris, sclera and colour options
// and the morph sliders; the RaceGenderAppearance has the limits of the body
// sections. Entitlements are not looked at: which options an account has
// unlocked is a retail store concern, so every option of the race is allowed.
//
// What is checked, in the client's JSON (FJsonObjectConverter of
// FCharacterCustomizationSaveData: lowerCamel member names, 64-bit ids as
// decimal strings), and what is not:
//
//   - the race (0-7; Tulnar, 8, has no player appearance and is refused),
//     gender (0-1) and class (1-8) are the client's enums, and the JSON's own
//     "race", "gender" and "class" names, when present, agree with them;
//   - headHair, facialHairLip/Chin/Cheek, eyebrows, racialHorns, eyeShape,
//     scleraShape, eyeColor, the four hair colours and nailColor are 0 (the
//     default the wire leaves out) or one of the race's options; racialHorns
//     0x9F65000082A14F49 is also accepted, the "no horns" id of the client;
//     skinSet is below the number of skin sets; presetGuid is 0 or one of the
//     race's presets (Normalize then stores it as 0: worldd does not take a
//     real preset, whose table lookup is not decoded);
//   - the normalized members (hue, pigmentation, lengths, contrast, nail
//     opacity: 0 to 1; the two gradients: -1 to 1) are within their range;
//   - a face morph weight is within -1 to 1, and a body section's X, Y and Z
//     within 0 to 1; a section is one of the 30 of the client's enum. The morph
//     records' slider ranges and the races' section limits are NOT enforced:
//     the creator's own default Dunir male look has chest/abdomen/lowerArms
//     values of 0 and 0.18 against a section limit of 0.2 to 0.7 (in-game,
//     2026-10-02), so those records are not the stored values' range, and the
//     retail captures carry no morph or section data to settle it.
//
// And what worldd needs of the stored text to build the pawn at join (its
// customization_from_json, server/worldd/bootstrap/character_setup.cpp): a
// character the service accepts must also join, so the checks above go with
// worldd's own. randomSeed and skinSet are integers (1.5, 1.0 and 1e2 are not)
// that fit 32 bits; every float member is a number a float32 holds (1e39 is not
// one: the three members that have no range, eyelashLength and the two normal
// detail strengths, are only held to that); no member is null; the decal data
// and blend groups are arrays of null or of objects with all their members of
// the right type (a decal's slotIndex is an integer of 0 to 255); the face morph
// names are unique once the first letter's case is folded (the client's JSON
// has "forehead_Round" where the replicated name starts with a capital), and
// the whole is no larger as the database stores it (jsonb text, see scan)
// than worldd reads, and no number in it, in a member nothing reads too, is
// past a double (worldd's JSON parser refuses the whole text for one).
// Unknown top-level members are allowed, up to maxUnknownKeys.
//
// Not checked: the contents of a decal's or blend group's members beyond their
// types, the helmet and cape flags beyond being booleans.
package customization

// -- CoA & C.Y.B --

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/chardef"
	"aocrop/services/internal/content"
)

// HornsNone is the "no horns" id of the client's racialHorns, in every race.
const HornsNone uint64 = 0x9F65000082A14F49

// maxAppearanceBytes bounds the JSON that is parsed at all (the real client's is
// 6.5 KB), and is the most worldd reads (kMaxAppearanceBytes).
const maxAppearanceBytes = 64 << 10

// maxMapEntries bounds the face morphs and body sections of one customization
// (the creator sends 84 morphs and 30 sections).
const maxMapEntries = 256

// maxUnknownKeys bounds the top-level members that nothing reads (the creator sends 41
// members, all known): worldd logs each one it does not know.
const maxUnknownKeys = 100

// tolerance for the float members: the client writes the double of a float32.
const tolerance = 1e-6

// bodySections are the names of FCharacterCustomizationBodySections as the JSON writes them (the enum's names with
// a small first letter; the exe's name table at 0x14b74ff50, worldd's kBodySections).
var bodySections = []string{
	"none", "lowerArms", "upperArms", "clavicle", "thigh", "calf", "neck", "lowerAbdomen", "abdomen", "chest",
	"hips", "size", "mouthGroup", "noseGroup", "eyeGroup", "browGroup", "earGroup", "jaw", "eyeSocketL", "eyeSocketR",
	"eyeInnerR", "eyeInnerL", "eyeOuterL", "eyeOuterR", "smilerL", "smilerR", "forehead", "cheeks", "racialGroup", "cCount",
}

// bodySectionSet is bodySections as a set.
var bodySectionSet = func() map[string]bool {
	m := map[string]bool{}
	for _, n := range bodySections {
		m[n] = true
	}
	return m
}()

// knownKeys are the top-level members worldd reads (its is_known_key): the 30 scalar members and these.
var knownKeys = func() map[string]bool {
	m := map[string]bool{}
	for _, k := range []string{
		"presetGuid", "randomSeed", "decalData", "decalBlendGroups", "bIsHelmetVisible", "bIsCapeVisible", "gender", "race", "class",
		"faceMorphWeightMaps", "sectionsValues",
		"skinColorHue", "skinColorPigmentation", "normalDetailStrength01", "normalDetailStrength02", "skinSet", "eyeColor", "eyeShape",
		"scleraShape", "eyebrows", "headHair", "headHairRootColor", "headHairTipColor", "headHairLength", "headHairContrast",
		"headHairGradient", "facialHairLip", "facialHairChin", "facialHairCheek", "facialHairLipLength", "facialHairChinLength",
		"facialHairCheekLength", "eyelashLength", "facialHairRootColor", "facialHairTipColor", "facialHairContrast",
		"facialHairGradient", "racialHorns", "racialHornsLength", "nailColor", "nailOpacity",
	} {
		m[k] = true
	}
	return m
}()

// Rules is what a character creation is checked against.
type Rules struct {
	appearances map[[2]int32]*content.Appearance // by race and gender
}

// [C Y B x C.O.A]

// Load reads the appearance of every playable race and both genders from db
// (races 0-7 of chardef; Tulnar has none). It fails when one is missing or
// does not read: content.db is then not one this service can check against.
func Load(db *content.DB) (*Rules, error) {
	r := &Rules{appearances: map[[2]int32]*content.Appearance{}}
	for race := chardef.RaceKaelar; race <= chardef.RaceNikua; race++ {
		raceName, _ := chardef.RaceName(race)
		for gender := chardef.GenderMale; gender <= chardef.GenderFemale; gender++ {
			genderName, _ := chardef.GenderName(gender)
			a, err := db.PlayerAppearance(raceName, genderName)
			if err != nil {
				return nil, err
			}
			if a == nil {
				return nil, fmt.Errorf("customization: content.db has no player appearance for a %s %s", genderName, raceName)
			}
			r.appearances[[2]int32{race, gender}] = a
		}
	}
	return r, nil
}

// New is a character to be created.
type New struct {
	Race, Gender, Class int32
	ClassName           string // "" for none; else the name of Class
	AppearanceJSON      string // the client's customization JSON
}

// Check returns nil when the character can be created, and otherwise an error
// that says every problem it found (up to eight), worded for the operator's
// log: it names the member and the option.
func (r *Rules) Check(c New) error {
	switch {
	case c.Race == chardef.RaceTulnar:
		return errors.New("customization: Tulnar cannot be created: the design data has no player appearance for it")
	case c.Race < chardef.RaceKaelar || c.Race > chardef.RaceNikua:
		return fmt.Errorf("customization: race %d is not a playable race (0 Kaelar to 7 Nikua)", c.Race)
	case c.Gender != chardef.GenderMale && c.Gender != chardef.GenderFemale:
		return fmt.Errorf("customization: gender %d is not 0 (Male) or 1 (Female)", c.Gender)
	case c.Class < chardef.ClassRanger || c.Class > chardef.ClassMage:
		return fmt.Errorf("customization: class %d is not a class (1 Ranger to 8 Mage)", c.Class)
	}
	if len(c.AppearanceJSON) == 0 {
		return errors.New("customization: the character has no customization")
	}
	if len(c.AppearanceJSON) > maxAppearanceBytes {
		return fmt.Errorf("customization: %d bytes of JSON, more than %d", len(c.AppearanceJSON), maxAppearanceBytes)
	}
	var obj map[string]json.RawMessage
	if err := json.Unmarshal([]byte(c.AppearanceJSON), &obj); err != nil || obj == nil {
		return errors.New("customization: not a JSON object")
	}
	// The text worldd reads at join is what the database stores: it must fit there, not only as it was sent, and its
	// parser refuses the whole text for a number no double holds, wherever the number is.
	size, huge := scan(c.AppearanceJSON)
	if huge != "" {
		return fmt.Errorf("customization: the number %s is too large for a double: worldd cannot read the JSON", huge)
	}
	if size > maxAppearanceBytes {
		return fmt.Errorf("customization: %d bytes as the database stores it, more than %d (what worldd reads)", size, maxAppearanceBytes)
	}
	if err := chardef.Agree(c.Race, c.Gender, c.Class, c.ClassName, c.AppearanceJSON); err != nil {
		return fmt.Errorf("customization: %w", err)
	}
	k := &checker{a: r.appearances[[2]int32{c.Race, c.Gender}], obj: obj}
	k.check()
	if len(k.problems) == 0 {
		return nil
	}
	more := ""
	if len(k.problems) > maxProblems {
		more = fmt.Sprintf(" (and %d more)", len(k.problems)-maxProblems)
		k.problems = k.problems[:maxProblems]
	}
	return fmt.Errorf("customization for %s: %s%s", k.a.Name, strings.Join(k.problems, "; "), more)
}

// Normalize returns the customization as it is stored: a presetGuid that names a real preset is stored as 0, and
// the rest is as it was sent (the keys come back in order, which the database changes anyway). The creator's JSON
// carries every member its preset resolved to, and with preset 0 the wire carries Race and Gender (worldd builds
// the pawn from the members alone; whether the client's preset table has a real preset, which decides if Race and
// Gender are on the wire, is not decoded, so worldd refuses one). Run it on what Check accepted; anything it
// cannot read comes back as it was.
func Normalize(appearanceJSON string) string {
	var obj map[string]json.RawMessage
	if json.Unmarshal([]byte(appearanceJSON), &obj) != nil || obj == nil {
		return appearanceJSON
	}
	raw, ok := obj["presetGuid"]
	if !ok {
		return appearanceJSON
	}
	if id, ok := parseID(raw); !ok || id == 0 {
		return appearanceJSON // already 0 (the string "0" or the number 0), or not an id: not for this to change
	}
	obj["presetGuid"] = json.RawMessage(`"0"`)
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if enc.Encode(obj) != nil {
		return appearanceJSON
	}
	return strings.TrimSuffix(buf.String(), "\n")
}

// scan walks the text of a JSON document (which must be valid JSON) and returns two things about it. The first is an
// upper bound of its size as the database hands it back, which is what worldd reads: PostgreSQL's jsonb text has no
// space between tokens but one after each ':' and ',', and writes a number out in full (1e5 is 100000, 1e-5 is
// 0.00001), so a text that was sent compact grows; a string is counted as it was sent (jsonb never writes one
// longer). The second is the first number literal that is infinite as a double ("" for none): worldd's JSON parser
// refuses the whole text for one, in a member nothing reads as well.
func scan(js string) (stored int, huge string) {
	for i := 0; i < len(js); {
		switch c := js[i]; {
		case c == '"':
			j := i + 1
			for j < len(js) && js[j] != '"' {
				if js[j] == '\\' {
					j++
				}
				j++
			}
			stored += j + 1 - i
			i = j + 1
		case c == ' ' || c == '\t' || c == '\n' || c == '\r':
			i++
		case c == ':' || c == ',':
			stored += 2
			i++
		case c == '-' || c >= '0' && c <= '9':
			j := i + 1
			for j < len(js) && strings.IndexByte("0123456789.eE+-", js[j]) >= 0 {
				j++
			}
			lit := js[i:j]
			stored += len(lit)
			if f, err := strconv.ParseFloat(lit, 64); err != nil && math.IsInf(f, 0) && huge == "" {
				huge = lit
				if len(huge) > 24 {
					huge = huge[:24] + "..."
				}
			}
			if e := strings.IndexAny(lit, "eE"); e >= 0 {
				exp, err := strconv.Atoi(lit[e+1:])
				if err != nil || exp > 1<<24 || exp < -(1<<24) {
					exp = 1 << 24
				}
				stored += max(exp, -exp)
			}
			i = j
		default:
			stored++
			i++
		}
	}
	return stored, huge
}

// C-Y-B : CoA

const maxProblems = 8

type checker struct {
	a        *content.Appearance
	obj      map[string]json.RawMessage
	problems []string
}

func (k *checker) problem(format string, args ...any) {
	k.problems = append(k.problems, fmt.Sprintf(format, args...))
}

func (k *checker) check() {
	for _, f := range []struct {
		key string
		set content.IDSet
		// extra ids that are valid besides 0 and the set
		extra []uint64
	}{
		{"headHair", k.a.HeadHair, nil},
		{"facialHairLip", k.a.FacialLipHair, nil},
		{"facialHairChin", k.a.FacialChinHair, nil},
		{"facialHairCheek", k.a.FacialCheekHair, nil},
		{"eyebrows", k.a.Brows, nil},
		{"racialHorns", k.a.Horns, []uint64{HornsNone}},
		{"eyeShape", k.a.Iris, nil},
		{"scleraShape", k.a.Sclera, nil},
		{"eyeColor", k.a.EyeColor, nil},
		{"headHairRootColor", k.a.HairColor, nil},
		{"headHairTipColor", k.a.HairColor, nil},
		{"facialHairRootColor", k.a.HairColor, nil},
		{"facialHairTipColor", k.a.HairColor, nil},
		{"nailColor", k.a.NailColor, nil},
		{"presetGuid", k.a.Presets, nil},
	} {
		k.id(f.key, f.set, f.extra)
	}

	if raw, ok := k.obj["skinSet"]; ok {
		if v, ok := integer(raw); !ok {
			k.problem("skinSet is not an integer")
		} else if v < 0 || v >= int64(k.a.SkinSets) {
			k.problem("skinSet %d is not one of the %d skin sets", v, k.a.SkinSets)
		}
	}
	if raw, ok := k.obj["randomSeed"]; ok {
		if v, ok := integer(raw); !ok || v < math.MinInt32 || v > math.MaxInt32 {
			k.problem("randomSeed is not a 32-bit integer")
		}
	}

	for _, key := range []string{"skinColorHue", "skinColorPigmentation", "headHairLength", "headHairContrast",
		"facialHairLipLength", "facialHairChinLength", "facialHairCheekLength", "facialHairContrast",
		"racialHornsLength", "nailOpacity"} {
		k.rangeOf(key, 0, 1)
	}
	for _, key := range []string{"headHairGradient", "facialHairGradient"} {
		k.rangeOf(key, -1, 1)
	}
	// the three with no range of their own are held to what worldd's float32 can carry
	for _, key := range []string{"normalDetailStrength01", "normalDetailStrength02", "eyelashLength"} {
		if raw, ok := k.obj[key]; ok {
			k.singleFloat(key, raw)
		}
	}
	for _, key := range []string{"bIsHelmetVisible", "bIsCapeVisible"} {
		if raw, ok := k.obj[key]; ok {
			if t := string(bytes.TrimSpace(raw)); t != "true" && t != "false" {
				k.problem("%s is not a boolean", key)
			}
		}
	}
	k.decals()
	k.blendGroups()
	k.morphs()
	k.sections()

	unknown := 0
	for key := range k.obj {
		if !knownKeys[key] {
			unknown++
		}
	}
	if unknown > maxUnknownKeys {
		k.problem("%d members that are no part of the customization, more than %d", unknown, maxUnknownKeys)
	}
}

// [Cyb - C O A]

// id checks a 64-bit id member: absent or 0, in set, or one of extra.
func (k *checker) id(key string, set content.IDSet, extra []uint64) {
	raw, ok := k.obj[key]
	if !ok {
		return
	}
	id, ok := parseID(raw)
	if !ok {
		k.problem("%s is not a decimal id", key)
		return
	}
	if id == 0 || set.Has(id) {
		return
	}
	for _, e := range extra {
		if id == e {
			return
		}
	}
	k.problem("%s %d is not one of the %d options", key, id, len(set))
}

// rangeOf checks a float member against [lo, hi].
func (k *checker) rangeOf(key string, lo, hi float64) {
	raw, ok := k.obj[key]
	if !ok {
		return
	}
	v, ok := number(raw)
	switch {
	case !ok:
		k.problem("%s is not a number", key)
	case v < lo-tolerance || v > hi+tolerance:
		k.problem("%s %v is outside %v to %v", key, v, lo, hi)
	}
}

// singleFloat checks that a member is a number worldd's float32 holds (not 1e39, which is infinite as a float32).
func (k *checker) singleFloat(name string, raw json.RawMessage) {
	v, ok := number(raw)
	switch {
	case !ok:
		k.problem("%s is not a number", name)
	case math.IsInf(float64(float32(v)), 0):
		k.problem("%s %v does not fit a 32-bit float", name, v)
	}
}

// object is one element of an array member: a decal or a blend group.
type object struct {
	path string // decalData[3]
	m    map[string]json.RawMessage
}

// objects reads the member key as an array of null (kept as an empty slot, nothing to check) and objects, and
// returns the objects; anything else is a problem.
func (k *checker) objects(key string) []object {
	raw, ok := k.obj[key]
	if !ok {
		return nil
	}
	var list []json.RawMessage
	if isNull(raw) || json.Unmarshal(raw, &list) != nil {
		k.problem("%s is not an array", key)
		return nil
	}
	var out []object
	for i, e := range list {
		if isNull(e) {
			continue
		}
		path := fmt.Sprintf("%s[%d]", key, i)
		var m map[string]json.RawMessage
		if json.Unmarshal(e, &m) != nil || m == nil {
			k.problem("%s is not an object or null", path)
			continue
		}
		out = append(out, object{path, m})
	}
	return out
}

// [C | O | A x C_Y_B]

// member is the value of the required member name of o and its path (decalData[3].guid); it is a problem when it is missing.
func (k *checker) member(o object, name string) (json.RawMessage, string, bool) {
	raw, ok := o.m[name]
	if !ok {
		k.problem("%s.%s is missing", o.path, name)
		return nil, "", false
	}
	return raw, o.path + "." + name, true
}

func (k *checker) memberID(o object, name string) {
	if raw, path, ok := k.member(o, name); ok {
		if _, ok := parseID(raw); !ok {
			k.problem("%s is not a decimal id", path)
		}
	}
}

func (k *checker) memberFloat(o object, name string) {
	if raw, path, ok := k.member(o, name); ok {
		k.singleFloat(path, raw)
	}
}

func (k *checker) memberByte(o object, name string) {
	if raw, path, ok := k.member(o, name); ok {
		if v, ok := integer(raw); !ok || v < 0 || v > 255 {
			k.problem("%s is not an integer of 0 to 255", path)
		}
	}
}

// decals checks the decal data as worldd reads it (read_decal): every member of each decal, of its type.
func (k *checker) decals() {
	for _, d := range k.objects("decalData") {
		k.memberID(d, "guid")
		k.memberFloat(d, "strength")
		k.memberFloat(d, "scaleU")
		k.memberFloat(d, "scaleV")
		k.memberByte(d, "slotIndex")
		k.memberID(d, "mergeSectionMaterialBase")
		k.memberFloat(d, "offsetU")
		k.memberFloat(d, "offsetV")
		k.memberFloat(d, "angle")
		k.memberID(d, "color")
		k.memberFloat(d, "discoloration")
	}
}

// blendGroups checks the decal blend groups as worldd reads them (read_blend_group).
func (k *checker) blendGroups() {
	for _, g := range k.objects("decalBlendGroups") {
		k.memberID(g, "decalAppearanceGuid")
		k.memberID(g, "colorGuid")
		k.memberFloat(g, "intensity")
	}
}

var memberName = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_]{0,63}$`)

func (k *checker) morphs() {
	raw, ok := k.obj["faceMorphWeightMaps"]
	if !ok {
		return
	}
	var weights map[string]json.RawMessage
	if isNull(raw) || json.Unmarshal(raw, &weights) != nil {
		k.problem("faceMorphWeightMaps is not an object")
		return
	}
	if len(weights) > maxMapEntries {
		k.problem("faceMorphWeightMaps has %d morphs, more than %d", len(weights), maxMapEntries)
		return
	}
	names := make([]string, 0, len(weights))
	for name := range weights {
		names = append(names, name)
	}
	sort.Strings(names)
	folded := map[string]string{} // the replicated name (first letter capital) -> the JSON's
	for _, name := range names {
		v, ok := number(weights[name])
		if !ok || !memberName.MatchString(name) {
			k.problem("face morph %q is not a name with a number", name)
			continue
		}
		const lo, hi = -1.0, 1.0
		if v < lo-tolerance || v > hi+tolerance {
			k.problem("face morph %s %v is outside %v to %v", name, v, lo, hi)
		}
		// worldd turns the JSON's small first letter into the capital of the replicated name, so two names that
		// differ only there are one morph twice, which the wire cannot carry
		key := strings.ToUpper(name[:1]) + name[1:]
		if other, dup := folded[key]; dup {
			k.problem("face morphs %q and %q are the same morph (the first letter's case does not count)", other, name)
		} else {
			folded[key] = name
		}
	}
}

func (k *checker) sections() {
	raw, ok := k.obj["sectionsValues"]
	if !ok {
		return
	}
	var values map[string]json.RawMessage
	if isNull(raw) || json.Unmarshal(raw, &values) != nil {
		k.problem("sectionsValues is not an object")
		return
	}
	if len(values) > maxMapEntries {
		k.problem("sectionsValues has %d sections, more than %d", len(values), maxMapEntries)
		return
	}
	names := make([]string, 0, len(values))
	for name := range values {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		var xyz map[string]json.RawMessage
		if isNull(values[name]) || json.Unmarshal(values[name], &xyz) != nil || !memberName.MatchString(name) {
			k.problem("body section %q is not a name with x, y and z", name)
			continue
		}
		if !bodySectionSet[name] {
			k.problem("body section %q is not one of the %d sections of the client's enum", name, len(bodySections))
			continue
		}
		for _, axis := range []string{"x", "y", "z"} {
			v, ok := number(xyz[axis])
			if !ok {
				k.problem("body section %s %s is not a number", name, axis)
				continue
			}
			const from, to = 0.0, 1.0
			if v < from-tolerance || v > to+tolerance {
				k.problem("body section %s %s %v is outside %v to %v", name, axis, v, from, to)
			}
		}
	}
}

// isNull reports whether raw is the JSON null, which json.Unmarshal reads into any type without an error and
// worldd refuses everywhere.
func isNull(raw json.RawMessage) bool { return string(bytes.TrimSpace(raw)) == "null" }

// number reads a JSON number; false for anything else (null and a missing value included), and for one a
// float64 cannot hold.
func number(raw json.RawMessage) (float64, bool) {
	var v float64
	if len(raw) == 0 || isNull(raw) || json.Unmarshal(raw, &v) != nil || math.IsNaN(v) || math.IsInf(v, 0) {
		return 0, false
	}
	return v, true
}

// Cyb + C O A

// integerLiteral is a JSON number written as an integer: no fraction and no exponent (worldd reads 1.0 and 1e2
// as floats and refuses them where it wants an integer).
var integerLiteral = regexp.MustCompile(`^(0|-?[1-9][0-9]*)$`)

// integer reads a JSON number written as an integer that fits 64 bits.
func integer(raw json.RawMessage) (int64, bool) {
	text := bytes.TrimSpace(raw)
	if !integerLiteral.Match(text) {
		return 0, false
	}
	v, err := strconv.ParseInt(string(text), 10, 64)
	return v, err == nil
}

// parseID reads a 64-bit id: the decimal string the client writes, or (leniently)
// a JSON integer.
func parseID(raw json.RawMessage) (uint64, bool) {
	var s string
	if json.Unmarshal(raw, &s) == nil {
		id, err := strconv.ParseUint(s, 10, 64)
		return id, err == nil
	}
	var n json.Number
	d := json.NewDecoder(strings.NewReader(string(raw)))
	d.UseNumber()
	if d.Decode(&n) != nil {
		return 0, false
	}
	id, err := strconv.ParseUint(n.String(), 10, 64)
	return id, err == nil
}

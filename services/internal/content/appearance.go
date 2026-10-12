// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"encoding/json"
	"fmt"
	"strings"
)

// The record the client reads to find how a player character may look: the
// RaceGenderAppearanceMap named PlayerCharacter (plan B1). Its fields
// Male<Race>Id and Female<Race>Id, one per ECharacterRace name, refer to a
// RaceGenderAppearance (Kae_M_PlayerCharacter ...), which refers to a
// RaceGenderCustomizationDef (Kae_M: the hair, facial hair, brow, horn, skin
// and preset options) and that to a CharacterCustomizationEntitlements row
// (the iris, sclera and colour options and the morphs). A race with no
// appearance has Guid 0 there: Tulnar, which cannot be created.
const (
	TypeRaceGenderAppearanceMap = 2578134533504376814 // RaceGenderAppearanceMapRecordBase
	GuidPlayerAppearanceMap     = 107405461378682     // PlayerCharacter
)

// IDSet is a set of record Guids as the client's customization carries them (64
// bit ids, written as decimal strings in its JSON).
type IDSet map[uint64]struct{}

// Has reports whether id is in the set.
func (s IDSet) Has(id uint64) bool { _, ok := s[id]; return ok }

// Range is a morph's slider range.
type Range struct{ Min, Max float64 }

// Vec3 is a body section's X, Y, Z.
type Vec3 [3]float64

// Appearance is what the design database allows a player character of one race
// and gender to choose in the character creator.
type Appearance struct {
	Name string // the customization definition's name ("Kae_M")

	// From the RaceGenderCustomizationDef: the options of each choice.
	HeadHair, FacialLipHair, FacialChinHair, FacialCheekHair, Brows, Horns IDSet
	Presets                                                                IDSet
	// SkinSets is how many skin sets there are: skinSet is 0 to SkinSets-1.
	SkinSets int

	// From its CharacterCustomizationEntitlements.
	Iris, Sclera, EyeColor, HairColor, NailColor IDSet
	// Morphs are the sliders of the entitlement's CharacterMorph record, by lower
	// case morph name (a name the client writes with a lower case first letter
	// compares equal: FNames compare without case).
	Morphs map[string]Range

	// From the RaceGenderAppearance: the limits of a body section's X, Y and Z
	// (sections without an entry are unlimited), by lower case section name
	// ("lowerarms").
	SectionMin, SectionMax map[string]Vec3
}

// CoA.C:Y:B

// reference is a reference to a record: {"Guid": 108136760497790, "Name": "...",
// "TypeId": 6170063386889671940}.
type reference struct {
	Guid   int64
	TypeID uint64 `json:"TypeId"`
}

// fields is the properties of a record by lower case name: contentc reads the
// design data without regard to case, and the spelling in record.data follows
// the mappings file in use.
type fields map[string]json.RawMessage

func (d *DB) fieldsOf(r reference) (fields, string, error) {
	rec, err := d.Record(r.TypeID, r.Guid)
	if err != nil {
		return nil, "", err
	}
	var raw map[string]json.RawMessage
	if err := json.Unmarshal(rec.Fields, &raw); err != nil {
		return nil, "", fmt.Errorf("content: record %d: %w", r.Guid, err)
	}
	f := make(fields, len(raw))
	for k, v := range raw {
		f[strings.ToLower(k)] = v
	}
	return f, rec.Name, nil
}

func (f fields) ref(name string) (reference, error) {
	var r reference
	raw, ok := f[strings.ToLower(name)]
	if !ok {
		return r, fmt.Errorf("no %s", name)
	}
	if err := json.Unmarshal(raw, &r); err != nil {
		return r, fmt.Errorf("%s is not a reference: %w", name, err)
	}
	return r, nil
}

func (f fields) idSet(name string) (IDSet, error) {
	raw, ok := f[strings.ToLower(name)]
	if !ok {
		return nil, fmt.Errorf("no %s", name)
	}
	var refs []reference
	if err := json.Unmarshal(raw, &refs); err != nil {
		return nil, fmt.Errorf("%s is not a list of references: %w", name, err)
	}
	set := make(IDSet, len(refs))
	for _, r := range refs {
		set[uint64(r.Guid)] = struct{}{}
	}
	return set, nil
}

// PlayerAppearance returns the options of a player character of the race and
// gender, by their ECharacterRace and ECharacterGender names ("Kaelar",
// "Male"). It returns nil and no error when the race has no appearance (the map
// holds Guid 0: Tulnar); an error for a map or a chain of records that is not
// what this reader expects.
func (d *DB) PlayerAppearance(race, gender string) (*Appearance, error) {
	what := gender + race
	m, _, err := d.fieldsOf(reference{Guid: GuidPlayerAppearanceMap, TypeID: TypeRaceGenderAppearanceMap})
	if err != nil {
		return nil, fmt.Errorf("content: the player appearance map: %w", err)
	}
	ref, err := m.ref(gender + race + "Id")
	if err != nil {
		return nil, fmt.Errorf("content: the player appearance map: %s: %w", what, err)
	}
	if ref.Guid == 0 {
		return nil, nil
	}
	a, err := d.appearance(ref)
	if err != nil {
		return nil, fmt.Errorf("content: appearance of %s: %w", what, err)
	}
	return a, nil
}

func (d *DB) appearance(ref reference) (*Appearance, error) {
	app, _, err := d.fieldsOf(ref)
	if err != nil {
		return nil, err
	}
	defRef, err := app.ref("CustomizationDefId")
	if err != nil {
		return nil, err
	}
	def, defName, err := d.fieldsOf(defRef)
	if err != nil {
		return nil, err
	}
	a := &Appearance{Name: defName}
	for _, p := range []struct {
		dst  *IDSet
		name string
	}{
		{&a.HeadHair, "HeadHairOptionsIds"}, {&a.FacialLipHair, "FacialLipHairOptionsIds"},
		{&a.FacialChinHair, "FacialChinHairOptionsIds"}, {&a.FacialCheekHair, "FacialCheekHairOptionsIds"},
		{&a.Brows, "FacialBrowHairOptionsIds"}, {&a.Horns, "RacialHornOptionsIds"}, {&a.Presets, "PresetsIds"},
	} {
		if *p.dst, err = def.idSet(p.name); err != nil {
			return nil, fmt.Errorf("%s: %w", defName, err)
		}
	}
	skins, err := def.idSet("SkinSetIds")
	if err != nil {
		return nil, fmt.Errorf("%s: %w", defName, err)
	}
	a.SkinSets = len(skins)

	entRef, err := def.ref("CustomizationEntitlementId")
	if err != nil {
		return nil, fmt.Errorf("%s: %w", defName, err)
	}
	ent, entName, err := d.fieldsOf(entRef)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", defName, err)
	}
	for _, p := range []struct {
		dst  *IDSet
		name string
	}{
		{&a.Iris, "IrisIDsIds"}, {&a.Sclera, "ScleraIDsIds"}, {&a.EyeColor, "IrisColorIDsIds"},
		{&a.HairColor, "HairColorIDsIds"}, {&a.NailColor, "NailColorIDsIds"},
	} {
		if *p.dst, err = ent.idSet(p.name); err != nil {
			return nil, fmt.Errorf("%s: %w", entName, err)
		}
	}
	morphRef, err := ent.ref("MorphsId")
	if err != nil {
		return nil, fmt.Errorf("%s: %w", entName, err)
	}
	if a.Morphs, err = d.morphs(morphRef); err != nil {
		return nil, fmt.Errorf("%s: %w", entName, err)
	}
	if a.SectionMin, a.SectionMax, err = sectionLimits(app); err != nil {
		return nil, err
	}
	return a, nil
}

// Cyb captures, C-O-A code

// morphs reads a CharacterMorph record: {"Morph": [{"Morphs": [{"MorphName",
// "Min", "Max"}]}]}.
func (d *DB) morphs(ref reference) (map[string]Range, error) {
	rec, _, err := d.fieldsOf(ref)
	if err != nil {
		return nil, err
	}
	var groups []struct {
		Morphs []struct {
			MorphName string
			Min, Max  float64
		}
	}
	if err := json.Unmarshal(rec["morph"], &groups); err != nil {
		return nil, fmt.Errorf("Morph is not a list of morph groups: %w", err)
	}
	out := map[string]Range{}
	for _, g := range groups {
		for _, m := range g.Morphs {
			out[strings.ToLower(m.MorphName)] = Range{m.Min, m.Max}
		}
	}
	return out, nil
}

// sectionLimits reads a RaceGenderAppearance's SectionsValuesLimits:
// {"MaxSectionsValues": [{"Key": "FCharacterCustomizationBodySections::Chest",
// "Value": {"X", "Y", "Z"}}], "MinSectionsValues": [...]}.
func sectionLimits(app fields) (lo, hi map[string]Vec3, err error) {
	var limits struct {
		MaxSectionsValues []sectionValue
		MinSectionsValues []sectionValue
	}
	if raw, ok := app["sectionsvalueslimits"]; ok {
		if err := json.Unmarshal(raw, &limits); err != nil {
			return nil, nil, fmt.Errorf("SectionsValuesLimits: %w", err)
		}
	}
	read := func(list []sectionValue) map[string]Vec3 {
		out := map[string]Vec3{}
		for _, s := range list {
			name := s.Key
			if i := strings.LastIndex(name, "::"); i >= 0 {
				name = name[i+2:]
			}
			out[strings.ToLower(name)] = Vec3{s.Value.X, s.Value.Y, s.Value.Z}
		}
		return out
	}
	return read(limits.MinSectionsValues), read(limits.MaxSectionsValues), nil
}

type sectionValue struct {
	Key   string
	Value struct{ X, Y, Z float64 }
}

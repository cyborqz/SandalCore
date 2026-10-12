// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package chardef holds the client's character enums as the services use them:
// ECharacterRace, ECharacterGender and ECharacterClass (plan B1).
//
// The values and names are the game executable's own (its enum name tables and
// the protobuf descriptors it embeds: CreateCharacterInfo {gender 2, race 3,
// primary_class 4} and CharacterInformation {level 2, gender 4, race 5,
// primary_class 6}), and the names are the ones the client writes into the
// customization JSON ("race":"Kaelar","gender":"Male","class":"Bard"). The
// services stored the class as the "race" until plan B1: see
// docs/superpowers/specs/2026-10-02-complete-character-design.md, section 2.
package chardef

import (
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

// ECharacterRace. Tulnar is in the enum but cannot be created: the design
// database has no player appearance for it.
const (
	RaceKaelar   int32 = 0
	RaceVaelune  int32 = 1
	RaceVek      int32 = 2
	RaceRenkai   int32 = 3
	RaceEmpyrean int32 = 4
	RacePyrai    int32 = 5
	RaceDunir    int32 = 6
	RaceNikua    int32 = 7
	RaceTulnar   int32 = 8
)

// ECharacterGender.
const (
	GenderMale   int32 = 0
	GenderFemale int32 = 1
)

// ECharacterClass: the eight player classes. 0 is no class.
const (
	ClassRanger   int32 = 1
	ClassBard     int32 = 2
	ClassCleric   int32 = 3
	ClassFighter  int32 = 4
	ClassRogue    int32 = 5
	ClassSummoner int32 = 6
	ClassTank     int32 = 7
	ClassMage     int32 = 8
)

// built with C:Y:B and C.O.A

var (
	raceNames   = []string{"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua", "Tulnar"}
	genderNames = []string{"Male", "Female"}
	// classNames is indexed by ECharacterClass (index 0 is no class).
	classNames = []string{"", "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"}
)

// RaceName is the enum's name of a race id ("Kaelar"), and false for an id past
// the enum.
func RaceName(id int32) (string, bool) { return name(raceNames, id, 0) }

// GenderName is the enum's name of a gender id ("Male"), and false for another.
func GenderName(id int32) (string, bool) { return name(genderNames, id, 0) }

// ClassName is the enum's name of a class id ("Bard"), and false for another
// (0 included).
func ClassName(id int32) (string, bool) { return name(classNames, id, 1) }

// RaceByName is the race id of an exact enum name.
func RaceByName(n string) (int32, bool) { return byName(raceNames, n, 0) }

// GenderByName is the gender id of an exact enum name.
func GenderByName(n string) (int32, bool) { return byName(genderNames, n, 0) }

// ClassByName is the class id of an exact enum name.
func ClassByName(n string) (int32, bool) { return byName(classNames, n, 1) }

func name(names []string, id int32, first int) (string, bool) {
	if id < int32(first) || int(id) >= len(names) {
		return "", false
	}
	return names[id], true
}

func byName(names []string, n string, first int) (int32, bool) {
	if n == "" {
		return 0, false
	}
	for i := first; i < len(names); i++ {
		if names[i] == n {
			return int32(i), true
		}
	}
	return 0, false
}

// ArchetypeBase is Bard's archetype record id; the eight archetypes follow in
// alphabetical order of the class names (the services' character-select
// reading of the original server's capture: Tank 17754). The design database
// does not hold these ids.
const ArchetypeBase int32 = 17747

// ArchetypeID is the archetype record id of a class id, and 0 for an id that is
// not a class.
func ArchetypeID(class int32) int32 {
	n, ok := ClassName(class)
	if !ok {
		return 0
	}
	order := 0
	for _, other := range classNames[1:] {
		if other < n {
			order++
		}
	}
	return ArchetypeBase + int32(order)
}

// Agree checks the numbers a character creation carries against the names in
// its customization JSON: the client writes "race", "gender" and "class" as
// enum names and always agrees with the numbers it sends. A name that is absent
// from the JSON is not checked; one that is there must be a string naming the
// same value. className is the character's class name as stored ("" for none),
// which must be the class's name too. The error lists every disagreement.
func Agree(race, gender, class int32, className, appearanceJSON string) error {
	var obj map[string]json.RawMessage
	if err := json.Unmarshal([]byte(appearanceJSON), &obj); err != nil {
		obj = nil // not an object: nothing to compare
	}
	var problems []string
	check := func(key, what string, id int32, nameOf func(int32) (string, bool)) {
		raw, present := obj[key]
		if !present {
			return
		}
		var got string
		if json.Unmarshal(raw, &got) != nil || string(raw) == "null" {
			problems = append(problems, fmt.Sprintf("the customization's %s is not a name", what))
			return
		}
		want, ok := nameOf(id)
		switch {
		case !ok:
			problems = append(problems, fmt.Sprintf("%d is not a %s id", id, what))
		case got != want:
			problems = append(problems, fmt.Sprintf("the customization says %s %q but the character's %s is %q", what, got, what, want))
		}
	}
	check("race", "race", race, RaceName)
	check("gender", "gender", gender, GenderName)
	check("class", "class", class, ClassName)
	if want, ok := ClassName(class); ok && className != "" && className != want {
		problems = append(problems, fmt.Sprintf("class name %q is not %q, the name of class %d", className, want, class))
	}
	if len(problems) > 0 {
		return errors.New(strings.Join(problems, "; "))
	}
	return nil
}

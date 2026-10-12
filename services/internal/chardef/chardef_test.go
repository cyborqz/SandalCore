// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package chardef

import (
	"strings"
	"testing"
)

// The values are the executable's: ECharacterRace Kaelar 0 ... Tulnar 8,
// ECharacterGender Male 0, Female 1, ECharacterClass Ranger 1, Bard 2, Cleric 3,
// Fighter 4, Rogue 5, Summoner 6, Tank 7, Mage 8.
func TestTheEnumsAreTheClients(t *testing.T) {
	for id, name := range map[int32]string{0: "Kaelar", 1: "Vaelune", 2: "Vek", 3: "Renkai", 4: "Empyrean", 5: "Pyrai", 6: "Dunir", 7: "Nikua", 8: "Tulnar"} {
		if got, ok := RaceName(id); !ok || got != name {
			t.Errorf("RaceName(%d) = %q, %v; want %q", id, got, ok, name)
		}
		if got, ok := RaceByName(name); !ok || got != id {
			t.Errorf("RaceByName(%q) = %d, %v; want %d", name, got, ok, id)
		}
	}
	for id, name := range map[int32]string{0: "Male", 1: "Female"} {
		if got, ok := GenderName(id); !ok || got != name {
			t.Errorf("GenderName(%d) = %q, %v; want %q", id, got, ok, name)
		}
		if got, ok := GenderByName(name); !ok || got != id {
			t.Errorf("GenderByName(%q) = %d, %v; want %d", name, got, ok, id)
		}
	}
	for id, name := range map[int32]string{1: "Ranger", 2: "Bard", 3: "Cleric", 4: "Fighter", 5: "Rogue", 6: "Summoner", 7: "Tank", 8: "Mage"} {
		if got, ok := ClassName(id); !ok || got != name {
			t.Errorf("ClassName(%d) = %q, %v; want %q", id, got, ok, name)
		}
		if got, ok := ClassByName(name); !ok || got != id {
			t.Errorf("ClassByName(%q) = %d, %v; want %d", name, got, ok, id)
		}
	}
	if RaceKaelar != 0 || RaceNikua != 7 || RaceTulnar != 8 || GenderFemale != 1 || ClassBard != 2 || ClassTank != 7 || ClassMage != 8 {
		t.Error("a constant is not the client's value")
	}
}

func TestIdsAndNamesOutsideTheEnumsAreRefused(t *testing.T) {
	for _, id := range []int32{-1, 9, 100} {
		if n, ok := RaceName(id); ok {
			t.Errorf("RaceName(%d) = %q", id, n)
		}
	}
	for _, id := range []int32{-1, 2} {
		if n, ok := GenderName(id); ok {
			t.Errorf("GenderName(%d) = %q", id, n)
		}
	}
	for _, id := range []int32{-1, 0, 9} { // 0 is no class
		if n, ok := ClassName(id); ok {
			t.Errorf("ClassName(%d) = %q", id, n)
		}
	}
	for _, name := range []string{"", "kaelar", "KAELAR", "Kaelar ", "Orc", "Male"} {
		if id, ok := RaceByName(name); ok {
			t.Errorf("RaceByName(%q) = %d", name, id)
		}
	}
	for _, name := range []string{"", "male", "Other", "Kaelar"} {
		if id, ok := GenderByName(name); ok {
			t.Errorf("GenderByName(%q) = %d", name, id)
		}
	}
	for _, name := range []string{"", "bard", "Paladin", "Bard "} {
		if id, ok := ClassByName(name); ok {
			t.Errorf("ClassByName(%q) = %d", name, id)
		}
	}
}

// C | Y | B.CoA

// The archetype record ids are in alphabetical order of the class names, from
// Bard 17747 to Tank 17754 (the services' reading of the original server's
// capture); a value that is no class has none.
func TestArchetypeIDs(t *testing.T) {
	for class, want := range map[int32]int32{
		ClassBard: 17747, ClassCleric: 17748, ClassFighter: 17749, ClassMage: 17750,
		ClassRanger: 17751, ClassRogue: 17752, ClassSummoner: 17753, ClassTank: 17754,
		0: 0, -1: 0, 9: 0,
	} {
		if got := ArchetypeID(class); got != want {
			t.Errorf("ArchetypeID(%d) = %d, want %d", class, got, want)
		}
	}
}

func TestAgree(t *testing.T) {
	const dunir = `{"race":"Dunir","gender":"Female","class":"Tank","skinSet":1}`
	for _, tc := range []struct {
		name                string
		race, gender, class int32
		className, json     string
		want                string // "" for agreement
	}{
		{"all three agree", RaceDunir, GenderFemale, ClassTank, "Tank", dunir, ""},
		{"no class name", RaceDunir, GenderFemale, ClassTank, "", dunir, ""},
		{"names that are absent are not checked", RaceDunir, GenderFemale, ClassTank, "Tank", `{"skinSet":1}`, ""},
		{"not an object: nothing to compare", RaceVek, GenderMale, ClassMage, "Mage", `[1]`, ""},
		{"not JSON: nothing to compare", RaceVek, GenderMale, ClassMage, "Mage", `nope`, ""},
		{"empty: nothing to compare", RaceVek, GenderMale, ClassMage, "", ``, ""},
		{"another race", RaceKaelar, GenderFemale, ClassTank, "Tank", dunir, `the customization says race "Dunir" but the character's race is "Kaelar"`},
		{"another gender", RaceDunir, GenderMale, ClassTank, "Tank", dunir, `the customization says gender "Female" but the character's gender is "Male"`},
		{"another class", RaceDunir, GenderFemale, ClassBard, "Bard", dunir, `the customization says class "Tank" but the character's class is "Bard"`},
		{"a class name that is not the class's", RaceDunir, GenderFemale, ClassTank, "Mage", dunir, `class name "Mage" is not "Tank"`},
		{"a name that is not a string", RaceDunir, GenderFemale, ClassTank, "", `{"race":6}`, "the customization's race is not a name"},
		{"a name that is null", RaceDunir, GenderFemale, ClassTank, "", `{"gender":null}`, "the customization's gender is not a name"},
		{"a race id that is no race", 9, GenderFemale, ClassTank, "", dunir, "9 is not a race id"},
		{"every problem is listed", RaceKaelar, GenderMale, ClassBard, "Tank", dunir, `race "Dunir" but the character's race is "Kaelar"; the customization says gender "Female"`},
		{"names are exact", RaceDunir, GenderFemale, ClassTank, "Tank", `{"race":"dunir"}`, `the customization says race "dunir"`},
	} {
		err := Agree(tc.race, tc.gender, tc.class, tc.className, tc.json)
		switch {
		case tc.want == "" && err != nil:
			t.Errorf("%s: %v", tc.name, err)
		case tc.want != "" && (err == nil || !strings.Contains(err.Error(), tc.want)):
			t.Errorf("%s: %v, want an error with %q", tc.name, err, tc.want)
		}
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

// The Destiny window's tracks in the game's own data (the shared build of real_test.go; skipped without AOC_DESIGN_DB and AOC_USMAP).

// The First Steps track quest has one objective, a SubQuest that starts the track's own quest and waits for it.
func TestRealDestinySubQuest(t *testing.T) {
	db := builtDB(t)
	var detail string
	if err := db.SQL().QueryRow(`SELECT detail FROM quest_objective WHERE guid = ?`, 6064634619246149635).Scan(&detail); err != nil {
		t.Fatal(err)
	}
	var d struct {
		SubQuest *subQuestJSON `json:"sq"`
	}
	if err := json.Unmarshal([]byte(detail), &d); err != nil {
		t.Fatalf("%s: %v", detail, err)
	}
	want := &subQuestJSON{Quest: 6064634478640431178, Kind: "AutoGrant", Wait: 1}
	if !reflect.DeepEqual(d.SubQuest, want) {
		t.Errorf("Destiny_Novice_FirstStep_02_Root_Obj holds %s, want sq %+v", detail, want)
	}
	// the track's own quest is made of SubQuest objectives that all name a quest: its eight tasks come first
	rows, err := db.SQL().Query(`SELECT idx, obj_type, detail FROM quest_objective WHERE quest_guid = ? ORDER BY idx`, 6064634478640431178)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	n := 0
	for rows.Next() {
		var idx int
		var typ, det string
		if err := rows.Scan(&idx, &typ, &det); err != nil {
			t.Fatal(err)
		}
		var o struct {
			SubQuest *subQuestJSON `json:"sq"`
		}
		if err := json.Unmarshal([]byte(det), &o); err != nil {
			t.Fatal(err)
		}
		if typ != "SubQuest" || o.SubQuest == nil || o.SubQuest.Kind != "AutoGrant" {
			t.Errorf("objective %d of Destiny_Novice_FirstStep_02 is %s with sq %+v", idx, typ, o.SubQuest)
		}
		n++
	}
	if n != 12 {
		t.Errorf("Destiny_Novice_FirstStep_02 has %d objectives, want 12", n)
	}
}

// destiny_track holds the six tracks in the DestinySystem record's order, each with the quest its track quest starts.
func TestRealDestinyTracks(t *testing.T) {
	db := builtDB(t)
	type row struct {
		Ord, Track, Quest, Tree int64
		Milestones              int
	}
	want := []row{
		{0, 6064634619243986946, 6064634478640431178, 6064634483139346458, 4}, // First Steps
		{1, 6064634619255848966, 6064634479026765923, 6064634483232866355, 4}, // Initiate Adventuring
		{2, 6064634619250147332, 6064634461176397824, 6064634455876894725, 4}, // Gathering
		{3, 6064634619260043272, 6064634477777846296, 6064634479203647612, 4}, // Processing
		{4, 6064634619231207424, 6064634478155333681, 6064634483063259136, 4}, // Crafting
		{5, 6064634693269913600, 6064634676602077184, 6064634676602208257, 3}, // Combat
	}
	rows, err := db.SQL().Query(`SELECT ord, track_quest, quest_guid, skill_tree, milestones FROM destiny_track ORDER BY ord`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var got []row
	for rows.Next() {
		var r row
		var ms string
		if err := rows.Scan(&r.Ord, &r.Track, &r.Quest, &r.Tree, &ms); err != nil {
			t.Fatal(err)
		}
		var list []int64
		if err := json.Unmarshal([]byte(ms), &list); err != nil {
			t.Fatalf("milestones %q: %v", ms, err)
		}
		r.Milestones = len(list)
		got = append(got, r)
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("destiny_track = %+v\nwant %+v", got, want)
	}
	// every track quest and every own quest is a quest of the data, and every own quest's first eight objectives (the tasks) have no activation rule
	for _, w := range want {
		for _, q := range []int64{w.Track, w.Quest} {
			if n := scalarInt(t, db, `SELECT count(*) FROM quest WHERE guid = ?`, q); n != 1 {
				t.Errorf("track %d: quest %d is in the quest table %d times", w.Ord, q, n)
			}
		}
		if n := scalarInt(t, db, `SELECT count(*) FROM quest_objective WHERE quest_guid = ? AND detail NOT LIKE '%"ar":%'`, w.Quest); n != 8 {
			t.Errorf("track %d: its own quest has %d objectives without an activation rule, want 8 (the tasks)", w.Ord, n)
		}
	}
}

// What the first tasks of First Steps and Combat count: the abilities each UseAbility objective lists or requires, and the kill query of KillEnemies.
func TestRealDestinyTaskDetails(t *testing.T) {
	db := builtDB(t)
	detail := func(guid int64) map[string]json.RawMessage {
		var text string
		if err := db.SQL().QueryRow(`SELECT detail FROM quest_objective WHERE guid = ?`, guid).Scan(&text); err != nil {
			t.Fatal(err)
		}
		var d struct {
			D map[string]json.RawMessage `json:"d"`
		}
		if err := json.Unmarshal([]byte(text), &d); err != nil {
			t.Fatalf("%s: %v", text, err)
		}
		return d.D
	}
	abilities := func(d map[string]json.RawMessage) []int64 {
		var l []int64
		if err := json.Unmarshal(d["ab"], &l); err != nil {
			t.Fatalf("ab = %s: %v", d["ab"], err)
		}
		return l
	}
	contains := func(l []int64, g int64) bool {
		for _, x := range l {
			if x == g {
				return true
			}
		}
		return false
	}
	// Sprint lists its ability; it counts in or out of a fight
	if d := detail(6064634686960506630); !reflect.DeepEqual(abilities(d), []int64{6064631641648332884}) || d["cmb"] != nil {
		t.Errorf("Sprint: %s", d)
	}
	// Dodge and Guard name no ability: they require a tag, and count in a fight only
	for _, c := range []struct {
		name   string
		guid   int64
		wanted int
		has    int64
		tag    string
	}{
		{"Dodge", 6064634686959261428, 4, 107258680465016, "ability.dodge"},
		{"Guard", 6064634686959785725, 1, 0, "ability.guard"},
		{"ComboMelee", 6064634686953428636, 16, 0, "ability.weaponcombo"},
		{"ComboRanged", 6064634686955263671, 6, 0, "ability.weaponcombo"},
	} {
		d := detail(c.guid)
		ab := abilities(d)
		if len(ab) != c.wanted || (c.has != 0 && !contains(ab, c.has)) || string(d["cmb"]) != "1" || !strings.Contains(string(d["tg"]), c.tag) {
			t.Errorf("%s: %d abilities (want %d), cmb %s, tg %s", c.name, len(ab), c.wanted, d["cmb"], d["tg"])
		}
	}
	// the rations: the four abilities of the record
	if ab := abilities(detail(6064634478835597392)); len(ab) != 4 {
		t.Errorf("UseRation lists %d abilities, want 4", len(ab))
	}
	// Kill 10 Enemies: the query is "any creature" (the root tag Character)
	if q := string(detail(6064634478936391775)["gq"]); q != `{"tok":[0,1,1,1,0],"tags":["character"]}` {
		t.Errorf("KillEnemies: gq = %s", q)
	}
}

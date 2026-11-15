// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// The certification dialogue actions and the recipe items on the real design database (the shared build of real_test.go).

import "testing"

// TestRealCertification checks the dialogue actions of the artisan certifiers and the recipe items against what the data holds.
func TestRealCertification(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	if meta["typed.item_learn"] != "2829" {
		t.Errorf("meta typed.item_learn = %q, want 2829", meta["typed.item_learn"])
	}

	// the dialogue: 94 lines carry a SetCertificationLevel action (47 Promote and 47 Demote), each names its profession and its direction, and the line has no other action
	const certLine = `json_extract(actions, '$[0].a') = 'SetCertificationLevel'`
	if n := scalarInt(t, db, `SELECT count(*) FROM dialogue_choice WHERE `+certLine); n != 94 {
		t.Errorf("%d dialogue lines carry a SetCertificationLevel action, want 94", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM dialogue_choice WHERE `+certLine+` AND json_array_length(actions) = 1 AND json_extract(actions, '$[0].p') IN (SELECT guid FROM profession)
		AND json_extract(actions, '$[0].ca') IN (1, 2)`); n != 94 {
		t.Errorf("%d of the 94 certification lines name one of the 22 professions and a direction, want all", n)
	}
	for ca, want := range map[int]int64{1: 47, 2: 47} {
		if n := scalarInt(t, db, `SELECT count(*) FROM dialogue_choice WHERE `+certLine+` AND json_extract(actions, '$[0].ca') = ?`, ca); n != want {
			t.Errorf("%d certification lines with direction %d, want %d", n, ca, want)
		}
	}
	// the 8 building menus of the artisan certifiers: 44 lines, one Promote and one Demote for each of the 22 professions
	rows, err := db.SQL().Query(`SELECT json_extract(c.actions, '$[0].p'), json_extract(c.actions, '$[0].ca'), count(*)
		FROM dialogue_choice c JOIN dialogue_node n ON n.guid = c.node_guid
		WHERE n.name LIKE '%CertProfChoice%' AND json_extract(c.actions, '$[0].a') = 'SetCertificationLevel' GROUP BY 1, 2`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	type pair struct{ profession, direction int64 }
	seen := map[pair]int64{}
	professions := map[int64]bool{}
	for rows.Next() {
		var p pair
		var n int64
		if err := rows.Scan(&p.profession, &p.direction, &n); err != nil {
			t.Fatal(err)
		}
		seen[p] = n
		professions[p.profession] = true
	}
	if len(seen) != 44 || len(professions) != 22 {
		t.Errorf("the building menus have %d (profession, direction) pairs for %d professions, want 44 for 22", len(seen), len(professions))
	}
	for p, n := range seen {
		if n != 1 || (p.direction != 1 && p.direction != 2) {
			t.Errorf("the building menus: profession %d direction %d on %d line(s), want one line of direction 1 or 2", p.profession, p.direction, n)
		}
	}

	// the recipe items: 2,858 items have the LearnRecipes activation; 2,829 name one recipe each and 29 name none (28 whose recipes the data does not have, and one whose list
	// holds a null id)
	if n := scalarInt(t, db, `SELECT count(*) FROM item_use WHERE activation = 'LearnRecipes'`); n != 2858 {
		t.Errorf("%d LearnRecipes items, want 2858", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM item_learn`); n != 2829 {
		t.Errorf("item_learn holds %d rows, want 2829", n)
	}
	if n := scalarInt(t, db, `SELECT count(DISTINCT item_guid) FROM item_learn`); n != 2829 {
		t.Errorf("item_learn holds %d items, want 2829 (one recipe each)", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM item_use u WHERE u.activation = 'LearnRecipes' AND NOT EXISTS (SELECT 1 FROM item_learn l WHERE l.item_guid = u.item_guid)`); n != 29 {
		t.Errorf("%d LearnRecipes items teach nothing, want 29", n)
	}
	// what each row asks: a profession and a certification level (2,397), a profession alone (360: Novice), neither (72)
	for _, c := range []struct {
		what  string
		where string
		want  int64
	}{
		{"a profession and a certification level", `profession_guid != 0 AND certification_guid != 0`, 2397},
		{"a profession and no certification level", `profession_guid != 0 AND certification_guid = 0`, 360},
		{"neither", `profession_guid = 0 AND certification_guid = 0`, 72},
		{"a certification level and no profession", `profession_guid = 0 AND certification_guid != 0`, 0},
	} {
		if n := scalarInt(t, db, `SELECT count(*) FROM item_learn WHERE `+c.where); n != c.want {
			t.Errorf("%d recipe items ask for %s, want %d", n, c.what, c.want)
		}
	}
	// every guid a row names is a row of its table
	for _, c := range []struct{ what, query string }{
		{"a profession that is none of the 22", `SELECT count(*) FROM item_learn WHERE profession_guid != 0 AND profession_guid NOT IN (SELECT guid FROM profession)`},
		{"a certification level that is none of the 5", `SELECT count(*) FROM item_learn WHERE certification_guid != 0 AND certification_guid NOT IN (SELECT guid FROM certification_level)`},
		{"a recipe that is no craft_recipe", `SELECT count(*) FROM item_learn WHERE recipe_guid NOT IN (SELECT guid FROM craft_recipe)`},
		{"an item that is no item", `SELECT count(*) FROM item_learn WHERE item_guid NOT IN (SELECT guid FROM item)`},
	} {
		if n := scalarInt(t, db, c.query); n != 0 {
			t.Errorf("%d item_learn rows name %s", n, c.what)
		}
	}
}

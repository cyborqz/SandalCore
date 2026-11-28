// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"
	"testing"

	"aocrop/services/internal/designdb"
)

// No quest column holds a JSON null. worldd reads a list from the key that holds it and refuses anything else, so one null in one row keeps the whole quest book from loading
// (every character then plays with its quests off). A tag query that names no tag is written as "tags": [].

// killObjective is a Kill objective record whose TagQuery has the token stream `tok` and the tags `tags`.
func killObjective(guid int64, tok []int64, tags ...string) designdb.Row {
	var stream, dict []designdb.Value
	for _, t := range tok {
		stream = append(stream, t)
	}
	for _, tag := range tags {
		dict = append(dict, designdb.Struct{"TagName": tag})
	}
	return designdb.Row{Guid: guid, Fields: designdb.Struct{
		"ObjectiveType": "ENarrativeObjectiveType::Kill",
		"KillObjectiveData": designdb.Struct{
			"TagQuery": designdb.Struct{"QueryTokenStream": stream, "TagDictionary": dict},
		},
	}}
}

// A Kill objective whose query holds no tag keeps the query's token stream and an empty list of tags (not null), and does not say it has a tag query ("tq").
func TestKillQueryWithoutTagsIsAnEmptyList(t *testing.T) {
	for _, c := range []struct {
		name string
		row  designdb.Row
		gq   string // the "gq" the detail has
		tq   bool   // whether it says "tq"
	}{
		{"all of no tags", killObjective(1, []int64{0, 1, 2, 0}), `{"tok":[0,1,2,0],"tags":[]}`, false},
		{"any of no tags", killObjective(2, []int64{0, 1, 1, 0}), `{"tok":[0,1,1,0],"tags":[]}`, false},
		{"one tag", killObjective(3, []int64{0, 1, 1, 1, 0}, "Character"), `{"tok":[0,1,1,1,0],"tags":["character"]}`, true},
	} {
		text, err := objectiveDetail(nil, c.row, nil)
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if strings.Contains(text, "null") {
			t.Errorf("%s: the detail holds a null: %s", c.name, text)
		}
		var d struct {
			D map[string]json.RawMessage `json:"d"`
		}
		if err := json.Unmarshal([]byte(text), &d); err != nil {
			t.Fatalf("%s: %s: %v", c.name, text, err)
		}
		if got := string(d.D["gq"]); got != c.gq {
			t.Errorf("%s: gq = %s, want %s", c.name, got, c.gq)
		}
		if _, has := d.D["tq"]; has != c.tq {
			t.Errorf("%s: tq present = %v, want %v (%s)", c.name, has, c.tq, text)
		}
	}
}

// nullsIn lists the paths of the JSON nulls in a document ("/d/gq/tags"); the indexes of a list read as "#".
func nullsIn(text string) ([]string, error) {
	var v any
	if err := json.Unmarshal([]byte(text), &v); err != nil {
		return nil, err
	}
	var out []string
	var walk func(x any, path string)
	walk = func(x any, path string) {
		switch x := x.(type) {
		case nil:
			out = append(out, path)
		case map[string]any:
			for k, e := range x {
				walk(e, path+"/"+k)
			}
		case []any:
			for _, e := range x {
				walk(e, path+"/#")
			}
		}
	}
	walk(v, "")
	sort.Strings(out)
	return out, nil
}

func TestNullsIn(t *testing.T) {
	got, err := nullsIn(`{"d":{"gq":{"tok":[0,1,2,0],"tags":null},"sets":[1,2]},"on":{"start":[{"a":null}]},"rw":[]}`)
	if err != nil {
		t.Fatal(err)
	}
	if want := []string{"/d/gq/tags", "/on/start/#/a"}; fmt.Sprint(got) != fmt.Sprint(want) {
		t.Errorf("nullsIn = %v, want %v", got, want)
	}
	if got, _ := nullsIn(`{"d":{"gq":{"tok":[0,1,2,0],"tags":[]}}}`); len(got) != 0 {
		t.Errorf("nullsIn of a document without a null = %v", got)
	}
}

// Every quest column of the real content.db, whose text is JSON, holds no null; and the 57 Kill objectives whose query has no tag carry an empty list (the shared build of
// real_test.go; skipped without AOC_DESIGN_DB and AOC_USMAP).
func TestRealQuestColumnsHaveNoJSONNull(t *testing.T) {
	db := builtDB(t)
	for _, c := range []struct{ table, column, key string }{
		{"quest_objective", "detail", "guid"},
		{"quest", "detail", "guid"},
		{"quest", "activation", "guid"},
		{"quest", "start_actions", "guid"},
		{"quest", "rewards", "guid"},
		{"destiny_track", "milestones", "ord"},
		{"dialogue_accept", "objectives", "node_guid"},
		{"dialogue_turnin", "objectives", "node_guid"},
	} {
		rows, err := db.SQL().Query(fmt.Sprintf(`SELECT %s, %s FROM %s ORDER BY %s`, c.key, c.column, c.table, c.key))
		if err != nil {
			t.Fatalf("%s.%s: %v", c.table, c.column, err)
		}
		n, bad := 0, 0
		paths := map[string]int{}
		var first int64
		for rows.Next() {
			var key int64
			var text string
			if err := rows.Scan(&key, &text); err != nil {
				rows.Close()
				t.Fatalf("%s.%s: %v", c.table, c.column, err)
			}
			n++
			if text == "" {
				continue
			}
			nulls, err := nullsIn(text)
			if err != nil {
				t.Errorf("%s.%s %d: %v: %s", c.table, c.column, key, err, text)
				continue
			}
			if len(nulls) > 0 {
				if bad == 0 {
					first = key
				}
				bad++
				for _, p := range nulls {
					paths[p]++
				}
			}
		}
		if err := rows.Err(); err != nil {
			t.Fatalf("%s.%s: %v", c.table, c.column, err)
		}
		rows.Close()
		if bad > 0 {
			t.Errorf("%s.%s: %d of %d rows hold a JSON null (first: %d), at %v", c.table, c.column, bad, n, first, paths)
		}
		t.Logf("%s.%s: %d rows, %d with a JSON null", c.table, c.column, n, bad)
	}

	// the Kill objectives whose tag query names no tag: their query is an empty list (53 "all tags" and 4 "any tag" queries in the data)
	rows, err := db.SQL().Query(`SELECT detail FROM quest_objective WHERE obj_type = 'Kill' AND detail LIKE '%"gq"%'`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	withQuery, noTags := 0, 0
	byTokens := map[string]int{}
	for rows.Next() {
		var text string
		if err := rows.Scan(&text); err != nil {
			t.Fatal(err)
		}
		var d struct {
			D struct {
				GQ *struct {
					Tok  []int64  `json:"tok"`
					Tags []string `json:"tags"`
				} `json:"gq"`
			} `json:"d"`
		}
		if err := json.Unmarshal([]byte(text), &d); err != nil {
			t.Fatalf("%s: %v", text, err)
		}
		if d.D.GQ == nil {
			continue
		}
		withQuery++
		if len(d.D.GQ.Tags) == 0 {
			noTags++
			byTokens[fmt.Sprint(d.D.GQ.Tok)]++
		}
	}
	t.Logf("Kill objectives with a tag query: %d, of which %d have no tag (by token stream: %v)", withQuery, noTags, byTokens)
	if noTags != 57 || byTokens["[0 1 2 0]"] != 53 || byTokens["[0 1 1 0]"] != 4 {
		t.Errorf("%d Kill queries have no tag (%v), want 57 (53 x [0 1 2 0] and 4 x [0 1 1 0])", noTags, byTokens)
	}
}

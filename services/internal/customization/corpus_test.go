// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package customization

// The corpus shared with worldd: fixtures/customization_cases.json. Each case is a character's race, gender, class
// and customization JSON with what the service says of it (accepted) and what worldd's customization_from_json
// builds from the text that is stored (worldd). The same file is read by worldd's test_worldd_character_setup, so
// the rule that matters, "a character the service accepts joins", is held on both sides: here Check (with Normalize)
// must say what 'accepted' says and every accepted case must be one worldd builds; there worldd must build exactly
// what 'worldd' says. A case worldd refuses is one the service has to refuse; the ones worldd builds and the service
// refuses are the creator's own ranges and options. The test needs the real content.db (AOC_CONTENT_DB): the ids of
// cyb and the retail captures are its.

import (
	"encoding/json"
	"os"
	"reflect"
	"strconv"
	"strings"
	"testing"

	"aocrop/services/internal/chardef"
)

type corpusCase struct {
	Name          string          `json:"name"`
	Race          int32           `json:"race"`
	Gender        int32           `json:"gender"`
	Class         int32           `json:"class"`
	Customization json.RawMessage `json:"customization"`
	Text          *string         `json:"text"`
	TextRepeat    *struct {
		Prefix string `json:"prefix"`
		Unit   string `json:"unit"`
		Count  int    `json:"count"`
		Suffix string `json:"suffix"`
	} `json:"text_repeat"`
	Accepted bool            `json:"accepted"`
	Worldd   string          `json:"worldd"` // "builds" or "refuses": worldd on the stored text
	Stored   json.RawMessage `json:"stored"` // the stored text when Normalize changes it
}

// sent is the customization text as the client sent it.
func (c corpusCase) sent() string {
	switch {
	case c.Text != nil:
		return *c.Text
	case c.TextRepeat != nil:
		r := c.TextRepeat
		var b strings.Builder
		b.WriteString(r.Prefix)
		for i := 0; i < r.Count; i++ {
			b.WriteString(strings.ReplaceAll(r.Unit, "{n}", strconv.Itoa(i)))
		}
		b.WriteString(r.Suffix)
		return b.String()
	}
	return string(c.Customization)
}

// [C-Y-B - C-O-A]

func loadCorpus(t *testing.T) []corpusCase {
	t.Helper()
	raw, err := os.ReadFile("../../../fixtures/customization_cases.json")
	if err != nil {
		t.Fatal(err)
	}
	var file struct {
		Cases []corpusCase `json:"cases"`
	}
	if err := json.Unmarshal(raw, &file); err != nil {
		t.Fatal(err)
	}
	return file.Cases
}

func TestCorpusIsConsistent(t *testing.T) {
	cases := loadCorpus(t)
	if len(cases) < 300 {
		t.Fatalf("%d cases, want the 125 retail customizations, cyb and the hostile ones (over 300)", len(cases))
	}
	seen := map[string]bool{}
	for _, c := range cases {
		if seen[c.Name] {
			t.Errorf("two cases are named %q", c.Name)
		}
		seen[c.Name] = true
		if c.Worldd != "builds" && c.Worldd != "refuses" {
			t.Errorf("%s: worldd is %q, want builds or refuses", c.Name, c.Worldd)
		}
		if c.Accepted && c.Worldd != "builds" {
			t.Errorf("%s: accepted, but worldd refuses it: a character the service takes must join", c.Name)
		}
		if c.Stored != nil && !c.Accepted {
			t.Errorf("%s: has a stored form but is not accepted", c.Name)
		}
		forms := 0
		for _, has := range []bool{c.Text != nil, len(c.Customization) > 0, c.TextRepeat != nil} {
			if has {
				forms++
			}
		}
		if forms != 1 {
			t.Errorf("%s: has %d of customization, text and text_repeat, want one", c.Name, forms)
		}
	}
}

func TestCorpusIsWhatTheServiceAccepts(t *testing.T) {
	rules := realRules(t)
	accepted := 0
	for _, c := range loadCorpus(t) {
		className, _ := chardef.ClassName(c.Class)
		sent := c.sent()
		err := rules.Check(New{Race: c.Race, Gender: c.Gender, Class: c.Class, ClassName: className, AppearanceJSON: sent})
		if (err == nil) != c.Accepted {
			t.Errorf("%s: Check = %v, the corpus says accepted: %v", c.Name, err, c.Accepted)
			continue
		}
		if err != nil {
			continue
		}
		accepted++
		stored := Normalize(sent)
		if c.Stored == nil {
			if stored != sent {
				t.Errorf("%s: Normalize changed the text it should leave alone", c.Name)
			}
			continue
		}
		var got, want any
		if err := json.Unmarshal([]byte(stored), &got); err != nil {
			t.Errorf("%s: the stored text is not JSON: %v", c.Name, err)
			continue
		}
		if err := json.Unmarshal(c.Stored, &want); err != nil {
			t.Fatalf("%s: stored: %v", c.Name, err)
		}
		if !reflect.DeepEqual(got, want) {
			t.Errorf("%s: Normalize gave %s, the corpus says %s", c.Name, stored, c.Stored)
		}
		if stored == sent {
			t.Errorf("%s: Normalize left the text as it was", c.Name)
		}
	}
	if accepted < 130 {
		t.Errorf("only %d cases are accepted: cyb and the 125 retail customizations are", accepted)
	}
}

func TestNormalize(t *testing.T) {
	const preset = `6064631211128848384`
	for _, tc := range []struct {
		name, in, want string // want "" for unchanged
	}{
		{"a real preset becomes 0", `{"race":"Kaelar","presetGuid":"` + preset + `","skinSet":1}`, `{"presetGuid":"0","race":"Kaelar","skinSet":1}`},
		{"a preset that is a JSON number", `{"presetGuid":` + preset + `}`, `{"presetGuid":"0"}`},
		{"the other members are as they were (numbers, nesting, escapes)", `{"presetGuid":"` + preset + `","a":1.50,"b":[1, {"c": "é<&>"}],"d":1e-7}`,
			`{"a":1.50,"b":[1,{"c":"é<&>"}],"d":1e-7,"presetGuid":"0"}`},
		{"0 as a string is left", `{"presetGuid": "0", "a": 1}`, ""},
		{"0 as a number is left", `{"presetGuid":0}`, ""},
		{"no preset is left", `{"a":1}`, ""},
		{"a preset that is no id is left", `{"presetGuid":"abc"}`, ""},
		{"not an object is left", `[1]`, ""},
		{"not JSON is left", `nope`, ""},
	} {
		got := Normalize(tc.in)
		want := tc.want
		if want == "" {
			want = tc.in
		}
		if got != want {
			t.Errorf("%s: Normalize(%s) = %s, want %s", tc.name, tc.in, got, want)
		}
	}
}

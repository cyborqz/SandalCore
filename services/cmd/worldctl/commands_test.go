// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// The command list and the run call of worldctl: the fields are read by name, and a field's type, range and choices are shown as the page shows them.
package main

import (
	"bytes"
	"strings"
	"testing"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
)

func TestParseRunReadsFieldsByName(t *testing.T) {
	call, err := parseRun([]string{"set-health", "character=cyb", "value=100"})
	if err != nil {
		t.Fatalf("parseRun(set-health character=cyb value=100): %v", err)
	}
	if call == nil {
		t.Fatal("parseRun returned no call")
	}
	if _, err := parseRun([]string{"set-health", "character=cyb", "value=100", "value=2"}); err == nil || !strings.Contains(err.Error(), "twice") {
		t.Fatalf("a field given twice: got %v, want an error that says twice", err)
	}
	if _, err := parseRun([]string{"set-health", "cyb"}); err == nil || !strings.Contains(err.Error(), "<field>=<value>") {
		t.Fatalf("a word without a field name: got %v, want an error that shows <field>=<value>", err)
	}
	if _, err := parseRun(nil); err == nil || !strings.Contains(err.Error(), "needs a command") {
		t.Fatalf("no command: got %v, want an error that asks for one", err)
	}
}

func TestFieldRulesShowRequiredRangeAndChoices(t *testing.T) {
	f := &worldadminv1.CommandField{
		Name: "value", Label: "Value", Type: worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_WHOLE_NUMBER, Required: true,
		HasMinimum: true, Minimum: 1, HasMaximum: true, Maximum: 100, Hint: "1 to 100",
	}
	got := fieldRules(f)
	for _, want := range []string{"required", "1 to 100"} {
		if !strings.Contains(got, want) {
			t.Fatalf("fieldRules(%v) = %q, want it to hold %q", f, got, want)
		}
	}
	choice := &worldadminv1.CommandField{
		Name: "rarity", Label: "Rarity", Type: worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_CHOICE, Required: false,
		Choices: []string{"Common", "Rare"},
	}
	got = fieldRules(choice)
	for _, want := range []string{"optional", "one of Common, Rare"} {
		if !strings.Contains(got, want) {
			t.Fatalf("fieldRules(%v) = %q, want it to hold %q", choice, got, want)
		}
	}
}

func TestFieldTypeNames(t *testing.T) {
	want := map[worldadminv1.CommandFieldType]string{
		worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_WHOLE_NUMBER: "whole number",
		worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_NUMBER:       "number",
		worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_TEXT:         "text",
		worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_CHARACTER:    "character",
		worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_ITEM:         "item",
		worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_PLACE:        "place",
		worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_CHOICE:       "choice",
	}
	for fieldType, name := range want {
		if got := fieldTypeName(fieldType); got != name {
			t.Fatalf("fieldTypeName(%v) = %q, want %q", fieldType, got, name)
		}
	}
}

func TestParseOptionsReadsTheChosenFieldsByName(t *testing.T) {
	call, err := parseOptions([]string{"place-station", "plot", "node=Verra_RVR_Joeva"}, "", 0, 0)
	if err != nil || call == nil {
		t.Fatalf("parseOptions(place-station plot node=Verra_RVR_Joeva): call %v, error %v", call != nil, err)
	}
	if _, err := parseOptions([]string{"give-item"}, "", 0, 0); err == nil || !strings.Contains(err.Error(), "needs a command and a field") {
		t.Fatalf("a command with no field: got %v, want an error that asks for both", err)
	}
	if _, err := parseOptions([]string{"place-station", "plot", "Joeva"}, "", 0, 0); err == nil || !strings.Contains(err.Error(), "<field>=<value>") {
		t.Fatalf("a chosen value with no field name: got %v, want an error that shows <field>=<value>", err)
	}
	if _, err := parseOptions([]string{"place-station", "plot", "node=a", "node=b"}, "", 0, 0); err == nil || !strings.Contains(err.Error(), "twice") {
		t.Fatalf("a field given twice: got %v, want an error that says twice", err)
	}
	if _, err := parseOptions([]string{"give-item", "item"}, "", -1, 0); err == nil {
		t.Fatal("a negative limit: want an error")
	}
	if _, err := parseOptions([]string{"give-item", "item"}, "sword", 10, 20); err != nil {
		t.Fatalf("a filter and a page: %v", err)
	}
}

func TestWriteOptionsShowsValueLabelPageAndNote(t *testing.T) {
	r := &worldadminv1.ListFieldOptionsResponse{
		Options: []*worldadminv1.FieldOption{{Value: "6064633860997710360", Label: "Emberwake Treads (Gear_Armor_Light_Emberwake_Feet)"}, {Value: "7", Label: "Seven"}},
		Total:   10568, More: true, Note: "",
	}
	var out bytes.Buffer
	writeOptions(&out, "give-item", "item", 50, r)
	got := out.String()
	for _, want := range []string{
		"give-item item: 10568 option(s), showing 2 from 51\n",
		"6064633860997710360\tEmberwake Treads (Gear_Armor_Light_Emberwake_Feet)\n",
		"7\tSeven\n",
		"(more: --offset 52)\n",
	} {
		if !strings.Contains(got, want) {
			t.Fatalf("writeOptions printed %q, want it to hold %q", got, want)
		}
	}
	if strings.Contains(got, "note:") {
		t.Fatalf("writeOptions printed a note for a list with none: %q", got)
	}
	empty := &worldadminv1.ListFieldOptionsResponse{Note: "Choose the node first."}
	out.Reset()
	writeOptions(&out, "place-station", "plot", 0, empty)
	if got := out.String(); got != "place-station plot: 0 option(s)\nnote: Choose the node first.\n" {
		t.Fatalf("writeOptions of an empty list printed %q", got)
	}
}

func TestFieldRulesSayWhereOptionsComeFrom(t *testing.T) {
	f := &worldadminv1.CommandField{Name: "plot", Label: "Plot", Type: worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_TEXT, Required: true, HasOptions: true, DependsOn: []string{"node", "quest"}}
	got := fieldRules(f)
	for _, want := range []string{"options from the world's data", "follows node and quest"} {
		if !strings.Contains(got, want) {
			t.Fatalf("fieldRules(%v) = %q, want it to hold %q", f, got, want)
		}
	}
	long := &worldadminv1.CommandField{Name: "item", Type: worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_ITEM, HasOptions: true, Searchable: true, PageSize: 50}
	if got := fieldRules(long); !strings.Contains(got, "searchable") {
		t.Fatalf("fieldRules of a searchable field = %q, want it to say searchable", got)
	}
	free := &worldadminv1.CommandField{Name: "count", Type: worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_WHOLE_NUMBER}
	if got := fieldRules(free); strings.Contains(got, "options") {
		t.Fatalf("fieldRules of a free number = %q, want no word of options", got)
	}
}

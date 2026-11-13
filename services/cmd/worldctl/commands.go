// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// The command list and the generic run call of worldctl (see main.go): the same two calls the admin app uses, so a new command needs no change here or there.
package main

import (
	"context"
	"fmt"
	"io"
	"strings"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
)

// fieldTypeName says how a field is written, in the words the page uses.
func fieldTypeName(t worldadminv1.CommandFieldType) string {
	switch t {
	case worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_WHOLE_NUMBER:
		return "whole number"
	case worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_NUMBER:
		return "number"
	case worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_TEXT:
		return "text"
	case worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_CHARACTER:
		return "character"
	case worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_ITEM:
		return "item"
	case worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_PLACE:
		return "place"
	case worldadminv1.CommandFieldType_COMMAND_FIELD_TYPE_CHOICE:
		return "choice"
	}
	return "?"
}

// fieldRules says whether a field is required and what it accepts.
func fieldRules(f *worldadminv1.CommandField) string {
	var parts []string
	if f.GetRequired() {
		parts = append(parts, "required")
	} else {
		parts = append(parts, "optional")
	}
	if f.GetHasMinimum() && f.GetHasMaximum() {
		parts = append(parts, fmt.Sprintf("%g to %g", f.GetMinimum(), f.GetMaximum()))
	} else if f.GetHasMinimum() {
		parts = append(parts, fmt.Sprintf("%g or more", f.GetMinimum()))
	} else if f.GetHasMaximum() {
		parts = append(parts, fmt.Sprintf("%g or less", f.GetMaximum()))
	}
	if len(f.GetChoices()) > 0 {
		parts = append(parts, "one of "+strings.Join(f.GetChoices(), ", "))
	}
	if f.GetHasOptions() {
		how := "options from the world's data (worldctl options)"
		if f.GetSearchable() {
			how = "options from the world's data, searchable (worldctl options)"
		}
		if len(f.GetDependsOn()) > 0 {
			how += ", follows " + strings.Join(f.GetDependsOn(), " and ")
		}
		parts = append(parts, how)
	}
	if f.GetHint() != "" {
		parts = append(parts, f.GetHint())
	}
	return strings.Join(parts, "; ")
}

// doCommands lists every command, or one when it is named.
func doCommands(pos []string) call {
	want := ""
	if len(pos) == 1 {
		want = pos[0]
	}
	return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
		r, err := c.ListCommands(ctx, &worldadminv1.ListCommandsRequest{})
		if err != nil {
			return err
		}
		shown := 0
		for _, cmd := range r.GetCommands() {
			if want != "" && !strings.EqualFold(cmd.GetName(), want) {
				continue
			}
			shown++
			character := "no character"
			if cmd.GetActsOnCharacter() {
				character = "names a character"
			}
			fmt.Fprintf(out, "%s  [%s, %s]\n  %s\n", cmd.GetName(), cmd.GetGroup(), character, cmd.GetDescription())
			if len(cmd.GetFields()) == 0 {
				fmt.Fprintln(out, "  (no fields)")
			}
			for _, f := range cmd.GetFields() {
				fmt.Fprintf(out, "  %-12s %-12s %s\n", f.GetName(), fieldTypeName(f.GetType()), fieldRules(f))
			}
		}
		if want != "" && shown == 0 {
			return fmt.Errorf("no command is named %q", want)
		}
		return nil
	}
}

// parseRun reads "run <command> <field>=<value> ...": the fields are given by name, and worldd checks each against the list's own rules.
func parseRun(pos []string) (call, error) {
	if len(pos) < 1 {
		return nil, usageError("needs a command: worldctl run <command> <field>=<value> ..., such as: worldctl run set-health character=cyb value=100")
	}
	command := pos[0]
	fields := make(map[string]string)
	for _, word := range pos[1:] {
		eq := strings.Index(word, "=")
		if eq <= 0 {
			return nil, usageError(fmt.Sprintf("%q is not <field>=<value>: every field is given by its name, such as character=cyb (worldctl commands %s lists them)", word, command))
		}
		name, value := word[:eq], word[eq+1:]
		if _, twice := fields[name]; twice {
			return nil, usageError(fmt.Sprintf("the field %q is given twice", name))
		}
		fields[name] = value
	}
	return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
		r, err := c.RunCommand(ctx, &worldadminv1.RunCommandRequest{Command: command, Fields: fields})
		if err != nil {
			return err
		}
		fmt.Fprintln(out, r.GetResult())
		return nil
	}, nil
}

// parseOptions reads "options <command> <field> [<field>=<value> ...]": the values chosen so far, by field name, that a dependent list follows (the node of a plot, the quest of an
// objective); the filter text and the page come from the flags.
func parseOptions(pos []string, filter string, limit, offset int) (call, error) {
	if len(pos) < 2 {
		return nil, usageError("needs a command and a field: worldctl options <command> <field> [<field>=<value> ...], such as: worldctl options place-station plot node=Verra_RVR_Joeva")
	}
	if limit < 0 || offset < 0 {
		return nil, usageError("--limit and --offset are not below 0")
	}
	command, field := pos[0], pos[1]
	chosen := make(map[string]string)
	for _, word := range pos[2:] {
		eq := strings.Index(word, "=")
		if eq <= 0 {
			return nil, usageError(fmt.Sprintf("%q is not <field>=<value>: what is chosen so far is given by field name, such as node=Verra_RVR_Joeva (worldctl commands %s lists the fields)", word, command))
		}
		name, value := word[:eq], word[eq+1:]
		if _, twice := chosen[name]; twice {
			return nil, usageError(fmt.Sprintf("the field %q is given twice", name))
		}
		chosen[name] = value
	}
	return func(ctx context.Context, c worldadminv1.WorldAdminServiceClient, out io.Writer) error {
		r, err := c.ListFieldOptions(ctx, &worldadminv1.ListFieldOptionsRequest{Command: command, Field: field, Chosen: chosen, Filter: filter, Limit: uint32(limit), Offset: uint32(offset)})
		if err != nil {
			return err
		}
		writeOptions(out, command, field, offset, r)
		return nil
	}, nil
}

// writeOptions prints one page of a field's options: a line that says how many there are, one line for each option (its value, a tab, its label) and the server's note.
func writeOptions(out io.Writer, command, field string, offset int, r *worldadminv1.ListFieldOptionsResponse) {
	shown := len(r.GetOptions())
	fmt.Fprintf(out, "%s %s: %d option(s)", command, field, r.GetTotal())
	if r.GetMore() || offset > 0 {
		fmt.Fprintf(out, ", showing %d from %d", shown, offset+1)
	}
	fmt.Fprintln(out)
	for _, o := range r.GetOptions() {
		fmt.Fprintf(out, "%s\t%s\n", o.GetValue(), o.GetLabel())
	}
	if r.GetMore() {
		fmt.Fprintf(out, "(more: --offset %d)\n", offset+shown)
	}
	if r.GetNote() != "" {
		fmt.Fprintf(out, "note: %s\n", r.GetNote())
	}
}

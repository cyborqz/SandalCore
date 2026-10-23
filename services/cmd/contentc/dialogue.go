// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"
	"sort"

	"aocrop/services/internal/designdb"
)

// NPC dialogue (quests milestone, slice 0; contentc version 17): dialogue_root, dialogue_node, dialogue_choice, dialogue_quest and npc_dialogue
// (internal/content/dialogue.go says what each holds).  The conditions are kept as the tree the data holds (a token stream over a list of criteria), not
// evaluated here: worldd evaluates them for the character who talks.

// dialogueRecordTypes are the record structs the builder reads.
var dialogueRecordTypes = []string{"DialogueRoot", "DialogueNode", "NarrativeQuestV2", "PopulationInstanceRecordBase", "PopulationAssetRecordBase"}

// critJSON is one criterion of a requirement, only what is set.
type critJSON struct {
	Kind  string   `json:"t"`
	Op    string   `json:"op,omitempty"`
	Value int64    `json:"v,omitempty"`
	Quest int64    `json:"q,omitempty"`
	QQ    string   `json:"qq,omitempty"`
	Obj   int64    `json:"o,omitempty"`
	OQ    string   `json:"oq,omitempty"`
	Var   string   `json:"var,omitempty"`
	Cur   int64    `json:"cur,omitempty"`
	Expr  string   `json:"x,omitempty"`
	Items []int64  `json:"items,omitempty"`
	Tags  []string `json:"tags,omitempty"`
	Loc   int64    `json:"loc,omitempty"`
	// VarRight is the right-hand operand's variable name when the criterion compares with a variable instead of a number.
	VarRight string `json:"rv,omitempty"`
}

// reqJSON is a GameplayRequirement.
type reqJSON struct {
	Tok []int64    `json:"tok"`
	Q   []critJSON `json:"q"`
}

// actionJSON is a dialogue action, only its kind, the window it opens and, for a shop window, the VendorDef it opens (vendors milestone, version 18).
type actionJSON struct {
	Action string `json:"a"`
	Window string `json:"w,omitempty"`
	Vendor int64  `json:"vd,omitempty"`
}

// intOf reads a number a decoded struct holds as an int64 (ints, floats and enums stored as numbers).
func intOf(v designdb.Value) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case uint64:
		return int64(x)
	case float64:
		return int64(x)
	case float32:
		return int64(x)
	case bool:
		if x {
			return 1
		}
	}
	return 0
}

func strOf(v designdb.Value) string {
	if s, ok := v.(string); ok {
		return s
	}
	return ""
}

// idGuid reads the Guid of an Id struct value (0 for anything else).
func idGuid(v designdb.Value) int64 {
	if s, ok := v.(designdb.Struct); ok {
		return intOf(s["Guid"])
	}
	return 0
}

func idsOf(v designdb.Value) []int64 {
	var out []int64
	if l, ok := v.([]designdb.Value); ok {
		for _, e := range l {
			if g := idGuid(e); g != 0 {
				out = append(out, g)
			}
		}
	}
	return out
}

func noneAsEmpty(s string) string {
	if s == "None" {
		return ""
	}
	return s
}

// readRequirement reads a GameplayRequirement struct into JSON text; "" when it holds no criterion.
func readRequirement(v designdb.Value) (string, *reqJSON, error) {
	s, ok := v.(designdb.Struct)
	if !ok {
		return "", nil, nil
	}
	qd, _ := s["QueryDictionary"].([]designdb.Value)
	ts, _ := s["TokenStream"].([]designdb.Value)
	if len(qd) == 0 || len(ts) == 0 {
		return "", nil, nil
	}
	r := &reqJSON{}
	for _, t := range ts {
		r.Tok = append(r.Tok, intOf(t))
	}
	for i, e := range qd {
		q, ok := e.(designdb.Struct)
		if !ok {
			return "", nil, fmt.Errorf("criterion %d is not a struct", i)
		}
		c := critJSON{
			Kind:  enumValue(strOf(q["ExpressionType"])),
			Op:    enumValue(strOf(q["Operator"])),
			Value: intOf(q["Value"]),
			Quest: idGuid(q["QuestId"]),
			Obj:   idGuid(q["ObjectiveId"]),
			Var:   noneAsEmpty(strOf(q["VariableName"])),
			Cur:   idGuid(q["CurrencyDefId"]),
			Expr:  strOf(q["Expression"]),
			Loc:   intOf(q["ItemLocation"]),
		}
		switch c.Kind {
		case "QuestState":
			c.QQ = enumValue(strOf(q["QuestQueryType"]))
		case "ObjectiveState":
			c.OQ = enumValue(strOf(q["ObjectiveQueryType"]))
			c.QQ = ""
		case "Item":
			if f, ok := q["ItemFilter"].(designdb.Struct); ok {
				c.Items = idsOf(f["ItemsIds"])
				if tg, ok := f["Tags"].(designdb.Struct); ok {
					if l, ok := tg["TagDictionary"].([]designdb.Value); ok {
						for _, t := range l {
							if ts, ok := t.(string); ok && ts != "" {
								c.Tags = append(c.Tags, ts)
							}
						}
					}
				}
			}
		}
		if b, ok := q["bEvaluateAgainstVariable"].(bool); ok && b {
			c.VarRight = noneAsEmpty(strOf(q["RightOperandVariableName"]))
		}
		r.Q = append(r.Q, c)
	}
	b, err := json.Marshal(r)
	if err != nil {
		return "", nil, err
	}
	return string(b), r, nil
}

// readActions reads a DialogueActions array into JSON text ("[]" when there is none).
func readActions(v designdb.Value) (string, error) {
	out := []actionJSON{}
	if l, ok := v.([]designdb.Value); ok {
		for _, e := range l {
			a, ok := e.(designdb.Struct)
			if !ok {
				continue
			}
			x := actionJSON{Action: enumValue(strOf(a["ActionType"]))}
			if x.Action == "OpenWindow" {
				x.Window = enumValue(strOf(a["WindowType"]))
				if x.Window == "Vendor" {
					x.Vendor = idGuid(a["VendorDefId"])
				}
			}
			out = append(out, x)
		}
	}
	b, err := json.Marshal(out)
	return string(b), err
}

func buildDialogue(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range dialogueRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	var nodes, choices, gated, roots, emptyRoots int
	questsNeeded := map[int64]bool{}
	noteQuests := func(r *reqJSON) {
		if r == nil {
			return
		}
		for _, c := range r.Q {
			if c.Quest != 0 && (c.Kind == "QuestState" || c.Kind == "ObjectiveState") {
				questsNeeded[c.Quest] = true
			}
		}
	}

	for _, r := range src.rows["DialogueRoot"] {
		f := newFields(fmt.Sprintf("DialogueRoot %s (%d)", r.Name, r.Guid), r.Fields)
		top, _ := f.id("TopNode")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO dialogue_root (guid, name, top_node) VALUES (?, ?, ?)`, r.Guid, r.Name, top); err != nil {
			return err
		}
		roots++
		if top == 0 {
			emptyRoots++
		}
	}

	for _, r := range src.rows["DialogueNode"] {
		f := newFields(fmt.Sprintf("DialogueNode %s (%d)", r.Name, r.Guid), r.Fields)
		nodeType := enumValue(f.str("NodeType"))
		exhaust := enumValue(f.str("ExhaustBehavior"))
		quest, _ := f.id("QuestId")
		enterV, _ := f.get("OnEnterDialogueActions")
		exitV, _ := f.get("OnExitDialogueActions")
		onEnter, err := readActions(enterV)
		if err != nil {
			return err
		}
		onExit, err := readActions(exitV)
		if err != nil {
			return err
		}
		list := f.list("Choices")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO dialogue_node (guid, name, node_type, exhaust, quest_guid, on_enter, on_exit) VALUES (?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, nodeType, exhaust, quest, onEnter, onExit); err != nil {
			return err
		}
		nodes++
		for i, e := range list {
			c, ok := e.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: choice %d is not a struct", f.where, i)
			}
			req, parsed, err := readRequirement(c["Requirements"])
			if err != nil {
				return fmt.Errorf("%s: choice %d: %w", f.where, i, err)
			}
			noteQuests(parsed)
			acts, err := readActions(c["DialogueActions"])
			if err != nil {
				return err
			}
			if err := w.exec(`INSERT INTO dialogue_choice (node_guid, idx, next_node, requirement, actions) VALUES (?, ?, ?, ?, ?)`,
				r.Guid, i, idGuid(c["NextNode"]), req, acts); err != nil {
				return err
			}
			choices++
			if req != "" {
				gated++
			}
		}
	}

	// the quests a condition names, and (to a fixed point) the quests their own activation rules name
	quests := map[int64]designdb.Row{}
	for _, r := range src.rows["NarrativeQuestV2"] {
		quests[r.Guid] = r
	}
	activation := map[int64]string{}
	for pass := 0; pass < 64; pass++ {
		grew := false
		ids := make([]int64, 0, len(questsNeeded))
		for g := range questsNeeded {
			ids = append(ids, g)
		}
		sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
		for _, g := range ids {
			if _, done := activation[g]; done {
				continue
			}
			row, ok := quests[g]
			if !ok {
				activation[g] = "" // a condition names a quest the data does not hold: it has no rule
				continue
			}
			text, parsed, err := readRequirement(row.Fields["ActivationRule"])
			if err != nil {
				return fmt.Errorf("NarrativeQuestV2 %s (%d): activation rule: %w", row.Name, g, err)
			}
			activation[g] = text
			before := len(questsNeeded)
			noteQuests(parsed)
			if len(questsNeeded) != before {
				grew = true
			}
		}
		if !grew {
			break
		}
	}
	ids := make([]int64, 0, len(activation))
	for g := range activation {
		ids = append(ids, g)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	for _, g := range ids {
		row, ok := quests[g]
		name, disabled, repeatable := "", int64(0), int64(0)
		if ok {
			name = row.Name
			f := newFields(fmt.Sprintf("NarrativeQuestV2 %s (%d)", row.Name, g), row.Fields)
			disabled = int64(b2i(f.boolean("bDisabled")))
			repeatable = int64(b2i(f.boolean("bRepeatable")))
			if f.err != nil {
				return f.err
			}
		}
		if err := w.exec(`INSERT INTO dialogue_quest (guid, name, disabled, repeatable, activation) VALUES (?, ?, ?, ?, ?)`, g, name, disabled, repeatable, activation[g]); err != nil {
			return err
		}
	}

	// which root an NPC opens
	var inst, asset int
	for _, r := range src.rows["PopulationInstanceRecordBase"] {
		g := idGuid(r.Fields["DialogueRootId"])
		if g == 0 {
			continue
		}
		if err := w.exec(`INSERT INTO npc_dialogue (source_kind, source_guid, root_guid) VALUES ('instance', ?, ?)`, r.Guid, g); err != nil {
			return err
		}
		inst++
	}
	for _, r := range src.rows["PopulationAssetRecordBase"] {
		g := idGuid(r.Fields["DialogueRoot"])
		if g == 0 {
			continue
		}
		if err := w.exec(`INSERT INTO npc_dialogue (source_kind, source_guid, root_guid) VALUES ('asset', ?, ?)`, r.Guid, g); err != nil {
			return err
		}
		asset++
	}
	meta["dialogue.roots"] = fmt.Sprintf("%d (%d without a conversation)", roots, emptyRoots)
	meta["dialogue.nodes"] = fmt.Sprint(nodes)
	meta["dialogue.choices"] = fmt.Sprintf("%d (%d with a condition)", choices, gated)
	meta["dialogue.quests"] = fmt.Sprint(len(ids))
	meta["dialogue.npcs"] = fmt.Sprintf("%d instances, %d assets", inst, asset)
	opt.logf("dialogue: %d roots (%d empty), %d nodes, %d choices (%d with a condition), %d quests named by conditions, %d instance and %d asset roots",
		roots, emptyRoots, nodes, choices, gated, len(ids), inst, asset)
	return nil
}

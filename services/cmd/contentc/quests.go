// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"

	"aocrop/services/internal/designdb"
)

// Quests (quests milestone, slice 1, "take a quest"; contentc version 21): quest, quest_objective, npc_offered_quest and dialogue_accept
// (internal/content/quests.go says what each holds).  The activation rules are kept as the requirement tree the data holds, as the conversations' conditions are
// (dialogue.go); worldd evaluates them for the character who asks.

// questRecordTypes are the record structs the builder reads.
var questRecordTypes = []string{"NarrativeQuestV2", "NarrativeObjectiveV2", "DialogueNode", "PopulationInstanceRecordBase", "PopulationAssetRecordBase", "ZOIRecordBase", "ItemRecordBase"}

// startActionJSON is one thing a quest does when it starts, only what is set.
type startActionJSON struct {
	Action string `json:"a"`
	Obj    int64  `json:"o,omitempty"`
	Quest  int64  `json:"q,omitempty"`
	Name   string `json:"n,omitempty"`
	Value  int64  `json:"v,omitempty"`
	Cond   int    `json:"c,omitempty"`
}

// readStartActions reads a quest's OnStartActions into JSON text ("[]" when there is none).
func readStartActions(v designdb.Value) (string, error) {
	out := []startActionJSON{}
	if l, ok := v.([]designdb.Value); ok {
		for _, e := range l {
			wrap, ok := e.(designdb.Struct)
			if !ok {
				continue
			}
			a, ok := wrap["Action"].(designdb.Struct)
			if !ok {
				continue
			}
			x := startActionJSON{
				Action: enumValue(strOf(a["ActionType"])),
				Obj:    idGuid(a["TargetObjectiveId"]),
				Quest:  idGuid(a["TargetQuestID"]),
				Name:   noneAsEmpty(strOf(a["TargetVariableName"])),
				Value:  intOf(a["TargetVariableValue"]),
			}
			if text, _, err := readRequirement(wrap["ActivationRule"]); err != nil {
				return "", err
			} else if text != "" {
				x.Cond = 1
			}
			out = append(out, x)
		}
	}
	b, err := json.Marshal(out)
	return string(b), err
}

func jsonIDs(ids []int64) string {
	if ids == nil {
		ids = []int64{}
	}
	b, _ := json.Marshal(ids)
	return string(b)
}

func buildQuests(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range questRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	objectives := map[int64]designdb.Row{}
	for _, r := range src.rows["NarrativeObjectiveV2"] {
		objectives[r.Guid] = r
	}

	var quests, objs, missing, gated int
	for _, r := range src.rows["NarrativeQuestV2"] {
		f := newFields(fmt.Sprintf("NarrativeQuestV2 %s (%d)", r.Name, r.Guid), r.Fields)
		activation, _, err := readRequirement(r.Fields["ActivationRule"])
		if err != nil {
			return fmt.Errorf("%s: activation rule: %w", f.where, err)
		}
		startV, _ := f.get("OnStartActions")
		start, err := readStartActions(startV)
		if err != nil {
			return fmt.Errorf("%s: start actions: %w", f.where, err)
		}
		objIDs := f.ids("ObjectiveIds")
		rewards := f.ids("Rewards")
		flavor := enumValue(f.str("Flavor"))
		locality := enumValue(f.str("Locality"))
		level := f.int("Level")
		canAbandon, repeatable, disabled, defAct := f.boolean("bCanAbandon"), f.boolean("bRepeatable"), f.boolean("bDisabled"), f.boolean("bDefaultActivate")
		if f.err != nil {
			return f.err
		}
		if err := w.exec(`INSERT INTO quest (guid, name, flavor, locality, level, can_abandon, repeatable, disabled, default_activate, activation, start_actions, rewards)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, flavor, locality, level, b2i(canAbandon), b2i(repeatable), b2i(disabled), b2i(defAct), activation, start, jsonIDs(rewards)); err != nil {
			return err
		}
		quests++
		if activation != "" {
			gated++
		}
		for i, og := range objIDs {
			orow, ok := objectives[og]
			if !ok {
				missing++ // a quest that names an objective the data does not hold keeps its place, as a gap the reader skips
				continue
			}
			of := newFields(fmt.Sprintf("NarrativeObjectiveV2 %s (%d)", orow.Name, og), orow.Fields)
			typ := enumValue(of.str("ObjectiveType"))
			count := of.int("CompletionCount")
			awake, hidden, completes, optional := of.boolean("bDefaultAwake"), of.boolean("bDefaultHidden"), of.boolean("bCompletesQuest"), of.boolean("bOptionalObjective")
			if of.err != nil {
				return of.err
			}
			if err := w.exec(`INSERT INTO quest_objective (quest_guid, idx, guid, name, obj_type, completion_count, default_awake, default_hidden, completes_quest, optional)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, i, og, orow.Name, typ, count, b2i(awake), b2i(hidden), b2i(completes), b2i(optional)); err != nil {
				return err
			}
			objs++
		}
	}

	// the quests an NPC offers
	var offers int
	offered := func(kind string, rows []designdb.Row) error {
		for _, r := range rows {
			l, _ := r.Fields["OfferedQuests"].([]designdb.Value)
			for i, e := range l {
				s, ok := e.(designdb.Struct)
				if !ok {
					continue
				}
				q := idGuid(s["Quest"])
				if q == 0 {
					continue
				}
				if err := w.exec(`INSERT INTO npc_offered_quest (source_kind, source_guid, idx, quest_guid, dynamic_node) VALUES (?, ?, ?, ?, ?)`,
					kind, r.Guid, i, q, idGuid(s["DynamicNodeId"])); err != nil {
					return err
				}
				offers++
			}
		}
		return nil
	}
	if err := offered("instance", src.rows["PopulationInstanceRecordBase"]); err != nil {
		return err
	}
	if err := offered("asset", src.rows["PopulationAssetRecordBase"]); err != nil {
		return err
	}

	// the nodes that offer a quest
	var accepts int
	for _, r := range src.rows["DialogueNode"] {
		if enumValue(strOf(r.Fields["NodeType"])) != "AcceptQuest" {
			continue
		}
		if err := w.exec(`INSERT INTO dialogue_accept (node_guid, quest_guid, on_success, objectives) VALUES (?, ?, ?, ?)`,
			r.Guid, idGuid(r.Fields["QuestId"]), idGuid(r.Fields["OnSuccessNode"]), jsonIDs(idsOf(r.Fields["ObjectiveIds"]))); err != nil {
			return err
		}
		accepts++
	}

	// the quests a zone of influence starts
	var zoiQuests int
	for _, r := range src.rows["ZOIRecordBase"] {
		for i, q := range idsOf(r.Fields["LocationQuests"]) {
			if err := w.exec(`INSERT INTO zoi_quest (zoi_guid, idx, quest_guid) VALUES (?, ?, ?)`, r.Guid, i, q); err != nil {
				return err
			}
			zoiQuests++
		}
	}

	// the items bound to quests
	var itemLinks int
	for _, r := range src.rows["ItemRecordBase"] {
		link := func(kind string, quests []int64) error {
			seen := map[int64]bool{}
			for _, q := range quests {
				if seen[q] {
					continue
				}
				seen[q] = true
				if err := w.exec(`INSERT INTO item_quest (item_guid, quest_guid, kind) VALUES (?, ?, ?)`, r.Guid, q, kind); err != nil {
					return err
				}
				itemLinks++
			}
			return nil
		}
		if d, ok := r.Fields["DeleteOnQuestsRemovedData"].(designdb.Struct); ok {
			kind := "delete_all"
			if enumValue(strOf(d["DeleteType"])) == "OnlyOne" {
				kind = "delete_one"
			}
			if err := link(kind, idsOf(d["Quests"])); err != nil {
				return err
			}
		}
		if err := link("abandon", idsOf(r.Fields["AbandonQuestsOnDelete"])); err != nil {
			return err
		}
	}

	meta["quests.quests"] = fmt.Sprintf("%d (%d with an activation rule)", quests, gated)
	meta["quests.objectives"] = fmt.Sprintf("%d (%d named by a quest but missing)", objs, missing)
	meta["quests.offers"] = fmt.Sprint(offers)
	meta["quests.accept_nodes"] = fmt.Sprint(accepts)
	meta["quests.zoi_quests"] = fmt.Sprint(zoiQuests)
	meta["quests.item_links"] = fmt.Sprint(itemLinks)
	opt.logf("quests: %d quests (%d with an activation rule), %d objectives (%d missing), %d offers by NPCs, %d accept nodes, %d zone quests, %d item links", quests, gated, objs, missing, offers, accepts, zoiQuests, itemLinks)
	return nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"

	"aocrop/services/internal/designdb"
)

// The objects of the quests (version 36): the interactable definitions the props of the quests are used by (a body, a crate, a root, a building site), the actor definitions that
// say whether an object leaves after its use, the items that belong in the Quest bag, the items that open the choose-one reward window and the speech bubble of an objective.

// defaultCallout is the map registration every objective record names for a speech bubble it does not have.
const defaultCallout = 6064633431944134657

// objectiveCallout is the map registration whose callout an objective of a talk, hand-in, interaction or collection raises over the person or object it names (0: none).
func objectiveCallout(f designdb.Struct, typ string) int64 {
	key := map[string]string{"Dialogue": "DialogueData", "TurnIn": "TurnInData", "Interact": "InteractData", "Collect": "CollectData", "EventNotification": "NotificationData"}[typ]
	if key == "" {
		return 0
	}
	g := idGuid(structOf(f[key])["CalloutMapRegistrationId"])
	if g == 0 || g == defaultCallout {
		return 0
	}
	return g
}

func buildObjects(w *writer, src *source, meta map[string]string) error {
	for _, schema := range []string{"InteractableComponentDefRecordBase", "PlayerInteractionDefRecordBase", "InteractableActorDefRecordBase"} {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	pids := map[int64]designdb.Struct{}
	for _, r := range src.rows["PlayerInteractionDefRecordBase"] {
		pids[r.Guid] = r.Fields
	}
	var icds, withPID, grants int
	for _, r := range src.rows["InteractableComponentDefRecordBase"] {
		f := r.Fields
		var requirement string
		var pid int64
		if pairs, ok := f["InteractInputPoints"].(designdb.Pairs); ok && len(pairs) > 0 {
			point := structOf(pairs[0].Value)
			text, _, err := readRequirement(point["GameplayRequirements"])
			if err != nil {
				return fmt.Errorf("InteractableComponentDefRecordBase %s (%d): requirement: %w", r.Name, r.Guid, err)
			}
			requirement = text
			pid = idGuid(point["PlayerInteractDefinition"])
		}
		p := pids[pid]
		var give []itemQty
		for _, e := range listOf(p["ItemsToGrantUponCompletion"]) {
			s := structOf(e)
			if item := idGuid(structOf(s["Item"])["ItemId"]); item != 0 {
				give = append(give, itemQty{item, intOf(s["Quantity"])})
			}
		}
		gj, err := json.Marshal(give)
		if err != nil {
			return err
		}
		if len(give) == 0 {
			gj = []byte("[]")
		}
		if err := w.exec(`INSERT INTO interactable (icd_guid, name, radius, tolerance, discovery, require_los, requirement, pid_guid, channel, busy, allow_combat, single_use, delete_done, delete_delay, anim, grants, reward_table, complete_objs, reveal_objs)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, floatOf(f["InteractRadius"]), floatOf(f["InteractTolerance"]), floatOf(f["DiscoveryDistance"]), b2i(boolOf(f["bRequireLOS"])), requirement, pid,
			floatOf(p["ChannelDuration"]), floatOf(p["BusyTime"]), b2i(boolOf(p["AllowInCombat"])), b2i(boolOf(p["SingleUse"])), b2i(boolOf(p["DeleteOnCompletion"])), floatOf(p["DeletionDelay"]),
			strOf(p["ChannelAnimation"]), string(gj), idGuid(p["RewardTableId"]), jsonIDs(idsOf(p["ObjectivesToCompleteUponCompletionIds"])), jsonIDs(idsOf(p["ObjectivesToRevealUponCompletionIds"]))); err != nil {
			return err
		}
		icds++
		if pid != 0 {
			withPID++
		}
		grants += len(give)
	}
	var iads int
	for _, r := range src.rows["InteractableActorDefRecordBase"] {
		if err := w.exec(`INSERT INTO interactable_actor_def (iad_guid, despawn_delay, despawn_final) VALUES (?, ?, ?)`,
			r.Guid, floatOf(r.Fields["DespawnDelayTime"]), b2i(boolOf(r.Fields["bDespawnAfterFinalState"]))); err != nil {
			return err
		}
		iads++
	}
	var named int
	for _, r := range src.rows["PopulationInstanceRecordBase"] {
		if iad := idGuid(r.Fields["InteractableActorDefId"]); iad != 0 {
			if err := w.exec(`INSERT INTO instance_iad (instance_guid, iad_guid) VALUES (?, ?)`, r.Guid, iad); err != nil {
				return err
			}
			named++
		}
	}
	var questItems, boxes int
	for _, r := range src.rows["ItemRecordBase"] {
		if enumValue(strOf(r.Fields["InventoryFilterType"])) == "Quest" {
			if err := w.exec(`INSERT INTO quest_item (item_guid, major) VALUES (?, ?)`, r.Guid, b2i(enumValue(strOf(r.Fields["QuestItemSignificance"])) == "Major")); err != nil {
				return err
			}
			questItems++
		}
		if table := idGuid(structOf(r.Fields["ActivationLootBoxData"])["RewardTable"]); table != 0 {
			if err := w.exec(`INSERT INTO item_box (item_guid, table_guid) VALUES (?, ?)`, r.Guid, table); err != nil {
				return err
			}
			boxes++
		}
	}
	meta["objects.interactables"] = fmt.Sprintf("%d (%d with a player interaction, %d items granted)", icds, withPID, grants)
	meta["objects.actor_defs"] = fmt.Sprintf("%d (%d population instances name one)", iads, named)
	meta["objects.quest_items"] = fmt.Sprint(questItems)
	meta["objects.item_boxes"] = fmt.Sprint(boxes)
	return nil
}

// listOf reads a list value (nil for anything else).
func listOf(v designdb.Value) []designdb.Value {
	l, _ := v.([]designdb.Value)
	return l
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"
	"unicode"

	"aocrop/services/internal/designdb"
)

// Quests (quests milestone, slice 1, "take a quest", contentc version 21; slice 2, "do it and hand it in", version 27): quest, quest_objective, npc_offered_quest and dialogue_accept
// (internal/content/quests.go says what each holds).  The activation rules are kept as the requirement tree the data holds, as the conversations' conditions are
// (dialogue.go); worldd evaluates them for the character who asks.

// questRecordTypes are the record structs the builder reads.
var questRecordTypes = []string{"NarrativeQuestV2", "NarrativeObjectiveV2", "DialogueNode", "PopulationInstanceRecordBase", "PopulationAssetRecordBase", "ZOIRecordBase", "ItemRecordBase"}

// itemQty is an item and how many of it an action takes.
type itemQty [2]int64

// startActionJSON is one action of a quest or an objective (the data's ActivationRuleActionPair), only what is set.
type startActionJSON struct {
	Action string          `json:"a"`
	Obj    int64           `json:"o,omitempty"`
	Quest  int64           `json:"q,omitempty"`
	Name   string          `json:"n,omitempty"`
	Value  int64           `json:"v,omitempty"`
	Cond   int             `json:"c,omitempty"`
	Rule   json.RawMessage `json:"r,omitempty"`  // the action's own condition, when it has one
	Reward int64           `json:"rw,omitempty"` // GrantItem: the reward table it gives
	Items  []itemQty       `json:"it,omitempty"` // RemoveItem: [[item, quantity]]; a quantity of 0 takes every one
	Text   string          `json:"t,omitempty"`  // Toast
	Spawns []int64         `json:"sp,omitempty"` // ActivateSpawns / DeactivateSpawns: the population sets
}

// readActionList reads an ActivationRuleActionPair list.
func readActionList(v designdb.Value) ([]startActionJSON, error) {
	out := []startActionJSON{}
	l, ok := v.([]designdb.Value)
	if !ok {
		return out, nil
	}
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
		if x.Action == "" || x.Action == "None" {
			continue
		}
		text, req, err := readRequirement(wrap["ActivationRule"])
		if err != nil {
			return nil, err
		}
		if text != "" {
			x.Cond = 1
			b, err := json.Marshal(req)
			if err != nil {
				return nil, err
			}
			x.Rule = b
		}
		switch x.Action {
		case "GrantItem":
			x.Reward = idGuid(a["ItemData"])
		case "RemoveItem":
			if l, ok := a["ItemsToRemoveAndQuantity"].([]designdb.Value); ok {
				for _, e := range l {
					s, ok := e.(designdb.Struct)
					if !ok {
						continue
					}
					item := idGuid(structOf(s["Item"])["ItemId"])
					if item == 0 {
						continue
					}
					qty := intOf(s["Quantity"])
					if b, ok := s["bRemoveAll"].(bool); ok && b {
						qty = 0
					}
					x.Items = append(x.Items, itemQty{item, qty})
				}
			}
		case "Toast":
			if t, ok := a["ToastText"].(designdb.Text); ok {
				x.Text = t.Source
				if x.Text == "" {
					x.Text = t.Key
				}
			}
		case "ActivateSpawns", "DeactivateSpawns":
			if l, ok := a["SpawnIDs"].([]designdb.Value); ok {
				for _, e := range l {
					if s, ok := e.(designdb.Struct); ok {
						if g := idGuid(s["PopulationSet"]); g != 0 {
							x.Spawns = append(x.Spawns, g)
						}
					}
				}
			}
		}
		out = append(out, x)
	}
	return out, nil
}

// structOf reads a value as a struct (an empty one for anything else).
func structOf(v designdb.Value) designdb.Struct {
	if s, ok := v.(designdb.Struct); ok {
		return s
	}
	return designdb.Struct{}
}

// readStartActions reads a quest's OnStartActions into JSON text ("[]" when there is none).
func readStartActions(v designdb.Value) (string, error) {
	out, err := readActionList(v)
	if err != nil {
		return "", err
	}
	b, err := json.Marshal(out)
	return string(b), err
}

// alnum is a name's letters and digits in lower case.
func alnum(s string) string {
	var b strings.Builder
	for _, r := range s {
		if unicode.IsLetter(r) || unicode.IsDigit(r) {
			b.WriteRune(unicode.ToLower(r))
		}
	}
	return b.String()
}

// dedupeIDs is the ids once each, in order.
func dedupeIDs(ids []int64) []int64 {
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	out := ids[:0:0]
	for i, id := range ids {
		if i == 0 || id != ids[i-1] {
			out = append(out, id)
		}
	}
	return out
}

func jsonIDs(ids []int64) string {
	if ids == nil {
		ids = []int64{}
	}
	b, _ := json.Marshal(ids)
	return string(b)
}

// questDetailJSON is the quest.detail column.
type questDetailJSON struct {
	OnSuccess []startActionJSON `json:"ok,omitempty"`
	OnFail    []startActionJSON `json:"fail,omitempty"`
	OnTick    []startActionJSON `json:"tick,omitempty"`
	Timeout   float64           `json:"to,omitempty"`
	TimeoutOK int               `json:"so,omitempty"`
	Giver     int64             `json:"gr,omitempty"` // map markers (version 30): the map registration the NPC that offers the quest shows while the quest is on offer (QuestGiverMapRegistrationId)
}

// objectiveActionsJSON are the four action lists of an objective.
type objectiveActionsJSON struct {
	Start []startActionJSON `json:"start,omitempty"`
	Tick  []startActionJSON `json:"tick,omitempty"`
	OK    []startActionJSON `json:"ok,omitempty"`
	Fail  []startActionJSON `json:"fail,omitempty"`
}

// objectiveDetailJSON is the quest_objective.detail column.
type objectiveDetailJSON struct {
	ShowDialogue int                   `json:"sd,omitempty"`
	Timeout      float64               `json:"to,omitempty"`
	TimeoutOK    int                   `json:"so,omitempty"`
	Activation   json.RawMessage       `json:"ar,omitempty"`
	Rewards      []int64               `json:"rw,omitempty"`
	Actions      *objectiveActionsJSON `json:"on,omitempty"`
	QuestItems   []map[string]any      `json:"qi,omitempty"`
	Data         map[string]any        `json:"d,omitempty"`
	Markers      *markerJSON           `json:"mk,omitempty"`
	Callout      int64                 `json:"cal,omitempty"` // (version 36) the map registration whose speech bubble the objective raises
}

// markerJSON is what the map pins of an objective need besides the data of its kind (version 30; see objectiveMarkers).
type markerJSON struct {
	Override  *overrideJSON  `json:"ov,omitempty"` // the record's own list of places: present when the record switches its base markers off (an empty list: no pin)
	Trackers  []int64        `json:"pt,omitempty"` // the population instances the objective tracks (Interact, Collect, TurnIn, EventNotification)
	NoDynamic int            `json:"nd,omitempty"` // the record switches the markers made from the population off (Dialogue, Kill)
	Cluster   map[string]any `json:"cl,omitempty"` // the clustering of the creatures' places (Kill), only when it is not the default
}

type overrideJSON struct {
	Locs []map[string]any `json:"l"`
}

// defaultCluster is the clustering every objective record has but 19 of 10,762 (ObjectiveTrackingClusteringParameters): objectiveMarkers writes the parameters only when they differ.
var defaultCluster = map[string]any{"e": 20000.0, "r0": 5000.0, "r1": 500000.0, "n": int64(2), "m": 6000.0, "c": int64(50), "d": "SpecificLocations", "w": int64(1)}

// markerLocations reads a list of FObjectiveIndicatorCluster (the places of a marker) into the form worldd reads: x, y, z, r, done, dt, hid, id.
func markerLocations(v designdb.Value) []map[string]any {
	out := []map[string]any{}
	ml, _ := v.([]designdb.Value)
	for _, e := range ml {
		s, ok := e.(designdb.Struct)
		if !ok {
			continue
		}
		c := structOf(s["Center"])
		m := map[string]any{"x": floatOf(c["X"]), "y": floatOf(c["Y"]), "z": floatOf(c["Z"]), "r": floatOf(s["Radius"])}
		if boolOf(s["bCompletesGoToObjective"]) {
			m["done"] = 1
		}
		if dt := enumValue(strOf(s["DisplayType"])); dt != "" && dt != "None" {
			m["dt"] = dt
			if dt == "Hidden" {
				m["hid"] = 1
			}
		}
		if id := intOf(s["QuestLocationId"]); id != 0 {
			m["id"] = id
		}
		out = append(out, m)
	}
	return out
}

// objectiveMarkers reads what the client's map pins of an objective come from, besides the places of a GoTo objective (version 30).  The client draws a pin for every active
// objective that has a place; the places are the record's own list when the record overrides its base markers, else they come from the kind: the places of a GoTo record,
// the people of a Dialogue / TurnIn record, the tracked instances of an Interact / Collect / TurnIn / EventNotification record, the creatures' spawn points of a Kill record.
func objectiveMarkers(f designdb.Struct, typ string) *markerJSON {
	m := &markerJSON{}
	if ov := structOf(f["OverrideObjectiveMarkerData"]); boolOf(ov["bOverrideBaseMarkerBehavior"]) {
		m.Override = &overrideJSON{Locs: markerLocations(structOf(ov["OverrideMapMarkerData"])["Locations"])}
	}
	switch typ {
	case "Interact":
		m.Trackers = idsOf(structOf(f["InteractData"])["PopInstanceTrackers"])
	case "Collect":
		m.Trackers = idsOf(structOf(f["CollectData"])["PopInstanceTrackers"])
	case "TurnIn":
		m.Trackers = idsOf(structOf(f["TurnInData"])["PopInstanceTrackers"])
	case "EventNotification":
		m.Trackers = idsOf(structOf(f["NotificationData"])["PopInstanceTrackers"])
	case "Dialogue":
		if boolOf(structOf(f["DialogueData"])["bDisableDynamicMarkers"]) {
			m.NoDynamic = 1
		}
	case "Kill":
		if boolOf(structOf(f["KillObjectiveData"])["bDisableDynamicMarkers"]) {
			m.NoDynamic = 1
		}
		c := structOf(f["ObjectiveTrackingClusteringParameters"])
		if len(c) > 0 {
			cl := map[string]any{
				"e":  floatOf(c["EpsilonBetweenNeighbors"]),
				"r0": floatOf(c["MinRadius"]),
				"r1": floatOf(c["MaxRadius"]),
				"n":  intOf(c["MinPointsPerCluster"]),
				"m":  floatOf(c["MergeDistance"]),
				"c":  intOf(c["MaxClustersOrPoints"]),
				"d":  enumValue(strOf(c["DefaultMarkerType"])),
				"w":  b2i64(boolOf(c["bUseWeightedCenter"])),
			}
			same := true
			for k, v := range defaultCluster {
				if cl[k] != v {
					same = false
				}
			}
			if !same {
				m.Cluster = cl
			}
		}
	}
	if m.Override == nil && len(m.Trackers) == 0 && m.NoDynamic == 0 && m.Cluster == nil {
		return nil
	}
	return m
}

func b2i64(b bool) int64 {
	if b {
		return 1
	}
	return 0
}

// sourceJSON is the DialogueSources list of a Dialogue or TurnIn objective.
func sourceJSON(v designdb.Value) []map[string]any {
	var out []map[string]any
	l, _ := v.([]designdb.Value)
	for _, e := range l {
		s, ok := e.(designdb.Struct)
		if !ok {
			continue
		}
		m := map[string]any{"k": enumValue(strOf(s["SourceType"]))}
		if g := idGuid(s["PopulationInstanceId"]); g != 0 {
			m["i"] = g
		}
		if g := idGuid(s["DialogueNode"]); g != 0 {
			m["n"] = g
		}
		if g := idGuid(s["InteractableId"]); g != 0 {
			m["x"] = g
		}
		if g := idGuid(s["ItemId"]); g != 0 {
			m["it"] = g
		}
		out = append(out, m)
	}
	return out
}

// tagNames reads the tag names of a GameplayTagQuery's dictionary.
func tagNames(v designdb.Value) []string {
	var out []string
	s := structOf(v)
	l, _ := s["TagDictionary"].([]designdb.Value)
	for _, e := range l {
		switch t := e.(type) {
		case string:
			if t != "" {
				out = append(out, t)
			}
		case designdb.Struct:
			if n := strOf(t["TagName"]); n != "" {
				out = append(out, n)
			}
		}
	}
	return out
}

// filterItems writes a NarrativeItemFilter into an objective's data: the named items, the tags, and "items" holding both the named items and every item that carries one of the tags
// (or a child of one), so that the engine counts by item records alone.
func filterItems(w *writer, data map[string]any, flt designdb.Struct) error {
	items := idsOf(flt["ItemsIds"])
	tags := tagNames(flt["Tags"])
	if len(tags) > 0 {
		data["tags"] = tags
		seen := map[int64]bool{}
		for _, id := range items {
			seen[id] = true
		}
		for _, t := range tags {
			rows, err := w.tx.Query(`SELECT DISTINCT item_guid FROM item_tag WHERE tag = ?1 OR substr(tag, 1, length(?1) + 1) = ?1 || '.' ORDER BY item_guid`, t)
			if err != nil {
				return fmt.Errorf("items of the tag %s: %w", t, err)
			}
			for rows.Next() {
				var id int64
				if err := rows.Scan(&id); err != nil {
					rows.Close()
					return err
				}
				if !seen[id] {
					seen[id] = true
					items = append(items, id)
				}
			}
			if err := rows.Err(); err != nil {
				rows.Close()
				return err
			}
			rows.Close()
		}
	}
	if len(items) > 0 {
		data["items"] = items
	}
	return nil
}

func floatOf(v designdb.Value) float64 {
	switch x := v.(type) {
	case float64:
		return x
	case float32:
		return decimal32(x)
	case int64:
		return float64(x)
	case uint64:
		return float64(x)
	}
	return 0
}

func boolOf(v designdb.Value) bool {
	b, _ := v.(bool)
	return b
}

// objectiveDetail reads everything the engine needs of an objective record besides the basic columns.
func objectiveDetail(w *writer, orow designdb.Row) (string, error) {
	f := orow.Fields
	typ := enumValue(strOf(f["ObjectiveType"]))
	d := objectiveDetailJSON{Timeout: floatOf(f["Timeout"]), Rewards: idsOf(f["Rewards"])}
	if boolOf(f["bShowQuestCompletionDialogue"]) {
		d.ShowDialogue = 1
	}
	if boolOf(f["bSucceedOnTimeout"]) {
		d.TimeoutOK = 1
	}
	if text, req, err := readRequirement(f["ActivationRule"]); err != nil {
		return "", fmt.Errorf("activation rule: %w", err)
	} else if text != "" {
		b, err := json.Marshal(req)
		if err != nil {
			return "", err
		}
		d.Activation = b
	}
	var acts objectiveActionsJSON
	var err error
	if acts.Start, err = readActionList(f["OnStartActions"]); err != nil {
		return "", fmt.Errorf("start actions: %w", err)
	}
	if acts.Tick, err = readActionList(f["OnTickActions"]); err != nil {
		return "", fmt.Errorf("tick actions: %w", err)
	}
	if acts.OK, err = readActionList(f["OnSuccessActions"]); err != nil {
		return "", fmt.Errorf("success actions: %w", err)
	}
	if acts.Fail, err = readActionList(f["OnFailureActions"]); err != nil {
		return "", fmt.Errorf("failure actions: %w", err)
	}
	if len(acts.Start)+len(acts.Tick)+len(acts.OK)+len(acts.Fail) > 0 {
		d.Actions = &acts
	}
	// the reward tables of quest items that listed creatures drop while the objective is active (QuestItemRewards)
	if l, ok := f["QuestItemRewards"].([]designdb.Value); ok {
		for _, e := range l {
			q, ok := e.(designdb.Struct)
			if !ok {
				continue
			}
			table := idGuid(q["RewardTableId"])
			if table == 0 {
				continue
			}
			m := map[string]any{"t": table}
			if ids := idsOf(q["PopulationInstanceQueries"]); len(ids) > 0 {
				m["inst"] = ids
			}
			if ids := idsOf(q["PopulationSetQueries"]); len(ids) > 0 {
				m["sets"] = ids
			}
			if len(tagNames(q["NPCTagQuery"])) > 0 {
				m["tq"] = 1
			}
			d.QuestItems = append(d.QuestItems, m)
		}
	}
	data := map[string]any{}
	switch typ {
	case "Kill":
		k := structOf(f["KillObjectiveData"])
		if l := idsOf(k["PopulationSets"]); len(l) > 0 {
			data["sets"] = l
		}
		if l := idsOf(k["PopulationInstances"]); len(l) > 0 {
			data["inst"] = l
		}
		if boolOf(k["bEnableAdvancedFilters"]) {
			data["adv"] = 1
			data["elite"] = intOf(structOf(k["AdvancedFilters"])["EliteRank"])
		}
		if boolOf(k["bKillQuestOwner"]) {
			data["owner"] = 1
		}
		if boolOf(k["bUseQuestContext"]) {
			data["ctx"] = 1
		}
		if len(tagNames(k["TagQuery"])) > 0 {
			data["tq"] = 1
		}
	case "Dialogue":
		if src := sourceJSON(structOf(f["DialogueData"])["DialogueSources"]); len(src) > 0 {
			data["src"] = src
		}
	case "Interact":
		i := structOf(f["InteractData"])
		if l := idsOf(i["InteractableComponentDefId"]); len(l) > 0 {
			data["comp"] = l
		}
		if l := idsOf(i["InteractableActorDefIds"]); len(l) > 0 {
			data["actor"] = l
		}
	case "Collect":
		c := structOf(f["CollectData"])
		data["cty"] = enumValue(strOf(c["CollectType"]))
		flt := structOf(c["Filter"])
		if err := filterItems(w, data, flt); err != nil {
			return "", err
		}
		if l, ok := flt["ItemRarities"].([]designdb.Value); ok && len(l) > 0 {
			var r []string
			for _, e := range l {
				r = append(r, enumValue(strOf(e)))
			}
			data["rar"] = r
		}
	case "GoTo":
		g := structOf(f["GoToData"])
		if locs := markerLocations(structOf(g["MapMarkerData"])["Locations"]); len(locs) > 0 {
			data["loc"] = locs
		}
		if boolOf(g["bUseAreaMap"]) {
			data["area"] = 1
		}
	case "TurnIn":
		t := structOf(f["TurnInData"])
		flt := structOf(t["Filter"])
		if err := filterItems(w, data, flt); err != nil {
			return "", err
		}
		if src := sourceJSON(t["DialogueSources"]); len(src) > 0 {
			data["src"] = src
		}
		if boolOf(t["bAcceptAnySource"]) {
			data["any"] = 1
		}
		if boolOf(t["bCrateTurnIn"]) {
			data["crate"] = 1
		}
		if l := idsOf(t["CrateTypeFilter"]); len(l) > 0 {
			data["ct"] = l
		}
		if l := idsOf(t["CrateItemFilter"]); len(l) > 0 {
			data["ci"] = l
		}
		if n := noneAsEmpty(strOf(t["QuestVariableName"])); n != "" {
			data["qv"] = n
		}
		if boolOf(t["bIncrementQuestVariableOnTurnIn"]) {
			data["inc"] = 1
		}
	case "EventNotification":
		n := structOf(f["NotificationData"])
		data["nt"] = enumValue(strOf(n["NotifyType"]))
		if g := idGuid(n["RecipeId"]); g != 0 {
			data["recipe"] = g
		}
	case "UseAbility":
		if l := idsOf(structOf(f["AbilityUseData"])["Abilities"]); len(l) > 0 {
			data["ab"] = l
		}
	case "Custom":
		// the rule is ONE criterion: kept as a requirement of one criterion (an ALL over it) so that the conversations' evaluator reads it
		rule := structOf(structOf(f["CustomData"])["CustomRule"])
		if crit, ok := rule["Criteria"].(designdb.Struct); ok {
			_, req, err := readRequirement(designdb.Struct{
				"QueryDictionary": []designdb.Value{crit},
				"TokenStream":     []designdb.Value{int64(1), int64(1), int64(0)},
			})
			if err != nil {
				return "", fmt.Errorf("custom rule: %w", err)
			}
			if req != nil {
				data["rule"] = req
			}
		}
	}
	if len(data) > 0 {
		d.Data = data
	}
	d.Markers = objectiveMarkers(f, typ)
	d.Callout = objectiveCallout(f, typ)
	b, err := json.Marshal(d)
	return string(b), err
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
	killSets := map[int64]bool{}
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
		var qd questDetailJSON
		if qd.OnSuccess, err = readActionList(r.Fields["OnSuccessActions"]); err != nil {
			return fmt.Errorf("%s: success actions: %w", f.where, err)
		}
		if qd.OnFail, err = readActionList(r.Fields["OnFailureActions"]); err != nil {
			return fmt.Errorf("%s: failure actions: %w", f.where, err)
		}
		if qd.OnTick, err = readActionList(r.Fields["OnTickActions"]); err != nil {
			return fmt.Errorf("%s: tick actions: %w", f.where, err)
		}
		qd.Timeout = floatOf(r.Fields["Timeout"])
		if boolOf(r.Fields["bSucceedOnTimeout"]) {
			qd.TimeoutOK = 1
		}
		qd.Giver = idGuid(r.Fields["QuestGiverMapRegistrationId"])
		detail, err := json.Marshal(qd)
		if err != nil {
			return err
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
		if err := w.exec(`INSERT INTO quest (guid, name, flavor, locality, level, can_abandon, repeatable, disabled, default_activate, activation, start_actions, rewards, detail)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, flavor, locality, level, b2i(canAbandon), b2i(repeatable), b2i(disabled), b2i(defAct), activation, start, jsonIDs(rewards), string(detail)); err != nil {
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
			od, err := objectiveDetail(w, orow)
			if err != nil {
				return fmt.Errorf("%s: %w", of.where, err)
			}
			if typ == "Kill" {
				for _, sg := range idsOf(structOf(orow.Fields["KillObjectiveData"])["PopulationSets"]) {
					killSets[sg] = true
				}
			}
			if l, ok := orow.Fields["QuestItemRewards"].([]designdb.Value); ok {
				for _, e := range l {
					for _, sg := range idsOf(structOf(e)["PopulationSetQueries"]) {
						killSets[sg] = true
					}
				}
			}
			if err := w.exec(`INSERT INTO quest_objective (quest_guid, idx, guid, name, obj_type, completion_count, default_awake, default_hidden, completes_quest, optional, detail)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
				r.Guid, i, og, orow.Name, typ, count, b2i(awake), b2i(hidden), b2i(completes), b2i(optional), od); err != nil {
				return err
			}
			objs++
		}
	}

	// the creatures of the population sets the Kill objectives name (a kill counts for the sets its creature stands in)
	var killMembers int
	for sg := range killSets {
		if err := w.exec(`INSERT OR IGNORE INTO quest_kill_set (set_guid, instance_guid) SELECT set_guid, instance_guid FROM npc_set_member WHERE set_guid = ?`, sg); err != nil {
			return err
		}
	}
	if err := w.tx.QueryRow(`SELECT COUNT(*) FROM quest_kill_set`).Scan(&killMembers); err != nil {
		return fmt.Errorf("quest_kill_set: %w", err)
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

	// the nodes that offer a quest, and the nodes that ask the player to confirm (a Confirmation node names no quest: its OnSuccessNode is where the talk goes on after a yes)
	var accepts int
	for _, r := range src.rows["DialogueNode"] {
		if nt := enumValue(strOf(r.Fields["NodeType"])); nt != "AcceptQuest" && nt != "Confirmation" {
			continue
		}
		if err := w.exec(`INSERT INTO dialogue_accept (node_guid, quest_guid, on_success, objectives) VALUES (?, ?, ?, ?)`,
			r.Guid, idGuid(r.Fields["QuestId"]), idGuid(r.Fields["OnSuccessNode"]), jsonIDs(idsOf(r.Fields["ObjectiveIds"]))); err != nil {
			return err
		}
		accepts++
	}

	// the nodes that take items for a quest (version 37): the quest, where the talk goes on after the hand-in and the TurnIn objectives the node lists
	var turnIns int
	for _, r := range src.rows["DialogueNode"] {
		if enumValue(strOf(r.Fields["NodeType"])) != "TurnIn" {
			continue
		}
		if err := w.exec(`INSERT INTO dialogue_turnin (node_guid, quest_guid, on_success, objectives) VALUES (?, ?, ?, ?)`,
			r.Guid, idGuid(r.Fields["QuestId"]), idGuid(r.Fields["OnSuccessNode"]), jsonIDs(idsOf(r.Fields["ObjectiveIds"]))); err != nil {
			return err
		}
		turnIns++
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

	// the Emberspring interactables of the places (ICD_<Place>_Attunable_Respawn_Shrine -> the attune point of that place, by name): what an Interact objective names
	var shrineICDs int
	{
		rows, err := w.tx.Query(`SELECT guid, name, descriptor FROM attune_point`)
		if err != nil {
			return fmt.Errorf("shrine_icd: read attune_point: %w", err)
		}
		places := map[string][]int64{}
		for rows.Next() {
			var g int64
			var name, desc string
			if err := rows.Scan(&g, &name, &desc); err != nil {
				rows.Close()
				return err
			}
			// "RVR_Samias_Hope" and "Samia's Hope" are one place: the key is the letters and digits, lower case
			places[alnum(name)] = append(places[alnum(name)], g)
			if i := strings.Index(name, "_"); i == 3 {
				places[alnum(name[i+1:])] = append(places[alnum(name[i+1:])], g)
			}
			places[alnum(desc)] = append(places[alnum(desc)], g)
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		rows.Close()
		// the interactable definitions are read from the record table (written above): the quest builder needs only their names
		icds, err := w.tx.Query(`SELECT guid, name FROM record WHERE schema = 'InteractableComponentDefRecordBase' AND name LIKE 'ICD%Attunable%' ORDER BY guid`)
		if err != nil {
			return fmt.Errorf("shrine_icd: read the interactable definitions: %w", err)
		}
		type icdRow struct {
			guid int64
			name string
		}
		var found []icdRow
		for icds.Next() {
			var r icdRow
			if err := icds.Scan(&r.guid, &r.name); err != nil {
				icds.Close()
				return err
			}
			found = append(found, r)
		}
		if err := icds.Err(); err != nil {
			icds.Close()
			return err
		}
		icds.Close()
		for _, r := range found {
			core := strings.TrimPrefix(r.name, "ICD_")
			i := strings.Index(core, "_Attunable")
			if !strings.HasPrefix(r.name, "ICD_") || i <= 0 {
				continue
			}
			list := dedupeIDs(places[alnum(core[:i])])
			if len(list) != 1 {
				continue // a place the attune points do not name, or two of them: no link
			}
			if err := w.exec(`INSERT INTO shrine_icd (icd_guid, attune_guid) VALUES (?, ?)`, r.guid, list[0]); err != nil {
				return err
			}
			shrineICDs++
		}
	}

	// the items bound to quests, and the crate type of each crate item
	var itemLinks, crateItems int
	for _, r := range src.rows["ItemRecordBase"] {
		if crate := idGuid(r.Fields["CrateId"]); crate != 0 {
			if err := w.exec(`INSERT INTO item_crate (item_guid, crate_guid) VALUES (?, ?)`, r.Guid, crate); err != nil {
				return err
			}
			crateItems++
		}
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
	meta["quests.turnin_nodes"] = fmt.Sprint(turnIns)
	meta["quests.zoi_quests"] = fmt.Sprint(zoiQuests)
	meta["quests.item_links"] = fmt.Sprint(itemLinks)
	meta["quests.crate_items"] = fmt.Sprint(crateItems)
	meta["quests.shrine_icds"] = fmt.Sprint(shrineICDs)
	meta["quests.kill_set_members"] = fmt.Sprint(killMembers)
	opt.logf("quests: %d quests (%d with an activation rule), %d objectives (%d missing), %d offers by NPCs, %d accept nodes, %d turn-in nodes, %d zone quests, %d item links, %d crate items, %d creatures of the kill objectives' sets, %d shrine interactables", quests, gated, objs, missing, offers, accepts, turnIns, zoiQuests, itemLinks, crateItems, killMembers, shrineICDs)
	return buildObjects(w, src, meta)
}

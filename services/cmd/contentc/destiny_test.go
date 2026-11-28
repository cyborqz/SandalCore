// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"reflect"
	"testing"

	"aocrop/services/internal/designdb"
)

func id(g int64) designdb.Struct { return designdb.Struct{"Guid": g} }

func subQuestObjective(guid, child int64, kind string, wait bool) designdb.Row {
	return designdb.Row{Guid: guid, Fields: designdb.Struct{
		"ObjectiveType": "ENarrativeObjectiveType::SubQuest",
		"SubQuestInfo": designdb.Struct{
			"InstantiationType":  "ESubquestInstantiationType::" + kind,
			"SubQuest":           id(child),
			"bWaitForCompletion": wait,
		},
	}}
}

// A SubQuest objective's child, how it is started and whether the objective waits for it are kept in the detail column.
func TestSubQuestDetail(t *testing.T) {
	o := subQuestObjective(6064634619246149635, 6064634478640431178, "AutoGrant", true)
	sq := subQuestOf(o.Fields)
	if sq == nil || sq.Quest != 6064634478640431178 || sq.Kind != "AutoGrant" || sq.Wait != 1 {
		t.Fatalf("subQuestOf = %+v", sq)
	}
	b, err := json.Marshal(sq)
	if err != nil {
		t.Fatal(err)
	}
	if string(b) != `{"q":6064634478640431178,"t":"AutoGrant","w":1}` {
		t.Errorf("json = %s", b)
	}
	// not waiting, and another kind: "w" is left out
	o = subQuestObjective(1, 2, "Instance", false)
	b, _ = json.Marshal(subQuestOf(o.Fields))
	if string(b) != `{"q":2,"t":"Instance"}` {
		t.Errorf("json = %s", b)
	}
	// a SubQuest that names no quest has nothing to keep
	if sq := subQuestOf(designdb.Struct{"SubQuestInfo": designdb.Struct{"InstantiationType": "ESubquestInstantiationType::AutoGrant"}}); sq != nil {
		t.Errorf("a SubQuestInfo without a quest gave %+v", sq)
	}
	if sq := subQuestOf(designdb.Struct{}); sq != nil {
		t.Errorf("a record without SubQuestInfo gave %+v", sq)
	}
}

// The tracks come in the record's order, each with the quest its first objective starts; a track whose first objective is not a SubQuest, or whose quest is not in the data, keeps 0.
func TestDestinyTracks(t *testing.T) {
	objectives := map[int64]designdb.Row{
		101: subQuestObjective(101, 11, "AutoGrant", true),
		102: subQuestObjective(102, 12, "AutoGrant", true),
		103: {Guid: 103, Fields: designdb.Struct{"ObjectiveType": "ENarrativeObjectiveType::Custom"}},
	}
	quests := []designdb.Row{
		{Guid: 1, Fields: designdb.Struct{"ObjectiveIds": []designdb.Value{id(101)}}},
		{Guid: 2, Fields: designdb.Struct{"ObjectiveIds": []designdb.Value{id(102), id(101)}}},
		{Guid: 3, Fields: designdb.Struct{"ObjectiveIds": []designdb.Value{id(103)}}},
	}
	track := func(quest, tree int64, ms ...int64) designdb.Value {
		var list []designdb.Value
		for _, m := range ms {
			list = append(list, id(m))
		}
		return designdb.Struct{"DestinyTrackQuest": id(quest), "DestinyTrackSkillTree": id(tree), "MilestoneRewardObjectives": list}
	}
	systems := []designdb.Row{{Guid: 9, Fields: designdb.Struct{"DestinyTrackDataArray": []designdb.Value{track(1, 31, 41, 42), track(2, 32), track(3, 33, 43), track(4, 34)}}}}
	got := destinyTracks(systems, quests, objectives)
	want := []destinyTrack{
		{Ord: 0, TrackQuest: 1, Quest: 11, SkillTree: 31, Milestones: []int64{41, 42}},
		{Ord: 1, TrackQuest: 2, Quest: 12, SkillTree: 32},
		{Ord: 2, TrackQuest: 3, Quest: 0, SkillTree: 33, Milestones: []int64{43}},
		{Ord: 3, TrackQuest: 4, Quest: 0, SkillTree: 34},
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("destinyTracks = %+v, want %+v", got, want)
	}
	if n := len(destinyTracks(nil, quests, objectives)); n != 0 {
		t.Errorf("no DestinySystem record gave %d tracks", n)
	}
}

func TestTagStrings(t *testing.T) {
	got := tagStrings([]designdb.Value{"ability.dodge", designdb.Struct{"TagName": "ability.Melee"}, "None", designdb.Struct{"TagName": ""}, ""})
	if !reflect.DeepEqual(got, []string{"ability.dodge", "ability.Melee"}) {
		t.Errorf("tagStrings = %v", got)
	}
	if tagStrings(nil) != nil || tagStrings("x") != nil {
		t.Errorf("tagStrings of nothing is not nil")
	}
}

// An ability carries a tag when it has it or a child of it; every required tag has to be carried; the names compare in lower case; the answer is in guid order.
func TestAbilityTagIndex(t *testing.T) {
	ab := func(guid int64, tags ...string) designdb.Row {
		var l []designdb.Value
		for _, tag := range tags {
			l = append(l, designdb.Struct{"TagName": tag})
		}
		return designdb.Row{Guid: guid, Fields: designdb.Struct{"AbilityTags": l}}
	}
	by := abilityTagIndex([]designdb.Row{
		ab(30, "ability.weaponcombo", "ability.Melee"),
		ab(10, "ability.dodge"),
		ab(20, "ability.weaponcombo", "ability.Ranged"),
		ab(40, "Ability.Dodge.Rogue"),
		ab(50),
		ab(60, "ability.weaponcombo"),
	})
	for _, c := range []struct {
		req  []string
		want []int64
	}{
		{[]string{"ability.dodge"}, []int64{10, 40}},
		{[]string{"ability.weaponcombo", "ability.melee"}, []int64{30}},
		{[]string{"ability.weaponcombo", "ability.Ranged"}, []int64{20}},
		{[]string{"ability.weaponcombo"}, []int64{20, 30, 60}},
		{[]string{"ability.guard"}, nil},
	} {
		if got := by(c.req); !reflect.DeepEqual(got, c.want) {
			t.Errorf("abilities with %v = %v, want %v", c.req, got, c.want)
		}
	}
}

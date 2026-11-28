// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"

	"aocrop/services/internal/designdb"
)

// The Destiny window's tracks (quests milestone, the Destiny window): the destiny_track table (internal/content/quests.go says what each column holds).
//
// The one DestinySystem record lists the six tracks. A track names its "track quest" (First Steps: Destiny_Novice_FirstStep_02_Root), a quest of one objective that starts the
// track's own quest (Destiny_Novice_FirstStep_02) and waits for it. The window shows the own quest and keys its instance guid on it, and the client finds it the same way: the
// first objective of the track quest, which must be a SubQuest, names it. The table keeps both.

// destinyTrack is one row of destiny_track.
type destinyTrack struct {
	Ord        int
	TrackQuest int64   // DestinyTrackQuest
	Quest      int64   // the quest its first objective starts (0: none)
	SkillTree  int64   // DestinyTrackSkillTree
	Milestones []int64 // MilestoneRewardObjectives
}

// destinyTracks reads the tracks of the DestinySystem records, in the records' order (by guid), and finds each track's own quest in `quests` (the NarrativeQuestV2 rows) and
// `objectives` (the NarrativeObjectiveV2 rows by guid).
func destinyTracks(systems, quests []designdb.Row, objectives map[int64]designdb.Row) []destinyTrack {
	byGuid := make(map[int64]designdb.Row, len(quests))
	for _, q := range quests {
		byGuid[q.Guid] = q
	}
	var out []destinyTrack
	for _, sys := range systems {
		list, _ := sys.Fields["DestinyTrackDataArray"].([]designdb.Value)
		for _, e := range list {
			t, ok := e.(designdb.Struct)
			if !ok {
				continue
			}
			tr := destinyTrack{
				Ord:        len(out),
				TrackQuest: idGuid(t["DestinyTrackQuest"]),
				SkillTree:  idGuid(t["DestinyTrackSkillTree"]),
				Milestones: idsOf(t["MilestoneRewardObjectives"]),
			}
			if q, ok := byGuid[tr.TrackQuest]; ok {
				if first := idsOf(q.Fields["ObjectiveIds"]); len(first) > 0 {
					if o, ok := objectives[first[0]]; ok && enumValue(strOf(o.Fields["ObjectiveType"])) == "SubQuest" {
						if sq := subQuestOf(o.Fields); sq != nil {
							tr.Quest = sq.Quest
						}
					}
				}
			}
			out = append(out, tr)
		}
	}
	return out
}

// buildDestinyTracks writes destiny_track. A design database without the DestinySystem table writes none (the window then has no tracks).
func buildDestinyTracks(w *writer, src *source, objectives map[int64]designdb.Row, meta map[string]string, opt *Options) error {
	tracks := destinyTracks(src.rows["DestinySystem"], src.rows["NarrativeQuestV2"], objectives)
	withQuest := 0
	for _, t := range tracks {
		ms, err := json.Marshal(append([]int64{}, t.Milestones...))
		if err != nil {
			return err
		}
		if err := w.exec(`INSERT INTO destiny_track (ord, track_quest, quest_guid, skill_tree, milestones) VALUES (?, ?, ?, ?, ?)`, t.Ord, t.TrackQuest, t.Quest, t.SkillTree, string(ms)); err != nil {
			return err
		}
		if t.Quest != 0 {
			withQuest++
		} else {
			opt.logf("destiny: the track quest %d starts no quest with its first objective: the window cannot show it", t.TrackQuest)
		}
	}
	meta["quests.destiny_tracks"] = fmt.Sprintf("%d (%d with a quest of their own)", len(tracks), withQuest)
	opt.logf("destiny: %d tracks, %d with a quest of their own", len(tracks), withQuest)
	return nil
}

// tagStrings reads a list of tag names: plain strings or {TagName} structs.
func tagStrings(v designdb.Value) []string {
	var out []string
	l, _ := v.([]designdb.Value)
	for _, e := range l {
		switch t := e.(type) {
		case string:
			if t != "" && t != "None" {
				out = append(out, t)
			}
		case designdb.Struct:
			if n := strOf(t["TagName"]); n != "" && n != "None" {
				out = append(out, n)
			}
		}
	}
	return out
}

// abilityTagIndex answers "which abilities carry every one of these tags?" from the ability records' AbilityTags. A tag is carried by an ability that has it or a child of it
// ("ability.weaponcombo" is carried by an ability tagged "ability.weaponcombo.x"); names compare in lower case. The answer is in guid order.
func abilityTagIndex(abilities []designdb.Row) func([]string) []int64 {
	type tagged struct {
		guid int64
		tags []string
	}
	var all []tagged
	for _, a := range abilities {
		if tags := tagStrings(a.Fields["AbilityTags"]); len(tags) > 0 {
			for i, t := range tags {
				tags[i] = strings.ToLower(t)
			}
			all = append(all, tagged{a.Guid, tags})
		}
	}
	sort.Slice(all, func(i, j int) bool { return all[i].guid < all[j].guid })
	return func(required []string) []int64 {
		var out []int64
		for _, a := range all {
			ok := true
			for _, r := range required {
				r = strings.ToLower(r)
				found := false
				for _, t := range a.tags {
					if t == r || strings.HasPrefix(t, r+".") {
						found = true
						break
					}
				}
				if !found {
					ok = false
					break
				}
			}
			if ok {
				out = append(out, a.guid)
			}
		}
		return out
	}
}

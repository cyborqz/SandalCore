// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package quests

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"

	questv1 "aocrop/services/gen/aoc/quest/v1"
	"aocrop/services/internal/account"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

// setup gives a migrated schema, a service over it and one character (its id).
func setup(t *testing.T) (*Service, string) {
	t.Helper()
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	acc, err := account.NewStore(pool, 0).Register(ctx, "owner", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	c, err := character.NewStore(pool).Create(ctx, character.New{AccountID: acc.ID, Name: "cyb", ArchetypeID: 17747, ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	return New(pool), c.ID
}

func guid(n int) string { return fmt.Sprintf("%032x", n) }

// sampleLog is a log with two quests (the first with two objectives, one of them partly done), a finished quest and two variables.
func sampleLog() *questv1.QuestLog {
	return &questv1.QuestLog{
		Instances: []*questv1.QuestInstance{
			{InstanceGuid: guid(1), RecordId: 6064634563262021659, Status: 0, StartTime: 12.5, SourceGuid: 6064634562958262298, Objectives: []*questv1.ObjectiveState{
				{InstanceGuid: guid(2), RecordId: 6064634563268640796, Active: true, Awake: true},
				{InstanceGuid: guid(3), RecordId: 6064634563305209885, Hidden: true, Count: 2, StartTime: 3.25},
			}},
			{InstanceGuid: guid(4), RecordId: 6064634641390699452, Status: 3},
		},
		Completed: []*questv1.CompletedQuest{{RecordId: 6064634760119582720, Count: 2}},
		Variables: []*questv1.PlayerVariable{{Name: "PickedSamias", Value: 1}, {Name: "WelcomeToVerra", Value: 1}},
	}
}

// A log is saved whole and read back as it was: the order of the quests, their guids, their objectives, what was finished and the variables; a new save replaces the old one, and a
// character that took nothing has an empty log.  A name finds the character as well as its id.
func TestSaveAndLoadTheWholeLog(t *testing.T) {
	s, id := setup(t)
	ctx := context.Background()
	empty, err := s.LoadLog(ctx, id)
	if err != nil || len(empty.GetInstances()) != 0 || len(empty.GetCompleted()) != 0 || len(empty.GetVariables()) != 0 {
		t.Fatalf("a new character's log: %v, %v", empty, err)
	}
	want := sampleLog()
	if err := s.SaveLog(ctx, id, want); err != nil {
		t.Fatal(err)
	}
	got, err := s.LoadLog(ctx, "CYB") // by name, in any case
	if err != nil {
		t.Fatal(err)
	}
	if !proto.Equal(got, want) {
		t.Fatalf("the log read back differs:\n got %v\nwant %v", got, want)
	}
	// the quest the player took first stays first (the book lists them in that order)
	second := &questv1.QuestLog{Instances: []*questv1.QuestInstance{want.Instances[1], want.Instances[0]}}
	if err := s.SaveLog(ctx, id, second); err != nil {
		t.Fatal(err)
	}
	got, err = s.LoadLog(ctx, id)
	if err != nil || !proto.Equal(got, second) {
		t.Fatalf("after a second save: %v, %v; want %v", got, err, second)
	}
	if err := s.SaveLog(ctx, id, &questv1.QuestLog{}); err != nil {
		t.Fatal(err)
	}
	if got, err = s.LoadLog(ctx, id); err != nil || len(got.GetInstances()) != 0 || len(got.GetCompleted()) != 0 || len(got.GetVariables()) != 0 {
		t.Fatalf("after an empty save: %v, %v", got, err)
	}
}

// A log the service refuses changes nothing: the one stored stays as it was.
func TestARefusedLogChangesNothing(t *testing.T) {
	s, id := setup(t)
	ctx := context.Background()
	good := sampleLog()
	if err := s.SaveLog(ctx, id, good); err != nil {
		t.Fatal(err)
	}
	mutate := func(f func(*questv1.QuestLog)) *questv1.QuestLog {
		l := proto.Clone(sampleLog()).(*questv1.QuestLog)
		f(l)
		return l
	}
	for name, bad := range map[string]*questv1.QuestLog{
		"a guid in capitals":          mutate(func(l *questv1.QuestLog) { l.Instances[0].InstanceGuid = strings.ToUpper(guid(0xabc)) }),
		"the zero guid":               mutate(func(l *questv1.QuestLog) { l.Instances[0].InstanceGuid = guid(0) }),
		"a short guid":                mutate(func(l *questv1.QuestLog) { l.Instances[0].InstanceGuid = "abc" }),
		"the same guid twice":         mutate(func(l *questv1.QuestLog) { l.Instances[1].InstanceGuid = l.Instances[0].InstanceGuid }),
		"the same quest twice":        mutate(func(l *questv1.QuestLog) { l.Instances[1].RecordId = l.Instances[0].RecordId }),
		"record 0":                    mutate(func(l *questv1.QuestLog) { l.Instances[0].RecordId = 0 }),
		"status 4":                    mutate(func(l *questv1.QuestLog) { l.Instances[0].Status = 4 }),
		"a negative start time":       mutate(func(l *questv1.QuestLog) { l.Instances[0].StartTime = -1 }),
		"an objective with no guid":   mutate(func(l *questv1.QuestLog) { l.Instances[0].Objectives[0].InstanceGuid = "" }),
		"a negative count":            mutate(func(l *questv1.QuestLog) { l.Instances[0].Objectives[1].Count = -1 }),
		"a quest finished 0 times":    mutate(func(l *questv1.QuestLog) { l.Completed[0].Count = 0 }),
		"a quest finished twice":      mutate(func(l *questv1.QuestLog) { l.Completed = append(l.Completed, l.Completed[0]) }),
		"a nameless variable":         mutate(func(l *questv1.QuestLog) { l.Variables[0].Name = "" }),
		"a variable twice":            mutate(func(l *questv1.QuestLog) { l.Variables[1].Name = l.Variables[0].Name }),
		"a variable with a long name": mutate(func(l *questv1.QuestLog) { l.Variables[0].Name = strings.Repeat("x", maxName+1) }),
		"too many quests": mutate(func(l *questv1.QuestLog) {
			l.Instances = nil
			for i := 1; i <= MaxInstances+1; i++ {
				l.Instances = append(l.Instances, &questv1.QuestInstance{InstanceGuid: guid(i), RecordId: int64(i)})
			}
		}),
		"too many objectives": mutate(func(l *questv1.QuestLog) {
			l.Instances[0].Objectives = nil
			for i := 1; i <= MaxObjectives+1; i++ {
				l.Instances[0].Objectives = append(l.Instances[0].Objectives, &questv1.ObjectiveState{InstanceGuid: guid(100 + i), RecordId: int64(i)})
			}
		}),
	} {
		if err := s.SaveLog(ctx, id, bad); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: SaveLog = %v, want an invalid-input error", name, err)
		}
	}
	got, err := s.LoadLog(ctx, id)
	if err != nil || !proto.Equal(got, good) {
		t.Fatalf("the stored log changed:\n got %v (%v)\nwant %v", got, err, good)
	}
	// a character that is not there
	if err := s.SaveLog(ctx, "ffffffffffffffffffffffffffffffff", good); !errors.Is(err, ErrNotFound) {
		t.Errorf("SaveLog for an unknown character = %v, want ErrNotFound", err)
	}
	if _, err := s.LoadLog(ctx, "nobody"); !errors.Is(err, ErrNotFound) {
		t.Errorf("LoadLog for an unknown character = %v, want ErrNotFound", err)
	}
	if _, err := s.LoadLog(ctx, " "); !errors.Is(err, ErrInvalid) {
		t.Errorf("LoadLog with no character = %v, want an invalid-input error", err)
	}
}

// A deleted character takes its quests along, and a save for it finds nothing.
func TestADeletedCharacterTakesItsQuestsAlong(t *testing.T) {
	s, id := setup(t)
	ctx := context.Background()
	if err := s.SaveLog(ctx, id, sampleLog()); err != nil {
		t.Fatal(err)
	}
	if _, err := s.QueueCommand(ctx, &questv1.QueueCommandRequest{CharacterId: id, Kind: "clear"}); err != nil {
		t.Fatal(err)
	}
	if _, err := s.pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, id); err != nil {
		t.Fatal(err)
	}
	for _, tb := range []string{"character_quests", "character_quests_completed", "character_quest_variables", "character_quest_commands"} {
		var n int
		if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM `+tb).Scan(&n); err != nil || n != 0 {
			t.Errorf("%d rows of %s outlive the character (%v)", n, tb, err)
		}
	}
	if err := s.SaveLog(ctx, id, sampleLog()); !errors.Is(err, ErrNotFound) {
		t.Errorf("SaveLog after the delete = %v, want ErrNotFound", err)
	}
}

// The GM commands wait in the character's inbox, oldest first, and are taken once.
func TestTheCommandInbox(t *testing.T) {
	s, id := setup(t)
	ctx := context.Background()
	queue := func(r *questv1.QueueCommandRequest) (int64, error) {
		r.CharacterId = id
		return s.QueueCommand(ctx, r)
	}
	var ids []int64
	for _, r := range []*questv1.QueueCommandRequest{
		{Kind: "give", Quest: " RVR_FTUE_QST_1415_OfTheRootways "},
		{Kind: "set-var", Name: "PickedSamias", Value: 1},
		{Kind: "finish", Quest: "6064634563262021659"},
		{Kind: "clear"},
	} {
		cmd, err := queue(r)
		if err != nil {
			t.Fatalf("%v: %v", r, err)
		}
		ids = append(ids, cmd)
	}
	for i := 1; i < len(ids); i++ {
		if ids[i] <= ids[i-1] {
			t.Fatalf("the command ids do not rise: %v", ids)
		}
	}
	for name, r := range map[string]*questv1.QueueCommandRequest{
		"an unknown kind":            {Kind: "teleport"},
		"a give with no quest":       {Kind: "give", Quest: "  "},
		"a set-var with no name":     {Kind: "set-var", Value: 1},
		"a set-var with a long name": {Kind: "set-var", Name: strings.Repeat("x", maxName+1)},
	} {
		if _, err := queue(r); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want an invalid-input error", name, err)
		}
	}
	if _, err := s.QueueCommand(ctx, &questv1.QueueCommandRequest{CharacterId: "nobody", Kind: "clear"}); !errors.Is(err, ErrNotFound) {
		t.Errorf("a command for an unknown character: %v, want ErrNotFound", err)
	}
	got, err := s.TakeCommands(ctx, "cyb")
	if err != nil || len(got) != 4 {
		t.Fatalf("TakeCommands = %v, %v; want the four commands", got, err)
	}
	for i, c := range got {
		if c.Id != ids[i] {
			t.Errorf("command %d has id %d, want %d (oldest first)", i, c.Id, ids[i])
		}
	}
	if got[0].Kind != "give" || got[0].Quest != "RVR_FTUE_QST_1415_OfTheRootways" || got[1].Name != "PickedSamias" || got[1].Value != 1 || got[2].Quest != "6064634563262021659" || got[3].Kind != "clear" {
		t.Errorf("the commands came back as %v", got)
	}
	if again, err := s.TakeCommands(ctx, id); err != nil || len(again) != 0 {
		t.Errorf("a second take = %v, %v; want nothing (they are removed)", again, err)
	}
	// one inbox holds at most MaxCommands
	for i := 0; i < MaxCommands; i++ {
		if _, err := queue(&questv1.QueueCommandRequest{Kind: "clear"}); err != nil {
			t.Fatalf("command %d: %v", i, err)
		}
	}
	if _, err := queue(&questv1.QueueCommandRequest{Kind: "clear"}); !errors.Is(err, ErrInvalid) {
		t.Errorf("a command over the limit: %v, want an invalid-input error", err)
	}
	if taken, err := s.TakeCommands(ctx, id); err != nil || len(taken) != 64 {
		t.Errorf("one take returns at most 64 commands: %d, %v", len(taken), err)
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package quests keeps the saved quest log of each character (quests milestone, slice 1, "take a quest"): the quests it took and has not finished, how often
// it finished each, its quest variables, and the inbox of GM commands for it. worldd is the only writer of a log while its character plays: it loads the
// log at login and saves the whole log whenever it changes. The rules of the quests (what a quest needs, what it does) are worldd's; this package stores
// and validates what it is told, and removes a character's rows with the character (the tables cascade).
package quests

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	questv1 "aocrop/services/gen/aoc/quest/v1"
)

// ErrNotFound is returned for a character that is not there; ErrInvalid (wrapped) for input the service refuses.
var (
	ErrNotFound = errors.New("quests: no such character")
	ErrInvalid  = errors.New("invalid")
)

// The bounds of one character's log. The game's own limit of quests at once is 500; the others keep one save small and bounded.
const (
	MaxInstances  = 600
	MaxObjectives = 64
	MaxCompleted  = 8192
	MaxVariables  = 2048
	MaxCommands   = 256
	maxName       = 128
)

var guidHex = regexp.MustCompile(`^[0-9a-f]{32}$`)

// Service is the quest service.
type Service struct {
	pool *pgxpool.Pool
}

// New serves the quest tables of pool.
func New(pool *pgxpool.Pool) *Service { return &Service{pool: pool} }

func invalid(format string, a ...any) error {
	return fmt.Errorf("%w: %s", ErrInvalid, fmt.Sprintf(format, a...))
}

// objectiveJSON is how an objective is kept in character_quests.objectives.
type objectiveJSON struct {
	Guid      string  `json:"guid"`
	Record    int64   `json:"record"`
	Active    bool    `json:"active,omitempty"`
	Awake     bool    `json:"awake,omitempty"`
	Hidden    bool    `json:"hidden,omitempty"`
	Completed bool    `json:"completed,omitempty"`
	Succeeded bool    `json:"succeeded,omitempty"`
	Failed    bool    `json:"failed,omitempty"`
	Count     int32   `json:"count,omitempty"`
	Start     float64 `json:"start,omitempty"`
}

// ResolveCharacter finds a character by id, else by name (the name has to name exactly one).
func (s *Service) ResolveCharacter(ctx context.Context, ref string) (string, error) {
	ref = strings.TrimSpace(ref)
	if ref == "" {
		return "", invalid("no character named")
	}
	var id string
	err := s.pool.QueryRow(ctx, `SELECT id FROM characters WHERE id = $1`, ref).Scan(&id)
	if err == nil {
		return id, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return "", err
	}
	rows, err := s.pool.Query(ctx, `SELECT id FROM characters WHERE lower(name) = lower($1) LIMIT 2`, ref)
	if err != nil {
		return "", err
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var one string
		if err := rows.Scan(&one); err != nil {
			return "", err
		}
		ids = append(ids, one)
	}
	if err := rows.Err(); err != nil {
		return "", err
	}
	switch len(ids) {
	case 0:
		return "", ErrNotFound
	case 1:
		return ids[0], nil
	}
	return "", invalid("%q names more than one character: use its id", ref)
}

// LoadLog returns a character's whole log (empty for a character that took nothing).
func (s *Service) LoadLog(ctx context.Context, character string) (*questv1.QuestLog, error) {
	id, err := s.ResolveCharacter(ctx, character)
	if err != nil {
		return nil, err
	}
	log := &questv1.QuestLog{}
	rows, err := s.pool.Query(ctx, `SELECT instance_guid, record_id, status, start_time, source_guid, objectives FROM character_quests WHERE character_id = $1 ORDER BY position`, id)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		q := &questv1.QuestInstance{}
		var status int16
		var raw []byte
		if err := rows.Scan(&q.InstanceGuid, &q.RecordId, &status, &q.StartTime, &q.SourceGuid, &raw); err != nil {
			rows.Close()
			return nil, err
		}
		q.Status = int32(status)
		var objs []objectiveJSON
		if err := json.Unmarshal(raw, &objs); err != nil {
			rows.Close()
			return nil, fmt.Errorf("quests: the objectives of quest %d of %s: %w", q.RecordId, id, err)
		}
		for _, o := range objs {
			q.Objectives = append(q.Objectives, &questv1.ObjectiveState{InstanceGuid: o.Guid, RecordId: o.Record, Active: o.Active, Awake: o.Awake, Hidden: o.Hidden,
				Completed: o.Completed, Succeeded: o.Succeeded, Failed: o.Failed, Count: o.Count, StartTime: o.Start})
		}
		log.Instances = append(log.Instances, q)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	rows, err = s.pool.Query(ctx, `SELECT record_id, completions FROM character_quests_completed WHERE character_id = $1 ORDER BY record_id`, id)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		c := &questv1.CompletedQuest{}
		if err := rows.Scan(&c.RecordId, &c.Count); err != nil {
			rows.Close()
			return nil, err
		}
		log.Completed = append(log.Completed, c)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	rows, err = s.pool.Query(ctx, `SELECT name, value FROM character_quest_variables WHERE character_id = $1 ORDER BY name`, id)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		v := &questv1.PlayerVariable{}
		if err := rows.Scan(&v.Name, &v.Value); err != nil {
			rows.Close()
			return nil, err
		}
		log.Variables = append(log.Variables, v)
	}
	rows.Close()
	return log, rows.Err()
}

// validate checks a log before anything is stored.
func validate(log *questv1.QuestLog) error {
	if len(log.GetInstances()) > MaxInstances {
		return invalid("%d quests in one log, at most %d", len(log.GetInstances()), MaxInstances)
	}
	if len(log.GetCompleted()) > MaxCompleted || len(log.GetVariables()) > MaxVariables {
		return invalid("a log with %d finished quests and %d variables (at most %d and %d)", len(log.GetCompleted()), len(log.GetVariables()), MaxCompleted, MaxVariables)
	}
	guids, records := map[string]bool{}, map[int64]bool{}
	for i, q := range log.GetInstances() {
		if !guidHex.MatchString(q.GetInstanceGuid()) || strings.Trim(q.GetInstanceGuid(), "0") == "" {
			return invalid("quest %d: the instance guid %q is not 32 lower-case hex digits (not all zero)", i, q.GetInstanceGuid())
		}
		if guids[q.GetInstanceGuid()] {
			return invalid("quest %d: the instance guid %s is in the log twice", i, q.GetInstanceGuid())
		}
		guids[q.GetInstanceGuid()] = true
		if q.GetRecordId() <= 0 {
			return invalid("quest %d: the record id %d", i, q.GetRecordId())
		}
		if records[q.GetRecordId()] {
			return invalid("quest %d: record %d is in the log twice", i, q.GetRecordId())
		}
		records[q.GetRecordId()] = true
		if q.GetStatus() < 0 || q.GetStatus() > 3 {
			return invalid("quest %d: the status %d", i, q.GetStatus())
		}
		if q.GetStartTime() < 0 || q.GetSourceGuid() < 0 {
			return invalid("quest %d: a negative start time or source", i)
		}
		if len(q.GetObjectives()) > MaxObjectives {
			return invalid("quest %d: %d objectives, at most %d", i, len(q.GetObjectives()), MaxObjectives)
		}
		for j, o := range q.GetObjectives() {
			if !guidHex.MatchString(o.GetInstanceGuid()) || o.GetRecordId() <= 0 || o.GetCount() < 0 || o.GetStartTime() < 0 {
				return invalid("quest %d objective %d: guid %q, record %d, count %d", i, j, o.GetInstanceGuid(), o.GetRecordId(), o.GetCount())
			}
		}
	}
	done := map[int64]bool{}
	for _, c := range log.GetCompleted() {
		if c.GetRecordId() <= 0 || c.GetCount() <= 0 || done[c.GetRecordId()] {
			return invalid("finished quest %d x %d (once each, a positive count)", c.GetRecordId(), c.GetCount())
		}
		done[c.GetRecordId()] = true
	}
	vars := map[string]bool{}
	for _, v := range log.GetVariables() {
		if v.GetName() == "" || len(v.GetName()) > maxName || vars[v.GetName()] {
			return invalid("the variable %q (non-empty, at most %d bytes, once each)", v.GetName(), maxName)
		}
		vars[v.GetName()] = true
	}
	return nil
}

// SaveLog replaces a character's whole log in one transaction.
func (s *Service) SaveLog(ctx context.Context, character string, log *questv1.QuestLog) error {
	if err := validate(log); err != nil {
		return err
	}
	id, err := s.ResolveCharacter(ctx, character)
	if err != nil {
		return err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx) //nolint:errcheck // a rollback after a commit is a no-op
	// the character row is the lock: a delete of the character waits for this save, and a save for a deleted character finds nothing
	var locked string
	if err := tx.QueryRow(ctx, `SELECT id FROM characters WHERE id = $1 FOR SHARE`, id).Scan(&locked); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return ErrNotFound
		}
		return err
	}
	for _, q := range []string{`DELETE FROM character_quests WHERE character_id = $1`, `DELETE FROM character_quests_completed WHERE character_id = $1`, `DELETE FROM character_quest_variables WHERE character_id = $1`} {
		if _, err := tx.Exec(ctx, q, id); err != nil {
			return err
		}
	}
	for i, q := range log.GetInstances() {
		objs := make([]objectiveJSON, 0, len(q.GetObjectives()))
		for _, o := range q.GetObjectives() {
			objs = append(objs, objectiveJSON{Guid: o.GetInstanceGuid(), Record: o.GetRecordId(), Active: o.GetActive(), Awake: o.GetAwake(), Hidden: o.GetHidden(),
				Completed: o.GetCompleted(), Succeeded: o.GetSucceeded(), Failed: o.GetFailed(), Count: o.GetCount(), Start: o.GetStartTime()})
		}
		raw, err := json.Marshal(objs)
		if err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, `INSERT INTO character_quests (character_id, instance_guid, record_id, status, start_time, source_guid, position, objectives) VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`,
			id, q.GetInstanceGuid(), q.GetRecordId(), int16(q.GetStatus()), q.GetStartTime(), q.GetSourceGuid(), i, raw); err != nil {
			return err
		}
	}
	for _, c := range log.GetCompleted() {
		if _, err := tx.Exec(ctx, `INSERT INTO character_quests_completed (character_id, record_id, completions) VALUES ($1, $2, $3)`, id, c.GetRecordId(), c.GetCount()); err != nil {
			return err
		}
	}
	for _, v := range log.GetVariables() {
		if _, err := tx.Exec(ctx, `INSERT INTO character_quest_variables (character_id, name, value) VALUES ($1, $2, $3)`, id, v.GetName(), v.GetValue()); err != nil {
			return err
		}
	}
	return tx.Commit(ctx)
}

// QueueCommand puts a GM command in a character's inbox and returns its id.
func (s *Service) QueueCommand(ctx context.Context, req *questv1.QueueCommandRequest) (int64, error) {
	kind := req.GetKind()
	switch kind {
	case "give", "finish", "fail", "abandon":
		if strings.TrimSpace(req.GetQuest()) == "" {
			return 0, invalid("%q needs a quest", kind)
		}
	case "clear":
	case "set-var":
		if req.GetName() == "" || len(req.GetName()) > maxName {
			return 0, invalid("set-var needs a variable name of at most %d bytes", maxName)
		}
	default:
		return 0, invalid("the command %q (give, finish, fail, abandon, clear or set-var)", kind)
	}
	id, err := s.ResolveCharacter(ctx, req.GetCharacterId())
	if err != nil {
		return 0, err
	}
	var queued int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_quest_commands WHERE character_id = $1`, id).Scan(&queued); err != nil {
		return 0, err
	}
	if queued >= MaxCommands {
		return 0, invalid("%d commands wait for this character already", queued)
	}
	var cmd int64
	err = s.pool.QueryRow(ctx, `INSERT INTO character_quest_commands (character_id, kind, quest, name, value) VALUES ($1, $2, $3, $4, $5) RETURNING id`,
		id, kind, strings.TrimSpace(req.GetQuest()), req.GetName(), req.GetValue()).Scan(&cmd)
	return cmd, err
}

// TakeCommands returns a character's queued commands, oldest first, and removes them.
func (s *Service) TakeCommands(ctx context.Context, character string) ([]*questv1.Command, error) {
	id, err := s.ResolveCharacter(ctx, character)
	if err != nil {
		return nil, err
	}
	rows, err := s.pool.Query(ctx, `DELETE FROM character_quest_commands WHERE id IN (SELECT id FROM character_quest_commands WHERE character_id = $1 ORDER BY id LIMIT 64 FOR UPDATE SKIP LOCKED)
		RETURNING id, kind, quest, name, value`, id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []*questv1.Command
	for rows.Next() {
		c := &questv1.Command{}
		if err := rows.Scan(&c.Id, &c.Kind, &c.Quest, &c.Name, &c.Value); err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	// DELETE .. RETURNING comes in no particular order
	for i := 1; i < len(out); i++ {
		for j := i; j > 0 && out[j].Id < out[j-1].Id; j-- {
			out[j], out[j-1] = out[j-1], out[j]
		}
	}
	return out, nil
}

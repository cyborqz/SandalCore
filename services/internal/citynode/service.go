// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package citynode is the city node service's logic (Nodes 1, stage 1): the live state of the game world's city nodes (their level,
// XP and vassal link), the node clock, the seed a fresh server starts with, and the stream of changes. It is the model behind
// citynodegrpc's CityNodeService; the in-game GM commands, the nodectl tool and the admin app to come all call the same methods.
//
// State lives in PostgreSQL (migration 0009: city_node_state, city_node_clock, city_node_seed). One citynode process owns the
// tables: it serialises every change with its commit and its event, so WatchNodes sees the changes in the order they happened.
package citynode

import (
	"context"
	"errors"
	"fmt"
	"io/fs"
	"log/slog"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var (
	// ErrNotFound is a node (or a seed file) that is not there.
	ErrNotFound = errors.New("not found")
	// ErrInvalid is input the service refuses: a level out of bounds, an XP total below zero, a clock speed it does not allow ...
	ErrInvalid = errors.New("invalid")
)

// Node is a city node's live state.
type Node struct {
	Guid        int64
	Name        string // the CityNodeRecordBase name ("Verra_RVR_Joeva")
	DisplayName string // "Joeva"
	Level       int    // ENodeLevel, 0..MaxNodeLevel
	Experience  int64  // the node's XP in total (CityNodeExpToLevel's scale); nothing levels the node from it in Nodes 1
	Percent     int    // 0-100, how far the XP is through the node's level (Progress)
	ToNext      int64  // the XP still missing for the next level (0 at the highest level)
	ParentGuid  int64  // the lord this node is a vassal of; 0 for none
	UpdatedAt   time.Time
}

// Cause says what changed a node.
type Cause int

const (
	CauseSetLevel Cause = iota + 1
	CauseAddExperience
	CauseSeed
)

func (c Cause) String() string {
	switch c {
	case CauseSetLevel:
		return "set-level"
	case CauseAddExperience:
		return "add-experience"
	case CauseSeed:
		return "seed"
	}
	return "unknown"
}

// C-Y-B and C O A

// NodeChange is one change of one node, as WatchNodes carries it.
type NodeChange struct {
	Node               Node
	PreviousLevel      int
	Cause              Cause
	VassalshipDisabled bool // SetNodeLevel's disable_vassalship flag (Nodes 1 has no vassal rules: it only travels with the change)
	PreviousParentGuid int64
	PreviousExperience int64
}

// Event is one entry of the change stream. Seq counts the changes since the service started, from 1, with no gap.
type Event struct {
	Seq   uint64
	Node  *NodeChange // a node changed, or
	Clock *Clock      // the node clock changed
}

// Snapshot is the whole state at one moment: Seq is the last change it includes.
type Snapshot struct {
	Seq   uint64
	Nodes []Node // every node, in guid order
	Clock Clock
}

// Service is the city node service.
type Service struct {
	pool     *pgxpool.Pool
	roster   *Roster
	seedPath string
	now      func() time.Time

	mu  sync.RWMutex // a change holds it for write from its transaction to its event; a read holds it for read
	seq uint64
	hub hub
}

// Option changes how New makes a Service.
type Option func(*Service)

// WithClock makes the service read the real time from now (the tests'); the default is time.Now.
func WithClock(now func() time.Time) Option { return func(s *Service) { s.now = now } }

// New makes a service over pool for the nodes of roster. seedPath is the seed file ApplySeed reads when it is not handed one
// (deploy/citynode_seed.json). Start must run before anything else.
func New(pool *pgxpool.Pool, roster *Roster, seedPath string, opts ...Option) *Service {
	s := &Service{pool: pool, roster: roster, seedPath: seedPath, now: time.Now}
	for _, o := range opts {
		o(s)
	}
	return s
}

// Roster is the nodes the service knows.
func (s *Service) Roster() *Roster { return s.roster }

// Start makes sure every node of the roster has its row (Wilderness, no parent, no XP) and applies the seed if it has not been
// applied yet (city_node_seed.applied_at null), in one transaction: a seed file that is missing or wrong stops the start, and
// nothing is applied. A seed applied before is left alone, and said so.
func (s *Service) Start(ctx context.Context) error {
	if err := s.ensureRows(ctx); err != nil {
		return fmt.Errorf("citynode: the node rows: %w", err)
	}
	// the seed at the start is the starting state, not a change: it is not published and does not count in the sequence
	res, err := s.applySeed(ctx, false, nil, false)
	if err != nil {
		return fmt.Errorf("citynode: the seed: %w", err)
	}
	if res.Applied {
		slog.Info("citynode: the seed was applied for the first time", "file", s.seedPath, "nodes_changed", res.Changed)
	} else {
		slog.Info("citynode: the seed was applied before; edits to the file need nodectl seed --force", "file", s.seedPath, "applied_at", res.AppliedAt.Format(time.RFC3339))
	}
	return nil
}

func (s *Service) ensureRows(ctx context.Context) error {
	guids := make([]int64, 0, s.roster.Len())
	for _, n := range s.roster.Nodes() {
		guids = append(guids, n.Guid)
	}
	_, err := s.pool.Exec(ctx, `INSERT INTO city_node_state (node_guid, level, experience, updated_at)
		SELECT g, 0, 0, $2 FROM unnest($1::bigint[]) AS g ON CONFLICT (node_guid) DO NOTHING`, guids, s.now())
	return err
}

// row is a city_node_state row.
type row struct {
	level      int
	experience int64
	parent     int64
	updatedAt  time.Time
}

type queryer interface {
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
}

// loadRows reads the table. A roster node with no row (the service was not started) reads as Wilderness.
func (s *Service) loadRows(ctx context.Context, q queryer, lock string) (map[int64]row, error) {
	rs, err := q.Query(ctx, `SELECT node_guid, level, experience, COALESCE(parent_guid, 0), updated_at FROM city_node_state ORDER BY node_guid`+lock)
	if err != nil {
		return nil, err
	}
	defer rs.Close()
	out := map[int64]row{}
	for rs.Next() {
		var g int64
		var r row
		if err := rs.Scan(&g, &r.level, &r.experience, &r.parent, &r.updatedAt); err != nil {
			return nil, err
		}
		out[g] = r
	}
	return out, rs.Err()
}

func (s *Service) toNode(info Info, r row) Node {
	pct, next := Progress(r.level, r.experience)
	return Node{Guid: info.Guid, Name: info.Name, DisplayName: info.DisplayName, Level: r.level, Experience: r.experience,
		Percent: pct, ToNext: next, ParentGuid: r.parent, UpdatedAt: r.updatedAt}
}

func (s *Service) nodesFrom(rows map[int64]row) []Node {
	out := make([]Node, 0, s.roster.Len())
	for _, info := range s.roster.Nodes() {
		out = append(out, s.toNode(info, rows[info.Guid]))
	}
	return out
}

// ListNodes is every node, in guid order, with the clock and the sequence number of the last change.
func (s *Service) ListNodes(ctx context.Context) (Snapshot, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.snapshotLocked(ctx)
}

// C O A.Cyb

func (s *Service) snapshotLocked(ctx context.Context) (Snapshot, error) {
	rows, err := s.loadRows(ctx, s.pool, "")
	if err != nil {
		return Snapshot{}, err
	}
	clock, err := s.readClock(ctx, s.pool, "")
	if err != nil {
		return Snapshot{}, err
	}
	return Snapshot{Seq: s.seq, Nodes: s.nodesFrom(rows), Clock: clock.at(s.now())}, nil
}

// GetNode is one node, found by its guid (when not 0) or by its name.
func (s *Service) GetNode(ctx context.Context, guid int64, name string) (Node, error) {
	info, err := s.roster.Resolve(guid, name)
	if err != nil {
		return Node{}, err
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.getNode(ctx, s.pool, info)
}

func (s *Service) getNode(ctx context.Context, q queryer, info Info) (Node, error) {
	var r row
	err := q.QueryRow(ctx, `SELECT level, experience, COALESCE(parent_guid, 0), updated_at FROM city_node_state WHERE node_guid = $1`, info.Guid).
		Scan(&r.level, &r.experience, &r.parent, &r.updatedAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return s.toNode(info, row{}), nil
	}
	if err != nil {
		return Node{}, err
	}
	return s.toNode(info, r), nil
}

// SetNodeLevel sets a node's level, 0 to MaxNodeLevel. It touches nothing else: not the XP, not the vassal link.
// disableVassalship is the client's GM flag bDisableVassalshipSystem; Nodes 1 has no vassal rules to switch off, so it only
// travels with the change. Setting the level a node already has changes nothing and sends no event (changed is false).
func (s *Service) SetNodeLevel(ctx context.Context, guid int64, name string, level int, disableVassalship bool) (n Node, changed bool, err error) {
	info, err := s.roster.Resolve(guid, name)
	if err != nil {
		return Node{}, false, err
	}
	if err := ValidateLevel(level); err != nil {
		return Node{}, false, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Node{}, false, err
	}
	defer tx.Rollback(ctx)
	old, err := s.lockRow(ctx, tx, info)
	if err != nil {
		return Node{}, false, err
	}
	if old.level == level {
		return s.toNode(info, old), false, nil
	}
	now := s.now()
	if _, err := tx.Exec(ctx, `UPDATE city_node_state SET level = $2, updated_at = $3 WHERE node_guid = $1`, info.Guid, level, now); err != nil {
		return Node{}, false, err
	}
	if err := tx.Commit(ctx); err != nil {
		return Node{}, false, err
	}
	nw := old
	nw.level, nw.updatedAt = level, now
	n = s.toNode(info, nw)
	s.publishNode(NodeChange{Node: n, PreviousLevel: old.level, Cause: CauseSetLevel, VassalshipDisabled: disableVassalship,
		PreviousParentGuid: old.parent, PreviousExperience: old.experience})
	return n, true, nil
}

// (C-Y-B | CoA)

// lockRow reads a node's row and holds it for the rest of the transaction. A roster node with no row is made one first.
func (s *Service) lockRow(ctx context.Context, tx pgx.Tx, info Info) (row, error) {
	if _, err := tx.Exec(ctx, `INSERT INTO city_node_state (node_guid, level, experience, updated_at) VALUES ($1, 0, 0, $2) ON CONFLICT (node_guid) DO NOTHING`, info.Guid, s.now()); err != nil {
		return row{}, err
	}
	var r row
	err := tx.QueryRow(ctx, `SELECT level, experience, COALESCE(parent_guid, 0), updated_at FROM city_node_state WHERE node_guid = $1 FOR UPDATE`, info.Guid).
		Scan(&r.level, &r.experience, &r.parent, &r.updatedAt)
	return r, err
}

// AddNodeExperience adds amount to a node's XP (negative takes XP away, down to 0 at the lowest: a total below 0 is refused).
// It stores the XP and moves the progress toward the next threshold, and does NOT change the level: Nodes 1 has no level-up
// from XP (SetNodeLevel is the only way a level changes).
func (s *Service) AddNodeExperience(ctx context.Context, guid int64, name string, amount int64) (Node, error) {
	info, err := s.roster.Resolve(guid, name)
	if err != nil {
		return Node{}, err
	}
	if amount == 0 {
		return Node{}, fmt.Errorf("%w: an amount of 0 adds nothing", ErrInvalid)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Node{}, err
	}
	defer tx.Rollback(ctx)
	old, err := s.lockRow(ctx, tx, info)
	if err != nil {
		return Node{}, err
	}
	total := old.experience + amount
	if (amount > 0 && total < old.experience) || total < 0 {
		return Node{}, fmt.Errorf("%w: %s holds %d XP; adding %d would leave %s", ErrInvalid, info.Name, old.experience, amount, outOfRange(amount))
	}
	now := s.now()
	if _, err := tx.Exec(ctx, `UPDATE city_node_state SET experience = $2, updated_at = $3 WHERE node_guid = $1`, info.Guid, total, now); err != nil {
		return Node{}, err
	}
	if err := tx.Commit(ctx); err != nil {
		return Node{}, err
	}
	nw := old
	nw.experience, nw.updatedAt = total, now
	n := s.toNode(info, nw)
	s.publishNode(NodeChange{Node: n, PreviousLevel: old.level, Cause: CauseAddExperience, PreviousParentGuid: old.parent, PreviousExperience: old.experience})
	return n, nil
}

func outOfRange(amount int64) string {
	if amount < 0 {
		return "less than 0"
	}
	return "more than the largest XP total"
}

// SeedResult is what ApplySeed did.
type SeedResult struct {
	Applied   bool      // the seed was applied now
	Forced    bool      // force was asked for (it overwrites levels and parents even when the seed was applied before)
	Changed   int       // nodes whose level or parent the seed changed
	AppliedAt time.Time // when the seed was applied (now, or before when Applied is false)
}

// ApplySeed applies a seed, in one transaction. Without force it does so only when city_node_seed.applied_at is null (a seed
// that was applied before is left alone, and Applied is false). With force it applies the seed again: EVERY node's level and
// parent are overwritten (a node the seed leaves out becomes Wilderness with no parent); the XP is left as it is. seedJSON is
// the seed's text; nil reads the service's seed file (deploy/citynode_seed.json). A seed that cannot be read or is wrong changes
// nothing.
func (s *Service) ApplySeed(ctx context.Context, force bool, seedJSON []byte) (SeedResult, error) {
	return s.applySeed(ctx, force, seedJSON, true)
}

// applySeed is ApplySeed; publish says whether the node changes it makes are sent to the watchers (not at the service's start).
func (s *Service) applySeed(ctx context.Context, force bool, seedJSON []byte, publish bool) (SeedResult, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return SeedResult{}, err
	}
	defer tx.Rollback(ctx)
	var appliedAt *time.Time
	if err := tx.QueryRow(ctx, `SELECT applied_at FROM city_node_seed WHERE id FOR UPDATE`).Scan(&appliedAt); err != nil {
		return SeedResult{}, err
	}
	if appliedAt != nil && !force {
		return SeedResult{AppliedAt: *appliedAt}, nil
	}
	var seed *Seed
	if seedJSON != nil {
		seed, err = ParseSeed(seedJSON, s.roster)
	} else {
		seed, err = LoadSeed(s.seedPath, s.roster)
		if errors.Is(err, fs.ErrNotExist) {
			err = fmt.Errorf("%w: %v", ErrNotFound, err)
		}
	}
	if err != nil {
		return SeedResult{}, err
	}
	// make sure every node has a row (a roster that grew since the service last started)
	for _, info := range s.roster.Nodes() {
		if _, err := tx.Exec(ctx, `INSERT INTO city_node_state (node_guid, level, experience, updated_at) VALUES ($1, 0, 0, $2) ON CONFLICT (node_guid) DO NOTHING`, info.Guid, s.now()); err != nil {
			return SeedResult{}, err
		}
	}
	rows, err := s.loadRows(ctx, tx, " FOR UPDATE")
	if err != nil {
		return SeedResult{}, err
	}
	now := s.now()
	var changes []NodeChange
	for _, info := range s.roster.Nodes() {
		old := rows[info.Guid]
		want := row{level: seed.Levels[info.Guid], experience: old.experience, parent: seed.Parents[info.Guid]}
		if old.level == want.level && old.parent == want.parent {
			continue
		}
		var parent any // NULL for no lord
		if want.parent != 0 {
			parent = want.parent
		}
		if _, err := tx.Exec(ctx, `UPDATE city_node_state SET level = $2, parent_guid = $3, updated_at = $4 WHERE node_guid = $1`, info.Guid, want.level, parent, now); err != nil {
			return SeedResult{}, err
		}
		want.updatedAt = now
		changes = append(changes, NodeChange{Node: s.toNode(info, want), PreviousLevel: old.level, Cause: CauseSeed,
			PreviousParentGuid: old.parent, PreviousExperience: old.experience})
	}
	if _, err := tx.Exec(ctx, `UPDATE city_node_seed SET applied_at = $1 WHERE id`, now); err != nil {
		return SeedResult{}, err
	}
	if err := tx.Commit(ctx); err != nil {
		return SeedResult{}, err
	}
	if publish {
		for _, c := range changes {
			s.publishNode(c)
		}
	}
	if force {
		slog.Warn("citynode: the seed was applied AGAIN (force): every node's level and parent were overwritten", "nodes_changed", len(changes))
	}
	return SeedResult{Applied: true, Forced: force, Changed: len(changes), AppliedAt: now}, nil
}

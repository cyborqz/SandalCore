// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynode

import (
	"context"
	"fmt"
	"log/slog"
	"math"
	"time"
)

// Node leveling (Nodes 2): a node levels up from its XP, and loses XP, and levels, when nobody feeds it.
//
//	the level-up   once a node's XP reaches the next level's threshold (CityNodeExpToLevel) and nothing holds it back, a countdown starts
//	               (Leveling.Countdown: the 5 s of the client's Toast_NodeEvent_Countdown_LevelUp, which worldd shows the players in the node's zone);
//	               when it ends the node is a level up, if it still may be.  One level at a time: a node with XP for two goes up twice.
//	the checks     a node above MaxNodeLevel (Town) does not go up; a vassal goes up only while its lord's VassalshipDef allows another vassal of
//	               the new level (the data's Max_Rules: a Village lord one Encampment, a Town lord two Encampments and two Villages; none of its
//	               own level).  A node held back keeps its XP up to the next threshold and no further.
//	the atrophy    every AtrophyInterval a node loses AtrophyPerHour of its level's span (the XP from its threshold to the next) for each hour;
//	               a node that falls more than DelevelGrace of the span below its threshold loses the level (FULL atrophy).  At MaxNodeLevel the
//	               XP may run one span past the threshold, a store against the atrophy.
//
// Modelled: the retail server's rules are not in the client; the numbers are the operator's (config/rates.json).  A level set by a game
// master or the seed brings the XP to that level's threshold, so the atrophy and the level-up start from the level.

// Leveling is how nodes level and atrophy.  The zero value levels nothing up and has no atrophy.
type Leveling struct {
	Enabled        bool          // nodes level up from their XP
	Countdown      time.Duration // between reaching the threshold and the new level
	AtrophyPerHour float64       // the part of the level's span a node loses each hour (0: none)
	DelevelGrace   float64       // how far below its threshold, as a part of the span below, a node may fall before it loses the level (0..1)
}

// DefaultLeveling is the leveling citynode runs with when its flags do not say otherwise.
var DefaultLeveling = Leveling{Enabled: true, Countdown: 5 * time.Second, AtrophyPerHour: 0.01, DelevelGrace: 0.25}

// AtrophyInterval is how often the atrophy runs.
const AtrophyInterval = time.Minute

// WithLeveling sets the service's leveling (the default is none: the Nodes 1 behaviour).
func WithLeveling(l Leveling) Option { return func(s *Service) { s.lev = l } }

// childRules are the VassalshipDef's ChildNodeRules (Max_Rules, content.db child_node_rule): {lord level, vassal level} -> the most vassals
// of that level the lord may have.
var childRules = map[[2]int]int{{3, 2}: 1, {4, 2}: 2, {4, 3}: 2, {5, 2}: 4, {5, 3}: 4, {5, 4}: 2, {6, 3}: 8, {6, 4}: 4, {6, 5}: 2}

// Span is the XP from a level's threshold to the next (at MaxNodeLevel and above: the span that reached it).
func Span(level int) int64 {
	if level < 0 {
		return 0
	}
	if level >= MaxNodeLevel {
		return ExpToLevel[MaxNodeLevel] - ExpToLevel[MaxNodeLevel-1]
	}
	return ExpToLevel[level+1] - ExpToLevel[level]
}

// threshold is the XP a node holds when it reaches level.
func threshold(level int) int64 {
	if level <= 0 {
		return 0
	}
	if level >= len(ExpToLevel) {
		return ExpToLevel[len(ExpToLevel)-1]
	}
	return ExpToLevel[level]
}

// xpCap is the most XP a node of row r may hold: one span past the threshold at MaxNodeLevel, the next threshold while it is held back.
func xpCap(level int, held bool) int64 {
	if level >= MaxNodeLevel {
		return threshold(MaxNodeLevel) + Span(MaxNodeLevel)
	}
	if held {
		return threshold(level + 1)
	}
	return math.MaxInt64
}

// heldBack says why the node of row r may not go a level up now ("" when it may).
func (s *Service) heldBack(ctx context.Context, q queryer, guid int64, r row) (string, error) {
	if r.level >= MaxNodeLevel {
		return fmt.Sprintf("it is %s, the highest level", LevelName(r.level)), nil
	}
	if r.parent == 0 {
		return "", nil
	}
	var lordLevel int
	if err := q.QueryRow(ctx, `SELECT level FROM city_node_state WHERE node_guid = $1`, r.parent).Scan(&lordLevel); err != nil {
		return "", err
	}
	next := r.level + 1
	allowed := childRules[[2]int{lordLevel, next}]
	rs, err := q.Query(ctx, `SELECT node_guid FROM city_node_state WHERE parent_guid = $1 AND level = $2 AND node_guid <> $3`, r.parent, next, guid)
	if err != nil {
		return "", err
	}
	defer rs.Close()
	have := 0
	for rs.Next() {
		have++
	}
	if err := rs.Err(); err != nil {
		return "", err
	}
	if have >= allowed {
		lord := fmt.Sprint(r.parent)
		if info, err := s.roster.Resolve(r.parent, ""); err == nil {
			lord = info.DisplayName
		}
		return fmt.Sprintf("its lord %s (%s) may have %d %s vassal(s) and has %d", lord, LevelName(lordLevel), allowed, LevelName(next), have), nil
	}
	return "", nil
}

// considerLevelUpLocked starts the countdown of a node that reached its next threshold and may go up (s.mu held for write).
func (s *Service) considerLevelUpLocked(ctx context.Context, info Info, r row) error {
	if !s.lev.Enabled || r.level >= MaxNodeLevel || r.experience < threshold(r.level+1) {
		return nil
	}
	if _, waiting := s.pending[info.Guid]; waiting {
		return nil
	}
	why, err := s.heldBack(ctx, s.pool, info.Guid, r)
	if err != nil {
		return err
	}
	if why != "" {
		key := [2]int64{info.Guid, int64(r.level)}
		if !s.heldLogged[key] {
			s.heldLogged[key] = true
			slog.Info("citynode: a node has the XP for the next level and is held back", "node", info.Name, "level", LevelName(r.level), "why", why)
		}
		return nil
	}
	at := s.now().Add(s.lev.Countdown)
	s.pending[info.Guid] = at
	n := s.toNode(info, r)
	slog.Info("citynode: a node levels up", "node", info.Name, "to", LevelName(r.level+1), "at", at.Format(time.RFC3339))
	s.publishNode(NodeChange{Node: n, PreviousLevel: r.level, Cause: CauseLevelUpPending, PreviousParentGuid: r.parent, PreviousExperience: r.experience})
	return nil
}

// Tick runs what the leveling has due: the countdowns that ended, and the atrophy every AtrophyInterval.  citynode calls it every second.
func (s *Service) Tick(ctx context.Context) error {
	now := s.now()
	s.mu.Lock()
	defer s.mu.Unlock()
	for guid, at := range s.pending {
		if now.Before(at) {
			continue
		}
		delete(s.pending, guid)
		if err := s.levelUpLocked(ctx, guid); err != nil {
			return err
		}
	}
	if s.lev.AtrophyPerHour <= 0 {
		return nil
	}
	if s.lastAtrophy.IsZero() {
		s.lastAtrophy = now
		return nil
	}
	if now.Sub(s.lastAtrophy) < AtrophyInterval {
		return nil
	}
	hours := now.Sub(s.lastAtrophy).Hours()
	s.lastAtrophy = now
	return s.atrophyLocked(ctx, hours)
}

func (s *Service) levelUpLocked(ctx context.Context, guid int64) error {
	info, err := s.roster.Resolve(guid, "")
	if err != nil {
		return nil
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	old, err := s.lockRow(ctx, tx, info)
	if err != nil {
		return err
	}
	if old.level >= MaxNodeLevel || old.experience < threshold(old.level+1) {
		slog.Info("citynode: a level-up is called off: the node lost the XP for it", "node", info.Name, "level", LevelName(old.level))
		return nil
	}
	why, err := s.heldBack(ctx, tx, guid, old)
	if err != nil {
		return err
	}
	if why != "" {
		slog.Info("citynode: a level-up is called off", "node", info.Name, "why", why)
		return nil
	}
	now := s.now()
	if _, err := tx.Exec(ctx, `UPDATE city_node_state SET level = $2, updated_at = $3 WHERE node_guid = $1`, guid, old.level+1, now); err != nil {
		return err
	}
	if err := tx.Commit(ctx); err != nil {
		return err
	}
	nw := old
	nw.level, nw.updatedAt = old.level+1, now
	slog.Info("citynode: a node levelled up", "node", info.Name, "level", LevelName(nw.level), "experience", nw.experience)
	s.publishNode(NodeChange{Node: s.toNode(info, nw), PreviousLevel: old.level, Cause: CauseLevelUp, PreviousParentGuid: old.parent, PreviousExperience: old.experience})
	return s.considerLevelUpLocked(ctx, info, nw)
}

func (s *Service) atrophyLocked(ctx context.Context, hours float64) error {
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	rows, err := s.loadRows(ctx, tx, " FOR UPDATE")
	if err != nil {
		return err
	}
	now := s.now()
	type change struct {
		info    Info
		old, nw row
	}
	var changes []change
	for _, info := range s.roster.Nodes() {
		old, ok := rows[info.Guid]
		if !ok || old.experience == 0 {
			continue
		}
		loss := int64(math.Round(s.lev.AtrophyPerHour * float64(Span(old.level)) * hours))
		if loss <= 0 {
			continue
		}
		nw := old
		nw.experience = max(0, old.experience-loss)
		if old.level > 0 && nw.experience < threshold(old.level)-int64(math.Round(s.lev.DelevelGrace*float64(Span(old.level-1)))) {
			nw.level = old.level - 1
		}
		nw.updatedAt = now
		if _, err := tx.Exec(ctx, `UPDATE city_node_state SET level = $2, experience = $3, updated_at = $4 WHERE node_guid = $1`, info.Guid, nw.level, nw.experience, now); err != nil {
			return err
		}
		changes = append(changes, change{info, old, nw})
	}
	if err := tx.Commit(ctx); err != nil {
		return err
	}
	for _, c := range changes {
		if c.nw.level != c.old.level {
			delete(s.pending, c.info.Guid)
			slog.Info("citynode: a node lost a level to atrophy", "node", c.info.Name, "level", LevelName(c.nw.level), "experience", c.nw.experience)
			s.publishNode(NodeChange{Node: s.toNode(c.info, c.nw), PreviousLevel: c.old.level, Cause: CauseLevelDown, PreviousParentGuid: c.old.parent,
				PreviousExperience: c.old.experience})
			continue
		}
		oldPct, _ := Progress(c.old.level, c.old.experience)
		newPct, _ := Progress(c.nw.level, c.nw.experience)
		if oldPct != newPct { // (the XP the client sees is the percent: a change it cannot see is not sent)
			s.publishNode(NodeChange{Node: s.toNode(c.info, c.nw), PreviousLevel: c.old.level, Cause: CauseAtrophy, PreviousParentGuid: c.old.parent,
				PreviousExperience: c.old.experience})
		}
	}
	return nil
}

// RunLeveling calls Tick every second until ctx ends.
func (s *Service) RunLeveling(ctx context.Context) {
	t := time.NewTicker(time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if err := s.Tick(ctx); err != nil && ctx.Err() == nil {
				slog.Warn("citynode: the leveling tick failed", "err", err)
			}
		}
	}
}

// normalizeLocked brings every node's XP to at least its level's threshold (the seed and the levels a game master set have none): the
// atrophy and the level-up then start from the level.  It is the start's and the seed's, not a change: nothing is published.
func (s *Service) normalize(ctx context.Context) (int64, error) {
	if !s.lev.Enabled && s.lev.AtrophyPerHour <= 0 {
		return 0, nil
	}
	var n int64
	for level := 1; level <= MaxNodeLevel; level++ {
		tag, err := s.pool.Exec(ctx, `UPDATE city_node_state SET experience = $2 WHERE level = $1 AND experience < $2`, level, threshold(level))
		if err != nil {
			return n, err
		}
		n += tag.RowsAffected()
	}
	return n, nil
}

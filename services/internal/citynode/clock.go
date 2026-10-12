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

// The node clock. Node time (what every node timer, once there are timers, reads) is
//
//	node time = real time x speed + offset
//
// with real time in unix seconds. Speed 1 and offset 0 are real time (decision D7); a GM or an admin skips ahead with
// AdvanceClock (the offset grows) and speeds a test server up with SetClockSpeed. The row is city_node_clock.

// Bounds of what the clock accepts.
const (
	MinClockSpeed = 0.001
	MaxClockSpeed = 100000
	// MaxAdvanceSeconds is the most one AdvanceClock moves the clock: 100 years.
	MaxAdvanceSeconds = 100 * 366 * 24 * 3600
)

// Clock is the node clock at one moment.
type Clock struct {
	Speed         float64
	OffsetSeconds int64
	RealUnixMs    int64 // the real time the clock was read at
	NodeUnixMs    int64 // node time then: real time x speed + offset
}

// clockRow is city_node_clock's one row.
type clockRow struct {
	speed  float64
	offset int64
}

func (c clockRow) at(real time.Time) Clock {
	ms := real.UnixMilli()
	return Clock{Speed: c.speed, OffsetSeconds: c.offset, RealUnixMs: ms, NodeUnixMs: int64(math.Round(float64(ms)*c.speed)) + c.offset*1000}
}

func (s *Service) readClock(ctx context.Context, q queryer, lock string) (clockRow, error) {
	var c clockRow
	err := q.QueryRow(ctx, `SELECT speed, offset_seconds FROM city_node_clock WHERE id`+lock).Scan(&c.speed, &c.offset)
	return c, err
}

// GetClock is the node clock now.
func (s *Service) GetClock(ctx context.Context) (Clock, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	c, err := s.readClock(ctx, s.pool, "")
	if err != nil {
		return Clock{}, err
	}
	return c.at(s.now()), nil
}

// AdvanceClock moves the node clock forward by seconds (1 to MaxAdvanceSeconds): a GM's skip. The speed stays.
func (s *Service) AdvanceClock(ctx context.Context, seconds int64) (Clock, error) {
	if seconds <= 0 || seconds > MaxAdvanceSeconds {
		return Clock{}, fmt.Errorf("%w: the clock moves forward by 1 to %d seconds, not %d", ErrInvalid, int64(MaxAdvanceSeconds), seconds)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Clock{}, err
	}
	defer tx.Rollback(ctx)
	c, err := s.readClock(ctx, tx, " FOR UPDATE")
	if err != nil {
		return Clock{}, err
	}
	c.offset += seconds
	if _, err := tx.Exec(ctx, `UPDATE city_node_clock SET offset_seconds = $1 WHERE id`, c.offset); err != nil {
		return Clock{}, err
	}
	if err := tx.Commit(ctx); err != nil {
		return Clock{}, err
	}
	out := c.at(s.now())
	s.publishClock(out)
	slog.Info("citynode: the node clock was advanced", "seconds", seconds, "speed", c.speed)
	return out, nil
}

// -- C:Y:B & C | O | A --

// SetClockSpeed sets the speed knob (MinClockSpeed to MaxClockSpeed; 1 is real time). Node time does not jump: the offset is
// recomputed (to the second) so the clock reads what it read a moment before.
func (s *Service) SetClockSpeed(ctx context.Context, factor float64) (Clock, error) {
	if math.IsNaN(factor) || math.IsInf(factor, 0) || factor < MinClockSpeed || factor > MaxClockSpeed {
		return Clock{}, fmt.Errorf("%w: the clock speed is %g to %g, not %g", ErrInvalid, MinClockSpeed, float64(MaxClockSpeed), factor)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Clock{}, err
	}
	defer tx.Rollback(ctx)
	c, err := s.readClock(ctx, tx, " FOR UPDATE")
	if err != nil {
		return Clock{}, err
	}
	realSec := float64(s.now().UnixMilli()) / 1000
	nodeNow := realSec*c.speed + float64(c.offset)
	c.offset = int64(math.Round(nodeNow - realSec*factor))
	c.speed = factor
	if _, err := tx.Exec(ctx, `UPDATE city_node_clock SET speed = $1, offset_seconds = $2 WHERE id`, c.speed, c.offset); err != nil {
		return Clock{}, err
	}
	if err := tx.Commit(ctx); err != nil {
		return Clock{}, err
	}
	out := c.at(s.now())
	s.publishClock(out)
	slog.Info("citynode: the node clock speed was set", "speed", factor)
	return out, nil
}

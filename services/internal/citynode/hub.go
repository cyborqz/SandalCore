// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynode

import (
	"context"
	"errors"
	"sync"
)

// The change stream. Every change is published while the service still holds its write lock, so the order of the events is the
// order of the commits, and a watcher that registers under the read lock gets a snapshot with no change missing or repeated.

var (
	// ErrLagged ends a Watch whose reader fell too far behind: the reader asks for a new one and gets the whole state again.
	ErrLagged = errors.New("the watcher fell behind the changes")
	// ErrShutdown ends every Watch when the service shuts down.
	ErrShutdown = errors.New("the node service is shutting down")
)

// watchBuffer is how many events a watcher may be behind before it is cut off.
var watchBuffer = 1024

// Watch is a subscription to the changes after a snapshot.
type Watch struct {
	// Snapshot is the state the changes follow: the first event has Seq Snapshot.Seq+1.
	Snapshot Snapshot

	ch  chan Event
	h   *hub
	mu  sync.Mutex
	err error
}

// Events is closed when the watch ends; Err says why.
func (w *Watch) Events() <-chan Event { return w.ch }

// Err is why Events was closed: ErrLagged, ErrShutdown, or nil after Close.
func (w *Watch) Err() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.err
}

// Close ends the watch.
func (w *Watch) Close() { w.h.drop(w, nil) }

type hub struct {
	mu     sync.Mutex
	subs   map[*Watch]struct{}
	closed bool
}

func (h *hub) add(w *Watch) error {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.closed {
		return ErrShutdown
	}
	if h.subs == nil {
		h.subs = map[*Watch]struct{}{}
	}
	h.subs[w] = struct{}{}
	return nil
}

// drop ends w with err (nil for the reader's own Close), once.
func (h *hub) drop(w *Watch, err error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.dropLocked(w, err)
}

// C:Y:B + C O A

func (h *hub) dropLocked(w *Watch, err error) {
	if _, ok := h.subs[w]; !ok {
		return
	}
	delete(h.subs, w)
	w.mu.Lock()
	w.err = err
	w.mu.Unlock()
	close(w.ch)
}

// publish hands ev to every watcher; one whose buffer is full is cut off, never waited for.
func (h *hub) publish(ev Event) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for w := range h.subs {
		select {
		case w.ch <- ev:
		default:
			h.dropLocked(w, ErrLagged)
		}
	}
}

// shutdown ends every watch and refuses new ones.
func (h *hub) shutdown() {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.closed = true
	for w := range h.subs {
		h.dropLocked(w, ErrShutdown)
	}
}

// publishNode and publishClock number a change and send it. The caller holds s.mu for write.
func (s *Service) publishNode(c NodeChange) {
	s.seq++
	s.hub.publish(Event{Seq: s.seq, Node: &c})
}

func (s *Service) publishClock(c Clock) {
	s.seq++
	s.hub.publish(Event{Seq: s.seq, Clock: &c})
}

// Watch is the state now and then every change, in order. Close it (or cancel the reader) when done.
func (s *Service) Watch(ctx context.Context) (*Watch, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	snap, err := s.snapshotLocked(ctx)
	if err != nil {
		return nil, err
	}
	w := &Watch{Snapshot: snap, ch: make(chan Event, watchBuffer), h: &s.hub}
	if err := s.hub.add(w); err != nil {
		return nil, err
	}
	return w, nil
}

// Shutdown ends every Watch (ErrShutdown) and refuses new ones. Call it before stopping the gRPC server, whose graceful stop
// otherwise waits for the streams.
func (s *Service) Shutdown() { s.hub.shutdown() }

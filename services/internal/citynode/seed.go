// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package citynode

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
)

// Seed is deploy/citynode_seed.json read and checked: the level, and the parent if any, of the nodes it names.  A node it does
// not name is Wilderness with no parent.
type Seed struct {
	Levels  map[int64]int   // node guid -> level (0..MaxNodeLevel)
	Parents map[int64]int64 // vassal guid -> its lord's guid
}

// seedFile is the file's JSON.  "comment" is free text for the person who edits the file.
type seedFile struct {
	Comment string `json:"comment"`
	Nodes   map[string]struct {
		Level  *int   `json:"level"`
		Parent string `json:"parent"`
	} `json:"nodes"`
}

// LoadSeed reads a seed file.
func LoadSeed(path string, r *Roster) (*Seed, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("citynode seed: %w", err)
	}
	s, err := ParseSeed(b, r)
	if err != nil {
		return nil, fmt.Errorf("citynode seed %s: %w", path, err)
	}
	return s, nil
}

// ParseSeed checks a seed's JSON against the roster.  It refuses what it cannot make sense of, naming it: an unknown field, a
// node that is not in the roster or is named twice, a level outside 0..MaxNodeLevel, a parent that is not a node, is the node
// itself or makes a loop.
func ParseSeed(b []byte, r *Roster) (*Seed, error) {
	var f seedFile
	dec := json.NewDecoder(bytes.NewReader(b))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&f); err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInvalid, err)
	}
	if _, err := dec.Token(); !errors.Is(err, io.EOF) {
		return nil, fmt.Errorf("%w: text after the JSON object", ErrInvalid)
	}
	s := &Seed{Levels: map[int64]int{}, Parents: map[int64]int64{}}
	// in a stable order, so a message about the file is the same every time
	names := make([]string, 0, len(f.Nodes))
	for name := range f.Nodes {
		names = append(names, name)
	}
	sort.Strings(names)
	named := map[int64]string{}
	for _, name := range names {
		e := f.Nodes[name]
		n, err := r.Resolve(0, name)
		if err != nil {
			return nil, fmt.Errorf("node %q: %w", name, err)
		}
		if first, dup := named[n.Guid]; dup {
			return nil, fmt.Errorf("%w: %q and %q are the same node (%s)", ErrInvalid, first, name, n.Name)
		}
		named[n.Guid] = name
		if e.Level == nil {
			return nil, fmt.Errorf("%w: node %q has no level", ErrInvalid, name)
		}
		if err := ValidateLevel(*e.Level); err != nil {
			return nil, fmt.Errorf("node %q: %w", name, err)
		}
		s.Levels[n.Guid] = *e.Level
		if e.Parent != "" {
			p, err := r.Resolve(0, e.Parent)
			if err != nil {
				return nil, fmt.Errorf("the parent of node %q: %w", name, err)
			}
			if p.Guid == n.Guid {
				return nil, fmt.Errorf("%w: node %q is its own parent", ErrInvalid, name)
			}
			s.Parents[n.Guid] = p.Guid
		}
	}
	// A parent is a node like any other: it may be left out of "nodes" (Wilderness).  A loop is refused.
	for g := range s.Parents {
		seen := map[int64]bool{g: true}
		for p, ok := s.Parents[g]; ok; p, ok = s.Parents[p] {
			if seen[p] {
				n, _ := r.ByGuid(g)
				return nil, fmt.Errorf("%w: the parents of %s make a loop", ErrInvalid, n.Name)
			}
			seen[p] = true
		}
	}
	return s, nil
}

// CoA.C Y B

// ValidateLevel is nil for a level a node may be set to: 0 up to the data's MaxNodeLevel.
func ValidateLevel(level int) error {
	if level < 0 || level > MaxNodeLevel {
		return fmt.Errorf("%w: level %d is outside 0..%d (%s..%s)", ErrInvalid, level, MaxNodeLevel, LevelName(0), LevelName(MaxNodeLevel))
	}
	return nil
}

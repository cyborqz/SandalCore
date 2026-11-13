// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"math"
	"reflect"
	"testing"
)

func TestValidateNodeReputations(t *testing.T) {
	ok := [][]NodeReputation{nil, {{NodeGUID: 753981145547737344, Reputation: 12.5, LastDecayMs: 1}}, {{NodeGUID: -7, Reputation: -700}, {NodeGUID: 8, Reputation: 100000}}}
	for _, rs := range ok {
		if err := ValidateNodeReputations(rs); err != nil {
			t.Errorf("%+v: %v, want nil", rs, err)
		}
	}
	many := make([]NodeReputation, MaxNodeReputations+1)
	for i := range many {
		many[i] = NodeReputation{NodeGUID: int64(i + 1), Reputation: 1}
	}
	bad := [][]NodeReputation{{{Reputation: 1}}, {{NodeGUID: 1, Reputation: 1}, {NodeGUID: 1, Reputation: 2}}, {{NodeGUID: 1, Reputation: math.NaN()}},
		{{NodeGUID: 1, Reputation: math.Inf(1)}}, {{NodeGUID: 1, Reputation: 2e9}}, {{NodeGUID: 1, Reputation: 1, LastDecayMs: -1}}, many}
	for _, rs := range bad {
		if err := ValidateNodeReputations(rs); !errors.Is(err, ErrInvalid) {
			t.Errorf("%d row(s): %v, want ErrInvalid", len(rs), err)
		}
	}
}

func TestNodeReputationIsTheCharacters(t *testing.T) {
	s, owner := setup(t)
	ctx := context.Background()
	first, err := s.Create(ctx, New{AccountID: owner, Name: "First", ClassID: 3})
	if err != nil {
		t.Fatal(err)
	}
	alt, err := s.Create(ctx, New{AccountID: owner, Name: "Alt", ClassID: 2})
	if err != nil {
		t.Fatal(err)
	}
	if got, err := s.NodeReputations(ctx, first.ID); err != nil || len(got) != 0 {
		t.Fatalf("a new character's reputation = %+v (%v); want none", got, err)
	}
	want := []NodeReputation{{NodeGUID: -5, Reputation: -650.5, LastDecayMs: 1700000000000}, {NodeGUID: 753981145547737344, Reputation: 10250, LastDecayMs: 1700000360000}}
	saved := append([]NodeReputation{{NodeGUID: 42, Reputation: 0}}, want...)   // (a reputation of 0 keeps no row)
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplaceNodeReputation: true, NodeReputation: saved}); err != nil {
		t.Fatal(err)
	}
	fresh := NewStore(s.pool)
	if got, err := fresh.NodeReputations(ctx, first.ID); err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("read back %+v (%v); want %+v", got, err, want)
	}
	if got, _ := fresh.NodeReputations(ctx, alt.ID); len(got) != 0 {
		t.Fatalf("another character of the account has %+v: reputation is the character's", got)
	}
	// a save without replace_node_reputation leaves it alone
	if _, err := s.SaveProgress(ctx, first.ID, Progress{Level: 2}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.NodeReputations(ctx, first.ID); !reflect.DeepEqual(got, want) {
		t.Fatalf("after a plain save: %+v; want %+v", got, want)
	}
	// a bad list is refused with nothing changed
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplaceNodeReputation: true, NodeReputation: []NodeReputation{{NodeGUID: 1, Reputation: math.NaN()}}}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a NaN reputation: %v, want ErrInvalid", err)
	}
	if _, err := s.SaveProgress(ctx, first.ID, Progress{NodeReputation: want}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("reputations without replace_node_reputation: %v, want ErrInvalid", err)
	}
	if got, _ := s.NodeReputations(ctx, first.ID); !reflect.DeepEqual(got, want) {
		t.Fatalf("after the refused saves: %+v; want %+v", got, want)
	}
	// an empty list takes it all away; the character's deletion takes its rows
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplaceNodeReputation: true}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.NodeReputations(ctx, first.ID); len(got) != 0 {
		t.Fatalf("after an empty list: %+v; want none", got)
	}
	if _, err := s.SaveProgress(ctx, alt.ID, Progress{ReplaceNodeReputation: true, NodeReputation: want}); err != nil {
		t.Fatal(err)
	}
	if err := s.Delete(ctx, owner, alt.ID); err != nil {
		t.Fatal(err)
	}
	var left int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_node_reputation WHERE character_id = $1`, alt.ID).Scan(&left); err != nil || left != 0 {
		t.Fatalf("a deleted character keeps %d reputation row(s) (%v)", left, err)
	}
	if _, err := s.NodeReputations(ctx, "0123456789abcdef0123456789abcdef"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("an unknown character: %v, want ErrNotFound", err)
	}
}

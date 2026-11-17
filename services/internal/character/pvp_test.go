// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// PvP: contributed by MrJambix in #23.

package character

import (
	"context"
	"errors"
	"math"
	"testing"
)

func TestValidatePvP(t *testing.T) {
	for _, p := range []PvP{{}, {Corruption: 4.5}, {Corruption: 0, Blight: 3}, {Corruption: 999999, Blight: 999999}} {
		if err := ValidatePvP(p); err != nil {
			t.Errorf("%+v: %v, want nil", p, err)
		}
	}
	for _, p := range []PvP{{Corruption: -1}, {Blight: -0.5}, {Corruption: math.NaN()}, {Blight: math.Inf(1)}, {Corruption: MaxPvPValue}} {
		if err := ValidatePvP(p); !errors.Is(err, ErrInvalid) {
			t.Errorf("%+v: %v, want ErrInvalid", p, err)
		}
	}
	if err := (Progress{PvP: PvP{Corruption: 1}}).Validate(); !errors.Is(err, ErrInvalid) {
		t.Errorf("a PvP state without ReplacePvP: %v, want ErrInvalid", err)
	}
}

func TestPvPIsTheCharacters(t *testing.T) {
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
	if got, err := s.PvPState(ctx, first.ID); err != nil || got != (PvP{}) {
		t.Fatalf("a new character's PvP state = %+v (%v); want none", got, err)
	}
	want := PvP{Corruption: 4.2, Blight: 2}
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplacePvP: true, PvP: want}); err != nil {
		t.Fatal(err)
	}
	fresh := NewStore(s.pool)
	if got, err := fresh.PvPState(ctx, first.ID); err != nil || got != want {
		t.Fatalf("read back %+v (%v); want %+v", got, err, want)
	}
	if got, _ := fresh.PvPState(ctx, alt.ID); got != (PvP{}) {
		t.Fatalf("another character of the account has %+v: the PvP state is the character's", got)
	}
	// a save without ReplacePvP leaves it alone; one with both 0 takes the row away
	if _, err := s.SaveProgress(ctx, first.ID, Progress{}); err != nil {
		t.Fatal(err)
	}
	if got, _ := fresh.PvPState(ctx, first.ID); got != want {
		t.Fatalf("a save without replace_pvp changed it to %+v", got)
	}
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplacePvP: true}); err != nil {
		t.Fatal(err)
	}
	var rows int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_pvp WHERE character_id = $1`, first.ID).Scan(&rows); err != nil || rows != 0 {
		t.Fatalf("both 0 keeps %d row(s) (%v); want none", rows, err)
	}
	if _, err := s.PvPState(ctx, "0123456789abcdef0123456789abcdef"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("an unknown character: %v, want ErrNotFound", err)
	}
}

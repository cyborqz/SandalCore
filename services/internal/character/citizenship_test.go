// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"testing"

	"aocrop/services/internal/account"
)

func TestValidateCitizenship(t *testing.T) {
	ok := []Citizenship{{}, {RenounceCooldownUntilMs: 5}, {NodeGUID: 753981145547737344, JoinedMs: 1, DuesDueMs: 2}, {NodeGUID: -7, JoinedMs: 1, PrimaryCharacterID: "0123456789abcdef0123456789abcdef"}}
	for _, c := range ok {
		if err := ValidateCitizenship(c); err != nil {
			t.Errorf("%+v: %v, want nil", c, err)
		}
	}
	bad := []Citizenship{{JoinedMs: -1}, {DuesDueMs: -1}, {RenounceCooldownUntilMs: -1}, {NodeGUID: 1}, {PrimaryCharacterID: "0123456789abcdef0123456789abcdef"},
		{NodeGUID: 1, JoinedMs: 1, PrimaryCharacterID: "not an id"}, {DuesDueMs: 3}}
	for _, c := range bad {
		if err := ValidateCitizenship(c); !errors.Is(err, ErrInvalid) {
			t.Errorf("%+v: %v, want ErrInvalid", c, err)
		}
	}
}

func TestCitizenshipIsTheAccounts(t *testing.T) {
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
	other, err := account.NewStore(s.pool, 0).Register(ctx, "other", "secret2")
	if err != nil {
		t.Fatal(err)
	}
	stranger, err := s.Create(ctx, New{AccountID: other.ID, Name: "Stranger", ClassID: 1})
	if err != nil {
		t.Fatal(err)
	}
	if _, has, err := s.Citizenship(ctx, first.ID); err != nil || has {
		t.Fatalf("a new account is a citizen (%v, %v); want none", has, err)
	}
	want := Citizenship{NodeGUID: 753981145547737344, PrimaryCharacterID: first.ID, JoinedMs: 1700000000000, DuesDueMs: 1701000000000}
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplaceCitizenship: true, Citizenship: want}); err != nil {
		t.Fatal(err)
	}
	fresh := NewStore(s.pool)
	for _, id := range []string{first.ID, alt.ID} {
		if got, has, err := fresh.Citizenship(ctx, id); err != nil || !has || got != want {
			t.Fatalf("the citizenship read from %s = %+v (%v, %v); want %+v: it is the account's", id, got, has, err, want)
		}
	}
	if _, has, _ := fresh.Citizenship(ctx, stranger.ID); has {
		t.Fatal("another account's character is a citizen")
	}
	// a save without replace_citizenship leaves it alone
	if _, err := s.SaveProgress(ctx, alt.ID, Progress{Level: 2}); err != nil {
		t.Fatal(err)
	}
	if got, _, _ := s.Citizenship(ctx, first.ID); got != want {
		t.Fatalf("after a plain save: %+v; want %+v", got, want)
	}
	// a primary character of another account is refused, with nothing changed
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplaceCitizenship: true, Citizenship: Citizenship{NodeGUID: 5, JoinedMs: 2, PrimaryCharacterID: stranger.ID}}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a stranger as the primary character: %v, want ErrInvalid", err)
	}
	if got, _, _ := s.Citizenship(ctx, first.ID); got != want {
		t.Fatalf("after the refused save: %+v; want %+v", got, want)
	}
	// renouncing keeps only the cooldown; an empty one removes the row
	renounced := Citizenship{RenounceCooldownUntilMs: 1702000000000}
	if _, err := s.SaveProgress(ctx, alt.ID, Progress{ReplaceCitizenship: true, Citizenship: renounced}); err != nil {
		t.Fatal(err)
	}
	if got, has, _ := s.Citizenship(ctx, first.ID); !has || got != renounced {
		t.Fatalf("after renouncing: %+v (%v); want %+v", got, has, renounced)
	}
	if _, err := s.SaveProgress(ctx, alt.ID, Progress{ReplaceCitizenship: true}); err != nil {
		t.Fatal(err)
	}
	if _, has, _ := s.Citizenship(ctx, first.ID); has {
		t.Fatal("an empty citizenship left its row")
	}
	// a citizenship without replace_citizenship is refused
	if _, err := s.SaveProgress(ctx, first.ID, Progress{Citizenship: want}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a citizenship without replace_citizenship: %v, want ErrInvalid", err)
	}
	// the primary character's deletion keeps the citizenship with no primary character
	if _, err := s.SaveProgress(ctx, first.ID, Progress{ReplaceCitizenship: true, Citizenship: Citizenship{NodeGUID: 9, JoinedMs: 3, PrimaryCharacterID: alt.ID}}); err != nil {
		t.Fatal(err)
	}
	if err := s.Delete(ctx, owner, alt.ID); err != nil {
		t.Fatal(err)
	}
	if got, has, _ := s.Citizenship(ctx, first.ID); !has || got.NodeGUID != 9 || got.PrimaryCharacterID != "" {
		t.Fatalf("after the primary character was deleted: %+v (%v); want node 9 and no primary character", got, has)
	}
	if _, _, err := s.Citizenship(ctx, "0123456789abcdef0123456789abcdef"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("an unknown character: %v, want ErrNotFound", err)
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package legacyimport

import (
	"context"
	"errors"
	"fmt"
	"io"

	"aocrop/services/internal/character"
)

// Store is the part of character.Store the import uses.
type Store interface {
	Get(ctx context.Context, id string) (character.Character, error)
	Create(ctx context.Context, n character.New) (character.Character, error)
}

var _ Store = (*character.Store)(nil)

// Summary counts what Import did with each record. Conflicts and Invalid are
// the characters that were NOT carried over and need a human.
type Summary struct{ Imported, AlreadyPresent, Conflicts, Invalid int }

// -- C-O-A x C.Y.B --

// Import carries chars into the store as accountID's characters (whatever
// AccountID the records carry), writing one line per character to w, and
// returns the counts. Records are judged one by one:
//
//   - an id already seen earlier in chars is a conflict (duplicate id in file)
//     and is not attempted;
//   - an id the store has for accountID is already present, and left alone;
//   - an id the store has for another account is a conflict;
//   - otherwise it is created, where a name or id taken by another character
//     is a conflict and invalid data is Invalid.
//
// Conflicts and invalid records are reported and the import goes on. It stops
// only on an infrastructure error (anything that is not one of the character
// sentinels), returning the counts so far.
func Import(ctx context.Context, w io.Writer, store Store, accountID int64, chars []character.New) (Summary, error) {
	var s Summary
	seen := map[string]bool{}
	for _, n := range chars {
		n.AccountID = accountID
		// A record without an id gets one from the store, so it cannot repeat.
		if n.ID != "" {
			if seen[n.ID] {
				fmt.Fprintf(w, "CONFLICT %s (%s): duplicate id in file\n", n.Name, n.ID)
				s.Conflicts++
				continue
			}
			seen[n.ID] = true
		}

		have, err := store.Get(ctx, n.ID)
		switch {
		case err == nil && have.AccountID == accountID:
			fmt.Fprintf(w, "skip %s (%s): already imported\n", n.Name, n.ID)
			s.AlreadyPresent++
			continue
		case err == nil:
			fmt.Fprintf(w, "CONFLICT %s (%s): id belongs to another account\n", n.Name, n.ID)
			s.Conflicts++
			continue
		case !errors.Is(err, character.ErrNotFound):
			return s, fmt.Errorf("%s: %w", n.Name, err)
		}

		c, err := store.Create(ctx, n)
		switch {
		case errors.Is(err, character.ErrNameTaken), errors.Is(err, character.ErrIDTaken):
			fmt.Fprintf(w, "CONFLICT %s (%s): %v\n", n.Name, n.ID, err)
			s.Conflicts++
		case errors.Is(err, character.ErrInvalid):
			fmt.Fprintf(w, "INVALID %s (%s): %v\n", n.Name, n.ID, err)
			s.Invalid++
		case err != nil:
			return s, fmt.Errorf("%s: %w", n.Name, err)
		default:
			fmt.Fprintf(w, "imported %s (%s, %s) for account %d\n", c.Name, c.ClassName, c.ID, accountID)
			s.Imported++
		}
	}
	return s, nil
}

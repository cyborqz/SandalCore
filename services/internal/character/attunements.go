// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"fmt"

	"github.com/jackc/pgx/v5"
)

// Embersprings (migration 0018): the places a character discovered, in the order it discovered them, the place that is its home and when its home respawn was last used. worldd keeps
// them while the character plays and saves them with the progress.

// MaxAttunements bounds one save: a place's number in the master list is 1 to 255 (a character's set holds one byte for each).
const MaxAttunements = 255

// Attunements are a character's discovered places.
type Attunements struct {
	Discovered     []int32 // the places' numbers in the master list, in the order they were discovered
	Home           int32   // the home place's number; 0 for none
	HomeLastUsedMs int64   // Unix milliseconds on the server's wall clock when the home respawn was last used; 0 for never
}

// ValidateAttunements checks the places of a save: each 1 to 255, each once, a home that is 0 or a place, and a time that is not negative.
func ValidateAttunements(a Attunements) error {
	if len(a.Discovered) > MaxAttunements {
		return fmt.Errorf("%w: %d discovered places (at most %d)", ErrInvalid, len(a.Discovered), MaxAttunements)
	}
	seen := make(map[int32]bool, len(a.Discovered))
	for i, n := range a.Discovered {
		switch {
		case n < 1 || n > MaxAttunements:
			return fmt.Errorf("%w: discovered place %d: %d is not 1 to %d", ErrInvalid, i, n, MaxAttunements)
		case seen[n]:
			return fmt.Errorf("%w: discovered place %d: %d again", ErrInvalid, i, n)
		}
		seen[n] = true
	}
	if a.Home < 0 || a.Home > MaxAttunements {
		return fmt.Errorf("%w: home place %d is not 0 to %d", ErrInvalid, a.Home, MaxAttunements)
	}
	if a.HomeLastUsedMs < 0 {
		return fmt.Errorf("%w: the home respawn's last use %d is negative", ErrInvalid, a.HomeLastUsedMs)
	}
	return nil
}

// Attunements returns the places a character discovered, in the order it discovered them, its home and the last use of its home respawn. A character that discovered nothing has an
// empty list and no home; an unknown character is ErrNotFound.
func (s *Store) Attunements(ctx context.Context, characterID string) (Attunements, error) {
	if !idPattern.MatchString(characterID) {
		return Attunements{}, ErrNotFound
	}
	var out Attunements
	rows, err := s.pool.Query(ctx, `SELECT attune_index FROM character_attunements WHERE character_id = $1 ORDER BY position`, characterID)
	if err != nil {
		return Attunements{}, err
	}
	for rows.Next() {
		var n int32
		if err := rows.Scan(&n); err != nil {
			rows.Close()
			return Attunements{}, err
		}
		out.Discovered = append(out.Discovered, n)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return Attunements{}, err
	}
	err = s.pool.QueryRow(ctx, `SELECT attune_index, last_used_ms FROM character_home_attunement WHERE character_id = $1`, characterID).Scan(&out.Home, &out.HomeLastUsedMs)
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return Attunements{}, err
	}
	if len(out.Discovered) == 0 && out.Home == 0 {
		var exists bool
		if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return Attunements{}, err
		}
		if !exists {
			return Attunements{}, ErrNotFound
		}
	}
	return out, nil
}

// replaceAttunements makes the character's places exactly a, in tx.
func replaceAttunements(ctx context.Context, tx pgx.Tx, characterID string, a Attunements) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_attunements WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	if len(a.Discovered) > 0 {
		if _, err := tx.Exec(ctx, `INSERT INTO character_attunements (character_id, attune_index, position)
			SELECT $1, t.n, t.i - 1 FROM unnest($2::int[]) WITH ORDINALITY AS t(n, i)`, characterID, a.Discovered); err != nil {
			return itemsError(err)
		}
	}
	if a.Home == 0 && a.HomeLastUsedMs == 0 {
		_, err := tx.Exec(ctx, `DELETE FROM character_home_attunement WHERE character_id = $1`, characterID)
		return err
	}
	_, err := tx.Exec(ctx, `INSERT INTO character_home_attunement (character_id, attune_index, last_used_ms) VALUES ($1, $2, $3)
		ON CONFLICT (character_id) DO UPDATE SET attune_index = EXCLUDED.attune_index, last_used_ms = EXCLUDED.last_used_ms`, characterID, a.Home, a.HomeLastUsedMs)
	return itemsError(err)
}

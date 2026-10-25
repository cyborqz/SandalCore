// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"fmt"

	"github.com/jackc/pgx/v5"
)

// Completed tutorials (missing systems, step 0, migration 0015): the tutorials a character finished, one row each in character_completed_ftues, with their place in the character's list so
// that the order they were finished in is kept. worldd keeps the list while the character plays and saves it with the progress.

// MaxCompletedFTUEs bounds one save: the design data holds fewer than 100 tutorials, and the client reports one id per call.
const MaxCompletedFTUEs = 1024

// ValidateCompletedFTUEs checks the tutorials of a save: each a record guid, each once, and not more than a character can have.
func ValidateCompletedFTUEs(ids []int64) error {
	if len(ids) > MaxCompletedFTUEs {
		return fmt.Errorf("%w: %d completed tutorials (at most %d)", ErrInvalid, len(ids), MaxCompletedFTUEs)
	}
	seen := make(map[int64]bool, len(ids))
	for i, id := range ids {
		switch {
		case id <= 0:
			return fmt.Errorf("%w: completed tutorial %d: %d is not a record", ErrInvalid, i, id)
		case seen[id]:
			return fmt.Errorf("%w: completed tutorial %d: %d again", ErrInvalid, i, id)
		}
		seen[id] = true
	}
	return nil
}

// CompletedFTUEs returns the tutorials a character finished, in the order they were finished. A character that finished none has an empty list; an unknown character is ErrNotFound.
func (s *Store) CompletedFTUEs(ctx context.Context, characterID string) ([]int64, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT ftue_guid FROM character_completed_ftues WHERE character_id = $1 ORDER BY position`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		out = append(out, id)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		var exists bool
		if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return nil, err
		}
		if !exists {
			return nil, ErrNotFound
		}
	}
	return out, nil
}

// replaceCompletedFTUEs makes the character's completed tutorials exactly ids, in this order, in tx.
func replaceCompletedFTUEs(ctx context.Context, tx pgx.Tx, characterID string, ids []int64) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_completed_ftues WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	if len(ids) == 0 {
		return nil
	}
	_, err := tx.Exec(ctx, `INSERT INTO character_completed_ftues (character_id, ftue_guid, position)
		SELECT $1, t.id, t.n - 1 FROM unnest($2::bigint[]) WITH ORDINALITY AS t(id, n)`, characterID, ids)
	return itemsError(err)
}

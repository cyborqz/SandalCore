// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"fmt"

	"github.com/jackc/pgx/v5"
)

// Known recipes (the crafting milestone, step 3, migration 0014): the recipes a character was taught although the game makes a player learn them, one row each in
// character_known_recipes. A recipe the game does not make a player learn is known to everybody and has no row. worldd keeps the list while the character plays and saves it with the
// items and the wallet.

// MaxKnownRecipes bounds one save: the design data holds fewer than 4,300 recipes in all.
const MaxKnownRecipes = 5000

// ValidateKnownRecipes checks the recipes of a save: each a record guid, each once, and not more than a character can have.
func ValidateKnownRecipes(recipes []int64) error {
	if len(recipes) > MaxKnownRecipes {
		return fmt.Errorf("%w: %d known recipes (at most %d)", ErrInvalid, len(recipes), MaxKnownRecipes)
	}
	seen := make(map[int64]bool, len(recipes))
	for i, r := range recipes {
		switch {
		case r <= 0:
			return fmt.Errorf("%w: known recipe %d: %d is not a record", ErrInvalid, i, r)
		case seen[r]:
			return fmt.Errorf("%w: known recipe %d: %d again", ErrInvalid, i, r)
		}
		seen[r] = true
	}
	return nil
}

// KnownRecipes returns the recipes a character was taught, in the order they were taught (then by guid). A character that was taught none has an empty list; an unknown character is
// ErrNotFound.
func (s *Store) KnownRecipes(ctx context.Context, characterID string) ([]int64, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT recipe_guid FROM character_known_recipes WHERE character_id = $1 ORDER BY taught_at, recipe_guid`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var r int64
		if err := rows.Scan(&r); err != nil {
			return nil, err
		}
		out = append(out, r)
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

// replaceKnownRecipes makes the character's known recipes exactly recipes in tx. A recipe that was known before keeps its taught_at (the list keeps its order), the others are taught now.
func replaceKnownRecipes(ctx context.Context, tx pgx.Tx, characterID string, recipes []int64) error {
	if len(recipes) == 0 { // (an empty list is a NULL array to the driver, and NOT (x = ANY(NULL)) deletes nothing: the plain delete is the whole answer)
		_, err := tx.Exec(ctx, `DELETE FROM character_known_recipes WHERE character_id = $1`, characterID)
		return err
	}
	if _, err := tx.Exec(ctx, `DELETE FROM character_known_recipes WHERE character_id = $1 AND NOT (recipe_guid = ANY($2::bigint[]))`, characterID, recipes); err != nil {
		return err
	}
	for _, r := range recipes {
		if _, err := tx.Exec(ctx, `INSERT INTO character_known_recipes (character_id, recipe_guid) VALUES ($1, $2) ON CONFLICT (character_id, recipe_guid) DO NOTHING`, characterID, r); err != nil {
			return itemsError(err)
		}
	}
	return nil
}

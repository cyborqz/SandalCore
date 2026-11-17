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

// Node citizenship (migration 0020): an account is a citizen of one city node, with the character that joined as its primary character. One row per account in
// account_citizenship; worldd keeps it while a character of the account plays and saves it with the progress.

// Citizenship is an account's node citizenship. NodeGUID 0 is none: then only RenounceCooldownUntilMs counts. Times are Unix milliseconds; 0 is none.
type Citizenship struct {
	NodeGUID                int64
	PrimaryCharacterID      string // "" when the primary character was deleted
	JoinedMs                int64
	DuesDueMs               int64
	RenounceCooldownUntilMs int64
}

// Empty reports whether the citizenship holds nothing to keep (no node and no cooldown): its row is removed.
func (c Citizenship) Empty() bool { return c.NodeGUID == 0 && c.RenounceCooldownUntilMs == 0 }

// ValidateCitizenship checks the citizenship of a save: no negative time, a node has its join time, and no primary character without a node. Whether the primary character
// is of the account is checked in the transaction.
func ValidateCitizenship(c Citizenship) error {
	if c.JoinedMs < 0 || c.DuesDueMs < 0 || c.RenounceCooldownUntilMs < 0 {
		return fmt.Errorf("%w: a citizenship time is negative", ErrInvalid)
	}
	if c.NodeGUID != 0 && c.JoinedMs == 0 {
		return fmt.Errorf("%w: a citizenship of node %d has no join time", ErrInvalid, c.NodeGUID)
	}
	if c.NodeGUID == 0 && (c.PrimaryCharacterID != "" || c.JoinedMs != 0 || c.DuesDueMs != 0) {
		return fmt.Errorf("%w: a citizenship with no node names a character, a join time or dues", ErrInvalid)
	}
	if c.PrimaryCharacterID != "" && !idPattern.MatchString(c.PrimaryCharacterID) {
		return fmt.Errorf("%w: the primary character %q is not a character id", ErrInvalid, c.PrimaryCharacterID)
	}
	return nil
}

// Citizenship returns the citizenship of the character's account; ok is false when the account never joined a node. An unknown character is ErrNotFound.
func (s *Store) Citizenship(ctx context.Context, characterID string) (c Citizenship, ok bool, err error) {
	if !idPattern.MatchString(characterID) {
		return Citizenship{}, false, ErrNotFound
	}
	var account int64
	if err := s.pool.QueryRow(ctx, `SELECT account_id FROM characters WHERE id = $1`, characterID).Scan(&account); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return Citizenship{}, false, ErrNotFound
		}
		return Citizenship{}, false, err
	}
	var primary *string
	err = s.pool.QueryRow(ctx, `SELECT node_guid, primary_character_id, joined_ms, dues_due_ms, renounce_cooldown_until_ms FROM account_citizenship WHERE account_id = $1`, account).
		Scan(&c.NodeGUID, &primary, &c.JoinedMs, &c.DuesDueMs, &c.RenounceCooldownUntilMs)
	if errors.Is(err, pgx.ErrNoRows) {
		return Citizenship{}, false, nil
	}
	if err != nil {
		return Citizenship{}, false, err
	}
	if primary != nil {
		c.PrimaryCharacterID = *primary
	}
	return c, true, nil
}

// replaceCitizenship makes the citizenship of the character's account exactly c, in tx (an empty one removes the row). The primary character must be of the account.
func replaceCitizenship(ctx context.Context, tx pgx.Tx, characterID string, c Citizenship) error {
	var account int64
	if err := tx.QueryRow(ctx, `SELECT account_id FROM characters WHERE id = $1`, characterID).Scan(&account); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return ErrNotFound
		}
		return err
	}
	if c.Empty() {
		_, err := tx.Exec(ctx, `DELETE FROM account_citizenship WHERE account_id = $1`, account)
		return err
	}
	var primary any
	if c.PrimaryCharacterID != "" {
		var same bool
		if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1 AND account_id = $2)`, c.PrimaryCharacterID, account).Scan(&same); err != nil {
			return err
		}
		if !same {
			return fmt.Errorf("%w: the primary character %s is not of the account", ErrInvalid, c.PrimaryCharacterID)
		}
		primary = c.PrimaryCharacterID
	}
	_, err := tx.Exec(ctx, `INSERT INTO account_citizenship (account_id, node_guid, primary_character_id, joined_ms, dues_due_ms, renounce_cooldown_until_ms) VALUES ($1, $2, $3, $4, $5, $6)
		ON CONFLICT (account_id) DO UPDATE SET node_guid = EXCLUDED.node_guid, primary_character_id = EXCLUDED.primary_character_id, joined_ms = EXCLUDED.joined_ms,
			dues_due_ms = EXCLUDED.dues_due_ms, renounce_cooldown_until_ms = EXCLUDED.renounce_cooldown_until_ms, updated_at = now()`,
		account, c.NodeGUID, primary, c.JoinedMs, c.DuesDueMs, c.RenounceCooldownUntilMs)
	return itemsError(err)
}

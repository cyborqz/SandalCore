// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"fmt"
	"math"

	"github.com/jackc/pgx/v5"
)

// PvP (migration 0025): what a character carries from one session to the next, as the game keeps it on the character's stats: Corruption (GameStatProfile.CorruptionId) and
// Corruption_Blight (CorruptionSoulburnId). One row in character_pvp for a character that has any; worldd keeps them while the character plays and saves them with the progress.

// PvP is a character's corruption and blight. The zero value is a character with neither (no row).
type PvP struct {
	Corruption float64
	Blight     float64
}

// MaxPvPValue bounds both numbers (the table's checks).
const MaxPvPValue = 1e6

// ValidatePvP checks a save's PvP state: each a finite number of 0 or more and under a million.
func ValidatePvP(p PvP) error {
	for _, v := range []struct {
		name  string
		value float64
	}{{"corruption", p.Corruption}, {"blight", p.Blight}} {
		if math.IsNaN(v.value) || math.IsInf(v.value, 0) || v.value < 0 || v.value >= MaxPvPValue {
			return fmt.Errorf("%w: the %s %v is out of range", ErrInvalid, v.name, v.value)
		}
	}
	return nil
}

// PvPState returns the character's PvP state (the zero value when it has none). An unknown character is ErrNotFound.
func (s *Store) PvPState(ctx context.Context, characterID string) (PvP, error) {
	if !idPattern.MatchString(characterID) {
		return PvP{}, ErrNotFound
	}
	var exists bool
	if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
		return PvP{}, err
	}
	if !exists {
		return PvP{}, ErrNotFound
	}
	var p PvP
	err := s.pool.QueryRow(ctx, `SELECT corruption, blight FROM character_pvp WHERE character_id = $1`, characterID).Scan(&p.Corruption, &p.Blight)
	if err == pgx.ErrNoRows {
		return PvP{}, nil
	}
	return p, err
}

// replacePvP makes the character's PvP state exactly p, in tx (a character with neither keeps no row).
func replacePvP(ctx context.Context, tx pgx.Tx, characterID string, p PvP) error {
	var exists bool
	if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
		return err
	}
	if !exists {
		return ErrNotFound
	}
	if p == (PvP{}) {
		_, err := tx.Exec(ctx, `DELETE FROM character_pvp WHERE character_id = $1`, characterID)
		return err
	}
	_, err := tx.Exec(ctx, `INSERT INTO character_pvp (character_id, corruption, blight) VALUES ($1, $2, $3)
		ON CONFLICT (character_id) DO UPDATE SET corruption = EXCLUDED.corruption, blight = EXCLUDED.blight, updated_at = now()`, characterID, p.Corruption, p.Blight)
	if err != nil {
		return itemsError(err)
	}
	return nil
}

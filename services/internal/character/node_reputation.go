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

// Node reputation (migration 0022): a character's reputation with each city node, as the game keeps it in UOwnerInfoComponent.NodeToPlayerReputation. One row per character
// and node in character_node_reputation; worldd keeps them while the character plays and saves them with the progress.

// NodeReputation is a character's reputation with one node. LastDecayMs is when it last decayed (Unix milliseconds; 0 for never).
type NodeReputation struct {
	NodeGUID    int64
	Reputation  float64
	LastDecayMs int64
}

// MaxNodeReputations is the most nodes a character keeps reputation with (the game has 81 city nodes).
const MaxNodeReputations = 512

// ValidateNodeReputations checks the reputations of a save: a node of 0 or one named twice, a reputation that is not a finite number within a billion either way, a negative decay
// time, or more than MaxNodeReputations rows.
func ValidateNodeReputations(rs []NodeReputation) error {
	if len(rs) > MaxNodeReputations {
		return fmt.Errorf("%w: %d node reputations, at most %d", ErrInvalid, len(rs), MaxNodeReputations)
	}
	seen := map[int64]bool{}
	for _, r := range rs {
		switch {
		case r.NodeGUID == 0:
			return fmt.Errorf("%w: a node reputation names no node", ErrInvalid)
		case seen[r.NodeGUID]:
			return fmt.Errorf("%w: node %d's reputation is given twice", ErrInvalid, r.NodeGUID)
		case math.IsNaN(r.Reputation) || math.IsInf(r.Reputation, 0) || math.Abs(r.Reputation) >= 1e9:
			return fmt.Errorf("%w: node %d's reputation %v is out of range", ErrInvalid, r.NodeGUID, r.Reputation)
		case r.LastDecayMs < 0:
			return fmt.Errorf("%w: node %d's reputation has a negative decay time", ErrInvalid, r.NodeGUID)
		}
		seen[r.NodeGUID] = true
	}
	return nil
}

// NodeReputations returns the character's node reputations, by node. An unknown character is ErrNotFound.
func (s *Store) NodeReputations(ctx context.Context, characterID string) ([]NodeReputation, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	var exists bool
	if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
		return nil, err
	}
	if !exists {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT node_guid, reputation, last_decay_ms FROM character_node_reputation WHERE character_id = $1 ORDER BY node_guid`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []NodeReputation
	for rows.Next() {
		var r NodeReputation
		if err := rows.Scan(&r.NodeGUID, &r.Reputation, &r.LastDecayMs); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

// replaceNodeReputations makes the character's node reputations exactly rs, in tx (a reputation of 0 keeps no row).
func replaceNodeReputations(ctx context.Context, tx pgx.Tx, characterID string, rs []NodeReputation) error {
	var exists bool
	if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
		return err
	}
	if !exists {
		return ErrNotFound
	}
	if _, err := tx.Exec(ctx, `DELETE FROM character_node_reputation WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	for _, r := range rs {
		if r.Reputation == 0 {
			continue
		}
		if _, err := tx.Exec(ctx, `INSERT INTO character_node_reputation (character_id, node_guid, reputation, last_decay_ms) VALUES ($1, $2, $3, $4)`,
			characterID, r.NodeGUID, r.Reputation, r.LastDecayMs); err != nil {
			return itemsError(err)
		}
	}
	return nil
}

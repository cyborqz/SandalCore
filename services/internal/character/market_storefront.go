// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Marketplace storefront descriptions (migration 0034): the text a seller gives his storefront at ONE Marketplace, and the count of times a Marketplace's storefront pages
// changed.
//
//	SetStorefrontDescription  in ONE transaction: raises the node's count (a row lock that also puts two edits of one Marketplace in a line), checks that the seller has a
//	                          listing at the node, and writes the text (an empty one deletes it). Two edits at once end with the last one, whole; a refused edit changes nothing.
//	MarketStorefronts         every stored description and every node's count, read when worldd starts.
//
// The page version a client holds is the game's own 1843 plus the node's count (worldd adds it): the client asks for pages again only when the version it holds is smaller,
// so a change that must reach a window that is open raises it.

import (
	"context"
	"errors"
	"fmt"
	"unicode/utf16"
	"unicode/utf8"

	"github.com/jackc/pgx/v5"
)

// MaxStorefrontDescription is the longest description a storefront may hold, in UTF-16 code units: the client's own edit box cuts the text at 50.
const MaxStorefrontDescription = 50

// StorefrontOutcome is how SetStorefrontDescription ended.
type StorefrontOutcome int

const (
	// StorefrontDone: the text was written (or deleted) and the node's count raised.
	StorefrontDone StorefrontOutcome = iota + 1
	// StorefrontUnchanged: the storefront already held exactly this text (or none, for an empty one): nothing was written and the count did not move. A repeat of an edit whose
	// answer was lost ends here.
	StorefrontUnchanged
	// StorefrontNoListing: the seller has no listing at the node, so no storefront to describe. Nothing was written.
	StorefrontNoListing
)

// StorefrontResult is the answer of SetStorefrontDescription.
type StorefrontResult struct {
	Outcome      StorefrontOutcome
	Description  string // the text the storefront holds now ("" for none)
	HeaderRaises int64  // the node's count now
}

// StorefrontText is one stored description.
type StorefrontText struct {
	Node        int64
	SellerID    string
	Description string
}

// NodeHeaderRaises is the count of one node whose storefront pages changed.
type NodeHeaderRaises struct {
	Node   int64
	Raises int64
}

// ValidateStorefrontDescription checks a description: well-formed UTF-8, at most 50 UTF-16 code units and no control character. It is NOT trimmed ("Journeyman  " keeps its
// spaces, as the recorded retail descriptions do). An empty text is valid: it clears the description.
func ValidateStorefrontDescription(text string) error {
	if !utf8.ValidString(text) {
		return fmt.Errorf("%w: the storefront description is not valid UTF-8", ErrInvalid)
	}
	for _, r := range text {
		if r < 0x20 || r == 0x7f {
			return fmt.Errorf("%w: the storefront description holds the control character U+%04X", ErrInvalid, r)
		}
	}
	if n := len(utf16.Encode([]rune(text))); n > MaxStorefrontDescription {
		return fmt.Errorf("%w: the storefront description is %d characters, at most %d", ErrInvalid, n, MaxStorefrontDescription)
	}
	return nil
}

// SetStorefrontDescription stores the seller's description for his storefront at `node` (see the top of this file). ErrNotFound for an unknown character; ErrInvalid, with
// nothing changed, for a node of 0 or a text ValidateStorefrontDescription refuses.
func (s *Store) SetStorefrontDescription(ctx context.Context, characterID string, node int64, description string) (StorefrontResult, error) {
	if !idPattern.MatchString(characterID) {
		return StorefrontResult{}, ErrNotFound
	}
	if node == 0 {
		return StorefrontResult{}, fmt.Errorf("%w: the market node is 0", ErrInvalid)
	}
	if err := ValidateStorefrontDescription(description); err != nil {
		return StorefrontResult{}, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return StorefrontResult{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	var exists bool
	if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
		return StorefrontResult{}, err
	}
	if !exists {
		return StorefrontResult{}, ErrNotFound
	}
	// The node's row is the lock of every edit at this Marketplace: it is made when it is not there (without raising it yet) and held until the commit, so two edits are
	// written one after the other, each reading the text the other left.
	if _, err := tx.Exec(ctx, `INSERT INTO node_market_headers (node, header_raises) VALUES ($1, 0) ON CONFLICT (node) DO NOTHING`, node); err != nil {
		return StorefrontResult{}, err
	}
	var raises int64
	if err := tx.QueryRow(ctx, `SELECT header_raises FROM node_market_headers WHERE node = $1 FOR UPDATE`, node).Scan(&raises); err != nil {
		return StorefrontResult{}, err
	}
	var listed bool
	if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM character_market_listings WHERE character_id = $1 AND market_node = $2 AND status = $3)`,
		characterID, node, MarketStatusListed).Scan(&listed); err != nil {
		return StorefrontResult{}, err
	}
	var current string
	err = tx.QueryRow(ctx, `SELECT description FROM market_storefronts WHERE node = $1 AND character_id = $2`, node, characterID).Scan(&current)
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return StorefrontResult{}, err
	}
	if !listed {
		// (the row made above for the lock is a count of 0 and changes nothing a client reads; the transaction is rolled back and leaves nothing at all)
		return StorefrontResult{Outcome: StorefrontNoListing, Description: current, HeaderRaises: raises}, nil
	}
	if current == description {
		return StorefrontResult{Outcome: StorefrontUnchanged, Description: current, HeaderRaises: raises}, nil
	}
	if description == "" {
		if _, err := tx.Exec(ctx, `DELETE FROM market_storefronts WHERE node = $1 AND character_id = $2`, node, characterID); err != nil {
			return StorefrontResult{}, err
		}
	} else if _, err := tx.Exec(ctx, `INSERT INTO market_storefronts (node, character_id, description, updated_ms) VALUES ($1, $2, $3, (extract(epoch FROM clock_timestamp()) * 1000)::bigint)
		ON CONFLICT (node, character_id) DO UPDATE SET description = EXCLUDED.description, updated_ms = EXCLUDED.updated_ms`, node, characterID, description); err != nil {
		return StorefrontResult{}, err
	}
	if err := tx.QueryRow(ctx, `UPDATE node_market_headers SET header_raises = header_raises + 1 WHERE node = $1 RETURNING header_raises`, node).Scan(&raises); err != nil {
		return StorefrontResult{}, err
	}
	if err := tx.Commit(ctx); err != nil {
		return StorefrontResult{}, err
	}
	return StorefrontResult{Outcome: StorefrontDone, Description: description, HeaderRaises: raises}, nil
}

// MarketStorefronts reads every stored description and the count of every node that has a row, in one snapshot.
func (s *Store) MarketStorefronts(ctx context.Context) ([]StorefrontText, []NodeHeaderRaises, error) {
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead, AccessMode: pgx.ReadOnly})
	if err != nil {
		return nil, nil, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	rows, err := tx.Query(ctx, `SELECT node, character_id, description FROM market_storefronts ORDER BY node, character_id`)
	if err != nil {
		return nil, nil, err
	}
	var texts []StorefrontText
	for rows.Next() {
		var t StorefrontText
		if err := rows.Scan(&t.Node, &t.SellerID, &t.Description); err != nil {
			rows.Close()
			return nil, nil, err
		}
		texts = append(texts, t)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, nil, err
	}
	rows, err = tx.Query(ctx, `SELECT node, header_raises FROM node_market_headers WHERE header_raises > 0 ORDER BY node`)
	if err != nil {
		return nil, nil, err
	}
	var raises []NodeHeaderRaises
	for rows.Next() {
		var r NodeHeaderRaises
		if err := rows.Scan(&r.Node, &r.Raises); err != nil {
			rows.Close()
			return nil, nil, err
		}
		raises = append(raises, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, nil, err
	}
	return texts, raises, tx.Commit(ctx)
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"fmt"
	"sort"

	"github.com/jackc/pgx/v5/pgconn"
)

// The wallet (the vendors milestone, step 0, migration 0012): the money a character carries, one row per currency in character_wallet. A currency is named by its BASE
// currency def's Guid, and its amount is in that unit (copper for money). worldd keeps the wallet while the character plays and saves it with the items.

const (
	// CopperCurrency is the base unit of money (the design database's Copper def): 100 copper = 1 silver, 100 silver = 1 gold.
	CopperCurrency int64 = 6064629132706054144
	// StartPurseCopper is the start purse of a test server (AOC_GM_STARTER_SET=1, SetStartPurse): 21 gold, for testing and development only.
	StartPurseCopper int64 = 210_000
	// MaxWalletAmount is the largest balance any currency can hold (the table's constraint); worldd holds the smaller cap of each currency.
	MaxWalletAmount int64 = 10_000_000_000_000
	// maxWalletEntries bounds one save: the design data has 27 currency defs.
	maxWalletEntries = 64
)

// WalletEntry is one currency of a character's wallet.
type WalletEntry struct {
	CurrencyGuid int64
	Amount       int64
}

// ValidateWallet checks the entries of a save: every currency named once, a Guid above 0, an amount from 0 to MaxWalletAmount.
func ValidateWallet(entries []WalletEntry) error {
	if len(entries) > maxWalletEntries {
		return fmt.Errorf("%w: a wallet of %d currencies (at most %d)", ErrInvalid, len(entries), maxWalletEntries)
	}
	seen := make(map[int64]bool, len(entries))
	for i, e := range entries {
		if e.CurrencyGuid <= 0 {
			return fmt.Errorf("%w: wallet entry %d names currency %d", ErrInvalid, i, e.CurrencyGuid)
		}
		if e.Amount < 0 || e.Amount > MaxWalletAmount {
			return fmt.Errorf("%w: wallet entry %d (currency %d) holds %d, outside 0 to %d", ErrInvalid, i, e.CurrencyGuid, e.Amount, MaxWalletAmount)
		}
		if seen[e.CurrencyGuid] {
			return fmt.Errorf("%w: wallet entry %d names currency %d again", ErrInvalid, i, e.CurrencyGuid)
		}
		seen[e.CurrencyGuid] = true
	}
	return nil
}

// SetStartPurse sets the copper a character with no wallet yet is given, once: StartPurseCopper on a test server (AOC_GM_STARTER_SET=1), 0 (the default) everywhere else.
func (s *Store) SetStartPurse(copper int64) {
	if copper < 0 {
		copper = 0
	}
	s.startPurse = copper
}

// Wallet returns the character's wallet by currency Guid. A character with no wallet row at all is given the start purse first (once: the rows exist from then on, even
// when it spends everything; the start purse is empty unless SetStartPurse gave it some copper). An unknown character is ErrNotFound.
func (s *Store) Wallet(ctx context.Context, characterID string) ([]WalletEntry, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	// the purse goes in only when the character exists and has no row; two readers at once insert the same row and the second one is a no-op
	if _, err := s.pool.Exec(ctx, `INSERT INTO character_wallet (character_id, currency_guid, amount)
		SELECT $1::text, $2::bigint, $3::bigint
		WHERE EXISTS (SELECT 1 FROM characters WHERE id = $1) AND NOT EXISTS (SELECT 1 FROM character_wallet WHERE character_id = $1)
		ON CONFLICT DO NOTHING`, characterID, CopperCurrency, s.startPurse); err != nil {
		return nil, err
	}
	rows, err := s.pool.Query(ctx, `SELECT currency_guid, amount FROM character_wallet WHERE character_id = $1 ORDER BY currency_guid`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []WalletEntry
	for rows.Next() {
		var e WalletEntry
		if err := rows.Scan(&e.CurrencyGuid, &e.Amount); err != nil {
			return nil, err
		}
		out = append(out, e)
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

// SaveInventoryAndWallet is SaveInventory plus the wallet, in the same transaction: every currency named is set to its amount, a currency not named is left as it is. The
// wallet is checked (ValidateWallet) before anything is written; the items are checked as SaveInventory does.
func (s *Store) SaveInventoryAndWallet(ctx context.Context, characterID string, items []Item, wallet []WalletEntry) error {
	if !idPattern.MatchString(characterID) {
		return ErrNotFound
	}
	if err := ValidateWallet(wallet); err != nil {
		return err
	}
	return s.saveInventory(ctx, characterID, items, wallet, nil, false, nil, false)
}

// txExecer is what upsertWallet needs of a transaction.
type txExecer interface {
	Exec(ctx context.Context, sql string, args ...any) (pgconn.CommandTag, error)
}

// upsertWallet sets the entries of a character's wallet in tx.
func upsertWallet(ctx context.Context, tx txExecer, characterID string, wallet []WalletEntry) error {
	sorted := append([]WalletEntry(nil), wallet...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].CurrencyGuid < sorted[j].CurrencyGuid })
	for _, e := range sorted {
		if _, err := tx.Exec(ctx, `INSERT INTO character_wallet (character_id, currency_guid, amount) VALUES ($1, $2, $3)
			ON CONFLICT (character_id, currency_guid) DO UPDATE SET amount = EXCLUDED.amount, updated_at = now()`, characterID, e.CurrencyGuid, e.Amount); err != nil {
			return itemsError(err)
		}
	}
	return nil
}

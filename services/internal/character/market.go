// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Marketplace (migration 0030): the listings a player registers at a Marketplace Manager. A listing is written in the same transaction as the seller's bags,
// wallet, jobs, recipes and warehouse, so the listed units leave the bags exactly when the listing is stored; a refused listing changes nothing. Every listed row
// is read with its seller's name once, when worldd starts.

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"github.com/jackc/pgx/v5"
)

// ErrListingSaved answers SaveInventoryAndListing for a listing id the seller already has: an earlier call stored the listing, so nothing is written (the transaction is rolled
// back). worldd asks for the same save again after an answer it could not read, and takes this answer as "saved".
var ErrListingSaved = errors.New("market listing already saved")

// MarketStatusListed is the client's status of a live listing; only a listed row is read.
const MarketStatusListed int32 = 0

// MarketListing is one listing: its GUID, the seller, the market's node, the listed stack (the item's own data; its container and slot are not used), the unit
// price in copper, the status and the issue and expiry times in Unix milliseconds. SellerName is filled by MarketListings.
type MarketListing struct {
	ID           string // 32 lowercase hex digits, not all zero
	SellerID     string // the seller's character id (32 lowercase hex digits)
	SellerName   string // the seller's name, read from the characters row
	Node         int64
	Item         Item
	PricePerUnit int64
	Status       int32
	IssuedAtMs   int64
	ExpiresAtMs  int64
}

// listingItem is the listed stack as the item's own checks see it: a listing has no container or slot, so the storage is the backpack's first slot, which the
// checks accept for every listing.
func listingItem(it Item) Item {
	it.Container = Backpack
	it.Slot = 0
	return it
}

// ValidateMarketListing checks one listing: its id, the seller's id, the node, the stack, the price, the status and the times.
func ValidateMarketListing(l MarketListing) error {
	if !guidPattern.MatchString(l.ID) || l.ID == strings.Repeat("0", 32) {
		return fmt.Errorf("%w: listing id %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, l.ID)
	}
	if !idPattern.MatchString(l.SellerID) {
		return fmt.Errorf("%w: seller id %q is not 32 lowercase hex digits", ErrInvalid, l.SellerID)
	}
	if l.Node == 0 {
		return fmt.Errorf("%w: the market node is 0", ErrInvalid)
	}
	if err := listingItem(l.Item).Validate(); err != nil {
		return fmt.Errorf("the listed stack: %w", err)
	}
	if l.PricePerUnit <= 0 {
		return fmt.Errorf("%w: the unit price is %d copper, not above 0", ErrInvalid, l.PricePerUnit)
	}
	if l.Status != MarketStatusListed {
		return fmt.Errorf("%w: status %d is not LISTED", ErrInvalid, l.Status)
	}
	if l.IssuedAtMs <= 0 {
		return fmt.Errorf("%w: issue time %d is not Unix milliseconds after 0", ErrInvalid, l.IssuedAtMs)
	}
	if l.ExpiresAtMs < 0 {
		return fmt.Errorf("%w: expiry %d is negative", ErrInvalid, l.ExpiresAtMs)
	}
	return nil
}

// SaveInventoryAndListing writes, in ONE transaction, the seller's whole bags, wallet, processing jobs, known recipes and warehouse exactly as SaveInventoryEverything
// does (the same checks; a save of no items is refused as there) AND the listing. The listed units leave the bags and the listing is stored together or not at all, and a
// listing the rules refuse changes nothing. ErrNotFound for an unknown character; ErrInvalid, with nothing changed, for a bag or a listing the rules refuse, the listed
// stack being in the bags or the warehouse included; ErrListingSaved, with nothing changed, for a listing id the seller already has.
func (s *Store) SaveInventoryAndListing(ctx context.Context, characterID string, items []Item, wallet []WalletEntry, jobs []ProcessingJob, known []int64,
	warehouse []WarehouseItem, listing MarketListing) error {
	if !idPattern.MatchString(characterID) {
		return ErrNotFound
	}
	listing.SellerID = characterID
	if err := ValidateMarketListing(listing); err != nil {
		return err
	}
	for _, it := range items {
		if it.InstanceGUID == listing.Item.InstanceGUID {
			return fmt.Errorf("%w: the listed stack %s is still in the bags", ErrInvalid, it.InstanceGUID)
		}
	}
	for _, w := range warehouse {
		if w.InstanceGUID == listing.Item.InstanceGUID {
			return fmt.Errorf("%w: the listed stack %s is in the warehouse", ErrInvalid, w.InstanceGUID)
		}
	}
	if err := checkInventoryParts(items, wallet, true, jobs, true, known, true, warehouse, true); err != nil {
		return err
	}
	return s.saveInventory(ctx, characterID, items, wallet, jobs, true, known, true, warehouse, true, &listing)
}

// MarketListings reads every listed row with its seller's name, oldest first by issue time with the id breaking a tie. The read is one transaction, one
// snapshot.
func (s *Store) MarketListings(ctx context.Context) ([]MarketListing, error) {
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	rows, err := tx.Query(ctx, `SELECT l.id, l.character_id, c.name, l.market_node, l.item_record_id, l.instance_guid, l.quantity, l.quality,
		l.durability, l.created_at, l.persisted_health, l.persisted_mana, l.lock_expiration, l.price_per_unit, l.status, l.issue_date_ms, l.expiry_date_ms
		FROM character_market_listings l JOIN characters c ON c.id = l.character_id
		WHERE l.status = $1 ORDER BY l.issue_date_ms, l.id`, MarketStatusListed)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []MarketListing
	for rows.Next() {
		var l MarketListing
		if err := rows.Scan(&l.ID, &l.SellerID, &l.SellerName, &l.Node, &l.Item.ItemRecordID, &l.Item.InstanceGUID, &l.Item.Quantity, &l.Item.Quality,
			&l.Item.Durability, &l.Item.CreatedAt, &l.Item.PersistedHealth, &l.Item.PersistedMana, &l.Item.LockExpiration, &l.PricePerUnit, &l.Status,
			&l.IssuedAtMs, &l.ExpiresAtMs); err != nil {
			return nil, err
		}
		out = append(out, l)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return out, tx.Commit(ctx)
}

// listingSaved says whether the seller already has the listing `id`.
func listingSaved(ctx context.Context, tx pgx.Tx, characterID, id string) (bool, error) {
	var saved bool
	err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM character_market_listings WHERE id = $1 AND character_id = $2)`, id, characterID).Scan(&saved)
	return saved, err
}

// insertMarketListing adds one listing in tx, after the seller's bags were replaced there.
func insertMarketListing(ctx context.Context, tx pgx.Tx, characterID string, l MarketListing) error {
	_, err := tx.Exec(ctx, `INSERT INTO character_market_listings (id, character_id, market_node, item_record_id, instance_guid, quantity, quality, durability,
		created_at, persisted_health, persisted_mana, lock_expiration, price_per_unit, status, issue_date_ms, expiry_date_ms)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16)`,
		l.ID, characterID, l.Node, l.Item.ItemRecordID, l.Item.InstanceGUID, l.Item.Quantity, l.Item.Quality, l.Item.Durability, l.Item.CreatedAt,
		l.Item.PersistedHealth, l.Item.PersistedMana, l.Item.LockExpiration, l.PricePerUnit, l.Status, l.IssuedAtMs, l.ExpiresAtMs)
	return err
}

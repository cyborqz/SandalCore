// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Player trading (migration 0032): atomic two-character inventory, wallet, processing jobs,
// known recipes and warehouse swap in ONE transaction with trade_id idempotency.

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"github.com/jackc/pgx/v5"
)

// MaxTradeCopper is the maximum copper that can be transferred on one side of a player trade (1,000 gold).
const MaxTradeCopper int64 = 10_000_000

// ErrTradeCommitted is returned by CommitPlayerTrade when trade_id already exists for a different pair of characters.
var ErrTradeCommitted = errors.New("trade id already committed for other characters")

// TradeSide is one participant's character id and post-trade persisted snapshot.
type TradeSide struct {
	ID   string
	Bags ClaimBags
}

// CommitTradeRequest describes an atomic player-to-player trade commit.
type CommitTradeRequest struct {
	TradeID       string
	SideA         TradeSide
	SideB         TradeSide
	CopperAToB    int64
	CopperBToA    int64
	ItemsAToB     int32
	ItemsBToA     int32
	CommittedAtMs int64
}

// CommitTradeResult reports whether the trade was replayed from an earlier commit of the same pair.
type CommitTradeResult struct {
	Replayed bool
}

func validateTradeSide(label string, side TradeSide) error {
	if !idPattern.MatchString(side.ID) {
		return fmt.Errorf("%w: %s id %q is not 32 lowercase hex digits", ErrInvalid, label, side.ID)
	}
	// Empty post-trade items are explicitly allowed so a player can gift their last item.
	if err := ValidateItems(side.Bags.Items); err != nil {
		return fmt.Errorf("%s: %w", label, err)
	}
	if side.Bags.ReplaceWallet {
		if err := ValidateWallet(side.Bags.Wallet); err != nil {
			return fmt.Errorf("%s: %w", label, err)
		}
	}
	if side.Bags.ReplaceJobs {
		if err := ValidateJobs(side.Bags.Jobs); err != nil {
			return fmt.Errorf("%s: %w", label, err)
		}
	}
	if side.Bags.ReplaceKnown {
		if err := ValidateKnownRecipes(side.Bags.KnownRecipes); err != nil {
			return fmt.Errorf("%s: %w", label, err)
		}
	}
	if side.Bags.ReplaceWarehouse {
		if err := ValidateWarehouseItems(side.Bags.Warehouse, side.Bags.Items); err != nil {
			return fmt.Errorf("%s: %w", label, err)
		}
	}
	return nil
}

func collectSideInstanceGUIDs(side TradeSide) map[string]struct{} {
	guids := make(map[string]struct{}, len(side.Bags.Items)+len(side.Bags.Warehouse))
	for _, it := range side.Bags.Items {
		guids[it.InstanceGUID] = struct{}{}
	}
	if side.Bags.ReplaceWarehouse {
		for _, w := range side.Bags.Warehouse {
			guids[w.InstanceGUID] = struct{}{}
		}
	}
	return guids
}

func checkStoredWarehouseOverlap(ctx context.Context, tx pgx.Tx, ownerID, otherLabel string, otherGUIDs map[string]struct{}) error {
	if len(otherGUIDs) == 0 {
		return nil
	}
	rows, err := tx.Query(ctx, `SELECT instance_guid FROM character_warehouse_items WHERE character_id = $1`, ownerID)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var g string
		if err := rows.Scan(&g); err != nil {
			return err
		}
		if _, dup := otherGUIDs[g]; dup {
			return fmt.Errorf("%w: instance guid %s is present on both characters (%s and stored warehouse)", ErrInvalid, g, otherLabel)
		}
	}
	return rows.Err()
}

// CommitPlayerTrade atomically commits a two-character trade in one PostgreSQL transaction:
//   - locks both characters in canonical id order to prevent deadlocks on concurrent opposite-order swaps
//   - checks character_trades for trade_id idempotency (replayed for the same unordered pair without
//     rewriting bags, ErrTradeCommitted for any other pair)
//   - replaces both characters' inventories, wallets, jobs, known recipes and warehouses (allowing an
//     empty post-trade inventory when a player gifts their last item)
//   - inserts the character_trades ledger row
func (s *Store) CommitPlayerTrade(ctx context.Context, req CommitTradeRequest) (CommitTradeResult, error) {
	zero := strings.Repeat("0", 32)
	if !guidPattern.MatchString(req.TradeID) || req.TradeID == zero {
		return CommitTradeResult{}, fmt.Errorf("%w: trade id %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, req.TradeID)
	}
	if err := validateTradeSide("side_a", req.SideA); err != nil {
		return CommitTradeResult{}, err
	}
	if err := validateTradeSide("side_b", req.SideB); err != nil {
		return CommitTradeResult{}, err
	}
	if req.SideA.ID == req.SideB.ID {
		return CommitTradeResult{}, fmt.Errorf("%w: a character cannot trade with itself (%s)", ErrInvalid, req.SideA.ID)
	}
	if req.CopperAToB < 0 || req.CopperAToB > MaxTradeCopper {
		return CommitTradeResult{}, fmt.Errorf("%w: copper_a_to_b %d is outside 0 to %d", ErrInvalid, req.CopperAToB, MaxTradeCopper)
	}
	if req.CopperBToA < 0 || req.CopperBToA > MaxTradeCopper {
		return CommitTradeResult{}, fmt.Errorf("%w: copper_b_to_a %d is outside 0 to %d", ErrInvalid, req.CopperBToA, MaxTradeCopper)
	}
	if req.ItemsAToB < 0 || req.ItemsBToA < 0 {
		return CommitTradeResult{}, fmt.Errorf("%w: transferred item counts cannot be negative (%d, %d)", ErrInvalid, req.ItemsAToB, req.ItemsBToA)
	}
	if req.CopperAToB == 0 && req.CopperBToA == 0 && req.ItemsAToB == 0 && req.ItemsBToA == 0 {
		return CommitTradeResult{}, fmt.Errorf("%w: trade transfers neither items nor copper", ErrInvalid)
	}
	if req.CommittedAtMs <= 0 {
		return CommitTradeResult{}, fmt.Errorf("%w: committed_at_ms %d must be positive", ErrInvalid, req.CommittedAtMs)
	}

	guidsA := collectSideInstanceGUIDs(req.SideA)
	guidsB := collectSideInstanceGUIDs(req.SideB)
	for g := range guidsB {
		if _, dup := guidsA[g]; dup {
			return CommitTradeResult{}, fmt.Errorf("%w: instance guid %s is present on both characters", ErrInvalid, g)
		}
	}

	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return CommitTradeResult{}, err
	}
	defer tx.Rollback(ctx)

	first, second := req.SideA.ID, req.SideB.ID
	if second < first {
		first, second = second, first
	}
	if err := lockCharacter(ctx, tx, first); err != nil {
		return CommitTradeResult{}, err
	}
	if err := lockCharacter(ctx, tx, second); err != nil {
		return CommitTradeResult{}, err
	}

	var existingA, existingB string
	err = tx.QueryRow(ctx, `SELECT char_a_id, char_b_id FROM character_trades WHERE id = $1`, req.TradeID).Scan(&existingA, &existingB)
	if err == nil {
		samePair := (existingA == req.SideA.ID && existingB == req.SideB.ID) ||
			(existingA == req.SideB.ID && existingB == req.SideA.ID)
		if !samePair {
			return CommitTradeResult{}, ErrTradeCommitted
		}
		if err := tx.Commit(ctx); err != nil {
			return CommitTradeResult{}, err
		}
		return CommitTradeResult{Replayed: true}, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return CommitTradeResult{}, err
	}

	if !req.SideA.Bags.ReplaceWarehouse {
		if err := checkStoredWarehouseOverlap(ctx, tx, req.SideA.ID, "side_b", guidsB); err != nil {
			return CommitTradeResult{}, err
		}
	}
	if !req.SideB.Bags.ReplaceWarehouse {
		if err := checkStoredWarehouseOverlap(ctx, tx, req.SideB.ID, "side_a", guidsA); err != nil {
			return CommitTradeResult{}, err
		}
	}

	writeSide := func(side TradeSide) error {
		var wallet []WalletEntry
		if side.Bags.ReplaceWallet {
			wallet = side.Bags.Wallet
		}
		return writeInventory(ctx, tx, side.ID, side.Bags.Items, wallet, side.Bags.Jobs, side.Bags.ReplaceJobs,
			side.Bags.KnownRecipes, side.Bags.ReplaceKnown, side.Bags.Warehouse, side.Bags.ReplaceWarehouse)
	}
	if err := writeSide(req.SideA); err != nil {
		return CommitTradeResult{}, err
	}
	if err := writeSide(req.SideB); err != nil {
		return CommitTradeResult{}, err
	}

	if _, err := tx.Exec(ctx, `INSERT INTO character_trades
		(id, char_a_id, char_b_id, copper_a_to_b, copper_b_to_a, items_a_to_b, items_b_to_a, committed_at_ms)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`,
		req.TradeID, req.SideA.ID, req.SideB.ID, req.CopperAToB, req.CopperBToA, req.ItemsAToB, req.ItemsBToA, req.CommittedAtMs); err != nil {
		return CommitTradeResult{}, itemsError(err)
	}

	if err := tx.Commit(ctx); err != nil {
		return CommitTradeResult{}, err
	}
	return CommitTradeResult{Replayed: false}, nil
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Marketplace (migration 0030): one listing is written with the seller's bags, wallet, jobs, recipes and warehouse in one transaction, so the listed units leave
// the bags exactly when the listing is stored; a refused listing changes nothing, an unknown character is ErrNotFound, and every listed row is read with its
// seller's name. The table is migration0030_test.go.

import (
	"context"
	"errors"
	"testing"
	"time"
)

const marketNode int64 = 108646121231993

// marketListingOf is a valid listing of the stack `it` at a node, for a test to change one field of.
func marketListingOf(it Item) MarketListing {
	return MarketListing{ID: guid(0x9001), Node: marketNode, Item: it, PricePerUnit: 10000, Status: MarketStatusListed,
		IssuedAtMs: 1_771_000_000_000, ExpiresAtMs: 1_771_259_200_000}
}

func TestSaveInventoryAndListingWritesTheBagsTheWalletAndTheListing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "seller", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	kit := smallKit()
	listed := kit[0] // the bag's stack
	saved := kit[1:]
	listing := marketListingOf(listed)
	const purse = 12_345
	if err := s.SaveInventoryAndListing(ctx, c.ID, saved, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: purse}}, nil, nil, nil, listing); err != nil {
		t.Fatalf("SaveInventoryAndListing: %v", err)
	}
	got, err := s.Inventory(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != len(saved) || got[0].InstanceGUID != saved[0].InstanceGUID || got[1].InstanceGUID != saved[1].InstanceGUID || got[2].InstanceGUID != saved[2].InstanceGUID {
		t.Errorf("the bags hold %d items after the save, want the %d that were sent without the listed stack", len(got), len(saved))
	}
	for _, it := range got {
		if it.InstanceGUID == listed.InstanceGUID {
			t.Errorf("the listed stack %s is still in the bags", listed.InstanceGUID)
		}
	}
	wallet, err := s.Wallet(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(wallet) != 1 || wallet[0].CurrencyGuid != CopperCurrency || wallet[0].Amount != purse {
		t.Errorf("the wallet is %+v, want the %d copper the save carried", wallet, purse)
	}
	var (
		node, price, status, issued, expires int64
		itemGUID                             string
		quantity, quality                    int32
	)
	if err := s.pool.QueryRow(ctx, `SELECT market_node, instance_guid, quantity, quality, price_per_unit, status, issue_date_ms, expiry_date_ms
		FROM character_market_listings WHERE id = $1 AND character_id = $2`, listing.ID, c.ID).Scan(&node, &itemGUID, &quantity, &quality, &price, &status,
		&issued, &expires); err != nil {
		t.Fatalf("the listing is not stored: %v", err)
	}
	if node != marketNode || itemGUID != listed.InstanceGUID || quantity != listed.Quantity || quality != listed.Quality || price != listing.PricePerUnit ||
		status != int64(MarketStatusListed) || issued != listing.IssuedAtMs || expires != listing.ExpiresAtMs {
		t.Errorf("the stored listing is node %d, item %s x%d quality %d, price %d, status %d, %d to %d; want %+v", node, itemGUID, quantity, quality, price,
			status, issued, expires, listing)
	}
}

func TestSaveInventoryAndListingRefusalsChangeNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "seller", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	before, err := s.Inventory(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.SaveInventory(ctx, c.ID, before); err != nil { // the wallet row exists from here on
		t.Fatal(err)
	}
	if _, err := s.pool.Exec(ctx, `UPDATE character_wallet SET amount = 500 WHERE character_id = $1`, c.ID); err != nil {
		t.Fatal(err)
	}
	changed := append([]Item(nil), before[1:]...) // the potion leaves the bags for the listing
	changed[0].Quantity = 7
	changedWallet := []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 0}}
	kit := smallKit()
	listed := kit[0]

	badPrice := marketListingOf(listed)
	badPrice.PricePerUnit = 0
	if err := s.SaveInventoryAndListing(ctx, c.ID, changed, changedWallet, nil, nil, nil, badPrice); !errors.Is(err, ErrInvalid) {
		t.Errorf("a listing with a zero price: %v, want ErrInvalid", err)
	}
	stillHeld := marketListingOf(listed)
	if err := s.SaveInventoryAndListing(ctx, c.ID, before, changedWallet, nil, nil, nil, stillHeld); !errors.Is(err, ErrInvalid) {
		t.Errorf("a listing whose stack is still in the bags: %v, want ErrInvalid", err)
	}
	if err := s.SaveInventoryAndListing(ctx, c.ID, changed, changedWallet, nil, nil, nil, marketListingOf(listed)); err != nil {
		t.Fatalf("the valid listing: %v", err)
	}
	// from here the valid listing and the changed bag and purse are stored; each following refusal must leave them as they are
	after, err := s.Inventory(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	wallet, err := s.Wallet(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name    string
		items   []Item
		wallet  []WalletEntry
		listing MarketListing
		want    error
	}{
		{"no items", nil, changedWallet, marketListingOf(listed), ErrInvalid},
		{"two items with one instance guid", append(append([]Item(nil), after...), item(Backpack, 9, 7001, after[0].InstanceGUID)), changedWallet,
			marketListingOf(listed), ErrInvalid},
		{"a listing the rules refuse", after, changedWallet, func() MarketListing { l := marketListingOf(listed); l.ID = "not a guid"; return l }(), ErrInvalid},
		{"a wallet the rules refuse", after, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: -1}}, marketListingOf(listed), ErrInvalid},
	} {
		if err := s.SaveInventoryAndListing(ctx, c.ID, tc.items, tc.wallet, nil, nil, nil, tc.listing); !errors.Is(err, tc.want) {
			t.Errorf("%s: %v, want %v", tc.name, err, tc.want)
		}
		got, err := s.Inventory(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		if len(got) != len(after) || got[0].Quantity != after[0].Quantity || got[0].InstanceGUID != after[0].InstanceGUID {
			t.Errorf("%s changed the bags", tc.name)
		}
		gotWallet, err := s.Wallet(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		if len(gotWallet) != 1 || gotWallet[0].Amount != wallet[0].Amount {
			t.Errorf("%s changed the purse: %+v", tc.name, gotWallet)
		}
		var rows int
		if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_market_listings WHERE character_id = $1`, c.ID).Scan(&rows); err != nil || rows != 1 {
			t.Errorf("%s changed the listings: %d rows, %v; want the one that was stored", tc.name, rows, err)
		}
	}
}

func TestSaveInventoryAndListingUnknownCharacter(t *testing.T) {
	s, _ := setup(t)
	ctx := context.Background()
	kit := smallKit()
	err := s.SaveInventoryAndListing(ctx, "ffffffffffffffffffffffffffffffff", kit[1:], nil, nil, nil, nil, marketListingOf(kit[0]))
	if !errors.Is(err, ErrNotFound) {
		t.Errorf("an unknown character: %v, want ErrNotFound", err)
	}
}

func TestMarketListingsReadsTheListedRowsWithTheirSellerName(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	first, err := s.Create(ctx, New{AccountID: acc, Name: "first", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	second, err := s.Create(ctx, New{AccountID: acc, Name: "second", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	kit := smallKit()
	one := kit[0]
	firstListing, secondListing := marketListingOf(one), marketListingOf(kit[1])
	firstListing.IssuedAtMs = 1_771_000_000_100
	secondListing.ID = guid(0x9002)
	secondListing.IssuedAtMs = 1_771_000_000_200
	if err := s.SaveInventoryAndListing(ctx, first.ID, kit[1:], nil, nil, nil, nil, firstListing); err != nil {
		t.Fatal(err)
	}
	withOutSecond := append([]Item{kit[0]}, kit[2:]...)
	if err := s.SaveInventoryAndListing(ctx, second.ID, withOutSecond, nil, nil, nil, nil, secondListing); err != nil {
		t.Fatal(err)
	}
	// an ended listing is not read: it keeps its row, as a delisted listing does.
	if _, err := s.pool.Exec(ctx, `INSERT INTO character_market_listings (id, character_id, market_node, item_record_id, instance_guid, quantity, quality,
		durability, price_per_unit, status, issue_date_ms, expiry_date_ms) VALUES ($1, $2, $3, 7001, $4, 1, 1, 0, 100, 1, 1771000000000, 0)`,
		guid(0x9003), first.ID, marketNode, guid(0x9103)); err != nil {
		t.Fatal(err)
	}
	listings, err := s.MarketListings(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(listings) != 2 {
		t.Fatalf("MarketListings returned %d listings, want the 2 listed rows (a status 1 row is not listed)", len(listings))
	}
	if listings[0].ID != firstListing.ID || listings[0].SellerID != first.ID || listings[0].SellerName != "first" ||
		listings[1].ID != secondListing.ID || listings[1].SellerID != second.ID || listings[1].SellerName != "second" {
		t.Errorf("the listings are %+v, want first's then second's with their names", listings)
	}
	if listings[0].Item.InstanceGUID != one.InstanceGUID || listings[0].Node != marketNode || listings[0].PricePerUnit != firstListing.PricePerUnit ||
		listings[0].IssuedAtMs != firstListing.IssuedAtMs || listings[0].ExpiresAtMs != firstListing.ExpiresAtMs {
		t.Errorf("the read listing lost a field: %+v", listings[0])
	}
}

// The listing's save makes every check the bag save makes (SaveInventoryEverything), and a listed instance that is in the warehouse is refused: a refusal changes nothing,
// the bags, the purse, the jobs, the recipes, the warehouse and the listings included.
func TestSaveInventoryAndListingRunsTheChecksOfTheBagSave(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "seller", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	kit := smallKit()
	listed := kit[0]
	bags := kit[1:]
	purse := []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 500}}
	jobs := []ProcessingJob{aJob(1)}
	known := []int64{101, 102}
	stored := []WarehouseItem{wh(WarehouseItems, 0, baseChunk, 1, 7001, 40, 2)}
	if err := s.SaveInventoryEverything(ctx, c.ID, kit, purse, true, jobs, true, known, true, stored, true); err != nil {
		t.Fatal(err)
	}
	startedBeforeQueued := aJob(1)
	startedBeforeQueued.StartedAt = startedBeforeQueued.QueuedAt.Add(-time.Second)
	listedInWarehouse := append([]WarehouseItem(nil), stored...)
	listedInWarehouse[0].InstanceGUID = listed.InstanceGUID
	sharedWithBags := append([]WarehouseItem(nil), stored...)
	sharedWithBags[0].InstanceGUID = bags[0].InstanceGUID
	for _, tc := range []struct {
		name      string
		jobs      []ProcessingJob
		known     []int64
		warehouse []WarehouseItem
	}{
		{"a job the checks refuse", []ProcessingJob{startedBeforeQueued}, known, stored},
		{"two jobs with one guid", []ProcessingJob{aJob(1), aJob(1)}, known, stored},
		{"a recipe known twice", jobs, []int64{101, 101}, stored},
		{"a recipe that is no record", jobs, []int64{101, 0}, stored},
		{"a warehouse item that holds an instance of the bags", jobs, known, sharedWithBags},
		{"the listed instance in the warehouse", jobs, known, listedInWarehouse},
	} {
		err := s.SaveInventoryAndListing(ctx, c.ID, bags, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 0}}, tc.jobs, tc.known, tc.warehouse, marketListingOf(listed))
		if !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", tc.name, err)
		}
		gotBags, err := s.Inventory(ctx, c.ID)
		if err != nil || len(gotBags) != len(kit) {
			t.Errorf("%s changed the bags: %d items, %v; want the %d stored", tc.name, len(gotBags), err, len(kit))
		}
		if wallet, err := s.Wallet(ctx, c.ID); err != nil || len(wallet) != 1 || wallet[0].Amount != 500 {
			t.Errorf("%s changed the purse: %+v, %v", tc.name, wallet, err)
		}
		if gotJobs, err := s.ProcessingJobs(ctx, c.ID); err != nil || len(gotJobs) != 1 || gotJobs[0].JobGUID != jobs[0].JobGUID {
			t.Errorf("%s changed the jobs: %+v, %v", tc.name, gotJobs, err)
		}
		if gotKnown, err := s.KnownRecipes(ctx, c.ID); err != nil || len(gotKnown) != 2 || gotKnown[0] != 101 || gotKnown[1] != 102 {
			t.Errorf("%s changed the recipes: %v, %v", tc.name, gotKnown, err)
		}
		sameWarehouse(t, tc.name+" changed the warehouse", mustWarehouse(t, s, c.ID), stored)
		var rows int
		if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_market_listings WHERE character_id = $1`, c.ID).Scan(&rows); err != nil || rows != 0 {
			t.Errorf("%s stored a listing: %d rows, %v", tc.name, rows, err)
		}
	}
	// the same call with nothing broken is stored: the checks are the bag save's, no stricter
	if err := s.SaveInventoryAndListing(ctx, c.ID, bags, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 400}}, jobs, known, stored, marketListingOf(listed)); err != nil {
		t.Errorf("the listing with valid jobs, recipes and warehouse: %v", err)
	}
}

func mustWarehouse(t *testing.T, s *Store, id string) []WarehouseItem {
	t.Helper()
	got, err := s.WarehouseItems(context.Background(), id)
	if err != nil {
		t.Fatal(err)
	}
	return got
}

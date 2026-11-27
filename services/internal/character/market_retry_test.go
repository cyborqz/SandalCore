// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Marketplace (migration 0030): a listing's save that is asked for again after an answer worldd could not read. A listing id the seller already has is answered "already
// saved" (ErrListingSaved) with nothing changed, so a try after a save that did commit does not give the units back or write older bags over newer ones.

import (
	"context"
	"errors"
	"testing"
)

func TestSaveInventoryAndListingAskedForTwiceIsAlreadySaved(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "seller", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	kit := smallKit()
	listed := kit[0]
	bags := kit[1:]
	listing := marketListingOf(listed)
	purse := []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 12_345}}
	if err := s.SaveInventoryAndListing(ctx, c.ID, bags, purse, nil, nil, nil, listing); err != nil {
		t.Fatalf("the first save: %v", err)
	}
	// a later state of the bags and the purse (what the seller did after the listing) must not be written by the try again either
	later := append([]Item(nil), bags...)
	later[0].Quantity = 9
	for _, tc := range []struct {
		name   string
		items  []Item
		wallet []WalletEntry
	}{
		{"the same call", bags, purse},
		{"other bags and another purse", later, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1}}},
	} {
		err := s.SaveInventoryAndListing(ctx, c.ID, tc.items, tc.wallet, nil, nil, nil, listing)
		if !errors.Is(err, ErrListingSaved) {
			t.Errorf("%s: %v, want ErrListingSaved", tc.name, err)
		}
		if errors.Is(err, ErrInvalid) {
			t.Errorf("%s: the answer is a refusal (ErrInvalid), which would give the units back although the listing is stored", tc.name)
		}
		got, err := s.Inventory(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		if len(got) != len(bags) || got[0].InstanceGUID != bags[0].InstanceGUID || got[0].Quantity != bags[0].Quantity {
			t.Errorf("%s changed the bags: %+v", tc.name, got)
		}
		wallet, err := s.Wallet(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		if len(wallet) != 1 || wallet[0].Amount != 12_345 {
			t.Errorf("%s changed the purse: %+v", tc.name, wallet)
		}
		var rows int
		if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_market_listings WHERE character_id = $1`, c.ID).Scan(&rows); err != nil || rows != 1 {
			t.Errorf("%s changed the listings: %d rows, %v; want the one that was stored", tc.name, rows, err)
		}
	}
	// the listing id of another seller is no "already saved": it is refused, and nothing of that seller changes
	other, err := s.Create(ctx, New{AccountID: acc, Name: "other", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	err = s.SaveInventoryAndListing(ctx, other.ID, smallKit()[1:], nil, nil, nil, nil, listing)
	if !errors.Is(err, ErrInvalid) || errors.Is(err, ErrListingSaved) {
		t.Errorf("a listing id another seller holds: %v, want ErrInvalid and not ErrListingSaved", err)
	}
	if got, err := s.Inventory(ctx, other.ID); err != nil || len(got) != len(smallKit()) {
		t.Errorf("the refused listing changed the other seller's bags: %d items, %v", len(got), err)
	}
}

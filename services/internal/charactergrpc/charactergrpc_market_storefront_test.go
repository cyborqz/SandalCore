// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// Marketplace storefront descriptions (migration 0034) over gRPC: SetStorefrontDescription passes the seller, the node and the text to the store whole and maps every outcome
// and every store error; ListMarketListings carries the descriptions and the nodes' counts; a store without these parts answers Unimplemented and sends none. On the real store
// the whole path works: the text is stored with its node's count, a listing the seller does not have is refused with nothing written, and the read carries what was stored.

import (
	"context"
	"errors"
	"testing"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

// storefrontFake is the fake store that also keeps the storefront descriptions (the part of character.Store that StorefrontStore names) and records what it was given.
type storefrontFake struct {
	*marketFake
	setID    string
	setNode  int64
	setText  string
	setAsked int
	setRet   character.StorefrontResult
	setErr   error
	texts    []character.StorefrontText
	raises   []character.NodeHeaderRaises
	readErr  error
}

var _ StorefrontStore = (*storefrontFake)(nil)

func (f *storefrontFake) SetStorefrontDescription(_ context.Context, id string, node int64, text string) (character.StorefrontResult, error) {
	f.setAsked++
	f.setID, f.setNode, f.setText = id, node, text
	return f.setRet, f.setErr
}

func (f *storefrontFake) MarketStorefronts(context.Context) ([]character.StorefrontText, []character.NodeHeaderRaises, error) {
	return f.texts, f.raises, f.readErr
}

func newStorefrontFake() *storefrontFake {
	return &storefrontFake{marketFake: &marketFake{recStore: &recStore{ret: fullCharacter()}, listings: []character.MarketListing{wantMarketListing()}}}
}

func TestSetStorefrontDescriptionPassesTheSellerTheNodeAndTheTextAndMapsTheAnswer(t *testing.T) {
	f := newStorefrontFake()
	f.setRet = character.StorefrontResult{Outcome: character.StorefrontDone, Description: "  Fine goods ", HeaderRaises: 4}
	c := dialRec(t, f)
	resp, err := c.SetStorefrontDescription(context.Background(), &characterv1.SetStorefrontDescriptionRequest{CharacterId: mailProtoGUID, Node: 108646121231993, Description: "  Fine goods "})
	if err != nil {
		t.Fatal(err)
	}
	if resp.GetOutcome() != characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_DONE || resp.GetDescription() != "  Fine goods " || resp.GetHeaderRaises() != 4 {
		t.Errorf("the answer is %v, want done with the text as stored and a count of 4", resp)
	}
	if f.setAsked != 1 || f.setID != mailProtoGUID || f.setNode != 108646121231993 || f.setText != "  Fine goods " {
		t.Errorf("the store was asked %d times for %q at node %d with %q, want one ask with the seller, the node and the text exactly as sent", f.setAsked, f.setID, f.setNode, f.setText)
	}
}

func TestEveryOutcomeOfAnEditReachesTheClient(t *testing.T) {
	want := map[character.StorefrontOutcome]characterv1.StorefrontOutcome{
		character.StorefrontDone:      characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_DONE,
		character.StorefrontUnchanged: characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_UNCHANGED,
		character.StorefrontNoListing: characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_NO_LISTING,
		0:                             characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_UNSPECIFIED,
	}
	for outcome, wire := range want {
		f := newStorefrontFake()
		f.setRet = character.StorefrontResult{Outcome: outcome, Description: "x", HeaderRaises: 9}
		c := dialRec(t, f)
		resp, err := c.SetStorefrontDescription(context.Background(), &characterv1.SetStorefrontDescriptionRequest{CharacterId: mailProtoGUID, Node: 1, Description: "x"})
		if err != nil || resp.GetOutcome() != wire || resp.GetHeaderRaises() != 9 {
			t.Errorf("outcome %d = %v, %v; want %v with a count of 9", outcome, resp, err, wire)
		}
	}
}

func TestStorefrontStoreErrorsGetTheirCodes(t *testing.T) {
	for _, tc := range []struct {
		err  error
		want codes.Code
	}{
		{character.ErrInvalid, codes.InvalidArgument},
		{character.ErrNotFound, codes.NotFound},
		{errors.New("the database fell over"), codes.Internal},
	} {
		f := newStorefrontFake()
		f.setErr = tc.err
		c := dialRec(t, f)
		if _, err := c.SetStorefrontDescription(context.Background(), &characterv1.SetStorefrontDescriptionRequest{CharacterId: mailProtoGUID, Node: 1, Description: "x"}); status.Code(err) != tc.want {
			t.Errorf("SetStorefrontDescription with %v: %v, want %v", tc.err, err, tc.want)
		}
	}
}

func TestListMarketListingsCarriesTheDescriptionsAndTheCountsOfTheNodes(t *testing.T) {
	f := newStorefrontFake()
	f.texts = []character.StorefrontText{{Node: 108646121231993, SellerID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", Description: "Fine goods"}, {Node: 5, SellerID: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", Description: " x "}}
	f.raises = []character.NodeHeaderRaises{{Node: 5, Raises: 2}, {Node: 108646121231993, Raises: 7}}
	c := dialRec(t, f)
	list, err := c.ListMarketListings(context.Background(), &characterv1.ListMarketListingsRequest{})
	if err != nil || len(list.GetListings()) != 1 || len(list.GetStorefronts()) != 2 || len(list.GetNodeHeaders()) != 2 {
		t.Fatalf("ListMarketListings = %v, %v; want the listing, two descriptions and two counts", list, err)
	}
	if got := list.GetStorefronts()[0]; got.GetNode() != 108646121231993 || got.GetSellerId() != "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" || got.GetDescription() != "Fine goods" {
		t.Errorf("the first description is %v", got)
	}
	if got := list.GetStorefronts()[1]; got.GetDescription() != " x " {
		t.Errorf("the second description is %q, want it untrimmed", got.GetDescription())
	}
	if got := list.GetNodeHeaders()[1]; got.GetNode() != 108646121231993 || got.GetRaises() != 7 {
		t.Errorf("the second node's count is %v", got)
	}
	f.readErr = errors.New("the database fell over")
	if _, err := c.ListMarketListings(context.Background(), &characterv1.ListMarketListingsRequest{}); status.Code(err) != codes.Internal {
		t.Errorf("a failed read of the descriptions: %v, want Internal", err)
	}
}

func TestAStoreWithoutStorefrontsRefusesTheEditAndSendsNone(t *testing.T) {
	st := &marketFake{recStore: &recStore{ret: fullCharacter()}, listings: []character.MarketListing{wantMarketListing()}}
	c := dialRec(t, st)
	if _, err := c.SetStorefrontDescription(context.Background(), &characterv1.SetStorefrontDescriptionRequest{CharacterId: mailProtoGUID, Node: 1, Description: "x"}); status.Code(err) != codes.Unimplemented {
		t.Errorf("SetStorefrontDescription without storefronts: %v, want Unimplemented", err)
	}
	list, err := c.ListMarketListings(context.Background(), &characterv1.ListMarketListingsRequest{})
	if err != nil || len(list.GetListings()) != 1 || len(list.GetStorefronts()) != 0 || len(list.GetNodeHeaders()) != 0 {
		t.Errorf("ListMarketListings = %v, %v; want the listing and no description", list, err)
	}
}

// On the real store the whole path works through the service: a seller with a listing sets a text, the read carries it with its node's count, an edit of a seller with no
// listing is refused with nothing written, and a text the rules refuse is InvalidArgument.
func TestStorefrontsThroughTheServiceOnTheRealStore(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var accountID int64
	if err := pool.QueryRow(ctx, `INSERT INTO accounts (username, password_hash) VALUES ('Owner', 'x') RETURNING id`).Scan(&accountID); err != nil {
		t.Fatal(err)
	}
	store := character.NewStore(pool)
	c := dialWith(t, store, &recKits{})
	make1 := func(name string) string {
		created, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: accountID, Name: name, ClassId: 2, ClassName: "Bard", AppearanceJson: "{}"})
		if err != nil {
			t.Fatal(err)
		}
		return created.GetCharacter().GetId()
	}
	seller, other := make1("seller"), make1("another")
	for _, id := range []string{seller, other} {
		inv, err := store.Inventory(ctx, id)
		if err != nil {
			t.Fatal(err)
		}
		if err := store.SaveInventoryAndWallet(ctx, id, inv, []character.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 50_000}}); err != nil {
			t.Fatal(err)
		}
	}
	const node = int64(108646121231993)
	listed := &characterv1.MarketListing{Id: fmtGUID(0x9001), MarketNode: node, PricePerUnit: 100, Status: character.MarketStatusListed,
		IssueDateMs: time.Now().UnixMilli(), ExpiryDateMs: time.Now().Add(72 * time.Hour).UnixMilli(),
		Item: &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 9, ItemRecordId: 7010, InstanceGuid: fmtGUID(0x5001), Quantity: 10, Quality: 3,
			Durability: 11, CreatedAtMs: 1_780_000_000_000}}
	inv := mustInventory(t, store, seller)
	if _, err := c.SaveInventoryAndListing(ctx, &characterv1.SaveInventoryAndListingRequest{CharacterId: seller, Items: toProtoItems(inv),
		Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 50_000}}, Listing: listed}); err != nil {
		t.Fatal(err)
	}
	set, err := c.SetStorefrontDescription(ctx, &characterv1.SetStorefrontDescriptionRequest{CharacterId: seller, Node: node, Description: "Fine goods at fair prices"})
	if err != nil || set.GetOutcome() != characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_DONE || set.GetDescription() != "Fine goods at fair prices" || set.GetHeaderRaises() != 1 {
		t.Fatalf("SetStorefrontDescription = %v, %v; want done with the text and a count of 1", set, err)
	}
	list, err := c.ListMarketListings(ctx, &characterv1.ListMarketListingsRequest{})
	if err != nil || len(list.GetListings()) != 1 || len(list.GetStorefronts()) != 1 || len(list.GetNodeHeaders()) != 1 {
		t.Fatalf("ListMarketListings = %v, %v; want the listing, one description and one count", list, err)
	}
	if got := list.GetStorefronts()[0]; got.GetNode() != node || got.GetSellerId() != seller || got.GetDescription() != "Fine goods at fair prices" {
		t.Errorf("the description read is %v", got)
	}
	if got := list.GetNodeHeaders()[0]; got.GetNode() != node || got.GetRaises() != 1 {
		t.Errorf("the count read is %v", got)
	}
	none, err := c.SetStorefrontDescription(ctx, &characterv1.SetStorefrontDescriptionRequest{CharacterId: other, Node: node, Description: "I list nothing"})
	if err != nil || none.GetOutcome() != characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_NO_LISTING || none.GetHeaderRaises() != 1 {
		t.Errorf("a seller with no listing = %v, %v; want NO_LISTING and the node's count unchanged at 1", none, err)
	}
	if _, err := c.SetStorefrontDescription(ctx, &characterv1.SetStorefrontDescriptionRequest{CharacterId: seller, Node: node, Description: "one\ntwo"}); status.Code(err) != codes.InvalidArgument {
		t.Errorf("a text with a new line: %v, want InvalidArgument", err)
	}
	if _, err := c.SetStorefrontDescription(ctx, &characterv1.SetStorefrontDescriptionRequest{CharacterId: fmtGUID(0xffff), Node: node, Description: "x"}); status.Code(err) != codes.NotFound {
		t.Errorf("an unknown character: %v, want NotFound", err)
	}
	cleared, err := c.SetStorefrontDescription(ctx, &characterv1.SetStorefrontDescriptionRequest{CharacterId: seller, Node: node})
	if err != nil || cleared.GetOutcome() != characterv1.StorefrontOutcome_STOREFRONT_OUTCOME_DONE || cleared.GetDescription() != "" || cleared.GetHeaderRaises() != 2 {
		t.Errorf("a clear = %v, %v; want done, no text and a count of 2", cleared, err)
	}
	list, err = c.ListMarketListings(ctx, &characterv1.ListMarketListingsRequest{})
	if err != nil || len(list.GetStorefronts()) != 0 || len(list.GetNodeHeaders()) != 1 || list.GetNodeHeaders()[0].GetRaises() != 2 {
		t.Errorf("ListMarketListings after the clear = %v, %v; want no description and the count at 2", list, err)
	}
}

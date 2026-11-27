// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package charactergrpc

// Marketplace (migration 0030) over gRPC: SaveInventoryAndListing passes the seller's bags, wallet and listing to the store and maps a refusal to InvalidArgument
// and an unknown character to NotFound; ListMarketListings maps every listed row with its seller's name; a store without listings answers Unimplemented, and on
// the real store the whole path works: the listing and the bags are stored together, the read carries the seller's name, a refused listing changes nothing.

import (
	"context"
	"errors"
	"testing"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

// marketFake is the fake store that also keeps the listings (the part of character.Store that MarketStore names) and records what it was given.
type marketFake struct {
	*recStore
	listings   []character.MarketListing
	listErr    error
	saveErr    error
	saveID     string
	savedItems []character.Item
	savedPurse []character.WalletEntry
	saved      character.MarketListing
	saveAsked  int
	listAsked  int
}

var _ MarketStore = (*marketFake)(nil)

func (m *marketFake) SaveInventoryAndListing(_ context.Context, id string, items []character.Item, wallet []character.WalletEntry, _ []character.ProcessingJob,
	_ []int64, _ []character.WarehouseItem, listing character.MarketListing) error {
	m.saveAsked++
	m.saveID, m.savedItems, m.savedPurse, m.saved = id, items, wallet, listing
	return m.saveErr
}

func (m *marketFake) MarketListings(_ context.Context) ([]character.MarketListing, error) {
	m.listAsked++
	return m.listings, m.listErr
}

func protoMarketListing() *characterv1.MarketListing {
	return &characterv1.MarketListing{Id: fmtGUID(0x9001), SellerId: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", SellerName: "cyb", MarketNode: 108646121231993,
		Item: &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 3, ItemRecordId: 7001, InstanceGuid: fmtGUID(0x9002),
			Quantity: 2, Quality: 6, Durability: 11, CreatedAtMs: 1_771_000_000_000},
		PricePerUnit: 10000, Status: character.MarketStatusListed, IssueDateMs: 1_771_000_000_000, ExpiryDateMs: 1_771_259_200_000}
}

func wantMarketListing() character.MarketListing {
	return character.MarketListing{ID: fmtGUID(0x9001), SellerID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", SellerName: "cyb", Node: 108646121231993,
		Item: character.Item{Container: character.Backpack, Slot: 3, ItemRecordID: 7001, InstanceGUID: fmtGUID(0x9002), Quantity: 2, Quality: 6, Durability: 11,
			CreatedAt: time.UnixMilli(1_771_000_000_000)},
		PricePerUnit: 10000, Status: character.MarketStatusListed, IssuedAtMs: 1_771_000_000_000, ExpiresAtMs: 1_771_259_200_000}
}

func TestSaveInventoryAndListingPassesTheBagsThePurseAndTheListing(t *testing.T) {
	st := &marketFake{recStore: &recStore{ret: fullCharacter()}}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	item := &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 1, ItemRecordId: 7001, InstanceGuid: fmtGUID(0x10),
		Quantity: 1, Quality: 2, Durability: 3, CreatedAtMs: 1_771_000_000_000}
	_, err := c.SaveInventoryAndListing(context.Background(), &characterv1.SaveInventoryAndListingRequest{CharacterId: id,
		Items: []*characterv1.Item{item}, Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 900}},
		Listing: protoMarketListing()})
	if err != nil {
		t.Fatalf("SaveInventoryAndListing = %v", err)
	}
	if st.saveAsked != 1 || st.saveID != id || len(st.savedItems) != 1 || st.savedItems[0].InstanceGUID != fmtGUID(0x10) ||
		len(st.savedPurse) != 1 || st.savedPurse[0].Amount != 900 || !sameMarketListing(st.saved, wantMarketListing()) {
		t.Errorf("the store was given %q %d items %+v, purse %+v and listing %+v; want the request's", st.saveID, len(st.savedItems), st.savedItems, st.savedPurse, st.saved)
	}
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"invalid":   {character.ErrInvalid, codes.InvalidArgument},
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		failing := dialRec(t, &marketFake{recStore: &recStore{ret: fullCharacter()}, saveErr: tc.err})
		if _, err := failing.SaveInventoryAndListing(context.Background(), &characterv1.SaveInventoryAndListingRequest{CharacterId: id,
			Items: []*characterv1.Item{item}, Listing: protoMarketListing()}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
	notAsked := &marketFake{recStore: &recStore{ret: fullCharacter()}}
	noListing := dialRec(t, notAsked)
	if _, err := noListing.SaveInventoryAndListing(context.Background(), &characterv1.SaveInventoryAndListingRequest{CharacterId: id}); status.Code(err) != codes.InvalidArgument {
		t.Errorf("a save without a listing: %v, want InvalidArgument", err)
	}
	if notAsked.saveAsked != 0 {
		t.Errorf("a save without a listing asked the store %d times, want none", notAsked.saveAsked)
	}
}

func TestListMarketListingsMapsTheSellerName(t *testing.T) {
	st := &marketFake{recStore: &recStore{ret: fullCharacter()}, listings: []character.MarketListing{wantMarketListing()}}
	c := dialRec(t, st)
	got, err := c.ListMarketListings(context.Background(), &characterv1.ListMarketListingsRequest{})
	if err != nil {
		t.Fatalf("ListMarketListings = %v", err)
	}
	if st.listAsked != 1 || len(got.GetListings()) != 1 || !proto.Equal(got.GetListings()[0], protoMarketListing()) {
		t.Errorf("the reply is %v, want the stored listing with its seller's name", got.GetListings())
	}
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		failing := dialRec(t, &marketFake{recStore: &recStore{ret: fullCharacter()}, listErr: tc.err})
		if _, err := failing.ListMarketListings(context.Background(), &characterv1.ListMarketListingsRequest{}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
}

// A store that keeps no listings answers Unimplemented, not a wrong answer.
func TestMarketCallsOnAStoreWithoutListings(t *testing.T) {
	c := dialRec(t, &recStore{ret: fullCharacter()})
	ctx := context.Background()
	if _, err := c.ListMarketListings(ctx, &characterv1.ListMarketListingsRequest{}); status.Code(err) != codes.Unimplemented {
		t.Errorf("ListMarketListings on a store without listings: %v, want Unimplemented", err)
	}
	if _, err := c.SaveInventoryAndListing(ctx, &characterv1.SaveInventoryAndListingRequest{CharacterId: "0123456789abcdef0123456789abcdef",
		Items: []*characterv1.Item{{ItemRecordId: 7001, InstanceGuid: fmtGUID(1), Quantity: 1, Quality: 1, CreatedAtMs: 1}}, Listing: protoMarketListing()}); status.Code(err) != codes.Unimplemented {
		t.Errorf("SaveInventoryAndListing on a store without listings: %v, want Unimplemented", err)
	}
}

// The whole path on the real store: the listing and the bags are stored together, the read carries the seller's name, a refused listing changes nothing, and an
// unknown character is NotFound.
func TestMarketListingThroughTheServiceOnTheRealStore(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var account int64
	if err := pool.QueryRow(ctx, `INSERT INTO accounts (username, password_hash) VALUES ('Owner', 'x') RETURNING id`).Scan(&account); err != nil {
		t.Fatal(err)
	}
	c := dialWith(t, character.NewStore(pool), &recKits{})
	created, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: account, Name: "cyb", ClassId: 2, ClassName: "Bard", AppearanceJson: "{}"})
	if err != nil {
		t.Fatal(err)
	}
	id := created.GetCharacter().GetId()
	kit := created.GetCharacter().GetItems()
	if len(kit) < 2 {
		t.Fatalf("the created character has %d items, want its kit", len(kit))
	}
	listed, bags := kit[0], kit[1:]
	listing := protoMarketListing()
	listing.Item = proto.Clone(listed).(*characterv1.Item)
	request := func(listing *characterv1.MarketListing) *characterv1.SaveInventoryAndListingRequest {
		return &characterv1.SaveInventoryAndListingRequest{CharacterId: id, Items: bags,
			Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 1000}}, Listing: listing}
	}
	if _, err := c.SaveInventoryAndListing(ctx, request(listing)); err != nil {
		t.Fatalf("SaveInventoryAndListing: %v", err)
	}
	list, err := c.ListMarketListings(ctx, &characterv1.ListMarketListingsRequest{})
	if err != nil {
		t.Fatalf("ListMarketListings: %v", err)
	}
	if len(list.GetListings()) != 1 || list.GetListings()[0].GetSellerName() != "cyb" || list.GetListings()[0].GetId() != listing.GetId() ||
		list.GetListings()[0].GetItem().GetInstanceGuid() != listed.GetInstanceGuid() {
		t.Errorf("the listings are %v, want cyb's one with the listed stack", list.GetListings())
	}
	// the same save once more is answered "already saved" (AlreadyExists) and stores nothing: a try after an answer that was lost does not store the listing twice
	if _, err := c.SaveInventoryAndListing(ctx, request(listing)); status.Code(err) != codes.AlreadyExists {
		t.Errorf("the same save once more: %v, want AlreadyExists", err)
	}
	if again, err := c.ListMarketListings(ctx, &characterv1.ListMarketListingsRequest{}); err != nil || len(again.GetListings()) != 1 {
		t.Errorf("the second save changed the list: %v, %v; want the one listing", again.GetListings(), err)
	}
	// a refused listing changes nothing
	bad := proto.Clone(listing).(*characterv1.MarketListing)
	bad.PricePerUnit = 0
	if _, err := c.SaveInventoryAndListing(ctx, request(bad)); status.Code(err) != codes.InvalidArgument {
		t.Errorf("a listing with a zero price: %v, want InvalidArgument", err)
	}
	if again, err := c.ListMarketListings(ctx, &characterv1.ListMarketListingsRequest{}); err != nil || len(again.GetListings()) != 1 {
		t.Errorf("the refused listing changed the list: %v, %v; want the one listing", again.GetListings(), err)
	}
	unknown := proto.Clone(listing).(*characterv1.MarketListing)
	unknown.Id = fmtGUID(0x9003)
	bad = unknown
	if _, err := c.SaveInventoryAndListing(ctx, &characterv1.SaveInventoryAndListingRequest{CharacterId: "ffffffffffffffffffffffffffffffff", Items: bags,
		Listing: bad}); status.Code(err) != codes.NotFound {
		t.Errorf("an unknown character: %v, want NotFound", err)
	}
}

func sameMarketListing(got, want character.MarketListing) bool {
	return got.ID == want.ID && got.SellerID == want.SellerID && got.SellerName == want.SellerName && got.Node == want.Node &&
		got.Item.ItemRecordID == want.Item.ItemRecordID && got.Item.InstanceGUID == want.Item.InstanceGUID && got.Item.Quantity == want.Item.Quantity &&
		got.Item.Quality == want.Item.Quality && got.Item.Durability == want.Item.Durability && got.Item.CreatedAt.Equal(want.Item.CreatedAt) &&
		got.PricePerUnit == want.PricePerUnit && got.Status == want.Status && got.IssuedAtMs == want.IssuedAtMs && got.ExpiresAtMs == want.ExpiresAtMs
}

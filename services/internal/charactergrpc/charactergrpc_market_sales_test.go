// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// Marketplace sales (migration 0031) over gRPC: BuyListing, RemoveListing and ExpireListing pass their requests and the bags to the store whole and map every outcome and every
// store error; ListMarketListings carries the nodes' rates; ClaimMailItems takes a mail's money through MailMoneyStore; a store without these parts answers Unimplemented. On the
// real store the whole path works: a purchase pays the seller by mail, the money is claimed into the purse, an expired listing is mailed back, and nothing is paid twice.

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

// salesFake is the fake store that also keeps the Marketplace's sales (the part of character.Store that MarketSalesStore names) and the money of mails, and records what it
// was given.
type salesFake struct {
	*recStore
	*marketFake
	*mailFake
	buyReq     character.BuyRequest
	buyRet     character.BuyResult
	buyAsked   int
	removeReq  character.RemoveRequest
	removeRet  character.RemoveResult
	removeAsk  int
	expireID   string
	expireName string
	expireRet  character.ExpireResult
	expireAsk  int
	taxes      []character.NodeMarketTax
	taxErr     error
	moneyAsked int
	moneyID    string
	moneyClaim []character.MailClaim
	moneyMails []character.MoneyClaim
	moneyBags  character.ClaimBags
	moneyRet   character.ClaimResult
	err        error
}

func newSalesFake() *salesFake {
	rs := &recStore{ret: fullCharacter()}
	return &salesFake{recStore: rs, marketFake: &marketFake{recStore: rs}, mailFake: &mailFake{recStore: rs}}
}

var (
	_ MarketSalesStore = (*salesFake)(nil)
	_ MailMoneyStore   = (*salesFake)(nil)
)

func (f *salesFake) BuyListing(_ context.Context, r character.BuyRequest) (character.BuyResult, error) {
	f.buyAsked++
	f.buyReq = r
	return f.buyRet, f.err
}

func (f *salesFake) RemoveListing(_ context.Context, r character.RemoveRequest) (character.RemoveResult, error) {
	f.removeAsk++
	f.removeReq = r
	return f.removeRet, f.err
}

func (f *salesFake) ExpireListing(_ context.Context, id, name string) (character.ExpireResult, error) {
	f.expireAsk++
	f.expireID, f.expireName = id, name
	return f.expireRet, f.err
}

func (f *salesFake) NodeMarketTaxes(context.Context) ([]character.NodeMarketTax, error) {
	return f.taxes, f.taxErr
}

func (f *salesFake) ClaimMail(_ context.Context, id string, claims []character.MailClaim, money []character.MoneyClaim, bags character.ClaimBags) (character.ClaimResult, error) {
	f.moneyAsked++
	f.moneyID, f.moneyClaim, f.moneyMails, f.moneyBags = id, claims, money, bags
	return f.moneyRet, f.err
}

// The methods both embedded fakes have are the shared store's: name the ones that clash.
func (f *salesFake) MarketListings(ctx context.Context) ([]character.MarketListing, error) {
	return f.marketFake.MarketListings(ctx)
}

func (f *salesFake) SaveInventoryAndListing(ctx context.Context, id string, items []character.Item, wallet []character.WalletEntry, jobs []character.ProcessingJob, known []int64,
	warehouse []character.WarehouseItem, listing character.MarketListing) error {
	return f.marketFake.SaveInventoryAndListing(ctx, id, items, wallet, jobs, known, warehouse, listing)
}

func (f *salesFake) ClaimMailItems(ctx context.Context, id string, claims []character.MailClaim, bags character.ClaimBags) (character.ClaimResult, error) {
	return f.mailFake.ClaimMailItems(ctx, id, claims, bags)
}

func bagsRequest() *characterv1.SaveInventoryRequest {
	return &characterv1.SaveInventoryRequest{
		Items: []*characterv1.Item{{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 4, ItemRecordId: 7010, InstanceGuid: fmtGUID(0x71), Quantity: 3, Quality: 3,
			Durability: 11, CreatedAtMs: 1_780_000_000_000}},
		ReplaceWallet: true, Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 12345}},
	}
}

func TestBuyListingPassesTheRequestAndTheBagsAndMapsTheAnswer(t *testing.T) {
	f := newSalesFake()
	f.buyRet = character.BuyResult{Outcome: character.OutcomeDone, SellerID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", SellerMail: character.Mail{ID: fmtGUID(0x90), Sender: "Marketplace", Subject: "Item sold",
		MoneyCopper: 300, MoneyClaimed: false}}
	c := dialRec(t, f)
	resp, err := c.BuyListing(context.Background(), &characterv1.BuyListingRequest{PurchaseId: fmtGUID(0x51), CharacterId: mailProtoGUID, ListingId: fmtGUID(0x61), Quantity: 3,
		PricePerUnit: 100, TaxRate: 0.5, Tax: 150, ItemName: "Oak Wood", Inventory: bagsRequest()})
	if err != nil {
		t.Fatal(err)
	}
	if resp.GetOutcome() != characterv1.MarketOutcome_MARKET_OUTCOME_DONE || resp.GetSellerId() != "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" || resp.GetSellerMail().GetMoneyCopper() != 300 ||
		resp.GetSellerMail().GetSubject() != "Item sold" || resp.GetSellerMail().GetMoneyClaimed() {
		t.Errorf("the answer is %v, want done for the seller with the sale mail of 300 copper", resp)
	}
	r := f.buyReq
	if f.buyAsked != 1 || r.PurchaseID != fmtGUID(0x51) || r.BuyerID != mailProtoGUID || r.ListingID != fmtGUID(0x61) || r.Quantity != 3 || r.PricePerUnit != 100 || r.TaxRate != 0.5 ||
		r.Tax != 150 || r.ItemName != "Oak Wood" {
		t.Errorf("the store was asked %d times for %+v", f.buyAsked, r)
	}
	if len(r.Bags.Items) != 1 || r.Bags.Items[0].InstanceGUID != fmtGUID(0x71) || r.Bags.Items[0].Slot != 4 || !r.Bags.Items[0].CreatedAt.Equal(time.UnixMilli(1_780_000_000_000)) ||
		!r.Bags.ReplaceWallet || len(r.Bags.Wallet) != 1 || r.Bags.Wallet[0].Amount != 12345 || r.Bags.ReplaceJobs || r.Bags.ReplaceKnown || r.Bags.ReplaceWarehouse {
		t.Errorf("the store got the bags %+v, want the one item and the purse", r.Bags)
	}
}

func TestEveryOutcomeOfAStepReachesTheClient(t *testing.T) {
	want := map[character.MarketOutcome]characterv1.MarketOutcome{
		character.OutcomeDone:           characterv1.MarketOutcome_MARKET_OUTCOME_DONE,
		character.OutcomeReplayed:       characterv1.MarketOutcome_MARKET_OUTCOME_REPLAYED,
		character.OutcomeListingGone:    characterv1.MarketOutcome_MARKET_OUTCOME_LISTING_GONE,
		character.OutcomeListingExpired: characterv1.MarketOutcome_MARKET_OUTCOME_LISTING_EXPIRED,
		character.OutcomeQuantity:       characterv1.MarketOutcome_MARKET_OUTCOME_QUANTITY,
		character.OutcomePriceChanged:   characterv1.MarketOutcome_MARKET_OUTCOME_PRICE_CHANGED,
		character.OutcomeRateChanged:    characterv1.MarketOutcome_MARKET_OUTCOME_RATE_CHANGED,
		character.OutcomeOwnListing:     characterv1.MarketOutcome_MARKET_OUTCOME_OWN_LISTING,
		character.OutcomeNotExpired:     characterv1.MarketOutcome_MARKET_OUTCOME_NOT_EXPIRED,
		0:                               characterv1.MarketOutcome_MARKET_OUTCOME_UNSPECIFIED,
	}
	for outcome, wire := range want {
		f := newSalesFake()
		f.buyRet = character.BuyResult{Outcome: outcome, Available: 7}
		f.removeRet = character.RemoveResult{Outcome: outcome, Available: 8}
		f.expireRet = character.ExpireResult{Outcome: outcome}
		c := dialRec(t, f)
		buy, err := c.BuyListing(context.Background(), &characterv1.BuyListingRequest{PurchaseId: fmtGUID(1), CharacterId: mailProtoGUID, Inventory: bagsRequest()})
		if err != nil || buy.GetOutcome() != wire || buy.GetAvailable() != 7 {
			t.Errorf("BuyListing with outcome %d = %v, %v; want %v with 7 available", outcome, buy, err, wire)
		}
		remove, err := c.RemoveListing(context.Background(), &characterv1.RemoveListingRequest{CharacterId: mailProtoGUID, Inventory: bagsRequest()})
		if err != nil || remove.GetOutcome() != wire || remove.GetAvailable() != 8 {
			t.Errorf("RemoveListing with outcome %d = %v, %v; want %v with 8 available", outcome, remove, err, wire)
		}
		expire, err := c.ExpireListing(context.Background(), &characterv1.ExpireListingRequest{ListingId: fmtGUID(2)})
		if err != nil || expire.GetOutcome() != wire {
			t.Errorf("ExpireListing with outcome %d = %v, %v; want %v", outcome, expire, err, wire)
		}
		if outcome != character.OutcomeDone && (buy.GetSellerMail() != nil || expire.GetSellerMail() != nil) {
			t.Errorf("outcome %d carries a mail", outcome)
		}
	}
}

func TestRemoveAndExpirePassTheirRequestsToTheStore(t *testing.T) {
	f := newSalesFake()
	f.removeRet = character.RemoveResult{Outcome: character.OutcomeDone}
	f.expireRet = character.ExpireResult{Outcome: character.OutcomeDone, SellerID: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", SellerMail: character.Mail{ID: fmtGUID(0x91), Subject: "Listing expired",
		Items: []character.MailItem{wantItem(1)}}}
	c := dialRec(t, f)
	if _, err := c.RemoveListing(context.Background(), &characterv1.RemoveListingRequest{CharacterId: mailProtoGUID, ListingId: fmtGUID(0x61), ExpectedQuantity: 6, Inventory: bagsRequest()}); err != nil {
		t.Fatal(err)
	}
	if f.removeAsk != 1 || f.removeReq.SellerID != mailProtoGUID || f.removeReq.ListingID != fmtGUID(0x61) || f.removeReq.ExpectedQuantity != 6 || len(f.removeReq.Bags.Items) != 1 ||
		len(f.removeReq.Bags.Wallet) != 1 {
		t.Errorf("the store was asked %d times for the removal %+v", f.removeAsk, f.removeReq)
	}
	resp, err := c.ExpireListing(context.Background(), &characterv1.ExpireListingRequest{ListingId: fmtGUID(0x61), ItemName: "Oak Wood"})
	if err != nil {
		t.Fatal(err)
	}
	if f.expireAsk != 1 || f.expireID != fmtGUID(0x61) || f.expireName != "Oak Wood" {
		t.Errorf("the store was asked %d times to expire %q (%q)", f.expireAsk, f.expireID, f.expireName)
	}
	if resp.GetSellerId() != "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" || resp.GetSellerMail().GetSubject() != "Listing expired" || len(resp.GetSellerMail().GetItems()) != 1 {
		t.Errorf("the expiry's answer is %v, want the seller and the mail with the stack", resp)
	}
}

func TestSalesStoreErrorsGetTheirCodes(t *testing.T) {
	for _, tc := range []struct {
		err  error
		want codes.Code
	}{
		{character.ErrInvalid, codes.InvalidArgument},
		{character.ErrNotFound, codes.NotFound},
		{errors.New("the database fell over"), codes.Internal},
	} {
		f := newSalesFake()
		f.err = tc.err
		c := dialRec(t, f)
		if _, err := c.BuyListing(context.Background(), &characterv1.BuyListingRequest{PurchaseId: fmtGUID(1), Inventory: bagsRequest()}); status.Code(err) != tc.want {
			t.Errorf("BuyListing with %v: %v, want %v", tc.err, err, tc.want)
		}
		if _, err := c.RemoveListing(context.Background(), &characterv1.RemoveListingRequest{Inventory: bagsRequest()}); status.Code(err) != tc.want {
			t.Errorf("RemoveListing with %v: %v, want %v", tc.err, err, tc.want)
		}
		if _, err := c.ExpireListing(context.Background(), &characterv1.ExpireListingRequest{ListingId: fmtGUID(2)}); status.Code(err) != tc.want {
			t.Errorf("ExpireListing with %v: %v, want %v", tc.err, err, tc.want)
		}
	}
	f := newSalesFake()
	c := dialRec(t, f)
	if _, err := c.BuyListing(context.Background(), &characterv1.BuyListingRequest{PurchaseId: fmtGUID(1)}); status.Code(err) != codes.InvalidArgument || f.buyAsked != 0 {
		t.Errorf("a purchase with no bags: %v (asked %d times), want InvalidArgument and no store call", err, f.buyAsked)
	}
	if _, err := c.RemoveListing(context.Background(), &characterv1.RemoveListingRequest{ListingId: fmtGUID(1)}); status.Code(err) != codes.InvalidArgument || f.removeAsk != 0 {
		t.Errorf("a removal with no bags: %v (asked %d times), want InvalidArgument and no store call", err, f.removeAsk)
	}
	badItem := bagsRequest()
	badItem.Items[0].Container = characterv1.ItemContainer_ITEM_CONTAINER_UNSPECIFIED
	if _, err := c.BuyListing(context.Background(), &characterv1.BuyListingRequest{PurchaseId: fmtGUID(1), Inventory: badItem}); status.Code(err) != codes.InvalidArgument || f.buyAsked != 0 {
		t.Errorf("a purchase with an item of no container: %v (asked %d times), want InvalidArgument and no store call", err, f.buyAsked)
	}
}

func TestAStoreWithoutSalesRefusesThemAndSendsNoRates(t *testing.T) {
	st := &marketFake{recStore: &recStore{ret: fullCharacter()}, listings: []character.MarketListing{wantMarketListing()}}
	c := dialRec(t, st)
	if _, err := c.BuyListing(context.Background(), &characterv1.BuyListingRequest{PurchaseId: fmtGUID(1), Inventory: bagsRequest()}); status.Code(err) != codes.Unimplemented {
		t.Errorf("BuyListing without sales: %v, want Unimplemented", err)
	}
	if _, err := c.RemoveListing(context.Background(), &characterv1.RemoveListingRequest{Inventory: bagsRequest()}); status.Code(err) != codes.Unimplemented {
		t.Errorf("RemoveListing without sales: %v, want Unimplemented", err)
	}
	if _, err := c.ExpireListing(context.Background(), &characterv1.ExpireListingRequest{ListingId: fmtGUID(2)}); status.Code(err) != codes.Unimplemented {
		t.Errorf("ExpireListing without sales: %v, want Unimplemented", err)
	}
	list, err := c.ListMarketListings(context.Background(), &characterv1.ListMarketListingsRequest{})
	if err != nil || len(list.GetListings()) != 1 || len(list.GetNodeTaxes()) != 0 {
		t.Errorf("ListMarketListings = %v, %v; want the listing and no rate", list, err)
	}
}

func TestListMarketListingsCarriesTheRatesOfTheNodes(t *testing.T) {
	f := newSalesFake()
	f.listings = []character.MarketListing{wantMarketListing()}
	f.taxes = []character.NodeMarketTax{{Node: 108646121231993, Rate: 0.25, CollectedCopper: 4321}, {Node: 5, Rate: 1, CollectedCopper: 0}}
	c := dialRec(t, f)
	list, err := c.ListMarketListings(context.Background(), &characterv1.ListMarketListingsRequest{})
	if err != nil || len(list.GetListings()) != 1 || len(list.GetNodeTaxes()) != 2 {
		t.Fatalf("ListMarketListings = %v, %v", list, err)
	}
	if got := list.GetNodeTaxes()[0]; got.GetNode() != 108646121231993 || got.GetRate() != 0.25 || got.GetCollectedCopper() != 4321 {
		t.Errorf("the first node's tax is %v", got)
	}
	f.taxErr = errors.New("the database fell over")
	if _, err := c.ListMarketListings(context.Background(), &characterv1.ListMarketListingsRequest{}); status.Code(err) != codes.Internal {
		t.Errorf("a failed read of the rates: %v, want Internal", err)
	}
}

func TestClaimMailItemsTakesAMailsMoneyThroughTheMoneyStore(t *testing.T) {
	f := newSalesFake()
	f.moneyRet = character.ClaimResult{Claimed: 1}
	f.claimRet = character.ClaimResult{Claimed: 9}
	c := dialRec(t, f)
	req := claimRequest(mailProtoGUID, fmtGUID(0x300), fmtGUID(0x201))
	req.MoneyMailIds = []string{fmtGUID(0x301), fmtGUID(0x302)}
	req.MoneyExpectedCopper = []int64{700, 25}
	resp, err := c.ClaimMailItems(context.Background(), req)
	if err != nil || resp.GetClaimed() != 1 {
		t.Fatalf("the claim with money: %v, %v; want the money store's 1", resp, err)
	}
	if f.moneyAsked != 1 || f.claimAsked != 0 || f.moneyID != mailProtoGUID || len(f.moneyClaim) != 1 || len(f.moneyMails) != 2 || f.moneyMails[0] != (character.MoneyClaim{MailID: fmtGUID(0x301), Copper: 700}) ||
		f.moneyMails[1] != (character.MoneyClaim{MailID: fmtGUID(0x302), Copper: 25}) || !f.moneyBags.ReplaceWallet {
		t.Errorf("the money store was asked %d times (items %d): %q %v %v %+v", f.moneyAsked, f.claimAsked, f.moneyID, f.moneyClaim, f.moneyMails, f.moneyBags)
	}
	plain := claimRequest(mailProtoGUID, fmtGUID(0x300), fmtGUID(0x201))
	if resp, err := c.ClaimMailItems(context.Background(), plain); err != nil || resp.GetClaimed() != 9 || f.claimAsked != 1 || f.moneyAsked != 1 {
		t.Errorf("a claim with no money: %v, %v (items %d, money %d); want the item store's 9", resp, err, f.claimAsked, f.moneyAsked)
	}
	// each money names the copper it credits: a money with no amount, or an amount with no money, is refused before the store is asked
	for _, tc := range []struct {
		name     string
		expected []int64
	}{{"no amounts", nil}, {"one amount for two mails", []int64{700}}, {"three amounts for two mails", []int64{700, 25, 1}}} {
		bad := claimRequest(mailProtoGUID, fmtGUID(0x300), fmtGUID(0x201))
		bad.MoneyMailIds = []string{fmtGUID(0x301), fmtGUID(0x302)}
		bad.MoneyExpectedCopper = tc.expected
		before := f.moneyAsked
		if _, err := c.ClaimMailItems(context.Background(), bad); status.Code(err) != codes.InvalidArgument || f.moneyAsked != before {
			t.Errorf("a claim with %s: %v (asked %d more times), want InvalidArgument and no ask", tc.name, err, f.moneyAsked-before)
		}
	}
	// a store that keeps no mail money refuses a claim of money
	mf := &mailFake{recStore: &recStore{ret: fullCharacter()}}
	mc := dialRec(t, mf)
	if _, err := mc.ClaimMailItems(context.Background(), req); status.Code(err) != codes.Unimplemented || mf.claimAsked != 0 {
		t.Errorf("a claim of money from a store with no money: %v (asked %d), want Unimplemented", err, mf.claimAsked)
	}
}

func TestMailMoneyReachesTheClient(t *testing.T) {
	f := newSalesFake()
	f.mails = []character.Mail{{ID: fmtGUID(0x300), Sender: "Marketplace", Subject: "Item sold", Body: "x", MoneyCopper: 1000, MoneyClaimed: true}}
	c := dialRec(t, f)
	resp, err := c.GetMails(context.Background(), &characterv1.GetMailsRequest{CharacterId: mailProtoGUID})
	if err != nil || len(resp.GetMails()) != 1 || resp.GetMails()[0].GetMoneyCopper() != 1000 || !resp.GetMails()[0].GetMoneyClaimed() {
		t.Errorf("GetMails = %v, %v; want the money and the claimed flag", resp, err)
	}
}

// On the real store the whole path works through the service: the purchase, the seller's mail with the money, the claim into the purse, the node's rate and total, and an
// expiry mailing the stack back.
func TestSalesThroughTheServiceOnTheRealStore(t *testing.T) {
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
	seller, buyer := make1("seller"), make1("buyer")
	bagsOf := func(id string, extra *characterv1.Item, purse int64) *characterv1.SaveInventoryRequest {
		inv, err := store.Inventory(ctx, id)
		if err != nil {
			t.Fatal(err)
		}
		req := &characterv1.SaveInventoryRequest{ReplaceWallet: true, Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: purse}}}
		req.Items = toProtoItems(inv)
		if extra != nil {
			req.Items = append(req.Items, extra)
		}
		return req
	}
	// both characters have a stored purse (a listing needs one to be saved with)
	for _, id := range []string{seller, buyer} {
		inv, err := store.Inventory(ctx, id)
		if err != nil {
			t.Fatal(err)
		}
		if err := store.SaveInventoryAndWallet(ctx, id, inv, []character.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 50_000}}); err != nil {
			t.Fatal(err)
		}
	}
	listed := &characterv1.MarketListing{Id: fmtGUID(0x9001), MarketNode: 108646121231993, PricePerUnit: 100, Status: character.MarketStatusListed,
		IssueDateMs: time.Now().UnixMilli(), ExpiryDateMs: time.Now().Add(72 * time.Hour).UnixMilli(),
		Item: &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 9, ItemRecordId: 7010, InstanceGuid: fmtGUID(0x5001), Quantity: 10, Quality: 3,
			Durability: 11, CreatedAtMs: 1_780_000_000_000}}
	sellerBags := bagsOf(seller, nil, 50_000)
	if _, err := c.SaveInventoryAndListing(ctx, &characterv1.SaveInventoryAndListingRequest{CharacterId: seller, Items: sellerBags.GetItems(), Wallet: sellerBags.GetWallet(), Listing: listed}); err != nil {
		t.Fatal(err)
	}
	units := &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 9, ItemRecordId: 7010, InstanceGuid: fmtGUID(0x7001), Quantity: 4, Quality: 3,
		Durability: 11, CreatedAtMs: 1_780_000_000_000}
	buyReq := &characterv1.BuyListingRequest{PurchaseId: fmtGUID(0xa1), CharacterId: buyer, ListingId: listed.GetId(), Quantity: 4, PricePerUnit: 100, TaxRate: 0.5,
		Tax: 200, ItemName: "Oak Wood", Inventory: bagsOf(buyer, units, 50_000-600)}
	buy, err := c.BuyListing(ctx, buyReq)
	if err != nil || buy.GetOutcome() != characterv1.MarketOutcome_MARKET_OUTCOME_DONE || buy.GetSellerId() != seller || buy.GetSellerMail().GetMoneyCopper() != 400 {
		t.Fatalf("BuyListing = %v, %v; want done and a sale mail of 400 copper for the seller", buy, err)
	}
	again, err := c.BuyListing(ctx, buyReq)
	if err != nil || again.GetOutcome() != characterv1.MarketOutcome_MARKET_OUTCOME_REPLAYED {
		t.Errorf("the same purchase again = %v, %v; want REPLAYED", again, err)
	}
	listAll, err := c.ListMarketListings(ctx, &characterv1.ListMarketListingsRequest{})
	if err != nil || len(listAll.GetListings()) != 1 || listAll.GetListings()[0].GetItem().GetQuantity() != 6 || len(listAll.GetNodeTaxes()) != 1 ||
		listAll.GetNodeTaxes()[0].GetRate() != 0.5 || listAll.GetNodeTaxes()[0].GetCollectedCopper() != 200 {
		t.Errorf("ListMarketListings = %v, %v; want the listing with 6 units and the node at 0.5 with 200 collected", listAll, err)
	}
	// the seller takes the money into the purse through the claim
	mails, err := c.GetMails(ctx, &characterv1.GetMailsRequest{CharacterId: seller})
	if err != nil || len(mails.GetMails()) != 1 || mails.GetMails()[0].GetMoneyCopper() != 400 {
		t.Fatalf("the seller's mails = %v, %v; want the sale mail", mails, err)
	}
	claim := &characterv1.ClaimMailItemsRequest{CharacterId: seller, MoneyMailIds: []string{mails.GetMails()[0].GetId()}, MoneyExpectedCopper: []int64{400}, Inventory: bagsOf(seller, nil, 50_400)}
	got, err := c.ClaimMailItems(ctx, claim)
	if err != nil || got.GetClaimed() != 1 || got.GetReplayed() {
		t.Fatalf("the money claim = %v, %v", got, err)
	}
	if again, err := c.ClaimMailItems(ctx, claim); err != nil || !again.GetReplayed() {
		t.Errorf("the money claim again = %v, %v; want replayed", again, err)
	}
	double := &characterv1.ClaimMailItemsRequest{CharacterId: seller, MoneyMailIds: claim.MoneyMailIds, MoneyExpectedCopper: []int64{400}, Inventory: bagsOf(seller, nil, 50_800)}
	if _, err := c.ClaimMailItems(ctx, double); status.Code(err) != codes.AlreadyExists {
		t.Errorf("a second claim of the money with a purse that holds it twice: %v, want AlreadyExists", err)
	}
	// a claim that credited another amount than the mail holds is refused with AlreadyExists too, and the answer says nothing was written
	wrong := &characterv1.ClaimMailItemsRequest{CharacterId: seller, MoneyMailIds: claim.MoneyMailIds, MoneyExpectedCopper: []int64{300}, Inventory: bagsOf(seller, nil, 50_300)}
	if _, err := c.ClaimMailItems(ctx, wrong); status.Code(err) != codes.AlreadyExists {
		t.Errorf("a claim that credited 300 copper for a mail of 400: %v, want AlreadyExists", err)
	}
	wallet, err := store.Wallet(ctx, seller)
	if err != nil || len(wallet) != 1 || wallet[0].Amount != 50_400 {
		t.Errorf("the seller's purse is %v (%v), want 50400", wallet, err)
	}
	// the seller takes the rest back
	sellerNow, err := store.Inventory(ctx, seller)
	if err != nil {
		t.Fatal(err)
	}
	back := &characterv1.SaveInventoryRequest{ReplaceWallet: true, Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 50_400}},
		Items: append(toProtoItems(sellerNow), &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 9, ItemRecordId: 7010, InstanceGuid: fmtGUID(0x5001),
			Quantity: 6, Quality: 3, Durability: 11, CreatedAtMs: 1_780_000_000_000})}
	removed, err := c.RemoveListing(ctx, &characterv1.RemoveListingRequest{CharacterId: seller, ListingId: listed.GetId(), ExpectedQuantity: 6, Inventory: back})
	if err != nil || removed.GetOutcome() != characterv1.MarketOutcome_MARKET_OUTCOME_DONE {
		t.Fatalf("RemoveListing = %v, %v", removed, err)
	}
	gone, err := c.RemoveListing(ctx, &characterv1.RemoveListingRequest{CharacterId: buyer, ListingId: listed.GetId(), ExpectedQuantity: 6, Inventory: back})
	if err != nil || gone.GetOutcome() != characterv1.MarketOutcome_MARKET_OUTCOME_LISTING_GONE {
		t.Errorf("another character's removal = %v, %v; want LISTING_GONE", gone, err)
	}
	// an expired listing is mailed back
	old := &characterv1.MarketListing{Id: fmtGUID(0x9002), MarketNode: 108646121231993, PricePerUnit: 100, Status: character.MarketStatusListed, IssueDateMs: time.Now().Add(-4 * 24 * time.Hour).UnixMilli(),
		ExpiryDateMs: time.Now().Add(-time.Second).UnixMilli(), Item: &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 9, ItemRecordId: 7011,
			InstanceGuid: fmtGUID(0x5002), Quantity: 2, Quality: 3, Durability: 11, CreatedAtMs: 1_780_000_000_000}}
	nowBags := bagsOf(seller, nil, 50_400)
	nowBags.Items = toProtoItems(mustInventory(t, store, seller))
	if _, err := c.SaveInventoryAndListing(ctx, &characterv1.SaveInventoryAndListingRequest{CharacterId: seller, Items: nowBags.GetItems(), Wallet: nowBags.GetWallet(), Listing: old}); err != nil {
		t.Fatal(err)
	}
	expired, err := c.ExpireListing(ctx, &characterv1.ExpireListingRequest{ListingId: old.GetId(), ItemName: "Maple Wood"})
	if err != nil || expired.GetOutcome() != characterv1.MarketOutcome_MARKET_OUTCOME_DONE || expired.GetSellerId() != seller || len(expired.GetSellerMail().GetItems()) != 1 ||
		expired.GetSellerMail().GetItems()[0].GetInstanceGuid() != fmtGUID(0x5002) {
		t.Errorf("ExpireListing = %v, %v; want the seller's mail with the stack", expired, err)
	}
}

func mustInventory(t *testing.T, store *character.Store, id string) []character.Item {
	t.Helper()
	inv, err := store.Inventory(context.Background(), id)
	if err != nil {
		t.Fatal(err)
	}
	return inv
}

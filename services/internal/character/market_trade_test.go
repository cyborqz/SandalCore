// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Marketplace sales (migration 0031): a purchase, a removal and an expiry, each ONE transaction. These tests run against PostgreSQL (a skip is not a pass): each step
// writes everything it names or nothing, the same purchase asked for again writes nothing, two buyers of the last units cannot both have them, a step refused for a reason of
// the listing leaves every row as it was, the node's total grows by exactly the tax of each sale, and another character's rows are never touched.

import (
	"context"
	"errors"
	"fmt"
	"math"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	tradeRecord   int64 = 7010
	tradePrice    int64 = 100 // copper a unit
	tradeUnits    int32 = 10
	tradePurse    int64 = 100_000
	tradeItemName       = "Oak Wood"
)

// tradeFixture is a seller with one listing (ten units at 100 copper), two buyers with a purse each, and the listing's stack as it was listed.
type tradeFixture struct {
	s       *Store
	acc     int64
	seller  Character
	buyer   Character
	buyer2  Character
	listing MarketListing
	stack   Item
	next    int
}

func nowMs() int64 { return time.Now().UnixMilli() }

func newTradeFixture(t *testing.T) *tradeFixture {
	t.Helper()
	s, acc := setup(t)
	ctx := context.Background()
	f := &tradeFixture{s: s, acc: acc, seller: newMailCharacter(t, s, acc, "seller"), buyer: newMailCharacter(t, s, acc, "buyer"), buyer2: newMailCharacter(t, s, acc, "buyertwo")}
	f.stack = item(Backpack, 5, tradeRecord, guid(0x5001))
	f.stack.Quantity, f.stack.Quality = tradeUnits, 3
	f.listing = MarketListing{ID: guid(0x9001), Node: marketNode, Item: f.stack, PricePerUnit: tradePrice, Status: MarketStatusListed, IssuedAtMs: nowMs(),
		ExpiresAtMs: nowMs() + 72*3600*1000}
	f.list(t, f.seller, f.listing)
	for _, c := range []Character{f.seller, f.buyer, f.buyer2} {
		items, err := s.Inventory(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		if err := s.SaveInventoryAndWallet(ctx, c.ID, items, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse}}); err != nil {
			t.Fatal(err)
		}
	}
	return f
}

// list stores a listing of `who` with the bags they have now (the listed stack is not among them).
func (f *tradeFixture) list(t *testing.T, who Character, l MarketListing) {
	t.Helper()
	items, err := f.s.Inventory(context.Background(), who.ID)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.s.SaveInventoryAndListing(context.Background(), who.ID, items, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse}}, nil, nil, nil, l); err != nil {
		t.Fatalf("list: %v", err)
	}
}

func (f *tradeFixture) freshGUID() string {
	f.next++
	return guid(0x7000 + f.next)
}

// free is the lowest empty Backpack slot of the items.
func free(items []Item) int32 {
	taken := map[int32]bool{}
	for _, it := range items {
		if it.Container == Backpack {
			taken[it.Slot] = true
		}
	}
	slot := int32(0)
	for taken[slot] {
		slot++
	}
	return slot
}

// buy is the request a session sends for `quantity` units of the listing: the buyer's stored bags plus the units (the listing's own guid for the whole stack, a new one for
// a part of it), and the purse after the price and the tax at `rate`.
func (f *tradeFixture) buy(t *testing.T, who Character, purchase string, quantity int32, rate float32) BuyRequest {
	t.Helper()
	items, err := f.s.Inventory(context.Background(), who.ID)
	if err != nil {
		t.Fatal(err)
	}
	units := f.stack
	units.Quantity = quantity
	units.InstanceGUID = f.freshGUID()
	if quantity == f.stack.Quantity {
		units.InstanceGUID = f.stack.InstanceGUID
	}
	units.Slot = free(items)
	charge, ok := ChargeFor(tradePrice, quantity, rate)
	if !ok {
		t.Fatalf("no charge for %d units at %v", quantity, rate)
	}
	return BuyRequest{PurchaseID: purchase, BuyerID: who.ID, ListingID: f.listing.ID, Quantity: quantity, PricePerUnit: tradePrice, TaxRate: rate, Tax: charge.Tax, ItemName: tradeItemName,
		Bags: ClaimBags{Items: append(items, units), Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: walletOf(t, f.s, who) - charge.Total}}, ReplaceWallet: true}}
}

func walletOf(t *testing.T, s *Store, c Character) int64 { return walletAmount(t, s, c) }

// moneyOf is the claim of one mail's money, crediting `copper` for it.
func moneyOf(mailID string, copper int64) []MoneyClaim {
	return []MoneyClaim{{MailID: mailID, Copper: copper}}
}

// listingRow reads a listing's status and units.
func (f *tradeFixture) listingRow(t *testing.T, id string) (status int32, quantity int32) {
	t.Helper()
	if err := f.s.pool.QueryRow(context.Background(), `SELECT status, quantity FROM character_market_listings WHERE id = $1`, id).Scan(&status, &quantity); err != nil {
		t.Fatal(err)
	}
	return status, quantity
}

// snapshot is every row a step may write, as one string: the three characters' bags and purse, the listings, the mails and their money, the nodes' totals and the purchases.
func (f *tradeFixture) snapshot(t *testing.T) string {
	t.Helper()
	ctx := context.Background()
	var b strings.Builder
	for _, c := range []Character{f.seller, f.buyer, f.buyer2} {
		items, err := f.s.Inventory(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		fmt.Fprintf(&b, "%s bags %v purse %d\n", c.Name, items, walletOf(t, f.s, c))
	}
	for _, q := range []string{
		`SELECT id, character_id, status, quantity, instance_guid FROM character_market_listings ORDER BY id`,
		`SELECT m.id, m.character_id, m.subject, m.body, m.status, coalesce(mm.copper, 0), coalesce(mm.claimed, false), coalesce(mm.sold_quantity, 0), coalesce(mm.listing_id, '') FROM character_mails m
			LEFT JOIN character_mail_money mm ON mm.mail_id = m.id ORDER BY m.id`,
		`SELECT mail_id, instance_guid, claimed FROM character_mail_items ORDER BY mail_id, position`,
		`SELECT node, rate, collected_copper FROM node_market_taxes ORDER BY node`,
		`SELECT purchase_id, listing_id, buyer_id, quantity, price_copper, tax_copper FROM market_purchases ORDER BY purchase_id`,
	} {
		rows, err := f.s.pool.Query(ctx, q)
		if err != nil {
			t.Fatal(err)
		}
		for rows.Next() {
			vals, err := rows.Values()
			if err != nil {
				rows.Close()
				t.Fatal(err)
			}
			fmt.Fprintf(&b, "%v\n", vals)
		}
		rows.Close()
		if err := rows.Err(); err != nil {
			t.Fatal(err)
		}
	}
	return b.String()
}

func (f *tradeFixture) collected(t *testing.T) (rate float32, copper int64) {
	t.Helper()
	taxes, err := f.s.NodeMarketTaxes(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	for _, x := range taxes {
		if x.Node == marketNode {
			return x.Rate, x.CollectedCopper
		}
	}
	return 0, 0
}

func (f *tradeFixture) saleMails(t *testing.T) []Mail {
	t.Helper()
	mails, err := f.s.Mails(context.Background(), f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	var out []Mail
	for _, m := range mails {
		if m.Sender == MarketMailSender && m.MoneyCopper > 0 {
			out = append(out, m)
		}
	}
	return out
}

// The tax is the client's float formula: the recorded rows of the retail buys, the values at the rates of the game, and the limits of its whole-number arithmetic.
func TestCartTaxIsTheClientsFloatFormula(t *testing.T) {
	// the client's formula written out once more, without the conversions that keep the server's from being fused: RoundToInt(F) = cvtss2si(F + F + 0.5) >> 1
	reference := func(sum int64, rate float32) int64 {
		f := float32(sum) * rate
		return int64(int32(math.RoundToEven(float64(f+f+0.5))) >> 1)
	}
	for _, tc := range []struct {
		sum  int64
		rate float32
		tax  int64
	}{
		{1, 0.1, 0}, {100, 0.1, 10}, {10_000, 0.1, 1000}, // the retail rows: 1 paid as 1, 100 as 110, 10,000 as 11,000
		{100, 0.5, 50}, {10_000, 0.5, 5000}, {1, 0.5, 1}, {3, 0.5, 2}, {5, 0.5, 3}, {7, 0.5, 4}, // (a half goes up: 0.5 is 1, 1.5 is 2, 2.5 is 3, 3.5 is 4)
		{100, 1, 100}, {10_000, 1, 10_000},
		{1 << 24, 0.5, 1 << 23}, {1<<24 + 1, 0.5, 1 << 23}, // 2^24 + 1 is not a single-precision number: the client sees 2^24
	} {
		got, ok := CartTax(tc.sum, tc.rate, 0)
		if !ok || got != tc.tax {
			t.Errorf("CartTax(%d, %v) = %d, %v; want %d", tc.sum, tc.rate, got, ok, tc.tax)
		}
	}
	for _, sum := range []int64{1, 100, 10_000, 1 << 24, 1<<24 + 1, 1<<31 - 1} {
		for _, rate := range []float32{0.1, 0.5, 1} {
			want := reference(sum, rate)
			got, ok := CartTax(sum, rate, 0)
			biased := float32(sum) * rate
			fits := float64(biased+biased+0.5) < 2147483648.0
			if ok != fits || (ok && got != want) {
				t.Errorf("CartTax(%d, %v) = %d, %v; the client's formula says %d (fits %v)", sum, rate, got, ok, want, fits)
			}
		}
	}
	// a flat part rides on the sum before the rounding
	if got, ok := CartTax(100, 0.1, 5); !ok || got != 15 {
		t.Errorf("CartTax(100, 0.1, flat 5) = %d, %v; want 15", got, ok)
	}
	if _, ok := CartTax(-1, 0.5, 0); ok {
		t.Error("a negative sum has a tax")
	}
}

// A sale's charge refuses what the client's whole-number arithmetic cannot show: a sum or a taxed sum of 2^31 or more, and a sum that would overflow.
func TestChargeForRefusesWhatTheClientCannotShow(t *testing.T) {
	for _, tc := range []struct {
		price int64
		qty   int32
		rate  float32
		ok    bool
		tax   int64
		total int64
	}{
		{100, 1, 0.1, true, 10, 110},
		{100, 10, 0.5, true, 500, 1500},
		{10_000, 1, 0.1, true, 1000, 11_000},
		{1, 1, 0.1, true, 0, 1},
		{1_000_000_000, 1, 0.1, true, 100_000_000, 1_100_000_000},
		{1<<31 - 1, 1, 0.1, false, 0, 0},     // the sum fits, the taxed sum is 2^31 or more
		{1 << 31, 1, 0.1, false, 0, 0},       // the sum is 2^31
		{1_000_000_000, 2, 0.1, false, 0, 0}, // 2,000,000,000 and its tax are above 2^31 - 1
		{1<<62 + 1, 4, 0.5, false, 0, 0},     // the product overflows
		{math.MaxInt64, 2, 0.5, false, 0, 0}, // the product overflows
		{0, 1, 0.5, false, 0, 0},
		{100, 0, 0.5, false, 0, 0},
		{100, -1, 0.5, false, 0, 0},
	} {
		got, ok := ChargeFor(tc.price, tc.qty, tc.rate)
		if ok != tc.ok || (ok && (got.Tax != tc.tax || got.Total != tc.total || got.Sum != tc.price*int64(tc.qty))) {
			t.Errorf("ChargeFor(%d, %d, %v) = %+v, %v; want ok %v, tax %d, total %d", tc.price, tc.qty, tc.rate, got, ok, tc.ok, tc.tax, tc.total)
		}
	}
}

func TestFormatCopperAndTheMailTexts(t *testing.T) {
	for copper, want := range map[int64]string{0: "0 copper", 1: "1 copper", 100: "1 silver", 1000: "10 silver", 10_000: "1 gold", 10_305: "1 gold 3 silver 5 copper", 120_000: "12 gold"} {
		if got := FormatCopper(copper); got != want {
			t.Errorf("FormatCopper(%d) = %q, want %q", copper, got, want)
		}
	}
	subject, body := saleMailText("Oak Wood", 10, 1000)
	if subject != "Item sold" || body != "10 x Oak Wood sold for 10 silver. The money is attached." {
		t.Errorf("sale mail = %q / %q", subject, body)
	}
	subject, body = expiredMailText("Oak Wood", 7)
	if subject != "Listing expired" || body != "Your listing of 7 x Oak Wood ended after 3 days. The item is attached." {
		t.Errorf("expiry mail = %q / %q", subject, body)
	}
	if got := sanitizedName("  \x00\x07Oak\nWood é "); got != "OakWood é" {
		t.Errorf("sanitizedName = %q", got)
	}
	if got := sanitizedName("\x01\x02"); got != "an item" {
		t.Errorf("an empty name = %q", got)
	}
	if got := sanitizedName(strings.Repeat("x", 300)); len(got) != 100 {
		t.Errorf("a long name is %d bytes, want 100", len(got))
	}
}

// A purchase of the whole stack writes everything together: the buyer's units and purse, the listing SOLD (it keeps its last quantity), the seller's mail with the full price,
// the tax in the node's total and the purchase's id.
func TestBuyListingWholeStackStoresEverythingTogether(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	req := f.buy(t, f.buyer, guid(0xa001), tradeUnits, 0.5)
	got, err := f.s.BuyListing(ctx, req)
	if err != nil || got.Outcome != OutcomeDone || got.SellerID != f.seller.ID {
		t.Fatalf("BuyListing = %+v, %v; want done for the seller", got, err)
	}
	bags, err := f.s.Inventory(ctx, f.buyer.ID)
	if err != nil {
		t.Fatal(err)
	}
	if n := storedGuidCount(t, f.s, f.buyer, f.stack.InstanceGUID); n != 1 {
		t.Errorf("the buyer holds the listing's stack %d times, want once", n)
	}
	found := false
	for _, it := range bags {
		found = found || (it.InstanceGUID == f.stack.InstanceGUID && it.Quantity == tradeUnits && it.ItemRecordID == tradeRecord && it.Quality == 3)
	}
	if !found {
		t.Errorf("the buyer's bags %v lack the ten units", bags)
	}
	if purse := walletOf(t, f.s, f.buyer); purse != tradePurse-1500 {
		t.Errorf("the buyer's purse is %d, want %d (the price 1000 and the tax 500 paid)", purse, tradePurse-1500)
	}
	if status, quantity := f.listingRow(t, f.listing.ID); status != MarketStatusSold || quantity != tradeUnits {
		t.Errorf("the listing is status %d x%d, want SOLD with its last quantity %d", status, quantity, tradeUnits)
	}
	mails := f.saleMails(t)
	if len(mails) != 1 {
		t.Fatalf("the seller has %d sale mails, want 1", len(mails))
	}
	m := mails[0]
	if m.MoneyCopper != 1000 || m.MoneyClaimed || m.Subject != "Item sold" || m.Body != "10 x Oak Wood sold for 10 silver. The money is attached." || m.ExpiresAtMs != 0 ||
		m.Status != 0 || len(m.Items) != 0 || got.SellerMail.ID != m.ID || got.SellerMail.MoneyCopper != 1000 {
		t.Errorf("the sale mail is %+v (answered %+v), want the full price 1000, unread, never expiring, no item", m, got.SellerMail)
	}
	if purse := walletOf(t, f.s, f.seller); purse != tradePurse {
		t.Errorf("the seller's purse changed to %d: the money comes by mail", purse)
	}
	if rate, copper := f.collected(t); rate != 0.5 || copper != 500 {
		t.Errorf("the node's rate is %v and its total %d, want 0.5 and the tax of the sale, 500", rate, copper)
	}
	var tax int64
	if err := f.s.pool.QueryRow(ctx, `SELECT tax_copper FROM market_purchases WHERE purchase_id = $1`, req.PurchaseID).Scan(&tax); err != nil || tax != 500 {
		t.Errorf("the purchase's record: tax %d, %v; want 500", tax, err)
	}
	listed, err := f.s.MarketListings(ctx)
	if err != nil || len(listed) != 0 {
		t.Errorf("MarketListings = %d rows, %v; a sold listing is not read again", len(listed), err)
	}
}

// A part of the stack: the listing keeps its guid and the rest of its units, the buyer's units have a new guid, and the sales of one listing add up in ONE sale mail whose
// text follows them and which is unread again when it grows.
func TestBuyListingPartsAddUpInOneSaleMail(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	first := f.buy(t, f.buyer, guid(0xa001), 3, 0.5)
	if res, err := f.s.BuyListing(ctx, first); err != nil || res.Outcome != OutcomeDone {
		t.Fatalf("the first part: %+v, %v", res, err)
	}
	if status, quantity := f.listingRow(t, f.listing.ID); status != MarketStatusListed || quantity != 7 {
		t.Fatalf("after 3 units the listing is status %d x%d, want listed x7", status, quantity)
	}
	if n := storedGuidCount(t, f.s, f.buyer, f.stack.InstanceGUID); n != 0 {
		t.Errorf("the buyer's part of the stack carries the listing's guid (%d times): the listing still holds it", n)
	}
	mails := f.saleMails(t)
	if len(mails) != 1 || mails[0].MoneyCopper != 300 || mails[0].Body != "3 x Oak Wood sold for 3 silver. The money is attached." {
		t.Fatalf("after the first part the sale mails are %+v", mails)
	}
	// the seller reads it: the read mark is saved
	read := mails[0]
	read.Status |= MailStatusRead
	if err := f.s.SaveMail(ctx, f.seller.ID, read); err != nil {
		t.Fatal(err)
	}
	second := f.buy(t, f.buyer2, guid(0xa002), 2, 0.5)
	if res, err := f.s.BuyListing(ctx, second); err != nil || res.Outcome != OutcomeDone || res.SellerMail.ID != mails[0].ID {
		t.Fatalf("the second part: %+v, %v; want done and the same mail", res, err)
	}
	mails = f.saleMails(t)
	if len(mails) != 1 || mails[0].MoneyCopper != 500 || mails[0].Body != "5 x Oak Wood sold for 5 silver. The money is attached." || mails[0].Status&MailStatusRead != 0 {
		t.Errorf("after the second part the sale mails are %+v, want one mail of 500 copper, 5 units, unread again", mails)
	}
	third := f.buy(t, f.buyer, guid(0xa003), 5, 0.5)
	if res, err := f.s.BuyListing(ctx, third); err != nil || res.Outcome != OutcomeDone {
		t.Fatalf("the rest: %+v, %v", res, err)
	}
	if status, quantity := f.listingRow(t, f.listing.ID); status != MarketStatusSold || quantity != 5 {
		t.Errorf("after the last units the listing is status %d x%d, want SOLD (its last quantity is the units that were left, 5)", status, quantity)
	}
	mails = f.saleMails(t)
	if len(mails) != 1 || mails[0].MoneyCopper != 1000 {
		t.Errorf("the sale mails are %+v, want ONE mail with the full price 1000", mails)
	}
	if _, copper := f.collected(t); copper != 150+100+250 {
		t.Errorf("the node's total is %d, want the tax of each sale added up: 150 + 100 + 250", copper)
	}
	var purchases int
	if err := f.s.pool.QueryRow(ctx, `SELECT count(*) FROM market_purchases`).Scan(&purchases); err != nil || purchases != 3 {
		t.Errorf("%d purchases are recorded, want 3", purchases)
	}
}

// The same purchase asked for again, as a retry after a lost answer is, answers REPLAYED and writes nothing, whatever became of the listing meanwhile.
func TestBuyListingAskedForAgainWritesNothing(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	req := f.buy(t, f.buyer, guid(0xa001), 4, 0.5)
	if res, err := f.s.BuyListing(ctx, req); err != nil || res.Outcome != OutcomeDone {
		t.Fatalf("the purchase: %+v, %v", res, err)
	}
	after := f.snapshot(t)
	for i := 0; i < 3; i++ {
		res, err := f.s.BuyListing(ctx, req)
		if err != nil || res.Outcome != OutcomeReplayed || res.SellerID != f.seller.ID {
			t.Fatalf("the repeat %d: %+v, %v; want REPLAYED for the seller", i, res, err)
		}
	}
	if f.snapshot(t) != after {
		t.Error("a repeat of the purchase changed rows")
	}
	// the listing is sold out by another buyer, then the first buyer's retry still comes: it is still "done"
	rest := f.buy(t, f.buyer2, guid(0xa002), 6, 0.5)
	if res, err := f.s.BuyListing(ctx, rest); err != nil || res.Outcome != OutcomeDone {
		t.Fatalf("the rest: %+v, %v", res, err)
	}
	after = f.snapshot(t)
	if res, err := f.s.BuyListing(ctx, req); err != nil || res.Outcome != OutcomeReplayed {
		t.Errorf("the first purchase asked for after the listing was sold out: %+v, %v; want REPLAYED", res, err)
	}
	if f.snapshot(t) != after {
		t.Error("a repeat after the listing was sold out changed rows")
	}
	// the id of another buyer's purchase is no one else's
	stolen := f.buy(t, f.buyer2, req.PurchaseID, 1, 0.5)
	if _, err := f.s.BuyListing(ctx, stolen); !errors.Is(err, ErrInvalid) {
		t.Errorf("a purchase id that was made for another buyer: %v, want ErrInvalid", err)
	}
}

// Each refusal for a reason of the listing answers its outcome and leaves every row as it was.
func TestBuyListingRefusalsChangeNothing(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if _, err := f.s.pool.Exec(ctx, `UPDATE character_wallet SET amount = amount WHERE character_id = $1`, f.buyer.ID); err != nil {
		t.Fatal(err)
	}
	soldOut := MarketListing{ID: guid(0x9002), Node: marketNode, Item: item(Backpack, 6, tradeRecord, guid(0x5002)), PricePerUnit: tradePrice, Status: MarketStatusListed,
		IssuedAtMs: nowMs(), ExpiresAtMs: nowMs() + 3_600_000}
	f.list(t, f.seller, soldOut)
	expired := MarketListing{ID: guid(0x9003), Node: marketNode, Item: item(Backpack, 7, tradeRecord, guid(0x5003)), PricePerUnit: tradePrice, Status: MarketStatusListed,
		IssuedAtMs: nowMs() - 100_000, ExpiresAtMs: nowMs() - 1000}
	f.list(t, f.seller, expired)
	if _, err := f.s.pool.Exec(ctx, `UPDATE character_market_listings SET status = $2 WHERE id = $1`, soldOut.ID, MarketStatusSold); err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.pool.Exec(ctx, `INSERT INTO node_market_taxes (node, rate, collected_copper) VALUES ($1, 0.6, 77)`, marketNode+1); err != nil {
		t.Fatal(err)
	}
	other := MarketListing{ID: guid(0x9004), Node: marketNode + 1, Item: item(Backpack, 8, tradeRecord, guid(0x5004)), PricePerUnit: tradePrice, Status: MarketStatusListed,
		IssuedAtMs: nowMs(), ExpiresAtMs: nowMs() + 3_600_000}
	other.Item.Quantity = 5
	f.list(t, f.seller, other)
	before := f.snapshot(t)

	asked := func(who Character, id string, mod func(*BuyRequest)) BuyRequest {
		r := f.buy(t, who, id, 2, 0.5)
		if mod != nil {
			mod(&r)
		}
		return r
	}
	for _, tc := range []struct {
		name      string
		req       BuyRequest
		want      MarketOutcome
		available int32
	}{
		{"the seller's own listing", asked(f.seller, guid(0xb001), nil), OutcomeOwnListing, 0},
		{"a listing that is not there", asked(f.buyer, guid(0xb002), func(r *BuyRequest) { r.ListingID = guid(0xdead) }), OutcomeListingGone, 0},
		{"a listing that was sold", asked(f.buyer, guid(0xb003), func(r *BuyRequest) { r.ListingID = soldOut.ID }), OutcomeListingGone, 0},
		{"a listing whose time has come", asked(f.buyer, guid(0xb004), func(r *BuyRequest) { r.ListingID = expired.ID }), OutcomeListingExpired, 0},
		{"another price than the listing's", asked(f.buyer, guid(0xb005), func(r *BuyRequest) {
			r.PricePerUnit = tradePrice + 1
			charge, _ := ChargeFor(r.PricePerUnit, r.Quantity, r.TaxRate)
			r.Tax = charge.Tax
		}), OutcomePriceChanged, tradeUnits},
		{"more units than are listed", asked(f.buyer, guid(0xb006), func(r *BuyRequest) {
			r.Quantity = tradeUnits + 1
			charge, _ := ChargeFor(r.PricePerUnit, r.Quantity, r.TaxRate)
			r.Tax = charge.Tax
		}), OutcomeQuantity, tradeUnits},
		{"another rate than the node's", asked(f.buyer, guid(0xb007), func(r *BuyRequest) { r.ListingID = other.ID }), OutcomeRateChanged, 1},
	} {
		got, err := f.s.BuyListing(ctx, tc.req)
		if err != nil || got.Outcome != tc.want {
			t.Errorf("%s: %+v, %v; want outcome %d", tc.name, got, err, tc.want)
			continue
		}
		if tc.name == "more units than are listed" && got.Available != tc.available {
			t.Errorf("%s: %d units available, want %d", tc.name, got.Available, tc.available)
		}
		if after := f.snapshot(t); after != before {
			t.Errorf("%s changed rows", tc.name)
			before = after
		}
	}
	// requests the rules refuse
	for _, tc := range []struct {
		name string
		mod  func(*BuyRequest)
		want error
	}{
		{"a tax that is not the formula's", func(r *BuyRequest) { r.Tax++ }, ErrInvalid},
		{"a rate below the game's minimum", func(r *BuyRequest) { r.TaxRate = 0.05 }, ErrInvalid},
		{"a rate above the game's maximum", func(r *BuyRequest) { r.TaxRate = 1.5 }, ErrInvalid},
		{"no units", func(r *BuyRequest) { r.Quantity = 0 }, ErrInvalid},
		{"a purchase id of zeros", func(r *BuyRequest) { r.PurchaseID = strings.Repeat("0", 32) }, ErrInvalid},
		{"a listing id that is no id", func(r *BuyRequest) { r.ListingID = "nope" }, ErrInvalid},
		{"no bags", func(r *BuyRequest) { r.Bags.Items = nil }, ErrInvalid},
		{"a purse the rules refuse", func(r *BuyRequest) { r.Bags.Wallet = []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: -5}} }, ErrInvalid},
		{"an unknown buyer", func(r *BuyRequest) { r.BuyerID = strings.Repeat("e", 32) }, ErrNotFound},
		{"a buyer that is no id", func(r *BuyRequest) { r.BuyerID = "nobody" }, ErrNotFound},
	} {
		r := f.buy(t, f.buyer, guid(0xc001), 2, 0.5)
		tc.mod(&r)
		if _, err := f.s.BuyListing(ctx, r); !errors.Is(err, tc.want) {
			t.Errorf("%s: %v, want %v", tc.name, err, tc.want)
		}
		if after := f.snapshot(t); after != before {
			t.Errorf("%s changed rows", tc.name)
			before = after
		}
	}
	// the listing stays listed with its guid: a bag that holds it too would be one guid in a bag and in a listed row
	dup := f.buy(t, f.buyer, guid(0xc002), 2, 0.5)
	dup.Bags.Items[len(dup.Bags.Items)-1].InstanceGUID = f.stack.InstanceGUID
	if _, err := f.s.BuyListing(ctx, dup); !errors.Is(err, ErrInvalid) {
		t.Errorf("a part of the stack under the listing's own guid: %v, want ErrInvalid", err)
	}
	if f.snapshot(t) != before {
		t.Error("the duplicate guid changed rows")
	}
}

// A failure in the middle of the transaction (the last write of the step) takes back every write before it: the buyer's bags and purse, the listing and the tax.
func TestBuyListingFailureAtTheLastWriteTakesBackTheFirstOnes(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	// an unclaimed sale mail that is full: the sale cannot be added to it, and that is found after the buyer's bags and the listing were written
	sale, err := f.s.CreateMail(ctx, f.seller.ID, NewMail{Sender: "Brannoc", Subject: "x"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.pool.Exec(ctx, `INSERT INTO character_mail_money (mail_id, copper, claimed, listing_id, sold_quantity) VALUES ($1, $2, false, $3, 1)`,
		sale.ID, MaxWalletAmount-10, f.listing.ID); err != nil {
		t.Fatal(err)
	}
	before := f.snapshot(t)
	if _, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 4, 0.5)); !errors.Is(err, ErrInvalid) {
		t.Fatalf("the purchase that cannot be added to the sale mail: %v, want ErrInvalid", err)
	}
	if f.snapshot(t) != before {
		t.Error("the failed purchase left rows written: the buyer's bags, the listing or the tax")
	}
}

// Two buyers of the last units: one gets them, the other is refused, and the node's total holds the tax of the one sale.
func TestBuyListingTwoBuyersOfTheLastUnits(t *testing.T) {
	for round := 0; round < 6; round++ {
		f := newTradeFixture(t)
		ctx := context.Background()
		reqs := []BuyRequest{f.buy(t, f.buyer, guid(0xa001), tradeUnits, 0.5), f.buy(t, f.buyer2, guid(0xa002), tradeUnits, 0.5)}
		type answer struct {
			res BuyResult
			err error
		}
		answers := make([]answer, 2)
		var ready, wg sync.WaitGroup
		start := make(chan struct{})
		for i := range reqs {
			wg.Add(1)
			ready.Add(1)
			go func(i int) {
				defer wg.Done()
				ready.Done()
				<-start
				answers[i].res, answers[i].err = f.s.BuyListing(ctx, reqs[i])
			}(i)
		}
		ready.Wait()
		close(start)
		wg.Wait()
		done, gone := 0, 0
		for i, a := range answers {
			if a.err != nil {
				t.Fatalf("round %d: buyer %d: %v", round, i, a.err)
			}
			switch a.res.Outcome {
			case OutcomeDone:
				done++
			case OutcomeListingGone:
				gone++
			default:
				t.Errorf("round %d: buyer %d got outcome %d", round, i, a.res.Outcome)
			}
		}
		if done != 1 || gone != 1 {
			t.Fatalf("round %d: %d done and %d refused, want one buyer served and one refused", round, done, gone)
		}
		held := storedGuidCount(t, f.s, f.buyer, f.stack.InstanceGUID) + storedGuidCount(t, f.s, f.buyer2, f.stack.InstanceGUID)
		if held != 1 {
			t.Errorf("round %d: the stack is in %d buyers' bags, want one", round, held)
		}
		losers := f.buyer2
		if answers[1].res.Outcome == OutcomeDone {
			losers = f.buyer
		}
		if purse := walletOf(t, f.s, losers); purse != tradePurse {
			t.Errorf("round %d: the refused buyer's purse is %d: nothing was taken", round, purse)
		}
		if _, copper := f.collected(t); copper != 500 {
			t.Errorf("round %d: the node's total is %d, want the one sale's 500", round, copper)
		}
		if mails := f.saleMails(t); len(mails) != 1 || mails[0].MoneyCopper != 1000 {
			t.Errorf("round %d: the seller's sale mails are %+v, want one of 1000", round, mails)
		}
	}
}

// Two part buys of the same listing at once: the units that are left decide, the loser is refused with the units the listing holds, and the sale mail holds one sale.
func TestBuyListingTwoPartBuysOfTheSameListing(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	reqs := []BuyRequest{f.buy(t, f.buyer, guid(0xa001), 6, 0.5), f.buy(t, f.buyer2, guid(0xa002), 6, 0.5)}
	results := make([]BuyResult, 2)
	var wg sync.WaitGroup
	start := make(chan struct{})
	for i := range reqs {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			<-start
			res, err := f.s.BuyListing(ctx, reqs[i])
			if err != nil {
				t.Error(err)
			}
			results[i] = res
		}(i)
	}
	close(start)
	wg.Wait()
	done, quantity := 0, 0
	for _, r := range results {
		switch r.Outcome {
		case OutcomeDone:
			done++
		case OutcomeQuantity:
			quantity++
			if r.Available != 4 {
				t.Errorf("the refused buyer is told %d units are left, want 4", r.Available)
			}
		}
	}
	if done != 1 || quantity != 1 {
		t.Fatalf("%d done and %d refused for the quantity, want one of each", done, quantity)
	}
	if status, left := f.listingRow(t, f.listing.ID); status != MarketStatusListed || left != 4 {
		t.Errorf("the listing is status %d x%d, want listed x4", status, left)
	}
}

// A purchase and a removal of the same listing at once: one of them stores, the other is refused, and the stack is in exactly one set of bags.
func TestBuyListingAgainstARemoval(t *testing.T) {
	for round := 0; round < 6; round++ {
		f := newTradeFixture(t)
		ctx := context.Background()
		buy := f.buy(t, f.buyer, guid(0xa001), tradeUnits, 0.5)
		sellerItems, err := f.s.Inventory(ctx, f.seller.ID)
		if err != nil {
			t.Fatal(err)
		}
		back := f.stack
		back.Slot = free(sellerItems)
		remove := RemoveRequest{SellerID: f.seller.ID, ListingID: f.listing.ID, ExpectedQuantity: tradeUnits,
			Bags: ClaimBags{Items: append(sellerItems, back), Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse}}, ReplaceWallet: true}}
		var bought BuyResult
		var removed RemoveResult
		var buyErr, removeErr error
		var wg sync.WaitGroup
		start := make(chan struct{})
		wg.Add(2)
		go func() { defer wg.Done(); <-start; bought, buyErr = f.s.BuyListing(ctx, buy) }()
		go func() { defer wg.Done(); <-start; removed, removeErr = f.s.RemoveListing(ctx, remove) }()
		close(start)
		wg.Wait()
		if buyErr != nil || removeErr != nil {
			t.Fatalf("round %d: %v / %v", round, buyErr, removeErr)
		}
		bothHold := storedGuidCount(t, f.s, f.buyer, f.stack.InstanceGUID) + storedGuidCount(t, f.s, f.seller, f.stack.InstanceGUID)
		switch {
		case bought.Outcome == OutcomeDone && removed.Outcome == OutcomeListingGone:
			if bothHold != 1 || storedGuidCount(t, f.s, f.buyer, f.stack.InstanceGUID) != 1 {
				t.Errorf("round %d: the purchase won but the stack is held %d times", round, bothHold)
			}
		case removed.Outcome == OutcomeDone && bought.Outcome == OutcomeListingGone:
			if bothHold != 1 || storedGuidCount(t, f.s, f.seller, f.stack.InstanceGUID) != 1 {
				t.Errorf("round %d: the removal won but the stack is held %d times", round, bothHold)
			}
			if purse := walletOf(t, f.s, f.buyer); purse != tradePurse {
				t.Errorf("round %d: the buyer paid %d for a listing that was removed", round, tradePurse-purse)
			}
			if mails := f.saleMails(t); len(mails) != 0 {
				t.Errorf("round %d: a removed listing has %d sale mails", round, len(mails))
			}
		default:
			t.Errorf("round %d: the purchase answered %d and the removal %d: exactly one must store", round, bought.Outcome, removed.Outcome)
		}
	}
}

// A removal stores the stack back in the seller's bags and marks the listing REMOVED; the fee is not given back, nothing is mailed and no money moves.
func TestRemoveListingGivesTheStackBack(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	items, err := f.s.Inventory(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	back := f.stack
	back.Slot = free(items)
	req := RemoveRequest{SellerID: f.seller.ID, ListingID: f.listing.ID, ExpectedQuantity: tradeUnits,
		Bags: ClaimBags{Items: append(items, back), Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse}}, ReplaceWallet: true}}

	thief := req
	thief.SellerID = f.buyer.ID
	beforeThief := f.snapshot(t)
	if res, err := f.s.RemoveListing(ctx, thief); err != nil || res.Outcome != OutcomeListingGone {
		t.Errorf("another character's removal: %+v, %v; want LISTING_GONE like a missing listing", res, err)
	}
	wrong := req
	wrong.ExpectedQuantity = tradeUnits - 1
	if res, err := f.s.RemoveListing(ctx, wrong); err != nil || res.Outcome != OutcomeQuantity || res.Available != tradeUnits {
		t.Errorf("a removal that expects other units: %+v, %v; want QUANTITY with %d", res, err, tradeUnits)
	}
	missing := req
	missing.ListingID = guid(0xdead)
	if res, err := f.s.RemoveListing(ctx, missing); err != nil || res.Outcome != OutcomeListingGone {
		t.Errorf("a removal of a listing that is not there: %+v, %v", res, err)
	}
	empty := req
	empty.Bags.Items = nil
	if _, err := f.s.RemoveListing(ctx, empty); !errors.Is(err, ErrInvalid) {
		t.Errorf("a removal with no bags: %v, want ErrInvalid", err)
	}
	if _, err := f.s.RemoveListing(ctx, RemoveRequest{SellerID: strings.Repeat("e", 32), ListingID: f.listing.ID, ExpectedQuantity: tradeUnits, Bags: req.Bags}); err != nil {
		// an unknown seller owns no listing: it is not this seller's
		t.Errorf("a removal by a character that is not there: %v, want the listing gone", err)
	}
	if f.snapshot(t) != beforeThief {
		t.Fatal("a refused removal changed rows")
	}

	res, err := f.s.RemoveListing(ctx, req)
	if err != nil || res.Outcome != OutcomeDone {
		t.Fatalf("RemoveListing = %+v, %v; want done", res, err)
	}
	if n := storedGuidCount(t, f.s, f.seller, f.stack.InstanceGUID); n != 1 {
		t.Errorf("the stack is in the seller's bags %d times, want once", n)
	}
	if status, _ := f.listingRow(t, f.listing.ID); status != MarketStatusRemoved {
		t.Errorf("the listing's status is %d, want REMOVED", status)
	}
	if purse := walletOf(t, f.s, f.seller); purse != tradePurse {
		t.Errorf("the seller's purse is %d: a removal moves no money", purse)
	}
	if mails := f.saleMails(t); len(mails) != 0 {
		t.Errorf("a removal made %d sale mails", len(mails))
	}
	if _, copper := f.collected(t); copper != 0 {
		t.Errorf("a removal added %d to the node's total", copper)
	}
	listed, err := f.s.MarketListings(ctx)
	if err != nil || len(listed) != 0 {
		t.Errorf("MarketListings = %d rows, %v; a removed listing is not read again", len(listed), err)
	}
	after := f.snapshot(t)
	if again, err := f.s.RemoveListing(ctx, req); err != nil || again.Outcome != OutcomeReplayed {
		t.Errorf("the same removal again: %+v, %v; want REPLAYED", again, err)
	}
	if f.snapshot(t) != after {
		t.Error("a repeat of the removal changed rows")
	}
	// a removed listing cannot be bought
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 1, 0.5)); err != nil || res.Outcome != OutcomeListingGone {
		t.Errorf("a purchase of a removed listing: %+v, %v; want LISTING_GONE", res, err)
	}
}

// A removal of a listing that was sold in part gives back the units that are left, and one whose time has come is refused (the expiry takes it).
func TestRemoveListingAfterAPartSaleAndAfterTheTime(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 4, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	items, err := f.s.Inventory(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	back := f.stack
	back.Quantity, back.Slot = 6, free(items)
	req := RemoveRequest{SellerID: f.seller.ID, ListingID: f.listing.ID, ExpectedQuantity: tradeUnits, // the world's cache is behind: it holds the ten
		Bags: ClaimBags{Items: append(items, back), Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse}}, ReplaceWallet: true}}
	if res, err := f.s.RemoveListing(ctx, req); err != nil || res.Outcome != OutcomeQuantity || res.Available != 6 {
		t.Fatalf("a removal with a stale quantity: %+v, %v; want QUANTITY with 6", res, err)
	}
	req.ExpectedQuantity = 6
	if res, err := f.s.RemoveListing(ctx, req); err != nil || res.Outcome != OutcomeDone {
		t.Fatalf("the removal of the 6 that are left: %+v, %v", res, err)
	}
	// the sale mail of the 4 sold units stays
	if mails := f.saleMails(t); len(mails) != 1 || mails[0].MoneyCopper != 400 {
		t.Errorf("the sale mails are %+v, want the one of the four units sold", mails)
	}

	late := MarketListing{ID: guid(0x9005), Node: marketNode, Item: item(Backpack, 9, tradeRecord, guid(0x5005)), PricePerUnit: tradePrice, Status: MarketStatusListed,
		IssuedAtMs: nowMs() - 100_000, ExpiresAtMs: nowMs() - 1000}
	f.list(t, f.seller, late)
	items, err = f.s.Inventory(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	lateBack := late.Item
	lateBack.Slot = free(items)
	before := f.snapshot(t)
	res, err := f.s.RemoveListing(ctx, RemoveRequest{SellerID: f.seller.ID, ListingID: late.ID, ExpectedQuantity: 1,
		Bags: ClaimBags{Items: append(items, lateBack), Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse}}, ReplaceWallet: true}})
	if err != nil || res.Outcome != OutcomeListingExpired {
		t.Fatalf("a removal of an expired listing: %+v, %v; want LISTING_EXPIRED", res, err)
	}
	if f.snapshot(t) != before {
		t.Error("the refused removal of an expired listing changed rows")
	}
}

// An expiry marks the listing EXPIRED_CLAIMED and mails the stack to the seller; it does not count against the mailbox's cap, it does not run early, and asked again it
// writes nothing.
func TestExpireListingMailsTheStack(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.ExpireListing(ctx, f.listing.ID, tradeItemName); err != nil || res.Outcome != OutcomeNotExpired {
		t.Fatalf("an expiry before the time: %+v, %v; want NOT_EXPIRED", res, err)
	}
	if res, err := f.s.ExpireListing(ctx, guid(0xdead), tradeItemName); err != nil || res.Outcome != OutcomeListingGone {
		t.Fatalf("an expiry of a listing that is not there: %+v, %v", res, err)
	}
	if _, err := f.s.ExpireListing(ctx, "nope", tradeItemName); !errors.Is(err, ErrInvalid) {
		t.Fatalf("an expiry of an id that is no id: %v, want ErrInvalid", err)
	}
	old := MarketListing{ID: guid(0x9006), Node: marketNode, Item: item(Backpack, 9, tradeRecord, guid(0x5006)), PricePerUnit: tradePrice, Status: MarketStatusListed,
		IssuedAtMs: nowMs() - 100_000, ExpiresAtMs: nowMs() - 1000}
	old.Item.Quantity = 7
	f.list(t, f.seller, old)
	// the mailbox is full: the Marketplace's mail comes all the same
	for i := 0; i < MaxMails; i++ {
		if _, err := f.s.CreateMail(ctx, f.seller.ID, NewMail{Sender: "Brannoc", Subject: fmt.Sprintf("m%d", i)}); err != nil {
			t.Fatalf("mail %d: %v", i, err)
		}
	}
	if _, err := f.s.CreateMail(ctx, f.seller.ID, NewMail{Sender: "Brannoc", Subject: "one too many"}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("the 201st mail: %v, want the cap", err)
	}
	// a buyer cannot buy it any more
	if res, err := f.s.BuyListing(ctx, func() BuyRequest {
		r := f.buy(t, f.buyer, guid(0xa001), 1, 0.5)
		r.ListingID = old.ID
		return r
	}()); err != nil || res.Outcome != OutcomeListingExpired {
		t.Fatalf("a purchase of an expired listing: %+v, %v; want LISTING_EXPIRED", res, err)
	}
	res, err := f.s.ExpireListing(ctx, old.ID, tradeItemName)
	if err != nil || res.Outcome != OutcomeDone || res.SellerID != f.seller.ID {
		t.Fatalf("ExpireListing = %+v, %v; want done for the seller", res, err)
	}
	if status, _ := f.listingRow(t, old.ID); status != MarketStatusExpiredClaimed {
		t.Errorf("the listing's status is %d, want EXPIRED_CLAIMED", status)
	}
	m := res.SellerMail
	if m.Sender != MarketMailSender || m.Subject != "Listing expired" || m.Body != "Your listing of 7 x Oak Wood ended after 3 days. The item is attached." || m.ExpiresAtMs != 0 ||
		len(m.Items) != 1 || m.Items[0].InstanceGUID != old.Item.InstanceGUID || m.Items[0].Quantity != 7 || m.Items[0].ItemRecordID != tradeRecord || m.Items[0].Claimed || m.MoneyCopper != 0 {
		t.Errorf("the expiry mail is %+v", m)
	}
	mails, err := f.s.Mails(ctx, f.seller.ID)
	if err != nil || len(mails) != MaxMails+1 {
		t.Errorf("the seller has %d mails (%v), want the 200 and the Marketplace's", len(mails), err)
	}
	after := f.snapshot(t)
	if again, err := f.s.ExpireListing(ctx, old.ID, tradeItemName); err != nil || again.Outcome != OutcomeReplayed {
		t.Errorf("the same expiry again: %+v, %v; want REPLAYED", again, err)
	}
	if f.snapshot(t) != after {
		t.Error("a repeat of the expiry changed rows")
	}
	if res, err := f.s.ExpireListing(ctx, f.listing.ID, tradeItemName); err != nil || res.Outcome != OutcomeNotExpired {
		t.Errorf("a listing that is not due: %+v, %v", res, err)
	}
	// a sold listing is not expired, and a removal of an expired listing's seller finds it gone
	if _, err := f.s.pool.Exec(ctx, `UPDATE character_market_listings SET status = $2 WHERE id = $1`, f.listing.ID, MarketStatusSold); err != nil {
		t.Fatal(err)
	}
	if res, err := f.s.ExpireListing(ctx, f.listing.ID, tradeItemName); err != nil || res.Outcome != OutcomeListingGone {
		t.Errorf("the expiry of a sold listing: %+v, %v; want LISTING_GONE", res, err)
	}
}

// The seller takes the sale's money into the purse: the purse is stored and the money marked claimed together, the claim asked again is answered and not written again,
// a second claim with another purse is refused, and another character's mail is never touched.
func TestClaimMailTakesTheMoneyIntoThePurseTogether(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), tradeUnits, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	mail := f.saleMails(t)[0]
	items, err := f.s.Inventory(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	claim := func(who string, money []MoneyClaim, purse int64) (ClaimResult, error) {
		return f.s.ClaimMail(ctx, who, nil, money, ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: purse}}, ReplaceWallet: true})
	}
	before := f.snapshot(t)
	if _, err := claim(f.buyer.ID, moneyOf(mail.ID, 1000), tradePurse+1000); !errors.Is(err, ErrNotFound) {
		t.Errorf("another character's claim of the money: %v, want ErrNotFound", err)
	}
	if _, err := claim(f.seller.ID, moneyOf(guid(0xdead), 1000), tradePurse+1000); !errors.Is(err, ErrNotFound) {
		t.Errorf("a claim of a mail that is not there: %v, want ErrNotFound", err)
	}
	plain, err := f.s.CreateMail(ctx, f.seller.ID, NewMail{Sender: "Brannoc", Subject: "hello"})
	if err != nil {
		t.Fatal(err)
	}
	before = f.snapshot(t)
	if _, err := claim(f.seller.ID, moneyOf(plain.ID, 1000), tradePurse+1000); !errors.Is(err, ErrNotFound) {
		t.Errorf("a claim of the money of a mail that has none: %v, want ErrNotFound", err)
	}
	if _, err := f.s.ClaimMail(ctx, f.seller.ID, nil, moneyOf(mail.ID, 1000), ClaimBags{Items: items}); !errors.Is(err, ErrInvalid) {
		t.Errorf("a claim of money without the purse: %v, want ErrInvalid", err)
	}
	if _, err := f.s.ClaimMail(ctx, f.seller.ID, nil, nil, ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1}}, ReplaceWallet: true}); !errors.Is(err, ErrInvalid) {
		t.Errorf("a claim of nothing: %v, want ErrInvalid", err)
	}
	if _, err := f.s.ClaimMail(ctx, f.seller.ID, nil, []MoneyClaim{{MailID: mail.ID, Copper: 1000}, {MailID: mail.ID, Copper: 1000}}, ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1}}, ReplaceWallet: true}); !errors.Is(err, ErrInvalid) {
		t.Errorf("a claim of the same money twice: %v, want ErrInvalid", err)
	}
	// an attachment that is not there refuses the whole claim: the money of the same request stays in the mail
	if _, err := f.s.ClaimMail(ctx, f.seller.ID, []MailClaim{{MailID: mail.ID, InstanceGUID: guid(0xbeef)}}, moneyOf(mail.ID, 1000),
		ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse + 1000}}, ReplaceWallet: true}); !errors.Is(err, ErrNotFound) {
		t.Errorf("a claim with a missing attachment: %v, want ErrNotFound", err)
	}
	if f.snapshot(t) != before {
		t.Fatal("a refused claim changed rows")
	}

	res, err := claim(f.seller.ID, moneyOf(mail.ID, 1000), tradePurse+1000)
	if err != nil || res.Claimed != 1 || res.Replayed {
		t.Fatalf("ClaimMail = %+v, %v; want the money claimed", res, err)
	}
	if purse := walletOf(t, f.s, f.seller); purse != tradePurse+1000 {
		t.Errorf("the purse is %d, want %d", purse, tradePurse+1000)
	}
	mails, err := f.s.Mails(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	for _, m := range mails {
		if m.ID == mail.ID && (!m.MoneyClaimed || m.MoneyCopper != 1000) {
			t.Errorf("the mail reads money %d claimed %v, want 1000 claimed", m.MoneyCopper, m.MoneyClaimed)
		}
	}
	after := f.snapshot(t)
	if again, err := claim(f.seller.ID, moneyOf(mail.ID, 1000), tradePurse+1000); err != nil || !again.Replayed || again.Claimed != 0 {
		t.Errorf("the same claim again: %+v, %v; want replayed, nothing claimed now", again, err)
	}
	if f.snapshot(t) != after {
		t.Error("a repeat of the claim changed rows")
	}
	if _, err := claim(f.seller.ID, moneyOf(mail.ID, 1000), tradePurse+2000); !errors.Is(err, ErrMailClaimed) {
		t.Errorf("a second claim of the money with a purse that holds it twice: %v, want ErrMailClaimed", err)
	}
	if f.snapshot(t) != after {
		t.Error("the refused second claim changed rows")
	}
	// a later sale of the same listing is a new mail: the claimed one is closed
	f2 := MarketListing{ID: guid(0x9007), Node: marketNode, Item: item(Backpack, 9, tradeRecord, guid(0x5007)), PricePerUnit: tradePrice, Status: MarketStatusListed, IssuedAtMs: nowMs(),
		ExpiresAtMs: nowMs() + 3_600_000}
	f2.Item.Quantity = 4
	f.list(t, f.seller, f2)
	bf := f.buy(t, f.buyer2, guid(0xa005), 2, 0.5)
	bf.ListingID = f2.ID
	bf.Bags.Items[len(bf.Bags.Items)-1].InstanceGUID = f.freshGUID()
	if res, err := f.s.BuyListing(ctx, bf); err != nil || res.Outcome != OutcomeDone {
		t.Fatalf("a purchase of another listing: %+v, %v", res, err)
	}
	if mails := f.saleMails(t); len(mails) != 2 {
		t.Errorf("the seller has %d sale mails, want the claimed one and the new one", len(mails))
	}
}

// A sale that is added to the mail after the claimer read it is never lost: the claim that credited the older amount is refused with nothing written (the purse, the mail's
// mark and every other row stay), and a claim that credits what the mail holds pays both sales.
func TestClaimMailRefusesAMailThatGrewAfterTheClaimerReadIt(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 4, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	read := f.saleMails(t)
	if len(read) != 1 || read[0].MoneyCopper != 400 {
		t.Fatalf("the seller reads %+v, want one sale mail of 400", read)
	}
	mail := read[0]
	items, err := f.s.Inventory(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	claim := func(money []MoneyClaim, credited int64) (ClaimResult, error) {
		return f.s.ClaimMail(ctx, f.seller.ID, nil, money, ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse + credited}}, ReplaceWallet: true})
	}
	// the second sale of the same listing lands after the seller read the mail
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer2, guid(0xa002), 3, 0.5)); err != nil || res.Outcome != OutcomeDone || res.SellerMail.ID != mail.ID || res.SellerMail.MoneyCopper != 700 {
		t.Fatalf("the second sale: %+v, %v; want it added to the same mail, now 700", res, err)
	}
	before := f.snapshot(t)
	if _, err := claim(moneyOf(mail.ID, 400), 400); !errors.Is(err, ErrMailClaimed) {
		t.Errorf("a claim that credited 400 for a mail of 700: %v, want ErrMailClaimed", err)
	}
	if purse := walletOf(t, f.s, f.seller); purse != tradePurse {
		t.Errorf("the purse is %d after the refused claim, want %d: nothing was paid", purse, tradePurse)
	}
	if f.snapshot(t) != before {
		t.Error("the refused claim changed rows: the mail was marked claimed or the purse written")
	}
	if got := f.saleMails(t); len(got) != 1 || got[0].MoneyClaimed || got[0].MoneyCopper != 700 {
		t.Errorf("the mail is %+v after the refused claim, want 700 and not claimed", got)
	}
	// more than the mail holds is refused the same way: the purse would hold money the mail never had
	if _, err := claim(moneyOf(mail.ID, 800), 800); !errors.Is(err, ErrMailClaimed) {
		t.Errorf("a claim that credited 800 for a mail of 700: %v, want ErrMailClaimed", err)
	}
	// a claim that names an amount no mail carries is no claim
	for _, copper := range []int64{0, -5, MaxWalletAmount + 1} {
		if _, err := claim(moneyOf(mail.ID, copper), 1); !errors.Is(err, ErrInvalid) {
			t.Errorf("a claim of %d copper: %v, want ErrInvalid", copper, err)
		}
	}
	if f.snapshot(t) != before {
		t.Error("a refused claim changed rows")
	}
	// the seller reads the mail again: the claim that credits what it holds pays both sales, once
	res, err := claim(moneyOf(mail.ID, 700), 700)
	if err != nil || res.Claimed != 1 || res.Replayed {
		t.Fatalf("the claim of 700 = %+v, %v; want the money claimed", res, err)
	}
	if purse := walletOf(t, f.s, f.seller); purse != tradePurse+700 {
		t.Errorf("the purse is %d, want %d: both sales paid", purse, tradePurse+700)
	}
	if got := f.saleMails(t); len(got) != 1 || !got[0].MoneyClaimed || got[0].MoneyCopper != 700 {
		t.Errorf("the mail is %+v after the claim, want 700 claimed", got)
	}
	after := f.snapshot(t)
	if again, err := claim(moneyOf(mail.ID, 700), 700); err != nil || !again.Replayed {
		t.Errorf("the same claim again: %+v, %v; want replayed (a retry after a lost answer)", again, err)
	}
	if _, err := claim(moneyOf(mail.ID, 400), 400); !errors.Is(err, ErrMailClaimed) {
		t.Errorf("the old claim of 400 after the money was taken: %v, want ErrMailClaimed", err)
	}
	if f.snapshot(t) != after {
		t.Error("the repeats changed rows")
	}
}

// One stale money refuses the whole claim: the money of a mail that did not change is not marked claimed either, and an attachment of the same request stays unclaimed.
func TestClaimMailWithOneStaleMoneyClaimsNothing(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 4, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	second := MarketListing{ID: guid(0x9009), Node: marketNode, Item: item(Backpack, 9, tradeRecord, guid(0x5009)), PricePerUnit: tradePrice, Status: MarketStatusListed, IssuedAtMs: nowMs(),
		ExpiresAtMs: nowMs() + 3_600_000}
	second.Item.Quantity = 6
	f.list(t, f.seller, second)
	other := f.buy(t, f.buyer2, guid(0xa003), 2, 0.5)
	other.ListingID = second.ID
	other.Bags.Items[len(other.Bags.Items)-1].InstanceGUID = f.freshGUID()
	if res, err := f.s.BuyListing(ctx, other); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	old := MarketListing{ID: guid(0x900a), Node: marketNode, Item: item(Backpack, 10, tradeRecord, guid(0x500a)), PricePerUnit: tradePrice, Status: MarketStatusListed, IssuedAtMs: nowMs() - 100_000,
		ExpiresAtMs: nowMs() - 1000}
	f.list(t, f.seller, old)
	exp, err := f.s.ExpireListing(ctx, old.ID, tradeItemName)
	if err != nil || exp.Outcome != OutcomeDone {
		t.Fatal(exp, err)
	}
	mails := f.saleMails(t)
	if len(mails) != 2 {
		t.Fatalf("%d sale mails, want 2", len(mails))
	}
	read := map[string]int64{}
	for _, m := range mails {
		read[m.ID] = m.MoneyCopper
	}
	// one of the two mails grows after the seller read both
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer2, guid(0xa004), 1, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	items, err := f.s.Inventory(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	att := exp.SellerMail.Items[0]
	placed := att.item()
	placed.Slot = free(items)
	bags := ClaimBags{Items: append(items, placed), Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse + read[mails[0].ID] + read[mails[1].ID]}}, ReplaceWallet: true}
	before := f.snapshot(t)
	stale := []MoneyClaim{{MailID: mails[0].ID, Copper: read[mails[0].ID]}, {MailID: mails[1].ID, Copper: read[mails[1].ID]}}
	if _, err := f.s.ClaimMail(ctx, f.seller.ID, []MailClaim{{MailID: exp.SellerMail.ID, InstanceGUID: att.InstanceGUID}}, stale, bags); !errors.Is(err, ErrMailClaimed) {
		t.Fatalf("a claim of two moneys and an item with one money grown: %v, want ErrMailClaimed", err)
	}
	if f.snapshot(t) != before {
		t.Error("the refused claim changed rows: the unchanged mail was marked, the item was claimed or the bags were written")
	}
	if n := storedGuidCount(t, f.s, f.seller, att.InstanceGUID); n != 0 {
		t.Errorf("the mailed stack is in the bags %d times after the refused claim, want 0", n)
	}
}

// The claim that commits first closes the mail: a later sale of the same listing makes a new mail, and the first claim's money is not touched by it.
func TestSaleAfterTheClaimOfTheSameListingMakesANewMail(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 4, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	first := f.saleMails(t)[0]
	items, err := f.s.Inventory(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	if res, err := f.s.ClaimMail(ctx, f.seller.ID, nil, moneyOf(first.ID, 400), ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse + 400}}, ReplaceWallet: true}); err != nil || res.Claimed != 1 {
		t.Fatalf("the first claim: %+v, %v", res, err)
	}
	sold, err := f.s.BuyListing(ctx, f.buy(t, f.buyer2, guid(0xa002), 3, 0.5))
	if err != nil || sold.Outcome != OutcomeDone {
		t.Fatal(sold, err)
	}
	if sold.SellerMail.ID == first.ID || sold.SellerMail.MoneyCopper != 300 || sold.SellerMail.MoneyClaimed {
		t.Errorf("the sale after the claim is in mail %s with %d copper (claimed %v), want a new mail of 300 that is not claimed", sold.SellerMail.ID, sold.SellerMail.MoneyCopper, sold.SellerMail.MoneyClaimed)
	}
	mails := f.saleMails(t)
	if len(mails) != 2 {
		t.Fatalf("%d sale mails, want the claimed one and the new one", len(mails))
	}
	for _, m := range mails {
		if m.ID == first.ID && (!m.MoneyClaimed || m.MoneyCopper != 400) {
			t.Errorf("the first mail is %+v, want 400 claimed and unchanged", m)
		}
	}
	if res, err := f.s.ClaimMail(ctx, f.seller.ID, nil, moneyOf(sold.SellerMail.ID, 300), ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse + 700}}, ReplaceWallet: true}); err != nil || res.Claimed != 1 {
		t.Fatalf("the claim of the new mail: %+v, %v", res, err)
	}
	if purse := walletOf(t, f.s, f.seller); purse != tradePurse+700 {
		t.Errorf("the purse is %d, want %d: every sale paid once", purse, tradePurse+700)
	}
}

// A sale and a claim of one seller at the same time, in either order: whichever commits first, no sale is lost. The claim is refused when the sale got in first (and is made right
// at the next read), or the sale makes a new mail when the claim got in first; at the end the purse holds both sales, once.
func TestClaimAndASaleOfTheSameListingNeverLoseMoney(t *testing.T) {
	refused, newMail := 0, 0
	const rounds = 12
	for round := 0; round < rounds; round++ {
		f := newTradeFixture(t)
		ctx := context.Background()
		if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 1, 0.5)); err != nil || res.Outcome != OutcomeDone {
			t.Fatal(res, err)
		}
		first := f.saleMails(t)[0]
		items, err := f.s.Inventory(ctx, f.seller.ID)
		if err != nil {
			t.Fatal(err)
		}
		second := f.buy(t, f.buyer2, guid(0xa002), 1, 0.5)
		var claimErr, buyErr error
		var bought BuyResult
		var wg sync.WaitGroup
		start := make(chan struct{})
		wg.Add(2)
		go func() {
			defer wg.Done()
			<-start
			_, claimErr = f.s.ClaimMail(ctx, f.seller.ID, nil, moneyOf(first.ID, 100), ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse + 100}}, ReplaceWallet: true})
		}()
		go func() {
			defer wg.Done()
			<-start
			bought, buyErr = f.s.BuyListing(ctx, second)
		}()
		close(start)
		wg.Wait()
		if buyErr != nil || bought.Outcome != OutcomeDone {
			t.Fatalf("round %d: the sale: %+v, %v", round, bought, buyErr)
		}
		switch {
		case claimErr == nil:
			newMail++
			if bought.SellerMail.ID == first.ID {
				t.Fatalf("round %d: the claim was stored, and the sale was added to the claimed mail", round)
			}
		case errors.Is(claimErr, ErrMailClaimed):
			refused++
			if bought.SellerMail.ID != first.ID {
				t.Fatalf("round %d: the claim was refused, but the sale is not in the mail it named (%s, not %s)", round, bought.SellerMail.ID, first.ID)
			}
		default:
			t.Fatalf("round %d: the claim: %v", round, claimErr)
		}
		// the seller takes whatever is unclaimed, each with the amount it holds now
		for _, m := range f.saleMails(t) {
			if m.MoneyClaimed {
				continue
			}
			purse := walletOf(t, f.s, f.seller)
			if _, err := f.s.ClaimMail(ctx, f.seller.ID, nil, moneyOf(m.ID, m.MoneyCopper), ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: purse + m.MoneyCopper}}, ReplaceWallet: true}); err != nil {
				t.Fatalf("round %d: the claim of the rest: %v", round, err)
			}
		}
		if purse := walletOf(t, f.s, f.seller); purse != tradePurse+200 {
			t.Fatalf("round %d: the purse is %d, want %d: a sale was lost or paid twice", round, purse, tradePurse+200)
		}
		var total int64
		for _, m := range f.saleMails(t) {
			if !m.MoneyClaimed {
				t.Errorf("round %d: a sale mail is still unclaimed: %+v", round, m)
			}
			total += m.MoneyCopper
		}
		if total != 200 {
			t.Errorf("round %d: the sale mails hold %d copper, want 200", round, total)
		}
	}
	t.Logf("%d rounds: the claim was refused for the sale that got in first %d times, the sale made a new mail %d times", rounds, refused, newMail)
}

// A claim of an attachment and of money in one request is one transaction too; the deleted mail's money cannot be claimed; and a sale mail the seller deleted is let go when the
// listing sells again.
func TestClaimMailItemAndMoneyTogetherAndADeletedSaleMail(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 3, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	sale := f.saleMails(t)[0]
	// the seller deletes the sale mail (it was read first): its money is forfeited by that act, and the next sale of the listing makes a new mail
	del := sale
	del.Status |= MailStatusRead | MailStatusDeleted
	if err := f.s.SaveMail(ctx, f.seller.ID, del); err != nil {
		t.Fatal(err)
	}
	items, err := f.s.Inventory(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.ClaimMail(ctx, f.seller.ID, nil, moneyOf(sale.ID, 300), ClaimBags{Items: items, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse + 300}}, ReplaceWallet: true}); !errors.Is(err, ErrNotFound) {
		t.Errorf("a claim of the money of a deleted mail: %v, want ErrNotFound", err)
	}
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer2, guid(0xa002), 2, 0.5)); err != nil || res.Outcome != OutcomeDone || res.SellerMail.ID == sale.ID || res.SellerMail.MoneyCopper != 200 {
		t.Fatalf("the sale after the mail was deleted: %+v, %v; want a new mail of 200", res, err)
	}

	// the expiry mail carries an item: claim it with money in one request
	old := MarketListing{ID: guid(0x9008), Node: marketNode, Item: item(Backpack, 9, tradeRecord, guid(0x5008)), PricePerUnit: tradePrice, Status: MarketStatusListed,
		IssuedAtMs: nowMs() - 100_000, ExpiresAtMs: nowMs() - 1000}
	f.list(t, f.seller, old)
	exp, err := f.s.ExpireListing(ctx, old.ID, tradeItemName)
	if err != nil || exp.Outcome != OutcomeDone {
		t.Fatal(exp, err)
	}
	money := f.saleMails(t)
	if len(money) != 1 {
		t.Fatalf("%d sale mails", len(money))
	}
	items, err = f.s.Inventory(ctx, f.seller.ID)
	if err != nil {
		t.Fatal(err)
	}
	att := exp.SellerMail.Items[0]
	placed := att.item()
	placed.Slot = free(items)
	bags := ClaimBags{Items: append(items, placed), Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: tradePurse + 200}}, ReplaceWallet: true}
	res, err := f.s.ClaimMail(ctx, f.seller.ID, []MailClaim{{MailID: exp.SellerMail.ID, InstanceGUID: att.InstanceGUID}}, moneyOf(money[0].ID, 200), bags)
	if err != nil || res.Claimed != 2 {
		t.Fatalf("ClaimMail of an item and money = %+v, %v; want 2 claimed", res, err)
	}
	if n := storedGuidCount(t, f.s, f.seller, att.InstanceGUID); n != 1 {
		t.Errorf("the mailed stack is in the bags %d times, want once", n)
	}
	if again, err := f.s.ClaimMail(ctx, f.seller.ID, []MailClaim{{MailID: exp.SellerMail.ID, InstanceGUID: att.InstanceGUID}}, moneyOf(money[0].ID, 200), bags); err != nil || !again.Replayed {
		t.Errorf("the same claim again: %+v, %v; want replayed", again, err)
	}
}

// Mail saves do not touch the money: SaveMail never claims it or changes it, and it keeps the Marketplace's own text.
func TestSaveMailLeavesTheMoneyAndTheMarketplaceTextAlone(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 3, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	mail := f.saleMails(t)[0]
	stale := mail
	stale.Body = "an older text"
	stale.MoneyCopper, stale.MoneyClaimed = 999_999, true
	stale.Status |= MailStatusRead
	if err := f.s.SaveMail(ctx, f.seller.ID, stale); err != nil {
		t.Fatal(err)
	}
	got := f.saleMails(t)
	if len(got) != 1 || got[0].MoneyCopper != 300 || got[0].MoneyClaimed || got[0].Body != mail.Body || got[0].Status&MailStatusRead == 0 {
		t.Errorf("after a save with other text and money the mail is %+v; want the money and the text as they were, and the read mark", got)
	}
}

// Every node that sold has its rate and its total read back; a node that never sold has none (it charges the default).
func TestNodeMarketTaxesAreReadBack(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if taxes, err := f.s.NodeMarketTaxes(ctx); err != nil || len(taxes) != 0 {
		t.Fatalf("before any sale: %v, %v; want no row", taxes, err)
	}
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 2, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	taxes, err := f.s.NodeMarketTaxes(ctx)
	if err != nil || len(taxes) != 1 || taxes[0] != (NodeMarketTax{Node: marketNode, Rate: 0.5, CollectedCopper: 100}) {
		t.Errorf("NodeMarketTaxes = %+v, %v; want the node at 0.5 with 100 collected", taxes, err)
	}
	// a node whose rate was changed (a later mayor's work): a sale charges that rate, and refuses the default
	if _, err := f.s.pool.Exec(ctx, `UPDATE node_market_taxes SET rate = 0.25 WHERE node = $1`, marketNode); err != nil {
		t.Fatal(err)
	}
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer2, guid(0xa002), 2, 0.5)); err != nil || res.Outcome != OutcomeRateChanged {
		t.Errorf("a sale at the old rate: %+v, %v; want RATE_CHANGED", res, err)
	}
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer2, guid(0xa003), 2, 0.25)); err != nil || res.Outcome != OutcomeDone {
		t.Errorf("a sale at the node's rate: %+v, %v", res, err)
	}
	if _, copper := f.collected(t); copper != 100+50 {
		t.Errorf("the node's total is %d, want 100 + 50", copper)
	}
}

// The listing read still shows only listed rows, with the units a part sale left.
func TestMarketListingsShowThePartSaleLeftovers(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.BuyListing(ctx, f.buy(t, f.buyer, guid(0xa001), 4, 0.5)); err != nil || res.Outcome != OutcomeDone {
		t.Fatal(res, err)
	}
	listed, err := f.s.MarketListings(ctx)
	if err != nil || len(listed) != 1 || listed[0].Item.Quantity != 6 || listed[0].ID != f.listing.ID || listed[0].Item.InstanceGUID != f.stack.InstanceGUID {
		t.Errorf("MarketListings = %+v, %v; want the listing with 6 units and its own guid", listed, err)
	}
}

// The Marketplace's mails do not count against the mailbox's cap of 200 live mails: a mailbox full of them still takes the other mail, and they are made past the cap.
func TestMarketplaceMailsDoNotCountAgainstTheMailboxCap(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	for i := 0; i < MaxMails-1; i++ {
		if _, err := f.s.CreateMail(ctx, f.seller.ID, NewMail{Sender: "Brannoc", Subject: fmt.Sprintf("m%d", i)}); err != nil {
			t.Fatalf("mail %d: %v", i, err)
		}
	}
	for i := 0; i < 5; i++ {
		l := MarketListing{ID: guid(0x9100 + i), Node: marketNode, Item: item(Backpack, 20+int32(i), tradeRecord, guid(0x5100+i)), PricePerUnit: tradePrice, Status: MarketStatusListed,
			IssuedAtMs: nowMs() - 100_000, ExpiresAtMs: nowMs() - 1000}
		f.list(t, f.seller, l)
		if res, err := f.s.ExpireListing(ctx, l.ID, tradeItemName); err != nil || res.Outcome != OutcomeDone {
			t.Fatalf("the expiry %d: %+v, %v", i, res, err)
		}
	}
	if _, err := f.s.CreateMail(ctx, f.seller.ID, NewMail{Sender: "Brannoc", Subject: "the 200th"}); err != nil {
		t.Errorf("the 200th mail of the others, with 5 Marketplace mails in the box: %v", err)
	}
	if _, err := f.s.CreateMail(ctx, f.seller.ID, NewMail{Sender: "Brannoc", Subject: "the 201st"}); !errors.Is(err, ErrInvalid) {
		t.Errorf("the 201st mail of the others: %v, want the cap", err)
	}
	mails, err := f.s.Mails(ctx, f.seller.ID)
	if err != nil || len(mails) != MaxMails+5 {
		t.Errorf("the mailbox holds %d mails (%v), want the 200 and the 5 of the Marketplace", len(mails), err)
	}
}

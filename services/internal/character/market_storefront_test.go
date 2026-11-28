// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Marketplace storefront descriptions (migration 0034): SetStorefrontDescription and MarketStorefronts. These tests run against PostgreSQL (a skip is not a pass): an edit
// writes the text and raises its node's count together or not at all, a text belongs to one Marketplace, an empty text clears, the same text again writes nothing, a seller with
// no listing at the node is refused with nothing written, a text the rules refuse changes nothing, and two edits at once end with the last one, whole.

import (
	"context"
	"errors"
	"strings"
	"sync"
	"testing"
)

const otherMarketNode int64 = marketNode + 1

// front reads a stored description and a node's count straight from the tables ("" and 0 for no row).
func (f *tradeFixture) front(t *testing.T, node int64, who Character) (text string, raises int64) {
	t.Helper()
	ctx := context.Background()
	var n int
	if err := f.s.pool.QueryRow(ctx, `SELECT count(*) FROM market_storefronts WHERE node = $1 AND character_id = $2`, node, who.ID).Scan(&n); err != nil {
		t.Fatal(err)
	}
	if n == 1 {
		if err := f.s.pool.QueryRow(ctx, `SELECT description FROM market_storefronts WHERE node = $1 AND character_id = $2`, node, who.ID).Scan(&text); err != nil {
			t.Fatal(err)
		}
	}
	if err := f.s.pool.QueryRow(ctx, `SELECT coalesce((SELECT header_raises FROM node_market_headers WHERE node = $1), 0)`, node).Scan(&raises); err != nil {
		t.Fatal(err)
	}
	return text, raises
}

// rowCounts is the number of rows of the two tables of migration 0034.
func (f *tradeFixture) rowCounts(t *testing.T) (fronts, headers int) {
	t.Helper()
	ctx := context.Background()
	if err := f.s.pool.QueryRow(ctx, `SELECT count(*) FROM market_storefronts`).Scan(&fronts); err != nil {
		t.Fatal(err)
	}
	if err := f.s.pool.QueryRow(ctx, `SELECT count(*) FROM node_market_headers`).Scan(&headers); err != nil {
		t.Fatal(err)
	}
	return fronts, headers
}

// listAt makes a second listing of `who` at `node` (the listing's id and stack are new).
func (f *tradeFixture) listAt(t *testing.T, who Character, node int64) {
	t.Helper()
	l := f.listing
	l.ID = f.freshGUID()
	l.Node = node
	l.Item = item(Backpack, 6, tradeRecord, f.freshGUID())
	l.Item.Quantity, l.Item.Quality = 2, 3
	f.list(t, who, l)
}

func TestSetStorefrontDescriptionStoresTheTextAndRaisesTheNodeCountTogether(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	res, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "Fine goods")
	if err != nil {
		t.Fatal(err)
	}
	if res.Outcome != StorefrontDone || res.Description != "Fine goods" || res.HeaderRaises != 1 {
		t.Fatalf("answer = %+v, want Done, the text and a count of 1", res)
	}
	if text, raises := f.front(t, marketNode, f.seller); text != "Fine goods" || raises != 1 {
		t.Errorf("stored = %q and %d, want the text and a count of 1", text, raises)
	}
	// a second change raises it again, and the stored text is the latest whole
	res, err = f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "Better goods")
	if err != nil || res.Outcome != StorefrontDone || res.HeaderRaises != 2 {
		t.Fatalf("second answer = %+v, %v; want Done with a count of 2", res, err)
	}
	if text, raises := f.front(t, marketNode, f.seller); text != "Better goods" || raises != 2 {
		t.Errorf("stored = %q and %d, want the second text and a count of 2", text, raises)
	}
	// the read for worldd's start carries both
	texts, counts, err := f.s.MarketStorefronts(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(texts) != 1 || texts[0] != (StorefrontText{Node: marketNode, SellerID: f.seller.ID, Description: "Better goods"}) {
		t.Errorf("MarketStorefronts texts = %+v, want the one description", texts)
	}
	if len(counts) != 1 || counts[0] != (NodeHeaderRaises{Node: marketNode, Raises: 2}) {
		t.Errorf("MarketStorefronts counts = %+v, want node %d at 2", counts, marketNode)
	}
}

func TestAStorefrontDescriptionBelongsToOneMarketplace(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	f.listAt(t, f.seller, otherMarketNode)
	if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "First market"); err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, otherMarketNode, "Second market"); err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, otherMarketNode, "Second market, changed"); err != nil {
		t.Fatal(err)
	}
	if text, raises := f.front(t, marketNode, f.seller); text != "First market" || raises != 1 {
		t.Errorf("the first market holds %q at count %d, want its own text at 1: an edit elsewhere must not touch it", text, raises)
	}
	if text, raises := f.front(t, otherMarketNode, f.seller); text != "Second market, changed" || raises != 2 {
		t.Errorf("the second market holds %q at count %d, want its own text at 2", text, raises)
	}
	// clearing at one market leaves the other
	if res, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, ""); err != nil || res.Outcome != StorefrontDone {
		t.Fatalf("clear at the first market: %+v, %v", res, err)
	}
	if text, _ := f.front(t, otherMarketNode, f.seller); text != "Second market, changed" {
		t.Errorf("clearing the first market changed the second one: %q", text)
	}
}

func TestTwoSellersAtOneMarketplaceKeepTheirOwnText(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	l := f.listing
	l.ID, l.Item = f.freshGUID(), item(Backpack, 5, tradeRecord, f.freshGUID())
	l.Item.Quantity, l.Item.Quality = 1, 3
	f.list(t, f.buyer, l)
	if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "the seller's"); err != nil {
		t.Fatal(err)
	}
	res, err := f.s.SetStorefrontDescription(ctx, f.buyer.ID, marketNode, "the other seller's")
	if err != nil || res.HeaderRaises != 2 {
		t.Fatalf("second seller: %+v, %v; want the node's count at 2", res, err)
	}
	if text, _ := f.front(t, marketNode, f.seller); text != "the seller's" {
		t.Errorf("the first seller's text = %q", text)
	}
	if text, _ := f.front(t, marketNode, f.buyer); text != "the other seller's" {
		t.Errorf("the second seller's text = %q", text)
	}
}

func TestAnEmptyDescriptionClearsAndTheSameTextAgainWritesNothing(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if res, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, ""); err != nil || res.Outcome != StorefrontUnchanged || res.HeaderRaises != 0 {
		t.Fatalf("clearing a storefront that has no text: %+v, %v; want Unchanged at 0", res, err)
	}
	if fronts, headers := f.rowCounts(t); fronts != 0 || headers != 0 {
		t.Fatalf("a clear of nothing left %d description row(s) and %d count row(s), want none: nothing is written", fronts, headers)
	}
	if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "To be cleared"); err != nil {
		t.Fatal(err)
	}
	// the same text again (an edit whose answer was lost, asked for again): nothing is written and the count stays
	res, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "To be cleared")
	if err != nil || res.Outcome != StorefrontUnchanged || res.HeaderRaises != 1 || res.Description != "To be cleared" {
		t.Fatalf("the same text again: %+v, %v; want Unchanged at 1 with the text", res, err)
	}
	res, err = f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "")
	if err != nil || res.Outcome != StorefrontDone || res.Description != "" || res.HeaderRaises != 2 {
		t.Fatalf("clear: %+v, %v; want Done, no text, a count of 2", res, err)
	}
	if text, raises := f.front(t, marketNode, f.seller); text != "" || raises != 2 {
		t.Errorf("after the clear: %q at count %d, want no text at 2", text, raises)
	}
	if fronts, _ := f.rowCounts(t); fronts != 0 {
		t.Errorf("the clear left %d description row(s), want none: the default text shows again", fronts)
	}
	if res, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, ""); err != nil || res.Outcome != StorefrontUnchanged || res.HeaderRaises != 2 {
		t.Errorf("a second clear: %+v, %v; want Unchanged at 2", res, err)
	}
}

func TestADescriptionIsStoredAsSentWithoutTrimming(t *testing.T) {
	f := newTradeFixture(t)
	for _, text := range []string{"  Journeyman \\  Apprentice Sickle, Pickaxe, Axe  ", "<b> & | ' ( ) * ! : ;", "Café 你好"} {
		res, err := f.s.SetStorefrontDescription(context.Background(), f.seller.ID, marketNode, text)
		if err != nil || res.Description != text {
			t.Fatalf("%q: %+v, %v", text, res, err)
		}
		if stored, _ := f.front(t, marketNode, f.seller); stored != text {
			t.Errorf("stored %q, want %q exactly (no trim, no change)", stored, text)
		}
	}
}

func TestASellerWithNoListingAtTheNodeIsRefusedWithNothingWritten(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	// a character who never listed, a seller at another market, and a seller whose only listing was sold, removed or expired
	for _, tc := range []struct {
		name string
		who  Character
		node int64
	}{
		{"a character with no listing", f.buyer, marketNode},
		{"a seller at the wrong market", f.seller, otherMarketNode},
	} {
		res, err := f.s.SetStorefrontDescription(ctx, tc.who.ID, tc.node, "I have nothing listed")
		if err != nil || res.Outcome != StorefrontNoListing {
			t.Errorf("%s: %+v, %v; want NoListing", tc.name, res, err)
		}
	}
	for _, status := range []int32{MarketStatusSold, MarketStatusRemoved, MarketStatusExpiredClaimed} {
		if _, err := f.s.pool.Exec(ctx, `UPDATE character_market_listings SET status = $1 WHERE id = $2`, status, f.listing.ID); err != nil {
			t.Fatal(err)
		}
		res, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "My listing is over")
		if err != nil || res.Outcome != StorefrontNoListing {
			t.Errorf("a listing with status %d: %+v, %v; want NoListing", status, res, err)
		}
	}
	if fronts, headers := f.rowCounts(t); fronts != 0 || headers != 0 {
		t.Errorf("the refusals left %d description row(s) and %d count row(s), want none: a refused edit changes nothing", fronts, headers)
	}
}

func TestADescriptionTheRulesRefuseChangesNothing(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "Kept"); err != nil {
		t.Fatal(err)
	}
	fifty := strings.Repeat("x", MaxStorefrontDescription)
	// 25 characters outside the first plane are 50 UTF-16 code units (the client's own limit); 26 are 52
	astral25, astral26 := strings.Repeat("\U0001F600", 25), strings.Repeat("\U0001F600", 26)
	for _, ok := range []string{fifty, astral25} {
		if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, ok); err != nil {
			t.Errorf("a text of 50 UTF-16 code units (%d bytes) was refused: %v", len(ok), err)
		}
	}
	if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "Kept"); err != nil {
		t.Fatal(err)
	}
	_, before := f.front(t, marketNode, f.seller)
	for _, tc := range []struct {
		name string
		text string
	}{
		{"a text of 51 characters", fifty + "x"},
		{"a text of 52 UTF-16 code units", astral26},
		{"a new line", "two\nlines"},
		{"a carriage return", "two\rlines"},
		{"a tab", "a\ttab"},
		{"a delete character", "a\x7fdelete"},
		{"a NUL", "a\x00nul"},
		{"text that is not UTF-8", "bad \xff\xfe bytes"},
	} {
		if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, tc.text); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: err = %v, want ErrInvalid", tc.name, err)
		}
	}
	if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, 0, "node zero"); !errors.Is(err, ErrInvalid) {
		t.Errorf("node 0: err = %v, want ErrInvalid", err)
	}
	if text, raises := f.front(t, marketNode, f.seller); text != "Kept" || raises != before {
		t.Errorf("after the refusals: %q at count %d, want %q at %d: nothing changed", text, raises, "Kept", before)
	}
	if _, err := f.s.SetStorefrontDescription(ctx, "ffffffffffffffffffffffffffffffff", marketNode, "ghost"); !errors.Is(err, ErrNotFound) {
		t.Errorf("an unknown character: err = %v, want ErrNotFound", err)
	}
	if _, err := f.s.SetStorefrontDescription(ctx, "not a character id", marketNode, "ghost"); !errors.Is(err, ErrNotFound) {
		t.Errorf("a malformed character id: err = %v, want ErrNotFound", err)
	}
}

// Many edits of one storefront at once: each runs after the other, every Done raises the count by exactly one (so the counts are 1..n with no gap and no repeat), and the
// stored text is exactly one of the texts, whole.
func TestManyEditsAtOnceEndWithTheLastOneWhole(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	const edits = 12
	texts := make([]string, edits)
	for i := range texts {
		texts[i] = strings.Repeat(string(rune('a'+i)), 30+i) // twelve texts of different letters and lengths: a mix of two would show
	}
	var wg sync.WaitGroup
	results := make([]StorefrontResult, edits)
	errs := make([]error, edits)
	start := make(chan struct{})
	for i := range texts {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			<-start
			results[i], errs[i] = f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, texts[i])
		}(i)
	}
	close(start)
	wg.Wait()
	seen := map[int64]bool{}
	for i, err := range errs {
		if err != nil {
			t.Fatalf("edit %d: %v", i, err)
		}
		if results[i].Outcome != StorefrontDone {
			t.Errorf("edit %d: %+v, want Done (each text is different from the one before it)", i, results[i])
		}
		if seen[results[i].HeaderRaises] {
			t.Errorf("two edits were answered with the count %d: the edits were not put in a line", results[i].HeaderRaises)
		}
		seen[results[i].HeaderRaises] = true
	}
	for n := int64(1); n <= edits; n++ {
		if !seen[n] {
			t.Errorf("no edit was answered with the count %d", n)
		}
	}
	stored, raises := f.front(t, marketNode, f.seller)
	if raises != edits {
		t.Errorf("the node's count = %d, want %d: one raise for each edit", raises, edits)
	}
	whole := -1
	for i, text := range texts {
		if stored == text {
			whole = i
		}
	}
	if whole < 0 {
		t.Fatalf("the stored text %q is none of the texts sent: the edits mixed", stored)
	}
	// the stored text is the one whose edit was the last in the line (the highest count)
	if results[whole].HeaderRaises != edits {
		t.Errorf("the stored text is edit %d's, answered with the count %d; the last edit in the line was answered with %d", whole, results[whole].HeaderRaises, edits)
	}
}

func TestTwoSellersEditingAtOnceBothKeepTheirText(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	l := f.listing
	l.ID, l.Item = f.freshGUID(), item(Backpack, 5, tradeRecord, f.freshGUID())
	l.Item.Quantity, l.Item.Quality = 1, 3
	f.list(t, f.buyer, l)
	var wg sync.WaitGroup
	errs := make([]error, 2)
	for i, who := range []Character{f.seller, f.buyer} {
		wg.Add(1)
		go func(i int, who Character) {
			defer wg.Done()
			_, errs[i] = f.s.SetStorefrontDescription(ctx, who.ID, marketNode, "text of "+who.Name)
		}(i, who)
	}
	wg.Wait()
	for i, err := range errs {
		if err != nil {
			t.Fatalf("seller %d: %v", i, err)
		}
	}
	if text, _ := f.front(t, marketNode, f.seller); text != "text of "+f.seller.Name {
		t.Errorf("the first seller's text = %q", text)
	}
	if text, raises := f.front(t, marketNode, f.buyer); text != "text of "+f.buyer.Name || raises != 2 {
		t.Errorf("the second seller's text = %q at the node's count %d, want his own text at 2", text, raises)
	}
}

func TestMarketStorefrontsReadsEveryNodeThatChanged(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if texts, counts, err := f.s.MarketStorefronts(ctx); err != nil || len(texts) != 0 || len(counts) != 0 {
		t.Fatalf("a database with no description: %+v, %+v, %v; want none", texts, counts, err)
	}
	f.listAt(t, f.seller, otherMarketNode)
	for _, step := range []struct {
		node int64
		text string
	}{{otherMarketNode, "two"}, {marketNode, "one"}, {marketNode, ""}} {
		if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, step.node, step.text); err != nil {
			t.Fatal(err)
		}
	}
	texts, counts, err := f.s.MarketStorefronts(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(texts) != 1 || texts[0].Node != otherMarketNode || texts[0].Description != "two" {
		t.Errorf("texts = %+v, want the one description at node %d (the other was cleared)", texts, otherMarketNode)
	}
	if len(counts) != 2 || counts[0] != (NodeHeaderRaises{Node: marketNode, Raises: 2}) || counts[1] != (NodeHeaderRaises{Node: otherMarketNode, Raises: 1}) {
		t.Errorf("counts = %+v, want node %d at 2 and node %d at 1, by node", counts, marketNode, otherMarketNode)
	}
}

// A storefront whose listing is gone keeps its text (the seller lists again and it shows again), and a deleted character takes it.
func TestADescriptionOutlivesItsListingAndGoesWithItsSeller(t *testing.T) {
	f := newTradeFixture(t)
	ctx := context.Background()
	if _, err := f.s.SetStorefrontDescription(ctx, f.seller.ID, marketNode, "Back soon"); err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.pool.Exec(ctx, `UPDATE character_market_listings SET status = $1 WHERE id = $2`, MarketStatusSold, f.listing.ID); err != nil {
		t.Fatal(err)
	}
	if text, _ := f.front(t, marketNode, f.seller); text != "Back soon" {
		t.Errorf("after the listing sold out the text is %q, want it kept", text)
	}
	if _, err := f.s.pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, f.seller.ID); err != nil {
		t.Fatal(err)
	}
	if fronts, _ := f.rowCounts(t); fronts != 0 {
		t.Errorf("a deleted seller left %d description row(s), want none", fronts)
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Marketplace sales (migration 0031): a purchase, a removal and an expiry, each ONE transaction, and the sales tax of a node.
//
//	BuyListing     locks the listing, then the buyer and the seller (in id order), checks the listing (listed, not expired, the units and the price asked for, the node's rate),
//	               writes the buyer's bags and purse, lowers the listing's units (the last unit marks it SOLD), adds the sale to the seller's mail, adds the tax to the node's total and
//	               records the purchase's id. The same purchase asked for again is answered REPLAYED and writes nothing.
//	RemoveListing  locks the listing and the seller, writes the seller's bags (the stack is back in them) and marks the listing REMOVED. The fee is not refunded.
//	ExpireListing  locks the listing and the seller, marks the listing EXPIRED_CLAIMED and adds a mail to the seller with the stack attached.
//
// The locks are always taken in the same order (the listing, then the characters by id, then their mail rows), so two steps never wait for each other. A step the rules refuse
// for a reason of the listing is answered with an outcome (nothing was written); a request that breaks the rules is ErrInvalid; an unknown character is ErrNotFound.

import (
	"context"
	"errors"
	"fmt"
	"math"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/jackc/pgx/v5"
)

// The client's status of a listing (EMarketplaceItemStatus). LISTED is MarketStatusListed (market.go).
const (
	MarketStatusSold           int32 = 1
	MarketStatusRemoved        int32 = 2
	MarketStatusExpiredClaimed int32 = 4
)

// The sender of the Marketplace's mails (the system's, not a character's).
const MarketMailSender = "Marketplace"

// DefaultMarketTaxRate is the game's default rate of the sales tax (TaxDefaultValue), the rate of a node with no row; its limits are the game's minimum and maximum.
const (
	DefaultMarketTaxRate float32 = 0.5
	MinMarketTaxRate     float32 = 0.1
	MaxMarketTaxRate     float32 = 1.0
)

// MaxMarketSum is the largest sum (the unit price times the units, or that and its tax) the client's own whole-number arithmetic can show: 2^31 and above it cannot.
const MaxMarketSum int64 = 1 << 31

// MarketOutcome is what a step answers when it is refused for a reason of the listing (nothing was written), or how it was stored.
type MarketOutcome int

const (
	OutcomeDone MarketOutcome = iota + 1
	OutcomeReplayed
	OutcomeListingGone
	OutcomeListingExpired
	OutcomeQuantity
	OutcomePriceChanged
	OutcomeRateChanged
	OutcomeOwnListing
	OutcomeNotExpired
)

// Stored says whether the outcome means the step is in the database now (done, or an earlier try of it).
func (o MarketOutcome) Stored() bool { return o == OutcomeDone || o == OutcomeReplayed }

// CartTax is the tax the client's cart computes for a sum: RoundToInt(float(sum) * percent + flat) in single precision, as `_mm_cvt_ss2si(F + F + 0.5) >> 1` (round to the
// nearest of the doubled value, so that a half goes up: 0.5 is 1, 1.5 is 2, 2.5 is 3). false for a value the client's own arithmetic cannot hold (the doubled value does not fit a 32-bit whole number) and for a negative sum.
func CartTax(sum int64, percent float32, flat int32) (int64, bool) {
	if sum < 0 {
		return 0, false
	}
	product := float32(float32(sum) * percent) // (an explicit conversion: no fused multiply-add)
	f := float32(product + float32(flat))
	doubled := float32(f + f)
	biased := float32(doubled + 0.5)
	if !(biased > -2147483648.0 && biased < 2147483648.0) {
		return 0, false
	}
	rounded := int32(math.RoundToEven(float64(biased)))
	return int64(rounded >> 1), true
}

// SaleCharge is what a buyer pays for `quantity` units at `price` copper each: the sum, the tax on it and both together.
type SaleCharge struct {
	Sum   int64
	Tax   int64
	Total int64
}

// ChargeFor works out a sale's charge at the node's rate (no flat part): false when the sum overflows, or the sum or the taxed total is 2^31 or more (what the client's
// arithmetic cannot show), or the units or the price are not above 0.
func ChargeFor(price int64, quantity int32, rate float32) (SaleCharge, bool) {
	if price <= 0 || quantity <= 0 {
		return SaleCharge{}, false
	}
	if price > MaxMarketSum/int64(quantity) {
		return SaleCharge{}, false // the sum would be 2^31 or more (and the product would not overflow before this check says so)
	}
	sum := price * int64(quantity)
	tax, ok := CartTax(sum, rate, 0)
	if !ok || tax < 0 || sum+tax >= MaxMarketSum {
		return SaleCharge{}, false
	}
	return SaleCharge{Sum: sum, Tax: tax, Total: sum + tax}, true
}

// BuyRequest is one purchase: the request's id, the buyer, the listing, the units, the price and the rate and tax the world charged, the name the sale mail shows and the
// buyer's bags and purse after the buy (ClaimBags: the whole item list, and the purse and the rest when their flags say to replace them).
type BuyRequest struct {
	PurchaseID   string
	BuyerID      string
	ListingID    string
	Quantity     int32
	PricePerUnit int64
	TaxRate      float32
	Tax          int64
	ItemName     string
	Bags         ClaimBags
}

// BuyResult is the answer of BuyListing.
type BuyResult struct {
	Outcome    MarketOutcome
	Available  int32  // QUANTITY: the units the listing holds
	SellerID   string // DONE and REPLAYED: the seller, whose mail changed
	SellerMail Mail   // DONE: the sale mail as it is now
}

// RemoveRequest takes a seller's listing off the market: the seller, the listing, the units the world holds for it and the seller's bags after the stack is back in them.
type RemoveRequest struct {
	SellerID         string
	ListingID        string
	ExpectedQuantity int32
	Bags             ClaimBags
}

// RemoveResult is the answer of RemoveListing.
type RemoveResult struct {
	Outcome   MarketOutcome
	Available int32
}

// ExpireResult is the answer of ExpireListing.
type ExpireResult struct {
	Outcome    MarketOutcome
	SellerID   string
	SellerMail Mail // DONE: the mail with the stack
}

// NodeMarketTax is a node's stored rate and the copper its sales have paid in.
type NodeMarketTax struct {
	Node            int64
	Rate            float32
	CollectedCopper int64
}

// lockedListing is a listing row as a step reads it under its lock.
type lockedListing struct {
	ID          string
	SellerID    string
	Node        int64
	Item        Item
	Price       int64
	Status      int32
	ExpiresAtMs int64
}

// readListingForUpdate reads and locks one listing's row; found is false when there is none.
func readListingForUpdate(ctx context.Context, tx pgx.Tx, id string) (l lockedListing, found bool, err error) {
	l.ID = id
	err = tx.QueryRow(ctx, `SELECT character_id, market_node, item_record_id, instance_guid, quantity, quality, durability, created_at, persisted_health, persisted_mana,
		lock_expiration, price_per_unit, status, expiry_date_ms FROM character_market_listings WHERE id = $1 FOR UPDATE`, id).Scan(&l.SellerID, &l.Node,
		&l.Item.ItemRecordID, &l.Item.InstanceGUID, &l.Item.Quantity, &l.Item.Quality, &l.Item.Durability, &l.Item.CreatedAt, &l.Item.PersistedHealth,
		&l.Item.PersistedMana, &l.Item.LockExpiration, &l.Price, &l.Status, &l.ExpiresAtMs)
	if errors.Is(err, pgx.ErrNoRows) {
		return l, false, nil
	}
	l.Item.Container, l.Item.Slot = Backpack, 0
	return l, err == nil, err
}

// databaseNowMs is the database's clock in Unix milliseconds: the one clock every step compares a listing's expiry with.
func databaseNowMs(ctx context.Context, tx pgx.Tx) (int64, error) {
	var ms int64
	err := tx.QueryRow(ctx, `SELECT (extract(epoch FROM clock_timestamp()) * 1000)::bigint`).Scan(&ms)
	return ms, err
}

// expiredAt says whether a listing's time has come at `nowMs` (an expiry of 0 never comes).
func (l lockedListing) expiredAt(nowMs int64) bool {
	return l.ExpiresAtMs > 0 && nowMs >= l.ExpiresAtMs
}

// lockCharacters takes the row locks of the characters in id order; ErrNotFound when one is missing. (A listing's seller always exists: the row belongs to it.)
func lockCharacters(ctx context.Context, tx pgx.Tx, ids ...string) error {
	seen := map[string]bool{}
	var distinct []string
	for _, id := range ids {
		if !seen[id] {
			seen[id] = true
			distinct = append(distinct, id)
		}
	}
	rows, err := tx.Query(ctx, `SELECT id FROM characters WHERE id = ANY($1) ORDER BY id FOR UPDATE`, distinct)
	if err != nil {
		return err
	}
	n := 0
	for rows.Next() {
		n++
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	if n != len(distinct) {
		return ErrNotFound
	}
	return nil
}

// checkBags runs the checks every write of the bags makes before anything is written (the claim's): a replaced part is valid, the item list is never empty.
func checkBags(bags ClaimBags) (wallet []WalletEntry, err error) {
	wallet = bags.Wallet
	if bags.ReplaceWallet {
		if err := ValidateWallet(wallet); err != nil {
			return nil, err
		}
	} else {
		wallet = nil
	}
	if bags.ReplaceJobs {
		if err := ValidateJobs(bags.Jobs); err != nil {
			return nil, err
		}
	}
	if bags.ReplaceKnown {
		if err := ValidateKnownRecipes(bags.KnownRecipes); err != nil {
			return nil, err
		}
	}
	warehouse := bags.Warehouse
	if bags.ReplaceWarehouse {
		if err := ValidateWarehouseItems(warehouse, bags.Items); err != nil {
			return nil, err
		}
	}
	if len(bags.Items) == 0 {
		return nil, fmt.Errorf("%w: a market step's bags are never empty (a character keeps its items)", ErrInvalid)
	}
	if err := ValidateItems(bags.Items); err != nil {
		return nil, err
	}
	return wallet, nil
}

// writeBags writes the bags a step carries in tx, with the character already locked.
func writeBags(ctx context.Context, tx pgx.Tx, characterID string, bags ClaimBags, wallet []WalletEntry) error {
	warehouse := bags.Warehouse
	if !bags.ReplaceWarehouse {
		warehouse = nil
	}
	return writeInventory(ctx, tx, characterID, bags.Items, wallet, bags.Jobs, bags.ReplaceJobs, bags.KnownRecipes, bags.ReplaceKnown, warehouse, bags.ReplaceWarehouse)
}

// sanitizedName is an item's name as a mail shows it: no control character, at most 100 bytes, "an item" when nothing is left.
func sanitizedName(name string) string {
	var b strings.Builder
	for _, r := range name {
		if r < 0x20 || r == 0x7f || r == utf8.RuneError {
			continue
		}
		b.WriteRune(r)
	}
	out := strings.TrimSpace(b.String())
	for len(out) > 100 {
		_, size := utf8.DecodeLastRuneInString(out)
		out = out[:len(out)-size]
	}
	out = strings.TrimSpace(out)
	if out == "" {
		return "an item"
	}
	return out
}

// FormatCopper writes an amount of copper as the mails say it: gold, silver and copper, each only when it is not 0 ("1 gold 25 silver 3 copper"; "0 copper" for none).
func FormatCopper(copper int64) string {
	gold, silver, rest := copper/10000, copper/100%100, copper%100
	var parts []string
	if gold > 0 {
		parts = append(parts, fmt.Sprintf("%d gold", gold))
	}
	if silver > 0 {
		parts = append(parts, fmt.Sprintf("%d silver", silver))
	}
	if rest > 0 || len(parts) == 0 {
		parts = append(parts, fmt.Sprintf("%d copper", rest))
	}
	return strings.Join(parts, " ")
}

// saleMailText is the subject and body of a sale mail that sums up `quantity` units sold for `copper`.
func saleMailText(itemName string, quantity int32, copper int64) (subject, body string) {
	return "Item sold", fmt.Sprintf("%d x %s sold for %s. The money is attached.", quantity, sanitizedName(itemName), FormatCopper(copper))
}

// expiredMailText is the subject and body of the mail an expired listing's stack comes back in.
func expiredMailText(itemName string, quantity int32) (subject, body string) {
	return "Listing expired", fmt.Sprintf("Your listing of %d x %s ended after 3 days. The item is attached.", quantity, sanitizedName(itemName))
}

// insertSystemMail adds one Marketplace mail for a character in tx, with the character already locked: no expiry (the money and the items are never lost), and it does not
// count against the mailbox's cap. `money` (copper, 0: none) is added in character_mail_money with the listing and the units a sale mail sums up.
func insertSystemMail(ctx context.Context, tx pgx.Tx, characterID, subject, body string, items []MailItem, money int64, listingID string, soldQuantity int32) (Mail, error) {
	id, err := freshMailGUID()
	if err != nil {
		return Mail{}, err
	}
	issued := time.Now().UnixMilli()
	m := Mail{ID: id, Sender: MarketMailSender, Subject: subject, Body: body, Status: 0, IssuedAtMs: issued, ExpiresAtMs: 0, Items: items}
	if err := ValidateMail(m); err != nil {
		return Mail{}, err
	}
	if _, err := tx.Exec(ctx, `INSERT INTO character_mails (id, character_id, sender, subject, body, status, issued_at_ms, expires_at_ms)
		VALUES ($1, $2, $3, $4, $5, 0, $6, 0)`, id, characterID, m.Sender, subject, body, issued); err != nil {
		return Mail{}, itemsError(err)
	}
	if err := insertMailItems(ctx, tx, id, items); err != nil {
		return Mail{}, itemsError(err)
	}
	if money > 0 {
		var listing any
		if listingID != "" {
			listing = listingID
		}
		if _, err := tx.Exec(ctx, `INSERT INTO character_mail_money (mail_id, copper, claimed, listing_id, sold_quantity) VALUES ($1, $2, false, $3, $4)`,
			id, money, listing, soldQuantity); err != nil {
			return Mail{}, itemsError(err)
		}
		m.MoneyCopper = money
	}
	return m, nil
}

// addSaleToMail adds a sale of `quantity` units for `copper` to the seller's unclaimed sale mail of the listing, or makes that mail. A sale mail the seller deleted is let
// go (its money was the seller's to throw away) and a new one is made.
func addSaleToMail(ctx context.Context, tx pgx.Tx, sellerID, listingID, itemName string, quantity int32, copper int64) (Mail, error) {
	var (
		mailID string
		stored int64
		sold   int32
		status int32
	)
	err := tx.QueryRow(ctx, `SELECT mm.mail_id, mm.copper, mm.sold_quantity, m.status FROM character_mail_money mm JOIN character_mails m ON m.id = mm.mail_id
		WHERE mm.listing_id = $1 AND NOT mm.claimed AND m.character_id = $2 FOR UPDATE OF mm, m`, listingID, sellerID).Scan(&mailID, &stored, &sold, &status)
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return Mail{}, err
	}
	if err == nil && status&MailStatusDeleted != 0 {
		if _, err := tx.Exec(ctx, `UPDATE character_mail_money SET listing_id = NULL WHERE mail_id = $1`, mailID); err != nil {
			return Mail{}, err
		}
		err = pgx.ErrNoRows
	}
	if errors.Is(err, pgx.ErrNoRows) {
		subject, body := saleMailText(itemName, quantity, copper)
		return insertSystemMail(ctx, tx, sellerID, subject, body, nil, copper, listingID, quantity)
	}
	total, units := stored+copper, sold+quantity
	if total > MaxWalletAmount {
		return Mail{}, fmt.Errorf("%w: the sale mail would carry %d copper (at most %d)", ErrInvalid, total, MaxWalletAmount)
	}
	subject, body := saleMailText(itemName, units, total)
	// the mail is unread again: it carries more than the seller saw
	if _, err := tx.Exec(ctx, `UPDATE character_mail_money SET copper = $2, sold_quantity = $3 WHERE mail_id = $1`, mailID, total, units); err != nil {
		return Mail{}, itemsError(err)
	}
	if _, err := tx.Exec(ctx, `UPDATE character_mails SET subject = $2, body = $3, status = status & ~($4::int) WHERE id = $1`, mailID, subject, body, MailStatusRead); err != nil {
		return Mail{}, itemsError(err)
	}
	return mailInTx(ctx, tx, mailID)
}

// mailInTx reads one mail with its attachments and its money in tx.
func mailInTx(ctx context.Context, tx pgx.Tx, id string) (Mail, error) {
	var m Mail
	err := tx.QueryRow(ctx, `SELECT m.id, m.sender, m.subject, m.body, m.status, m.issued_at_ms, m.expires_at_ms, COALESCE(mm.copper, 0), COALESCE(mm.claimed, false)
		FROM character_mails m LEFT JOIN character_mail_money mm ON mm.mail_id = m.id WHERE m.id = $1`, id).Scan(&m.ID, &m.Sender, &m.Subject, &m.Body, &m.Status,
		&m.IssuedAtMs, &m.ExpiresAtMs, &m.MoneyCopper, &m.MoneyClaimed)
	if err != nil {
		return Mail{}, err
	}
	rows, err := tx.Query(ctx, `SELECT item_record_id, instance_guid, quantity, quality, durability, created_at, persisted_health, persisted_mana, lock_expiration, claimed
		FROM character_mail_items WHERE mail_id = $1 ORDER BY position`, id)
	if err != nil {
		return Mail{}, err
	}
	for rows.Next() {
		var it MailItem
		if err := rows.Scan(&it.ItemRecordID, &it.InstanceGUID, &it.Quantity, &it.Quality, &it.Durability, &it.CreatedAt, &it.PersistedHealth, &it.PersistedMana,
			&it.LockExpiration, &it.Claimed); err != nil {
			rows.Close()
			return Mail{}, err
		}
		m.Items = append(m.Items, it)
	}
	rows.Close()
	return m, rows.Err()
}

// validateGUIDField says what is wrong with a 32-digit id a request carries.
func validateGUIDField(what, id string) error {
	if !guidPattern.MatchString(id) || id == strings.Repeat("0", 32) {
		return fmt.Errorf("%w: %s %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, what, id)
	}
	return nil
}

// NodeMarketTaxes reads the stored rate and the collected copper of every node that has a row, by node.
func (s *Store) NodeMarketTaxes(ctx context.Context) ([]NodeMarketTax, error) {
	rows, err := s.pool.Query(ctx, `SELECT node, rate, collected_copper FROM node_market_taxes ORDER BY node`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []NodeMarketTax
	for rows.Next() {
		var t NodeMarketTax
		if err := rows.Scan(&t.Node, &t.Rate, &t.CollectedCopper); err != nil {
			return nil, err
		}
		out = append(out, t)
	}
	return out, rows.Err()
}

// nodeRateForUpdate reads (and locks) the node's stored rate: the default when the node has no row.
func nodeRateForUpdate(ctx context.Context, tx pgx.Tx, node int64) (float32, error) {
	var rate float32
	err := tx.QueryRow(ctx, `SELECT rate FROM node_market_taxes WHERE node = $1 FOR UPDATE`, node).Scan(&rate)
	if errors.Is(err, pgx.ErrNoRows) {
		return DefaultMarketTaxRate, nil
	}
	return rate, err
}

// BuyListing stores one purchase (see the top of this file).
func (s *Store) BuyListing(ctx context.Context, r BuyRequest) (BuyResult, error) {
	if !idPattern.MatchString(r.BuyerID) {
		return BuyResult{}, ErrNotFound
	}
	if err := validateGUIDField("purchase id", r.PurchaseID); err != nil {
		return BuyResult{}, err
	}
	if err := validateGUIDField("listing id", r.ListingID); err != nil {
		return BuyResult{}, err
	}
	if r.Quantity < 1 || r.Quantity > MaxQuantity {
		return BuyResult{}, fmt.Errorf("%w: a purchase of %d units", ErrInvalid, r.Quantity)
	}
	if r.PricePerUnit <= 0 {
		return BuyResult{}, fmt.Errorf("%w: a unit price of %d copper", ErrInvalid, r.PricePerUnit)
	}
	if !(r.TaxRate >= MinMarketTaxRate && r.TaxRate <= MaxMarketTaxRate) {
		return BuyResult{}, fmt.Errorf("%w: a tax rate of %v (%v to %v)", ErrInvalid, r.TaxRate, MinMarketTaxRate, MaxMarketTaxRate)
	}
	charge, ok := ChargeFor(r.PricePerUnit, r.Quantity, r.TaxRate)
	if !ok || charge.Tax != r.Tax {
		return BuyResult{}, fmt.Errorf("%w: the sale of %d units at %d copper at the rate %v is charged %v, not a tax of %d", ErrInvalid, r.Quantity, r.PricePerUnit, r.TaxRate, charge, r.Tax)
	}
	wallet, err := checkBags(r.Bags)
	if err != nil {
		return BuyResult{}, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return BuyResult{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	l, found, err := readListingForUpdate(ctx, tx, r.ListingID)
	if err != nil {
		return BuyResult{}, err
	}
	// the purchase's own id first, under the listing's lock: an earlier try of this purchase that was stored (its answer lost) is answered done, whatever became of the listing
	var prevBuyer, prevListing string
	err = tx.QueryRow(ctx, `SELECT buyer_id, listing_id FROM market_purchases WHERE purchase_id = $1`, r.PurchaseID).Scan(&prevBuyer, &prevListing)
	switch {
	case err == nil:
		if prevBuyer != r.BuyerID || prevListing != r.ListingID {
			return BuyResult{}, fmt.Errorf("%w: purchase id %s was made for another buyer or listing", ErrInvalid, r.PurchaseID)
		}
		return BuyResult{Outcome: OutcomeReplayed, SellerID: l.SellerID}, nil
	case !errors.Is(err, pgx.ErrNoRows):
		return BuyResult{}, err
	}
	if !found {
		return BuyResult{Outcome: OutcomeListingGone}, nil
	}
	if l.SellerID == r.BuyerID {
		return BuyResult{Outcome: OutcomeOwnListing}, nil
	}
	if l.Status != MarketStatusListed {
		return BuyResult{Outcome: OutcomeListingGone}, nil
	}
	now, err := databaseNowMs(ctx, tx)
	if err != nil {
		return BuyResult{}, err
	}
	if l.expiredAt(now) {
		return BuyResult{Outcome: OutcomeListingExpired}, nil
	}
	if l.Price != r.PricePerUnit {
		return BuyResult{Outcome: OutcomePriceChanged, Available: l.Item.Quantity}, nil
	}
	if r.Quantity > l.Item.Quantity {
		return BuyResult{Outcome: OutcomeQuantity, Available: l.Item.Quantity}, nil
	}
	if err := lockCharacters(ctx, tx, r.BuyerID, l.SellerID); err != nil {
		return BuyResult{}, err
	}
	stored, err := nodeRateForUpdate(ctx, tx, l.Node)
	if err != nil {
		return BuyResult{}, err
	}
	if stored != r.TaxRate {
		return BuyResult{Outcome: OutcomeRateChanged, Available: l.Item.Quantity}, nil
	}
	whole := r.Quantity == l.Item.Quantity
	if !whole {
		// the listing stays listed with its guid: no bag may hold it too
		for _, it := range r.Bags.Items {
			if it.InstanceGUID == l.Item.InstanceGUID {
				return BuyResult{}, fmt.Errorf("%w: the buyer's bags hold the guid %s of a listing that stays listed", ErrInvalid, it.InstanceGUID)
			}
		}
	}
	if err := writeBags(ctx, tx, r.BuyerID, r.Bags, wallet); err != nil {
		return BuyResult{}, err
	}
	if whole {
		_, err = tx.Exec(ctx, `UPDATE character_market_listings SET status = $2 WHERE id = $1`, l.ID, MarketStatusSold) // (the row keeps its last quantity: the table forbids 0)
	} else {
		_, err = tx.Exec(ctx, `UPDATE character_market_listings SET quantity = quantity - $2 WHERE id = $1`, l.ID, r.Quantity)
	}
	if err != nil {
		return BuyResult{}, itemsError(err)
	}
	mail, err := addSaleToMail(ctx, tx, l.SellerID, l.ID, r.ItemName, r.Quantity, charge.Sum)
	if err != nil {
		return BuyResult{}, err
	}
	if _, err := tx.Exec(ctx, `INSERT INTO node_market_taxes (node, rate, collected_copper) VALUES ($1, $2, $3)
		ON CONFLICT (node) DO UPDATE SET collected_copper = node_market_taxes.collected_copper + EXCLUDED.collected_copper`, l.Node, stored, charge.Tax); err != nil {
		return BuyResult{}, itemsError(err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO market_purchases (purchase_id, listing_id, buyer_id, node, quantity, price_copper, tax_copper, created_ms)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`, r.PurchaseID, l.ID, r.BuyerID, l.Node, r.Quantity, charge.Sum, charge.Tax, now); err != nil {
		return BuyResult{}, itemsError(err)
	}
	if err := tx.Commit(ctx); err != nil {
		return BuyResult{}, err
	}
	return BuyResult{Outcome: OutcomeDone, SellerID: l.SellerID, SellerMail: mail}, nil
}

// RemoveListing stores a seller's removal of a listing (see the top of this file).
func (s *Store) RemoveListing(ctx context.Context, r RemoveRequest) (RemoveResult, error) {
	if !idPattern.MatchString(r.SellerID) {
		return RemoveResult{}, ErrNotFound
	}
	if err := validateGUIDField("listing id", r.ListingID); err != nil {
		return RemoveResult{}, err
	}
	if r.ExpectedQuantity < 1 || r.ExpectedQuantity > MaxQuantity {
		return RemoveResult{}, fmt.Errorf("%w: a removal that expects %d units", ErrInvalid, r.ExpectedQuantity)
	}
	wallet, err := checkBags(r.Bags)
	if err != nil {
		return RemoveResult{}, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return RemoveResult{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	l, found, err := readListingForUpdate(ctx, tx, r.ListingID)
	if err != nil {
		return RemoveResult{}, err
	}
	if !found || l.SellerID != r.SellerID {
		return RemoveResult{Outcome: OutcomeListingGone}, nil // another player's listing looks like a missing one
	}
	if l.Status == MarketStatusRemoved {
		return RemoveResult{Outcome: OutcomeReplayed}, nil // an earlier try stored it (the world asks once for each listing, so this is that try again)
	}
	if l.Status != MarketStatusListed {
		return RemoveResult{Outcome: OutcomeListingGone}, nil
	}
	now, err := databaseNowMs(ctx, tx)
	if err != nil {
		return RemoveResult{}, err
	}
	if l.expiredAt(now) {
		return RemoveResult{Outcome: OutcomeListingExpired}, nil
	}
	if l.Item.Quantity != r.ExpectedQuantity {
		return RemoveResult{Outcome: OutcomeQuantity, Available: l.Item.Quantity}, nil
	}
	if err := lockCharacters(ctx, tx, r.SellerID); err != nil {
		return RemoveResult{}, err
	}
	if err := writeBags(ctx, tx, r.SellerID, r.Bags, wallet); err != nil {
		return RemoveResult{}, err
	}
	if _, err := tx.Exec(ctx, `UPDATE character_market_listings SET status = $2 WHERE id = $1`, l.ID, MarketStatusRemoved); err != nil {
		return RemoveResult{}, itemsError(err)
	}
	if err := tx.Commit(ctx); err != nil {
		return RemoveResult{}, err
	}
	return RemoveResult{Outcome: OutcomeDone}, nil
}

// ExpireListing ends a listing whose time has come: the seller gets a mail with the stack attached (see the top of this file).
func (s *Store) ExpireListing(ctx context.Context, listingID, itemName string) (ExpireResult, error) {
	if err := validateGUIDField("listing id", listingID); err != nil {
		return ExpireResult{}, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return ExpireResult{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	l, found, err := readListingForUpdate(ctx, tx, listingID)
	if err != nil {
		return ExpireResult{}, err
	}
	if !found {
		return ExpireResult{Outcome: OutcomeListingGone}, nil
	}
	if l.Status == MarketStatusExpiredClaimed {
		return ExpireResult{Outcome: OutcomeReplayed, SellerID: l.SellerID}, nil
	}
	if l.Status != MarketStatusListed {
		return ExpireResult{Outcome: OutcomeListingGone}, nil
	}
	now, err := databaseNowMs(ctx, tx)
	if err != nil {
		return ExpireResult{}, err
	}
	if !l.expiredAt(now) {
		return ExpireResult{Outcome: OutcomeNotExpired}, nil
	}
	if err := lockCharacters(ctx, tx, l.SellerID); err != nil {
		return ExpireResult{}, err
	}
	if _, err := tx.Exec(ctx, `UPDATE character_market_listings SET status = $2 WHERE id = $1`, l.ID, MarketStatusExpiredClaimed); err != nil {
		return ExpireResult{}, itemsError(err)
	}
	attached := MailItem{ItemRecordID: l.Item.ItemRecordID, InstanceGUID: l.Item.InstanceGUID, Quantity: l.Item.Quantity, Quality: l.Item.Quality, Durability: l.Item.Durability,
		CreatedAt: l.Item.CreatedAt, PersistedHealth: l.Item.PersistedHealth, PersistedMana: l.Item.PersistedMana, LockExpiration: l.Item.LockExpiration}
	subject, body := expiredMailText(itemName, l.Item.Quantity)
	mail, err := insertSystemMail(ctx, tx, l.SellerID, subject, body, []MailItem{attached}, 0, "", 0)
	if err != nil {
		return ExpireResult{}, err
	}
	if err := tx.Commit(ctx); err != nil {
		return ExpireResult{}, err
	}
	return ExpireResult{Outcome: OutcomeDone, SellerID: l.SellerID, SellerMail: mail}, nil
}

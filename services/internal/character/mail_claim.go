// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Claiming the items of a mail (migration 0027's tables). A claim is ONE transaction: it checks that every named attachment belongs to a live mail of the character and
// is unclaimed, writes the character's bags (and wallet, jobs, recipes and warehouse, as SaveInventory does) and marks the attachments claimed. Any check or write that
// fails rolls the whole claim back, so the bags can never hold an item whose attachment is still claimable, and an attachment can never be marked claimed with its item
// missing from the bags. The character's row lock serialises a claim with every save of the same character, so two claims of one attachment cannot both pass.

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"strings"

	"github.com/jackc/pgx/v5"
)

// ErrMailClaimed is returned by ClaimMailItems when a named attachment is claimed already and the request is not a repeat of the claim that did it: the item was given once and
// is not given again. Nothing is written.
var ErrMailClaimed = errors.New("mail attachment is claimed already")

// MailClaim names one attachment a claim takes: its mail and its instance guid.
type MailClaim struct {
	MailID       string // 32 lowercase hex digits, not all zero
	InstanceGUID string // 32 lowercase hex digits, not all zero
}

// MoneyClaim names the money of one mail a claim takes, and the copper the claimer is crediting for it: the amount the mail held when the claimer read it. The claim is
// refused (ErrMailClaimed, nothing written) when the mail holds another amount by the time the claim runs, so a sale that was added to the mail in between is never marked
// claimed without being paid.
type MoneyClaim struct {
	MailID string // 32 lowercase hex digits, not all zero
	Copper int64  // the amount the purse of the claim holds for this mail: at least 1
}

// ClaimBags is what a claim writes to the character's bags: the whole item list the bags hold after the claim, and the other parts of SaveInventoryEverything when their
// flag says to replace them.
type ClaimBags struct {
	Items            []Item
	Wallet           []WalletEntry
	ReplaceWallet    bool
	Jobs             []ProcessingJob
	ReplaceJobs      bool
	KnownRecipes     []int64
	ReplaceKnown     bool
	Warehouse        []WarehouseItem
	ReplaceWarehouse bool
}

// ClaimResult says what a claim did: how many attachments (and mails' money) it marked claimed now, and whether it was a repeat of an earlier claim (nothing written now).
type ClaimResult struct {
	Claimed  int
	Replayed bool
}

// validateClaims checks the names of the attachments a claim takes and of the mails whose money it takes: at least one of them, none twice, each a mail id (and an instance
// guid).
func validateClaims(claims []MailClaim, moneyMails []MoneyClaim) error {
	if len(claims) == 0 && len(moneyMails) == 0 {
		return fmt.Errorf("%w: a claim names no attachment", ErrInvalid)
	}
	if len(claims) > MaxItems {
		return fmt.Errorf("%w: a claim names %d attachments (at most %d)", ErrInvalid, len(claims), MaxItems)
	}
	if len(moneyMails) > MaxMails {
		return fmt.Errorf("%w: a claim names the money of %d mails (at most %d)", ErrInvalid, len(moneyMails), MaxMails)
	}
	zero := strings.Repeat("0", 32)
	seenMoney := make(map[string]bool, len(moneyMails))
	for i, m := range moneyMails {
		if !guidPattern.MatchString(m.MailID) || m.MailID == zero {
			return fmt.Errorf("%w: money claim %d: mail id %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, i, m.MailID)
		}
		if m.Copper < 1 || m.Copper > MaxWalletAmount {
			return fmt.Errorf("%w: money claim %d: %d copper is not an amount a mail carries (1 to %d)", ErrInvalid, i, m.Copper, MaxWalletAmount)
		}
		if seenMoney[m.MailID] {
			return fmt.Errorf("%w: money claim %d names the money of mail %s again", ErrInvalid, i, m.MailID)
		}
		seenMoney[m.MailID] = true
	}
	seen := make(map[MailClaim]bool, len(claims))
	for i, c := range claims {
		if !guidPattern.MatchString(c.MailID) || c.MailID == zero {
			return fmt.Errorf("%w: claim %d: mail id %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, i, c.MailID)
		}
		if !guidPattern.MatchString(c.InstanceGUID) || c.InstanceGUID == zero {
			return fmt.Errorf("%w: claim %d: instance guid %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, i, c.InstanceGUID)
		}
		if seen[c] {
			return fmt.Errorf("%w: claim %d names the attachment %s of mail %s again", ErrInvalid, i, c.InstanceGUID, c.MailID)
		}
		seen[c] = true
	}
	return nil
}

// ClaimMailItems claims the named attachments for the character and stores the bags that hold them, in one transaction (see the top of this file). ErrNotFound for an
// unknown character and for an attachment that is not the character's (another character's mail looks the same), is not there or belongs to a mail the player deleted;
// ErrInvalid for names or bags the rules refuse; ErrMailClaimed for an attachment claimed before by a request with other bags. A request that repeats an earlier one whole
// (every named attachment claimed and the stored items the same as the request's) answers with Replayed and writes nothing: a retry after a lost answer changes nothing.
func (s *Store) ClaimMailItems(ctx context.Context, characterID string, claims []MailClaim, bags ClaimBags) (ClaimResult, error) {
	return s.ClaimMail(ctx, characterID, claims, nil, bags)
}

// ClaimMail is ClaimMailItems that also takes the money of the named mails (Marketplace sales, migration 0031) into the purse: the bags' wallet is the purse after the money
// (ReplaceWallet is required with a money mail), and the money is marked claimed in the same transaction that writes it. Each money names the copper the purse was credited
// for it; under the mail's row lock the claim is refused (ErrMailClaimed, nothing written) when the mail holds another amount, as it does when a later sale of the same listing
// was added to the mail after the claimer read it. A mail whose money was claimed before is refused (ErrMailClaimed) unless the request repeats the claim that did it: the
// bags' items and the purse are then the stored ones (Replayed). A mail with no money, or one that is not the character's or was deleted, is ErrNotFound. A claim names an
// attachment or a mail's money at least once; one may name both.
func (s *Store) ClaimMail(ctx context.Context, characterID string, claims []MailClaim, moneyMails []MoneyClaim, bags ClaimBags) (ClaimResult, error) {
	if !idPattern.MatchString(characterID) {
		return ClaimResult{}, ErrNotFound
	}
	if err := validateClaims(claims, moneyMails); err != nil {
		return ClaimResult{}, err
	}
	if len(moneyMails) > 0 && !bags.ReplaceWallet {
		return ClaimResult{}, fmt.Errorf("%w: a claim of money carries the purse that holds it", ErrInvalid)
	}
	wallet := bags.Wallet
	if bags.ReplaceWallet {
		if err := ValidateWallet(wallet); err != nil {
			return ClaimResult{}, err
		}
	} else {
		wallet = nil
	}
	if bags.ReplaceJobs {
		if err := ValidateJobs(bags.Jobs); err != nil {
			return ClaimResult{}, err
		}
	}
	if bags.ReplaceKnown {
		if err := ValidateKnownRecipes(bags.KnownRecipes); err != nil {
			return ClaimResult{}, err
		}
	}
	warehouse := bags.Warehouse
	if bags.ReplaceWarehouse {
		if err := ValidateWarehouseItems(warehouse, bags.Items); err != nil {
			return ClaimResult{}, err
		}
	} else {
		warehouse = nil
	}
	// the same refusals saveInventory makes: a bag is never saved empty, and every item and slot is checked before anything is written
	if len(bags.Items) == 0 {
		return ClaimResult{}, fmt.Errorf("%w: a claim's bags are never empty (a character keeps its items)", ErrInvalid)
	}
	if err := ValidateItems(bags.Items); err != nil {
		return ClaimResult{}, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return ClaimResult{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	if err := lockCharacter(ctx, tx, characterID); err != nil {
		return ClaimResult{}, err
	}
	// every named attachment must be the character's, in a live mail; the rows are locked until the end of the transaction
	already := 0
	for _, c := range claims {
		var claimed bool
		var status int32
		err := tx.QueryRow(ctx, `SELECT i.claimed, m.status FROM character_mail_items i JOIN character_mails m ON m.id = i.mail_id
			WHERE m.id = $1 AND m.character_id = $2 AND i.instance_guid = $3 FOR UPDATE OF i`, c.MailID, characterID, c.InstanceGUID).Scan(&claimed, &status)
		if errors.Is(err, pgx.ErrNoRows) {
			return ClaimResult{}, fmt.Errorf("%w: the attachment %s of mail %s is not a claimable attachment of the character", ErrNotFound, c.InstanceGUID, c.MailID)
		}
		if err != nil {
			return ClaimResult{}, err
		}
		if status&MailStatusDeleted != 0 {
			return ClaimResult{}, fmt.Errorf("%w: mail %s was deleted", ErrNotFound, c.MailID)
		}
		if claimed {
			already++
		}
	}
	// ... and every named money must be the character's, in a live mail, and be the amount the claim credited: the row is locked, so a sale cannot be added to it from here to
	// the end of the transaction, and one that was added before is seen now
	for _, m := range moneyMails {
		var claimed bool
		var status int32
		var copper int64
		err := tx.QueryRow(ctx, `SELECT mm.claimed, mm.copper, m.status FROM character_mail_money mm JOIN character_mails m ON m.id = mm.mail_id
			WHERE m.id = $1 AND m.character_id = $2 FOR UPDATE OF mm`, m.MailID, characterID).Scan(&claimed, &copper, &status)
		if errors.Is(err, pgx.ErrNoRows) {
			return ClaimResult{}, fmt.Errorf("%w: mail %s has no money of the character's to claim", ErrNotFound, m.MailID)
		}
		if err != nil {
			return ClaimResult{}, err
		}
		if status&MailStatusDeleted != 0 {
			return ClaimResult{}, fmt.Errorf("%w: mail %s was deleted", ErrNotFound, m.MailID)
		}
		if copper != m.Copper {
			return ClaimResult{}, fmt.Errorf("%w: the money of mail %s is %d copper now, not the %d the claim was made for", ErrMailClaimed, m.MailID, copper, m.Copper)
		}
		if claimed {
			already++
		}
	}
	if already > 0 {
		if already == len(claims)+len(moneyMails) {
			same, err := sameStoredItems(ctx, tx, characterID, bags.Items)
			if err != nil {
				return ClaimResult{}, err
			}
			if same && len(moneyMails) > 0 {
				// money is only given once: the purse of a repeat of the claim is the one the claim stored, and any other purse is a second claim of money that was taken
				same, err = sameStoredWallet(ctx, tx, characterID, wallet)
				if err != nil {
					return ClaimResult{}, err
				}
			}
			if same {
				// the request repeats the claim that did it: the answer is Ok and nothing is written
				if err := tx.Commit(ctx); err != nil {
					return ClaimResult{}, err
				}
				return ClaimResult{Replayed: true}, nil
			}
		}
		return ClaimResult{}, fmt.Errorf("%w: %d of the %d attachments and moneys were claimed before", ErrMailClaimed, already, len(claims)+len(moneyMails))
	}
	// the bags first, then the marks: a failure of either leaves nothing
	if err := writeInventory(ctx, tx, characterID, bags.Items, wallet, bags.Jobs, bags.ReplaceJobs, bags.KnownRecipes, bags.ReplaceKnown, warehouse, bags.ReplaceWarehouse); err != nil {
		return ClaimResult{}, err
	}
	for _, c := range claims {
		tag, err := tx.Exec(ctx, `UPDATE character_mail_items SET claimed = true WHERE mail_id = $1 AND instance_guid = $2 AND NOT claimed`, c.MailID, c.InstanceGUID)
		if err != nil {
			return ClaimResult{}, err
		}
		if tag.RowsAffected() != 1 {
			return ClaimResult{}, fmt.Errorf("%w: the attachment %s of mail %s was claimed while this claim ran", ErrMailClaimed, c.InstanceGUID, c.MailID)
		}
	}
	for _, m := range moneyMails {
		tag, err := tx.Exec(ctx, `UPDATE character_mail_money SET claimed = true WHERE mail_id = $1 AND NOT claimed AND copper = $2`, m.MailID, m.Copper)
		if err != nil {
			return ClaimResult{}, err
		}
		if tag.RowsAffected() != 1 {
			return ClaimResult{}, fmt.Errorf("%w: the money of mail %s was claimed or changed while this claim ran", ErrMailClaimed, m.MailID)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return ClaimResult{}, err
	}
	return ClaimResult{Claimed: len(claims) + len(moneyMails)}, nil
}

// sameStoredWallet says whether the character's stored balance of each currency in `wallet` is the amount it names (a currency the character has no row for holds 0).
func sameStoredWallet(ctx context.Context, tx pgx.Tx, characterID string, wallet []WalletEntry) (bool, error) {
	rows, err := tx.Query(ctx, `SELECT currency_guid, amount FROM character_wallet WHERE character_id = $1`, characterID)
	if err != nil {
		return false, err
	}
	stored := map[int64]int64{}
	for rows.Next() {
		var guid, amount int64
		if err := rows.Scan(&guid, &amount); err != nil {
			rows.Close()
			return false, err
		}
		stored[guid] = amount
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return false, err
	}
	for _, e := range wallet {
		if stored[e.CurrencyGuid] != e.Amount {
			return false, nil
		}
	}
	return true, nil
}

// sameStoredItems says whether the character's stored items (both tables) are exactly items, place for place and field for field, read in tx.
func sameStoredItems(ctx context.Context, tx pgx.Tx, characterID string, items []Item) (bool, error) {
	rows, err := tx.Query(ctx, allItemsSQL, characterID)
	if err != nil {
		return false, err
	}
	var stored []Item
	for rows.Next() {
		var it Item
		if err := rows.Scan(&it.Container, &it.Slot, &it.ItemRecordID, &it.InstanceGUID, &it.Quantity, &it.Quality, &it.Durability, &it.CreatedAt,
			&it.PersistedHealth, &it.PersistedMana, &it.LockExpiration); err != nil {
			rows.Close()
			return false, err
		}
		stored = append(stored, it)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return false, err
	}
	if len(stored) != len(items) {
		return false, nil
	}
	want := append([]Item(nil), items...)
	order := func(list []Item) {
		sort.Slice(list, func(i, j int) bool {
			if list[i].Container != list[j].Container {
				return list[i].Container < list[j].Container
			}
			return list[i].Slot < list[j].Slot
		})
	}
	order(stored)
	order(want)
	for i := range stored {
		a, b := stored[i], want[i]
		if a.Container != b.Container || a.Slot != b.Slot || a.ItemRecordID != b.ItemRecordID || a.InstanceGUID != b.InstanceGUID || a.Quantity != b.Quantity ||
			a.Quality != b.Quality || a.Durability != b.Durability || a.CreatedAt.UnixMilli() != b.CreatedAt.UnixMilli() || a.LockExpiration != b.LockExpiration ||
			!samePersisted(a.PersistedHealth, b.PersistedHealth) || !samePersisted(a.PersistedMana, b.PersistedMana) {
			return false, nil
		}
	}
	return true, nil
}

// samePersisted says whether two persisted stats are both none or the same number.
func samePersisted(a, b *float32) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	return *a == *b
}

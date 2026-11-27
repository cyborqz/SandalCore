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

// ClaimResult says what a claim did: how many attachments it marked claimed now, and whether it was a repeat of an earlier claim (nothing written now).
type ClaimResult struct {
	Claimed  int
	Replayed bool
}

// validateClaims checks the names of the attachments a claim takes: at least one, none twice, each a mail id and an instance guid.
func validateClaims(claims []MailClaim) error {
	if len(claims) == 0 {
		return fmt.Errorf("%w: a claim names no attachment", ErrInvalid)
	}
	if len(claims) > MaxItems {
		return fmt.Errorf("%w: a claim names %d attachments (at most %d)", ErrInvalid, len(claims), MaxItems)
	}
	zero := strings.Repeat("0", 32)
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
	if !idPattern.MatchString(characterID) {
		return ClaimResult{}, ErrNotFound
	}
	if err := validateClaims(claims); err != nil {
		return ClaimResult{}, err
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
	if already > 0 {
		if already == len(claims) {
			same, err := sameStoredItems(ctx, tx, characterID, bags.Items)
			if err != nil {
				return ClaimResult{}, err
			}
			if same {
				// the request repeats the claim that did it: the answer is Ok and nothing is written
				if err := tx.Commit(ctx); err != nil {
					return ClaimResult{}, err
				}
				return ClaimResult{Replayed: true}, nil
			}
		}
		return ClaimResult{}, fmt.Errorf("%w: %d of the %d attachments were claimed before", ErrMailClaimed, already, len(claims))
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
	if err := tx.Commit(ctx); err != nil {
		return ClaimResult{}, err
	}
	return ClaimResult{Claimed: len(claims)}, nil
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

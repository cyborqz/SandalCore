// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package charactergrpc

// ClaimMailItems over gRPC: the claims and the bags reach the store whole, each store error gets its own code (a claim of another character's mail looks like a mail that is
// not there, a second claim with other bags is AlreadyExists, bags the rules refuse are InvalidArgument), and on the real store a claim writes the bags and the claimed
// mark together, a repeat of it answers replayed and another character cannot take the item.

import (
	"context"
	"errors"
	"fmt"
	"testing"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

func claimRequest(character string, mailID string, guid string) *characterv1.ClaimMailItemsRequest {
	return &characterv1.ClaimMailItemsRequest{
		CharacterId: character,
		Claims:      []*characterv1.MailClaim{{MailId: mailID, InstanceGuid: guid}},
		Inventory: &characterv1.SaveInventoryRequest{
			Items: []*characterv1.Item{{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 4, ItemRecordId: 7001, InstanceGuid: guid, Quantity: 2, Quality: 3,
				Durability: 1.5, CreatedAtMs: 1_780_000_000_000}},
			ReplaceWallet: true, Wallet: []*characterv1.WalletEntry{{CurrencyGuid: 1, Amount: 40}},
			ReplaceKnownRecipes: true, KnownRecipes: []int64{11, 12},
		},
	}
}

func TestClaimMailItemsPassesTheClaimsAndTheBagsToTheStore(t *testing.T) {
	st := &mailFake{recStore: &recStore{ret: fullCharacter()}, claimRet: character.ClaimResult{Claimed: 1}}
	c := dialRec(t, st)
	guid := fmtGUID(0x201)
	resp, err := c.ClaimMailItems(context.Background(), claimRequest(mailProtoGUID, fmtGUID(0x300), guid))
	if err != nil {
		t.Fatal(err)
	}
	if resp.GetClaimed() != 1 || resp.GetReplayed() {
		t.Errorf("the answer is %v, want 1 claimed and not replayed", resp)
	}
	if st.claimAsked != 1 || st.claimID != mailProtoGUID || len(st.claimed) != 1 || st.claimed[0] != (character.MailClaim{MailID: fmtGUID(0x300), InstanceGUID: guid}) {
		t.Errorf("the store was asked %d times for %q with claims %v; want once for the character with the one claim", st.claimAsked, st.claimID, st.claimed)
	}
	b := st.claimBags
	if len(b.Items) != 1 || b.Items[0].Container != character.Backpack || b.Items[0].Slot != 4 || b.Items[0].InstanceGUID != guid || b.Items[0].Quantity != 2 ||
		!b.Items[0].CreatedAt.Equal(time.UnixMilli(1_780_000_000_000)) {
		t.Errorf("the store got the items %+v, want the one item as sent", b.Items)
	}
	if !b.ReplaceWallet || len(b.Wallet) != 1 || b.Wallet[0].Amount != 40 || !b.ReplaceKnown || len(b.KnownRecipes) != 2 || b.ReplaceJobs || b.ReplaceWarehouse {
		t.Errorf("the store got the bags %+v, want the wallet and the recipes replaced and nothing else", b)
	}
	// a request with no bags at all is refused before the store is asked
	if _, err := c.ClaimMailItems(context.Background(), &characterv1.ClaimMailItemsRequest{CharacterId: mailProtoGUID}); status.Code(err) != codes.InvalidArgument || st.claimAsked != 1 {
		t.Errorf("a claim without bags: %v after %d store questions, want InvalidArgument and no new question", err, st.claimAsked)
	}
}

func TestClaimMailItemsGivesEachStoreErrorItsOwnCode(t *testing.T) {
	for _, tc := range []struct {
		name string
		err  error
		code codes.Code
	}{
		{"claimed before", fmt.Errorf("%w: 1 of the 1 attachments", character.ErrMailClaimed), codes.AlreadyExists},
		{"not the character's mail", fmt.Errorf("%w: not a claimable attachment", character.ErrNotFound), codes.NotFound},
		{"bags the rules refuse", fmt.Errorf("%w: a slot", character.ErrInvalid), codes.InvalidArgument},
		{"the store failed", errors.New("the database is down"), codes.Internal},
	} {
		st := &mailFake{recStore: &recStore{ret: fullCharacter()}, err: tc.err}
		c := dialRec(t, st)
		if _, err := c.ClaimMailItems(context.Background(), claimRequest(mailProtoGUID, fmtGUID(0x300), fmtGUID(0x201))); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", tc.name, err, tc.code)
		}
	}
}

// The real store through the service: the claim, a repeat, a claim with other bags, and another character's attempt.
func TestClaimMailItemsThroughTheServiceOnTheRealStore(t *testing.T) {
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
	make1 := func(name string) string {
		created, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: account, Name: name, ClassId: 2, ClassName: "Bard", AppearanceJson: "{}"})
		if err != nil {
			t.Fatal(err)
		}
		return created.GetCharacter().GetId()
	}
	recipient, other := make1("cyb"), make1("thief")
	sent, err := c.CreateMail(ctx, &characterv1.CreateMailRequest{CharacterId: recipient, Sender: "Brannoc", Subject: "Parcel",
		Items: []*characterv1.MailItem{{ItemRecordId: 7001, Quantity: 2, Quality: 3, Durability: 1.5}}})
	if err != nil {
		t.Fatal(err)
	}
	mail := sent.GetMail()
	guid := mail.GetItems()[0].GetInstanceGuid()
	req := claimRequest(recipient, mail.GetId(), guid)
	req.GetInventory().GetItems()[0].CreatedAtMs = mail.GetItems()[0].GetCreatedAtMs()
	resp, err := c.ClaimMailItems(ctx, req)
	if err != nil || resp.GetClaimed() != 1 || resp.GetReplayed() {
		t.Fatalf("the claim: %v, %v; want one attachment claimed", resp, err)
	}
	list, err := c.GetMails(ctx, &characterv1.GetMailsRequest{CharacterId: recipient})
	if err != nil || len(list.GetMails()) != 1 || !list.GetMails()[0].GetItems()[0].GetClaimed() {
		t.Fatalf("after the claim GetMails = %v, %v; want the attachment claimed", list, err)
	}
	if again, err := c.ClaimMailItems(ctx, req); err != nil || !again.GetReplayed() || again.GetClaimed() != 0 {
		t.Errorf("the same request again: %v, %v; want replayed, nothing claimed now", again, err)
	}
	moved := claimRequest(recipient, mail.GetId(), guid)
	moved.GetInventory().GetItems()[0].Slot = 9
	moved.GetInventory().GetItems()[0].CreatedAtMs = mail.GetItems()[0].GetCreatedAtMs()
	if _, err := c.ClaimMailItems(ctx, moved); status.Code(err) != codes.AlreadyExists {
		t.Errorf("a claim of the claimed attachment with other bags: %v, want AlreadyExists", err)
	}
	kitBefore, err := character.NewStore(pool).Inventory(ctx, other)
	if err != nil {
		t.Fatal(err)
	}
	theirs := claimRequest(other, mail.GetId(), guid)
	theirs.GetInventory().GetItems()[0].CreatedAtMs = mail.GetItems()[0].GetCreatedAtMs()
	if _, err := c.ClaimMailItems(ctx, theirs); status.Code(err) != codes.NotFound {
		t.Errorf("another character's claim: %v, want NotFound", err)
	}
	if items, err := character.NewStore(pool).Inventory(ctx, other); err != nil || len(items) != len(kitBefore) {
		t.Errorf("the other character's bags hold %d items (%v), want the %d of its kit: its claim wrote nothing", len(items), err, len(kitBefore))
	}
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// ClaimMailItems: the bags and the claimed flag are written in ONE transaction. These tests run against PostgreSQL (a skip is not a pass): the claim writes the
// bags and marks the attachments together or not at all, a second claim of an attachment gives no second item, a repeat of the same request is answered and not
// written again, and another character's mail is never touched.

import (
	"context"
	"errors"
	"reflect"
	"sync"
	"testing"
	"time"
)

// claimFixture is a character with one mail of two attachments and the bags a claim of the first would write.
type claimFixture struct {
	s    *Store
	acc  int64
	c    Character
	mail Mail
	bags ClaimBags
}

func newClaimFixture(t *testing.T) claimFixture {
	t.Helper()
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	mail, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Parcel", Body: "Two things.", Items: []MailItem{
		{ItemRecordID: 8001, Quantity: 1, Quality: 1, Durability: 3},
		{ItemRecordID: 8002, Quantity: 2, Quality: 1, Durability: 4},
	}})
	if err != nil {
		t.Fatal(err)
	}
	return claimFixture{s: s, acc: acc, c: c, mail: mail, bags: bagsWith(t, s, c, mail.Items[0])}
}

// bagsWith is the character's stored bags plus the attachment in the first free Backpack slot (what a session sends with a claim).
func bagsWith(t *testing.T, s *Store, c Character, att MailItem) ClaimBags {
	t.Helper()
	items, err := s.Inventory(context.Background(), c.ID)
	if err != nil {
		t.Fatal(err)
	}
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
	placed := att.item()
	placed.Slot = slot
	return ClaimBags{Items: append(items, placed), Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 77}}, ReplaceWallet: true}
}

func (f claimFixture) claim(guid string) []MailClaim { return []MailClaim{{MailID: f.mail.ID, InstanceGUID: guid}} }

// storedGuidCount says how many times an instance guid is in the stored bags.
func storedGuidCount(t *testing.T, s *Store, c Character, guid string) int {
	t.Helper()
	items, err := s.Inventory(context.Background(), c.ID)
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for _, it := range items {
		if it.InstanceGUID == guid {
			n++
		}
	}
	return n
}

func claimedFlags(t *testing.T, s *Store, mailID string) []bool {
	t.Helper()
	_, items := mailRowStatus(t, s, mailID)
	out := make([]bool, len(items))
	for i, it := range items {
		out[i] = it.Claimed
	}
	return out
}

func walletAmount(t *testing.T, s *Store, c Character) int64 {
	t.Helper()
	w, err := s.Wallet(context.Background(), c.ID)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range w {
		if e.CurrencyGuid == CopperCurrency {
			return e.Amount
		}
	}
	return 0
}

// The claim writes the bags, the wallet and the claimed flag together, and a new store (a restart) reads them.
func TestClaimMailItemsStoresTheBagsAndMarksTheAttachmentTogether(t *testing.T) {
	f := newClaimFixture(t)
	ctx := context.Background()
	res, err := f.s.ClaimMailItems(ctx, f.c.ID, f.claim(f.mail.Items[0].InstanceGUID), f.bags)
	if err != nil || res.Claimed != 1 || res.Replayed {
		t.Fatalf("ClaimMailItems = %+v, %v; want one attachment claimed now", res, err)
	}
	fresh := NewStore(f.s.pool)
	if n := storedGuidCount(t, fresh, f.c, f.mail.Items[0].InstanceGUID); n != 1 {
		t.Errorf("the claimed item is in the stored bags %d times, want 1", n)
	}
	if got := claimedFlags(t, fresh, f.mail.ID); len(got) != 2 || !got[0] || got[1] {
		t.Errorf("claimed flags = %v, want [true false]: only the named attachment is marked", got)
	}
	if got := walletAmount(t, fresh, f.c); got != 77 {
		t.Errorf("the wallet is %d, want the 77 the claim carried", got)
	}
}

// A claim whose mark fails leaves the bags and the wallet as they were: the transaction is all or nothing.
func TestClaimMailItemsWritesNothingWhenTheMarkFails(t *testing.T) {
	f := newClaimFixture(t)
	ctx := context.Background()
	before, _ := f.s.Inventory(ctx, f.c.ID)
	if _, err := f.s.pool.Exec(ctx, `CREATE FUNCTION refuse_mark() RETURNS trigger AS $$ BEGIN RAISE EXCEPTION 'the mark is refused'; END $$ LANGUAGE plpgsql;
		CREATE TRIGGER refuse_mark BEFORE UPDATE ON character_mail_items FOR EACH ROW EXECUTE FUNCTION refuse_mark()`); err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.ClaimMailItems(ctx, f.c.ID, f.claim(f.mail.Items[0].InstanceGUID), f.bags); err == nil {
		t.Fatal("the claim succeeded although its mark failed")
	}
	after, _ := f.s.Inventory(ctx, f.c.ID)
	if len(after) != len(before) || storedGuidCount(t, f.s, f.c, f.mail.Items[0].InstanceGUID) != 0 {
		t.Errorf("the bags changed to %d items although the mark failed (they were %d, and the item must not be in them)", len(after), len(before))
	}
	if got := claimedFlags(t, f.s, f.mail.ID); got[0] || got[1] {
		t.Errorf("claimed flags = %v, want none", got)
	}
	if got := walletAmount(t, f.s, f.c); got == 77 {
		t.Errorf("the wallet was written (%d) although the mark failed", got)
	}
}

// A claim whose bag write fails marks nothing.
func TestClaimMailItemsMarksNothingWhenTheBagsFail(t *testing.T) {
	f := newClaimFixture(t)
	ctx := context.Background()
	if _, err := f.s.pool.Exec(ctx, `CREATE FUNCTION refuse_bag() RETURNS trigger AS $$ BEGIN RAISE EXCEPTION 'the bag write is refused'; END $$ LANGUAGE plpgsql;
		CREATE TRIGGER refuse_bag BEFORE INSERT ON character_items FOR EACH ROW EXECUTE FUNCTION refuse_bag()`); err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.ClaimMailItems(ctx, f.c.ID, f.claim(f.mail.Items[0].InstanceGUID), f.bags); err == nil {
		t.Fatal("the claim succeeded although its bag write failed")
	}
	if got := claimedFlags(t, f.s, f.mail.ID); got[0] || got[1] {
		t.Errorf("claimed flags = %v after a failed bag write, want none", got)
	}
}

// Bags the rules refuse are refused with nothing written, the attachment still claimable.
func TestClaimMailItemsRefusesBagsThatBreakTheRules(t *testing.T) {
	f := newClaimFixture(t)
	ctx := context.Background()
	empty := ClaimBags{}
	if _, err := f.s.ClaimMailItems(ctx, f.c.ID, f.claim(f.mail.Items[0].InstanceGUID), empty); !errors.Is(err, ErrInvalid) {
		t.Errorf("empty bags: %v, want ErrInvalid", err)
	}
	twice := f.bags
	twice.Items = append(append([]Item(nil), f.bags.Items...), f.bags.Items[len(f.bags.Items)-1])
	if _, err := f.s.ClaimMailItems(ctx, f.c.ID, f.claim(f.mail.Items[0].InstanceGUID), twice); !errors.Is(err, ErrInvalid) {
		t.Errorf("one item twice: %v, want ErrInvalid", err)
	}
	for _, claims := range [][]MailClaim{nil, {{MailID: f.mail.ID, InstanceGUID: f.mail.Items[0].InstanceGUID}, {MailID: f.mail.ID, InstanceGUID: f.mail.Items[0].InstanceGUID}}} {
		if _, err := f.s.ClaimMailItems(ctx, f.c.ID, claims, f.bags); !errors.Is(err, ErrInvalid) {
			t.Errorf("claims %v: %v, want ErrInvalid (none, or one attachment twice)", claims, err)
		}
	}
	if got := claimedFlags(t, f.s, f.mail.ID); got[0] || got[1] {
		t.Errorf("claimed flags = %v after refused claims, want none", got)
	}
	if n := storedGuidCount(t, f.s, f.c, f.mail.Items[0].InstanceGUID); n != 0 {
		t.Errorf("a refused claim left the item in the bags %d times", n)
	}
}

// Two claims of one attachment, at the same time and with different bags, give one item: one answers Ok, the other is refused.
func TestTwoClaimsOfOneAttachmentGiveOneItem(t *testing.T) {
	f := newClaimFixture(t)
	ctx := context.Background()
	guid := f.mail.Items[0].InstanceGUID
	other := f.bags
	other.Items = append([]Item(nil), f.bags.Items...)
	other.Items[len(other.Items)-1].Slot += 5   // another place in the bag: a different snapshot, so the second is no replay
	other.Wallet = []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 99}}
	var wg sync.WaitGroup
	errs := make([]error, 2)
	start := make(chan struct{})
	for i, bags := range []ClaimBags{f.bags, other} {
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			_, errs[i] = f.s.ClaimMailItems(ctx, f.c.ID, f.claim(guid), bags)
		}()
	}
	close(start)
	wg.Wait()
	ok, refused := 0, 0
	for _, err := range errs {
		switch {
		case err == nil:
			ok++
		case errors.Is(err, ErrMailClaimed):
			refused++
		default:
			t.Errorf("a claim answered %v, want Ok or ErrMailClaimed", err)
		}
	}
	if ok != 1 || refused != 1 {
		t.Fatalf("answers = %d Ok and %d refused, want one of each", ok, refused)
	}
	if n := storedGuidCount(t, f.s, f.c, guid); n != 1 {
		t.Errorf("the item is in the stored bags %d times, want exactly 1", n)
	}
}

// The same request sent again (a retry after a lost answer) is answered Ok and writes nothing; a request with other bags for a claimed attachment is refused.
func TestARepeatedClaimIsAnsweredAndNotWrittenAgain(t *testing.T) {
	f := newClaimFixture(t)
	ctx := context.Background()
	guid := f.mail.Items[0].InstanceGUID
	if _, err := f.s.ClaimMailItems(ctx, f.c.ID, f.claim(guid), f.bags); err != nil {
		t.Fatal(err)
	}
	// something else changes the wallet; a repeat must not write the old bags over it
	if err := f.s.SaveInventoryAndWallet(ctx, f.c.ID, f.bags.Items, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 5}}); err != nil {
		t.Fatal(err)
	}
	res, err := f.s.ClaimMailItems(ctx, f.c.ID, f.claim(guid), f.bags)
	if err != nil || !res.Replayed || res.Claimed != 0 {
		t.Fatalf("the repeated claim = %+v, %v; want Replayed with nothing claimed now", res, err)
	}
	if got := walletAmount(t, f.s, f.c); got != 5 {
		t.Errorf("the wallet is %d after the repeat, want the 5 it had: a repeat writes nothing", got)
	}
	if n := storedGuidCount(t, f.s, f.c, guid); n != 1 {
		t.Errorf("the item is in the bags %d times, want 1", n)
	}
	moved := f.bags
	moved.Items = append([]Item(nil), f.bags.Items...)
	moved.Items[len(moved.Items)-1].Slot += 9
	if _, err := f.s.ClaimMailItems(ctx, f.c.ID, f.claim(guid), moved); !errors.Is(err, ErrMailClaimed) {
		t.Errorf("a claim of a claimed attachment with other bags: %v, want ErrMailClaimed", err)
	}
}

// Another character's mail is refused as if it were not there, and nothing of either character is written (an insecure direct object reference).
func TestAnotherCharactersMailIsRefused(t *testing.T) {
	f := newClaimFixture(t)
	ctx := context.Background()
	thief := newMailCharacter(t, f.s, f.acc, "thief")
	theirs := bagsWith(t, f.s, thief, f.mail.Items[0])
	before, _ := f.s.Inventory(ctx, thief.ID)
	_, err := f.s.ClaimMailItems(ctx, thief.ID, f.claim(f.mail.Items[0].InstanceGUID), theirs)
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("the thief's claim = %v, want ErrNotFound", err)
	}
	after, _ := f.s.Inventory(ctx, thief.ID)
	if len(after) != len(before) {
		t.Errorf("the thief's bags changed from %d to %d items", len(before), len(after))
	}
	if got := claimedFlags(t, f.s, f.mail.ID); got[0] || got[1] {
		t.Errorf("claimed flags = %v after the thief's claim, want none", got)
	}
	if _, err := f.s.ClaimMailItems(ctx, "ffffffffffffffffffffffffffffffff", f.claim(f.mail.Items[0].InstanceGUID), theirs); !errors.Is(err, ErrNotFound) {
		t.Errorf("an unknown character: %v, want ErrNotFound", err)
	}
}

// A deleted mail and an attachment the mail does not have are refused; a claim that names one good and one bad attachment writes nothing for the good one either.
func TestClaimMailItemsIsAllOrNothingOverTheAttachmentsItNames(t *testing.T) {
	f := newClaimFixture(t)
	ctx := context.Background()
	unknown := "00000000000000000000000000000abc"
	claims := []MailClaim{{MailID: f.mail.ID, InstanceGUID: f.mail.Items[0].InstanceGUID}, {MailID: f.mail.ID, InstanceGUID: unknown}}
	if _, err := f.s.ClaimMailItems(ctx, f.c.ID, claims, f.bags); !errors.Is(err, ErrNotFound) {
		t.Fatalf("a claim naming an attachment that is not there: %v, want ErrNotFound", err)
	}
	if got := claimedFlags(t, f.s, f.mail.ID); got[0] || got[1] || storedGuidCount(t, f.s, f.c, f.mail.Items[0].InstanceGUID) != 0 {
		t.Errorf("the good attachment was claimed (%v) or stored although the claim as a whole was refused", got)
	}
	// both attachments of the mail, together, are fine
	both := []MailClaim{{MailID: f.mail.ID, InstanceGUID: f.mail.Items[0].InstanceGUID}, {MailID: f.mail.ID, InstanceGUID: f.mail.Items[1].InstanceGUID}}
	bags := bagsWith(t, f.s, f.c, f.mail.Items[0])
	second := f.mail.Items[1].item()
	second.Slot = bags.Items[len(bags.Items)-1].Slot + 1
	bags.Items = append(bags.Items, second)
	if res, err := f.s.ClaimMailItems(ctx, f.c.ID, both, bags); err != nil || res.Claimed != 2 {
		t.Fatalf("a claim of both attachments = %+v, %v; want 2 claimed", res, err)
	}
	// a mail the player deleted is not claimable
	mail2, err := f.s.CreateMail(ctx, f.c.ID, NewMail{Sender: "Brannoc", Subject: "Gone", Body: "", Items: []MailItem{{ItemRecordID: 8003, Quantity: 1, Quality: 1}}})
	if err != nil {
		t.Fatal(err)
	}
	mail2.Status |= MailStatusDeleted
	if err := f.s.SaveMail(ctx, f.c.ID, mail2); err != nil {
		t.Fatal(err)
	}
	gone := bagsWith(t, f.s, f.c, mail2.Items[0])
	if _, err := f.s.ClaimMailItems(ctx, f.c.ID, []MailClaim{{MailID: mail2.ID, InstanceGUID: mail2.Items[0].InstanceGUID}}, gone); !errors.Is(err, ErrNotFound) {
		t.Errorf("a claim from a deleted mail: %v, want ErrNotFound", err)
	}
}

// Several mails, one claim: every attachment is marked in the one transaction, and one that is missing refuses all of them.
func TestClaimMailItemsClaimsSeveralMailsInOneTransaction(t *testing.T) {
	f := newClaimFixture(t)
	ctx := context.Background()
	other, err := f.s.CreateMail(ctx, f.c.ID, NewMail{Sender: "Mirelle", Subject: "More", Body: "", Items: []MailItem{{ItemRecordID: 8010, Quantity: 1, Quality: 1}}})
	if err != nil {
		t.Fatal(err)
	}
	bags := bagsWith(t, f.s, f.c, f.mail.Items[0])
	second := other.Items[0].item()
	second.Slot = bags.Items[len(bags.Items)-1].Slot + 1
	bags.Items = append(bags.Items, second)
	claims := []MailClaim{{MailID: f.mail.ID, InstanceGUID: f.mail.Items[0].InstanceGUID}, {MailID: other.ID, InstanceGUID: other.Items[0].InstanceGUID}}
	if res, err := f.s.ClaimMailItems(ctx, f.c.ID, claims, bags); err != nil || res.Claimed != 2 {
		t.Fatalf("a claim over two mails = %+v, %v; want 2 claimed", res, err)
	}
	if a, b := claimedFlags(t, f.s, f.mail.ID), claimedFlags(t, f.s, other.ID); !a[0] || a[1] || !b[0] {
		t.Errorf("claimed flags = %v and %v, want [true false] and [true]", a, b)
	}
}

// A true repeat of a stored claim is answered Replayed and writes nothing, whatever the items hold: this is what lets the world server treat every "claimed already" as a refusal.
// The bags use every stored field of an item in both item tables (a persisted health and mana, a lock expiration, a created time with sub-second precision), plus a warehouse item
// and a wallet; the request is sent again as it was, and again after another save wrote the same items.
func TestARepeatOfAStoredClaimWithEveryStoredFieldIsReplayedAndWritesNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	mail, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Parcel", Body: "Two things.", Items: attachments(2)})
	if err != nil {
		t.Fatal(err)
	}
	bags := bagsWith(t, s, c, mail.Items[0])
	bags.Items = append(bags.Items,
		Item{Container: Backpack, Slot: 60, ItemRecordID: 9001, InstanceGUID: guid(0x900), Quantity: 3, Quality: 4, Durability: 12.75, CreatedAt: time.Unix(1_780_000_000, 123_456_000),
			PersistedHealth: float32Ptr(3210.25), PersistedMana: float32Ptr(120.5), LockExpiration: 1_790_000_000},
		Item{Container: Resource0, Slot: 5, ItemRecordID: 9002, InstanceGUID: guid(0x901), Quantity: 7, Quality: 2, Durability: 0.5, CreatedAt: time.Unix(1_780_000_001, 999_000_000),
			PersistedHealth: float32Ptr(1.5), PersistedMana: float32Ptr(2.5), LockExpiration: 1_790_000_001},
		Item{Container: Equipment, Slot: 40, ItemRecordID: 9003, InstanceGUID: guid(0x902), Quantity: 1, Quality: 9, Durability: 99.25, CreatedAt: time.Unix(1_780_000_002, 7_000),
			PersistedHealth: float32Ptr(0), PersistedMana: float32Ptr(0), LockExpiration: 0},
		Item{Container: Quest, Slot: 3, ItemRecordID: 9004, InstanceGUID: guid(0x903), Quantity: 2, Quality: 1, Durability: 1, CreatedAt: time.UnixMilli(1_780_000_003_001)},
	)
	bags.Wallet = []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 77}, {CurrencyGuid: CopperCurrency + 1, Amount: 5}}
	bags.ReplaceWallet = true
	bags.Warehouse = []WarehouseItem{{Area: WarehouseItems, NodeGUID: 0, ContainerGUID: baseChunk, Slot: 4,
		Item: Item{ItemRecordID: 9005, InstanceGUID: guid(0x904), Quantity: 5, Quality: 3, Durability: 7.5, CreatedAt: time.Unix(1_780_000_004, 654_321_000),
			PersistedHealth: float32Ptr(10.5), PersistedMana: float32Ptr(11.5), LockExpiration: 1_790_000_004}}}
	bags.ReplaceWarehouse = true
	claims := []MailClaim{{MailID: mail.ID, InstanceGUID: mail.Items[0].InstanceGUID}}
	if res, err := s.ClaimMailItems(ctx, c.ID, claims, bags); err != nil || res.Claimed != 1 || res.Replayed {
		t.Fatalf("the claim = %+v, %v; want one attachment claimed now", res, err)
	}
	type state struct {
		Items     []Item
		Wallet    []WalletEntry
		Warehouse []WarehouseItem
		Flags     []bool
	}
	read := func() state {
		items, err := s.Inventory(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		wallet, err := s.Wallet(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		stored, err := s.WarehouseItems(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		return state{Items: items, Wallet: wallet, Warehouse: stored, Flags: claimedFlags(t, s, mail.ID)}
	}
	first := read()
	res, err := s.ClaimMailItems(ctx, c.ID, claims, bags)
	if err != nil || !res.Replayed || res.Claimed != 0 {
		t.Fatalf("the identical request again = %+v, %v; want Replayed with nothing claimed now (an answer of 'claimed before' here would give the item back and lose it)", res, err)
	}
	if got := read(); !reflect.DeepEqual(got, first) {
		t.Errorf("the repeat changed the stored state:\n got  %+v\n want %+v", got, first)
	}
	// another save writes the same items again (and a new wallet); the same request is still a repeat and writes nothing over it
	if err := s.SaveInventoryAndWallet(ctx, c.ID, bags.Items, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 5}}); err != nil {
		t.Fatal(err)
	}
	second := read()
	res, err = s.ClaimMailItems(ctx, c.ID, claims, bags)
	if err != nil || !res.Replayed || res.Claimed != 0 {
		t.Fatalf("the identical request after another save = %+v, %v; want Replayed", res, err)
	}
	if got := read(); !reflect.DeepEqual(got, second) {
		t.Errorf("the repeat after another save changed the stored state:\n got  %+v\n want %+v", got, second)
	}
}

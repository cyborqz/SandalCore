// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Mail (migration 0027): a character's mails and their attachments. A mail is created with its sender, subject, body, expiry as a duration and its attachments;
// its id, issue time, absolute expiry and each new attachment's instance guid and creation time come from the store. A mail is read with its items and status,
// saved whole (its content, its status and its claimed items), refused when a save would clear a stored mark or a claim, and removed with its character. The caps
// (200 live mails, 20 attachments, the text limits in bytes, a 10-year expiry) and the stale-save rule are tested here; the migration is migration0027_test.go.

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"
)

const mailTestGUID = "0123456789abcdef0123456789abcdef"

// newMailCharacter creates a Bard with a small kit, as the other social tests do.
func newMailCharacter(t *testing.T, s *Store, acc int64, name string) Character {
	t.Helper()
	c, err := s.Create(context.Background(), New{AccountID: acc, Name: name, ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func float32Ptr(v float32) *float32 { return &v }

// attachments makes n attachments, each with every field different, so a field that slips or a place that swaps is seen.
func attachments(n int) []MailItem {
	out := make([]MailItem, n)
	for i := range out {
		out[i] = MailItem{
			ItemRecordID:    7001 + int64(i),
			InstanceGUID:    guid(0x200 + i),
			Quantity:        int32(1 + i),
			Quality:         int32(2 + i),
			Durability:      float32(10.5 + float64(i)),
			CreatedAt:       time.UnixMilli(1_780_000_000_000 + int64(i)),
			PersistedHealth: float32Ptr(float32(3210.5 + float64(i))),
			PersistedMana:   float32Ptr(float32(120.5 + float64(i))),
			LockExpiration:  int64(1_772_000_000 + i),
		}
	}
	return out
}

// mailByID finds one mail of a read list, or fails the test.
func mailByID(t *testing.T, mails []Mail, id string) Mail {
	t.Helper()
	for _, m := range mails {
		if m.ID == id {
			return m
		}
	}
	t.Fatalf("the mail %s is not in the %d that were read", id, len(mails))
	return Mail{}
}

// mailRowStatus reads one mail's stored status, items and claimed flags without the live filter.
func mailRowStatus(t *testing.T, s *Store, id string) (int32, []MailItem) {
	t.Helper()
	ctx := context.Background()
	var status int32
	if err := s.pool.QueryRow(ctx, `SELECT status FROM character_mails WHERE id = $1`, id).Scan(&status); err != nil {
		t.Fatalf("reading the stored status of %s: %v", id, err)
	}
	rows, err := s.pool.Query(ctx, `SELECT item_record_id, instance_guid, quantity, quality, durability, created_at, persisted_health, persisted_mana, lock_expiration, claimed
		FROM character_mail_items WHERE mail_id = $1 ORDER BY position`, id)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var items []MailItem
	for rows.Next() {
		var it MailItem
		if err := rows.Scan(&it.ItemRecordID, &it.InstanceGUID, &it.Quantity, &it.Quality, &it.Durability, &it.CreatedAt, &it.PersistedHealth, &it.PersistedMana,
			&it.LockExpiration, &it.Claimed); err != nil {
			t.Fatal(err)
		}
		items = append(items, it)
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	return status, items
}

// The mail row is written and read back whole, with its attachments in order, and a new store (a restart) reads what the earlier one wrote.
func TestMailsAreCreatedAndReadBackAcrossAStoreRestart(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	first, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Welcome", Body: "Welcome to the Riverlands."})
	if err != nil {
		t.Fatalf("CreateMail: %v", err)
	}
	second, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Mirelle", Subject: "A delivery", Body: "Two things for you.", ExpiresInMs: 3_600_000,
		Items: []MailItem{{ItemRecordID: 7001, Quantity: 2, Quality: 3, Durability: 1.5}, {ItemRecordID: 7002, Quantity: 1, Quality: 1, Durability: 0}}})
	if err != nil {
		t.Fatalf("CreateMail with items: %v", err)
	}
	fresh := NewStore(s.pool) // a new store reads the database, as after a restart
	got, err := fresh.Mails(ctx, c.ID)
	if err != nil || len(got) != 2 {
		t.Fatalf("Mails = %d mails, %v; want the 2 that were written", len(got), err)
	}
	one := mailByID(t, got, first.ID)
	if one.Sender != "Brannoc" || one.Subject != "Welcome" || one.Body != "Welcome to the Riverlands." || one.Status != 0 ||
		one.IssuedAtMs != first.IssuedAtMs || one.ExpiresAtMs != 0 || len(one.Items) != 0 {
		t.Errorf("the first mail came back as %+v; want the fields it was created with", one)
	}
	two := mailByID(t, got, second.ID)
	if len(two.Items) != 2 {
		t.Fatalf("the second mail holds %d attachments, want 2", len(two.Items))
	}
	for i, it := range two.Items {
		if it.ItemRecordID != second.Items[i].ItemRecordID || it.InstanceGUID != second.Items[i].InstanceGUID || it.Quantity != second.Items[i].Quantity ||
			it.Quality != second.Items[i].Quality || it.Durability != second.Items[i].Durability || !it.CreatedAt.Equal(second.Items[i].CreatedAt) || it.Claimed {
			t.Errorf("attachment %d came back as %+v, want the stored %+v, in the mail's order", i, it, second.Items[i])
		}
	}
}

// A read mark is kept by a save, and a fresh read counts one fewer unread.
func TestASaveKeepsTheReadMark(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	one, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "One", Body: ""})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Two", Body: ""}); err != nil {
		t.Fatal(err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 2 || UnreadCount(mails) != 2 {
		t.Fatalf("before the save: %d mails, %d unread, %v; want 2 and 2", len(mails), UnreadCount(mails), err)
	}
	read := mailByID(t, mails, one.ID)
	read.Status |= MailStatusRead
	if err := s.SaveMail(ctx, c.ID, read); err != nil {
		t.Fatalf("SaveMail with the read bit: %v", err)
	}
	fresh, err := NewStore(s.pool).Mails(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got := mailByID(t, fresh, one.ID); got.Status != MailStatusRead {
		t.Errorf("the mail came back with status %d, want %d (the read mark is kept)", got.Status, MailStatusRead)
	}
	if got := UnreadCount(fresh); got != 1 {
		t.Errorf("the unread count is %d after the read, want 1", got)
	}
}

// A delete mark is kept: the mail is not read again, its row keeps status 2, and its attachments stay; saving the same delete again is allowed.
func TestASaveKeepsTheDeleteMark(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	doomed, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Delete me", Body: "", Items: attachments(1)})
	if err != nil {
		t.Fatal(err)
	}
	live, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Keep me", Body: ""})
	if err != nil {
		t.Fatal(err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 2 {
		t.Fatalf("Mails: %d, %v; want 2", len(mails), err)
	}
	deleted := mailByID(t, mails, doomed.ID)
	deleted.Status |= MailStatusDeleted
	if err := s.SaveMail(ctx, c.ID, deleted); err != nil {
		t.Fatalf("SaveMail with the delete bit: %v", err)
	}
	fresh, err := s.Mails(ctx, c.ID)
	if err != nil || len(fresh) != 1 || fresh[0].ID != live.ID {
		t.Errorf("Mails = %+v, %v; want only the live mail %s", fresh, err, live.ID)
	}
	status, items := mailRowStatus(t, s, doomed.ID)
	if status != MailStatusDeleted {
		t.Errorf("the deleted mail's row holds status %d, want %d", status, MailStatusDeleted)
	}
	if len(items) != 1 {
		t.Errorf("the deleted mail keeps %d attachments, want its 1", len(items))
	}
	// the same delete again changes nothing and is not stale
	if err := s.SaveMail(ctx, c.ID, deleted); err != nil {
		t.Errorf("saving the same delete again: %v", err)
	}
	if status, items = mailRowStatus(t, s, doomed.ID); status != MailStatusDeleted || len(items) != 1 {
		t.Errorf("after the repeated delete the row holds status %d and %d attachments, want %d and 1", status, len(items), MailStatusDeleted)
	}
}

// The list carries each live mail's sender, subject and status; a deleted mail is not in it.
func TestTheMailListLeavesOutADeletedMail(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	live, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Mirelle", Subject: "Live", Body: ""})
	if err != nil {
		t.Fatal(err)
	}
	doomed, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Gone", Body: ""})
	if err != nil {
		t.Fatal(err)
	}
	doomed.Status |= MailStatusDeleted
	if err := s.SaveMail(ctx, c.ID, doomed); err != nil {
		t.Fatal(err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 1 {
		t.Fatalf("Mails = %d mails, %v; want only the live one", len(mails), err)
	}
	if got := mails[0]; got.ID != live.ID || got.Sender != "Mirelle" || got.Subject != "Live" || got.Status&MailStatusRead != 0 {
		t.Errorf("the list holds %+v; want the live mail with its sender, subject and unread status", got)
	}
}

// The unread count leaves out the read and the deleted mails; a SQL count over the same rows agrees.
func TestUnreadCountLeavesOutReadAndDeleted(t *testing.T) {
	if got := UnreadCount([]Mail{{Status: 0}, {Status: MailStatusRead}, {Status: MailStatusRead | MailStatusDeleted}, {Status: MailStatusDeleted}}); got != 1 {
		t.Errorf("UnreadCount of statuses 0, 4, 6 and 2 = %d, want 1", got)
	}
	if got := UnreadCount(nil); got != 0 {
		t.Errorf("UnreadCount(nil) = %d, want 0", got)
	}
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	for i, status := range []int32{0, MailStatusRead, MailStatusRead | MailStatusDeleted, MailStatusDeleted} {
		m, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: fmt.Sprintf("Mail %d", i), Body: ""})
		if err != nil {
			t.Fatal(err)
		}
		if _, err := s.pool.Exec(ctx, `UPDATE character_mails SET status = $2 WHERE id = $1`, m.ID, status); err != nil {
			t.Fatal(err)
		}
	}
	var counted int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1 AND (status & $2) = 0 AND (status & $3) = 0`,
		c.ID, MailStatusDeleted, MailStatusRead).Scan(&counted); err != nil {
		t.Fatal(err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got := UnreadCount(mails); got != 1 || got != int32(counted) {
		t.Errorf("UnreadCount(read list) = %d and the SQL count = %d, want 1 and the same", got, counted)
	}
	if len(mails) != 2 {
		t.Errorf("the live list holds %d mails, want 2 (the two without the delete bit)", len(mails))
	}
}

// The count query agrees with UnreadCount for unread, read, deleted and expired mails.
func TestUnreadMailsAgreesWithUnreadCount(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	expired := time.Now().Add(-time.Hour).UnixMilli()
	for i, spec := range []struct {
		status  int32
		expires int64
	}{
		{0, 0},                                    // unread
		{0, expired},                              // unread, expired: still counted, as UnreadCount reads the list
		{MailStatusRead, 0},                       // read
		{MailStatusDeleted, 0},                    // deleted
		{MailStatusRead | MailStatusDeleted, 0},   // read and deleted
	} {
		m, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: fmt.Sprintf("Mail %d", i), Body: ""})
		if err != nil {
			t.Fatal(err)
		}
		if _, err := s.pool.Exec(ctx, `UPDATE character_mails SET status = $2, expires_at_ms = $3 WHERE id = $1`, m.ID, spec.status, spec.expires); err != nil {
			t.Fatal(err)
		}
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	want := UnreadCount(mails)
	got, err := s.UnreadMails(ctx, c.ID)
	if err != nil || got != want || want != 2 {
		t.Errorf("UnreadMails = %d, %v; UnreadCount = %d; want the same 2 (the unread and the expired unread)", got, err, want)
	}
	if _, err := s.UnreadMails(ctx, strings.Repeat("f", 32)); !errors.Is(err, ErrNotFound) {
		t.Errorf("UnreadMails for an unknown character = %v, want ErrNotFound", err)
	}
}

// The detail's fields are read back: sender, subject, body, issue time, expiry and the attachments.
func TestAMailsDetailFieldsAreReadBack(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	created, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "A letter", Body: "Line one.\nLine two.", ExpiresInMs: 3_600_000,
		Items: []MailItem{{ItemRecordID: 7001, Quantity: 1, Quality: 1, Durability: 2}, {ItemRecordID: 7002, Quantity: 4, Quality: 5, Durability: 6}}})
	if err != nil {
		t.Fatal(err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 1 {
		t.Fatalf("Mails = %d, %v; want 1", len(mails), err)
	}
	got := mails[0]
	if got.ID != created.ID || got.Sender != "Brannoc" || got.Subject != "A letter" || got.Body != "Line one.\nLine two." ||
		got.IssuedAtMs != created.IssuedAtMs || got.IssuedAtMs <= 0 || got.ExpiresAtMs != created.IssuedAtMs+3_600_000 || got.Status != 0 {
		t.Errorf("the detail came back as %+v; want the sender, subject, body, issue time and absolute expiry that were stored", got)
	}
	if len(got.Items) != 2 || got.Items[0].ItemRecordID != 7001 || got.Items[1].ItemRecordID != 7002 {
		t.Errorf("the detail's attachments are %+v, want the two that were attached, in order", got.Items)
	}
}

// Every attachment field is stored and read back in the mail's order.
func TestMailItemsKeepTheirStateAndOrder(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	created, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "With things", Body: ""})
	if err != nil {
		t.Fatal(err)
	}
	items := attachments(3)
	created.Items = items
	if err := s.SaveMail(ctx, c.ID, created); err != nil {
		t.Fatalf("SaveMail with every item field: %v", err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 1 {
		t.Fatalf("Mails = %d, %v; want 1", len(mails), err)
	}
	got := mails[0].Items
	if len(got) != len(items) {
		t.Fatalf("the mail holds %d attachments, want %d", len(got), len(items))
	}
	for i := range items {
		a, b := got[i], items[i]
		if a.ItemRecordID != b.ItemRecordID || a.InstanceGUID != b.InstanceGUID || a.Quantity != b.Quantity || a.Quality != b.Quality ||
			a.Durability != b.Durability || !a.CreatedAt.Equal(b.CreatedAt) || a.LockExpiration != b.LockExpiration || a.Claimed != b.Claimed {
			t.Errorf("attachment %d came back as %+v, want %+v", i, a, b)
		}
		if (a.PersistedHealth == nil) != (b.PersistedHealth == nil) || (a.PersistedHealth != nil && *a.PersistedHealth != *b.PersistedHealth) ||
			(a.PersistedMana == nil) != (b.PersistedMana == nil) || (a.PersistedMana != nil && *a.PersistedMana != *b.PersistedMana) {
			t.Errorf("attachment %d keeps persisted health/mana %v/%v, want %v/%v", i, a.PersistedHealth, a.PersistedMana, b.PersistedHealth, b.PersistedMana)
		}
	}
	// one attachment with no persisted state stays that way
	one, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Plain", Body: "",
		Items: []MailItem{{ItemRecordID: 7009, Quantity: 1, Quality: 1, Durability: 0}}})
	if err != nil {
		t.Fatal(err)
	}
	read, err := s.Mails(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	plain := mailByID(t, read, one.ID).Items
	if len(plain) != 1 || plain[0].PersistedHealth != nil || plain[0].PersistedMana != nil || plain[0].LockExpiration != 0 {
		t.Errorf("the plain attachment came back as %+v, want no persisted state and no lock", plain)
	}
}

// A claim is a save of the mail: the claimed flag is kept per attachment and a later claim adds to it.
func TestASaveKeepsItemClaims(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	created, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Claim me", Body: "", Items: attachments(2)})
	if err != nil {
		t.Fatal(err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 1 || len(mails[0].Items) != 2 {
		t.Fatalf("Mails = %+v, %v; want one mail with two attachments", mails, err)
	}
	mail := mails[0]
	mail.Items[0].Claimed = true
	if err := s.SaveMail(ctx, c.ID, mail); err != nil {
		t.Fatalf("saving the first claim: %v", err)
	}
	got, err := s.Mails(ctx, c.ID)
	if err != nil || len(got) != 1 || !got[0].Items[0].Claimed || got[0].Items[1].Claimed {
		t.Errorf("after the first claim the flags are %+v, %v; want only the first claimed", got, err)
	}
	var claimed int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items WHERE mail_id = $1 AND claimed`, created.ID).Scan(&claimed); err != nil || claimed != 1 {
		t.Errorf("%d stored attachments are claimed (%v), want 1", claimed, err)
	}
	mail = got[0]
	mail.Items[1].Claimed = true
	if err := s.SaveMail(ctx, c.ID, mail); err != nil {
		t.Fatalf("saving the second claim: %v", err)
	}
	got, err = s.Mails(ctx, c.ID)
	if err != nil || len(got) != 1 || !got[0].Items[0].Claimed || !got[0].Items[1].Claimed {
		t.Errorf("after claiming both, the flags are %+v, %v; want both claimed", got, err)
	}
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items WHERE mail_id = $1 AND claimed`, created.ID).Scan(&claimed); err != nil || claimed != 2 {
		t.Errorf("%d stored attachments are claimed (%v), want 2", claimed, err)
	}
}

// The admin send adds one mail: the store gives it its id, its issue time and an absolute expiry, and gives every new attachment its instance guid and creation
// time; an attachment sent as claimed is refused.
func TestCreateMailAddsAMailForTheCharacter(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	before := time.Now().UnixMilli()
	created, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Your reward", Body: "Take these.", ExpiresInMs: 3_600_000,
		Items: []MailItem{{ItemRecordID: 7001, InstanceGUID: strings.Repeat("ab", 16), Quantity: 2, Quality: 3, Durability: 4.5, CreatedAt: time.UnixMilli(1_000)},
			{ItemRecordID: 7002, InstanceGUID: strings.Repeat("ab", 16), Quantity: 1, Quality: 1, Durability: 0, CreatedAt: time.UnixMilli(1_000)}}})
	if err != nil {
		t.Fatalf("CreateMail: %v", err)
	}
	if !idPattern.MatchString(created.ID) || created.ID == strings.Repeat("0", 32) {
		t.Errorf("the store's mail id %q is not 32 lowercase hex digits (not all zero)", created.ID)
	}
	if created.IssuedAtMs < before || created.Status != 0 || created.ExpiresAtMs != created.IssuedAtMs+3_600_000 {
		t.Errorf("the stored mail is %+v; want status 0, an issue time now and the expiry at issue+duration", created)
	}
	if len(created.Items) != 2 || created.Items[0].ItemRecordID != 7001 || created.Items[1].ItemRecordID != 7002 {
		t.Fatalf("the stored attachments are %+v, want the two records in order", created.Items)
	}
	seen := map[string]bool{}
	for i, it := range created.Items {
		if !guidPattern.MatchString(it.InstanceGUID) || it.InstanceGUID == strings.Repeat("0", 32) || seen[it.InstanceGUID] || it.InstanceGUID == strings.Repeat("ab", 16) {
			t.Errorf("attachment %d's instance guid %q is not a fresh 32-digit hex guid", i, it.InstanceGUID)
		}
		seen[it.InstanceGUID] = true
		if !it.CreatedAt.Equal(time.UnixMilli(created.IssuedAtMs)) {
			t.Errorf("attachment %d was made at %v, want the mail's issue time %v", i, it.CreatedAt, time.UnixMilli(created.IssuedAtMs))
		}
		if it.Claimed {
			t.Errorf("attachment %d was stored as claimed, want it claimable", i)
		}
	}
	// a duration of 0 keeps no expiry
	noExpiry, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "No expiry", Body: ""})
	if err != nil || noExpiry.ExpiresAtMs != 0 {
		t.Errorf("a mail with no duration has expiry %d, %v; want 0", noExpiry.ExpiresAtMs, err)
	}
	// an attachment sent as claimed is refused, and nothing is written
	if _, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Already claimed", Body: "",
		Items: []MailItem{{ItemRecordID: 7001, Quantity: 1, Quality: 1, Claimed: true}}}); !errors.Is(err, ErrInvalid) {
		t.Errorf("an attachment sent as claimed: CreateMail = %v, want ErrInvalid", err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 2 {
		t.Errorf("after the refused create the character holds %d mails, %v; want 2", len(mails), err)
	}
}

// The live mails are read newest first by issue time, the mail id breaking a tie.
func TestMailsAreReadNewestFirst(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	var made []Mail
	for i := 0; i < 3; i++ {
		m, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: fmt.Sprintf("Mail %d", i), Body: ""})
		if err != nil {
			t.Fatal(err)
		}
		made = append(made, m)
	}
	// the oldest first created, two mails with the same issue time
	for id, issued := range map[int]int64{0: 1_760_000_000_000, 1: 1_770_000_000_000, 2: 1_770_000_000_000} {
		if _, err := s.pool.Exec(ctx, `UPDATE character_mails SET issued_at_ms = $2 WHERE id = $1`, made[id].ID, issued); err != nil {
			t.Fatal(err)
		}
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 3 {
		t.Fatalf("Mails = %d, %v; want 3", len(mails), err)
	}
	newer, older := made[1].ID, made[2].ID
	if older < newer {
		newer, older = older, newer
	}
	want := []string{newer, older, made[0].ID}
	for i, id := range want {
		if mails[i].ID != id {
			t.Errorf("mail %d is %s, want %s (newest first, the id breaking a tie): got %v", i, mails[i].ID, id, []string{mails[0].ID, mails[1].ID, mails[2].ID})
		}
	}
}

// The largest mailbox reads back whole; a 201st live mail is refused, and a deleted mail frees a place.
func TestTheLargestMailboxIsSavedWhole(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	ids := make(map[string]bool, MaxMails)
	for i := 0; i < MaxMails; i++ {
		m, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: fmt.Sprintf("Mail %d", i), Body: ""})
		if err != nil {
			t.Fatalf("mail %d of %d: %v", i+1, MaxMails, err)
		}
		ids[m.ID] = true
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != MaxMails {
		t.Fatalf("Mails = %d, %v; want the %d live mails", len(mails), err, MaxMails)
	}
	for _, m := range mails {
		if !ids[m.ID] {
			t.Errorf("the mailbox holds a mail that was not written: %s", m.ID)
		}
	}
	if _, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "One too many", Body: ""}); !errors.Is(err, ErrInvalid) {
		t.Errorf("the 201st mail: CreateMail = %v, want ErrInvalid", err)
	}
	if mails, err = s.Mails(ctx, c.ID); err != nil || len(mails) != MaxMails {
		t.Errorf("after the refused 201st the mailbox holds %d mails, %v; want %d", len(mails), err, MaxMails)
	}
	// a deleted mail does not count: its place frees
	freed := mails[0]
	freed.Status |= MailStatusDeleted
	if err := s.SaveMail(ctx, c.ID, freed); err != nil {
		t.Fatal(err)
	}
	if _, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "After a delete", Body: ""}); err != nil {
		t.Errorf("a create after a deleted mail: %v, want it accepted", err)
	}
}

// Two creates at the last place give exactly one more mail.
func TestTheMailboxCapHoldsUnderTwoCreates(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	for i := 0; i < MaxMails-1; i++ {
		if _, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: fmt.Sprintf("Mail %d", i), Body: ""}); err != nil {
			t.Fatal(err)
		}
	}
	errs := make(chan error, 2)
	var wg sync.WaitGroup
	for i := 0; i < 2; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: fmt.Sprintf("The last place %d", i), Body: ""})
			errs <- err
		}(i)
	}
	wg.Wait()
	close(errs)
	created, refused := 0, 0
	for err := range errs {
		switch {
		case err == nil:
			created++
		case errors.Is(err, ErrInvalid):
			refused++
		default:
			t.Errorf("a concurrent create failed with %v, want nil or ErrInvalid", err)
		}
	}
	if created != 1 || refused != 1 {
		t.Errorf("two creates at the last place: %d created, %d refused; want 1 and 1", created, refused)
	}
	var count int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1 AND (status & $2) = 0`, c.ID, MailStatusDeleted).Scan(&count); err != nil || count != MaxMails {
		t.Errorf("the mailbox holds %d live mails (%v), want %d", count, err, MaxMails)
	}
}

// Twenty attachments are accepted; a 21st is refused with nothing written.
func TestCreateMailRefusesTooManyItems(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	if _, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Twenty", Body: "", Items: attachments(MaxMailItems)}); err != nil {
		t.Errorf("a mail with %d attachments: %v, want it accepted", MaxMailItems, err)
	}
	if _, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Twenty-one", Body: "", Items: attachments(MaxMailItems + 1)}); !errors.Is(err, ErrInvalid) {
		t.Errorf("a mail with %d attachments: CreateMail = %v, want ErrInvalid", MaxMailItems+1, err)
	}
	var mails, items int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1`, c.ID).Scan(&mails); err != nil || mails != 1 {
		t.Errorf("after the refused 21st the character holds %d mails (%v), want 1", mails, err)
	}
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items i JOIN character_mails m ON m.id = i.mail_id WHERE m.character_id = $1`, c.ID).Scan(&items); err != nil || items != MaxMailItems {
		t.Errorf("the accepted mail holds %d attachments (%v), want %d", items, err, MaxMailItems)
	}
}

// ValidateMail and ValidateNewMail refuse every bad field without a database.
func TestValidateMail(t *testing.T) {
	good := Mail{ID: mailTestGUID, Sender: "Brannoc", Subject: "Welcome", Body: "A body.", Status: MailStatusRead,
		IssuedAtMs: 1_771_000_000_000, ExpiresAtMs: 1_771_600_000_000, Items: attachments(1)}
	if err := ValidateMail(good); err != nil {
		t.Fatalf("a good mail: %v", err)
	}
	long := func(n int) string { return strings.Repeat("b", n) }
	for _, tc := range []struct {
		name string
		mod  func(*Mail)
	}{
		{"an empty id", func(m *Mail) { m.ID = "" }},
		{"a short id", func(m *Mail) { m.ID = "abc" }},
		{"an id in capitals", func(m *Mail) { m.ID = strings.ToUpper(mailTestGUID) }},
		{"the zero id", func(m *Mail) { m.ID = strings.Repeat("0", 32) }},
		{"a negative status", func(m *Mail) { m.Status = -1 }},
		{"an issue time of 0", func(m *Mail) { m.IssuedAtMs = 0 }},
		{"a negative expiry", func(m *Mail) { m.ExpiresAtMs = -1 }},
		{"an empty sender", func(m *Mail) { m.Sender = "" }},
		{"a sender with a space behind", func(m *Mail) { m.Sender = "Brannoc " }},
		{"a sender behind the byte limit", func(m *Mail) { m.Sender = strings.Repeat("é", MaxMailSenderLength/2+1) }},
		{"a sender with a control character", func(m *Mail) { m.Sender = "Brann\x01oc" }},
		{"an empty subject", func(m *Mail) { m.Subject = "" }},
		{"a subject behind the byte limit", func(m *Mail) { m.Subject = strings.Repeat("é", MaxMailSubjectLength/2+1) }},
		{"a body behind the byte limit", func(m *Mail) { m.Body = long(MaxMailBodyLength + 1) }},
		{"a body that is not UTF-8", func(m *Mail) { m.Body = "\xff" }},
		{"too many attachments", func(m *Mail) { m.Items = attachments(MaxMailItems + 1) }},
		{"an attachment twice", func(m *Mail) {
			m.Items = []MailItem{{ItemRecordID: 7001, InstanceGUID: mailTestGUID, Quantity: 1, Quality: 1, CreatedAt: time.UnixMilli(1_000)},
				{ItemRecordID: 7002, InstanceGUID: mailTestGUID, Quantity: 1, Quality: 1, CreatedAt: time.UnixMilli(1_000)}}
		}},
		{"an attachment without a record", func(m *Mail) { m.Items[0].ItemRecordID = 0 }},
		{"an attachment without a creation time", func(m *Mail) { m.Items[0].CreatedAt = time.Time{} }},
		{"an attachment with a zero guid", func(m *Mail) { m.Items[0].InstanceGUID = strings.Repeat("0", 32) }},
		{"an attachment with quantity 0", func(m *Mail) { m.Items[0].Quantity = 0 }},
		{"an attachment with quality past the limit", func(m *Mail) { m.Items[0].Quality = MaxQuality + 1 }},
		{"an attachment with a negative durability", func(m *Mail) { m.Items[0].Durability = -1 }},
	} {
		m := good
		m.Items = append([]MailItem(nil), good.Items...)
		tc.mod(&m)
		if err := ValidateMail(m); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: ValidateMail = %v, want ErrInvalid", tc.name, err)
		}
	}
	newGood := NewMail{Sender: "Brannoc", Subject: "Welcome", Body: "A body.", ExpiresInMs: 3_600_000, Items: attachments(1)}
	if err := ValidateNewMail(newGood); err != nil {
		t.Fatalf("a good new mail: %v", err)
	}
	for _, tc := range []struct {
		name string
		mod  func(*NewMail)
	}{
		{"a negative duration", func(n *NewMail) { n.ExpiresInMs = -1 }},
		{"a duration past the cap", func(n *NewMail) { n.ExpiresInMs = MaxMailExpiryMs + 1 }},
		{"an empty sender", func(n *NewMail) { n.Sender = "" }},
		{"a body with a NUL", func(n *NewMail) { n.Body = "a\x00b" }},
		{"too many attachments", func(n *NewMail) { n.Items = attachments(MaxMailItems + 1) }},
		{"an attachment sent as claimed", func(n *NewMail) { n.Items[0].Claimed = true }},
		{"an attachment without a record", func(n *NewMail) { n.Items[0].ItemRecordID = 0 }},
	} {
		n := newGood
		n.Items = append([]MailItem(nil), newGood.Items...)
		tc.mod(&n)
		if err := ValidateNewMail(n); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: ValidateNewMail = %v, want ErrInvalid", tc.name, err)
		}
	}
	// the text limits are bytes: 64 and 128 bytes of two-byte letters pass, one more byte does not
	if err := ValidateNewMail(NewMail{Sender: strings.Repeat("é", 32), Subject: strings.Repeat("é", 64), Body: long(MaxMailBodyLength)}); err != nil {
		t.Errorf("text exactly at the byte limit: %v, want it accepted", err)
	}
}

// A create that breaks a rule changes nothing.
func TestCreateMailRefusesWhatBreaksTheRules(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	good := NewMail{Sender: "Brannoc", Subject: "Welcome", Body: "A body.", ExpiresInMs: 3_600_000, Items: attachments(1)}
	for _, tc := range []struct {
		name string
		mod  func(*NewMail)
	}{
		{"an empty sender", func(n *NewMail) { n.Sender = "" }},
		{"a sender with a space in front", func(n *NewMail) { n.Sender = " Brannoc" }},
		{"a sender behind the byte limit", func(n *NewMail) { n.Sender = strings.Repeat("é", MaxMailSenderLength/2+1) }},
		{"an empty subject", func(n *NewMail) { n.Subject = "" }},
		{"a subject behind the byte limit", func(n *NewMail) { n.Subject = strings.Repeat("é", MaxMailSubjectLength/2+1) }},
		{"a body behind the byte limit", func(n *NewMail) { n.Body = strings.Repeat("b", MaxMailBodyLength+1) }},
		{"a body with a NUL", func(n *NewMail) { n.Body = "a\x00b" }},
		{"a negative duration", func(n *NewMail) { n.ExpiresInMs = -1 }},
		{"a duration past the cap", func(n *NewMail) { n.ExpiresInMs = MaxMailExpiryMs + 1 }},
		{"an attachment without a record", func(n *NewMail) { n.Items[0].ItemRecordID = 0 }},
		{"an attachment with quantity 0", func(n *NewMail) { n.Items[0].Quantity = 0 }},
		{"an attachment with quality 0", func(n *NewMail) { n.Items[0].Quality = 0 }},
		{"an attachment with a negative durability", func(n *NewMail) { n.Items[0].Durability = -1 }},
		{"an attachment with a negative persisted health", func(n *NewMail) { n.Items[0].PersistedHealth = float32Ptr(-1) }},
		{"an attachment with a negative lock", func(n *NewMail) { n.Items[0].LockExpiration = -1 }},
		{"an attachment sent as claimed", func(n *NewMail) { n.Items[0].Claimed = true }},
	} {
		n := good
		n.Items = append([]MailItem(nil), good.Items...)
		tc.mod(&n)
		if _, err := s.CreateMail(ctx, c.ID, n); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: CreateMail = %v, want ErrInvalid", tc.name, err)
		}
	}
	var mails, items int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1`, c.ID).Scan(&mails); err != nil || mails != 0 {
		t.Errorf("after the refusals the character holds %d mails (%v), want none", mails, err)
	}
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items`).Scan(&items); err != nil || items != 0 {
		t.Errorf("after the refusals there are %d attachments (%v), want none", items, err)
	}
}

// A save that breaks a rule is refused with nothing changed.
func TestSaveMailRefusesWhatBreaksTheRules(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	created, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Welcome", Body: "A body.", Items: attachments(1)})
	if err != nil {
		t.Fatal(err)
	}
	good := created
	if err := s.SaveMail(ctx, c.ID, good); err != nil {
		t.Fatalf("a save of the stored mail: %v", err)
	}
	for _, tc := range []struct {
		name string
		mod  func(*Mail)
	}{
		{"an empty id", func(m *Mail) { m.ID = "" }},
		{"a malformed id", func(m *Mail) { m.ID = "not a guid" }},
		{"a negative status", func(m *Mail) { m.Status = -1 }},
		{"an issue time of 0", func(m *Mail) { m.IssuedAtMs = 0 }},
		{"a negative expiry", func(m *Mail) { m.ExpiresAtMs = -1 }},
		{"an empty sender", func(m *Mail) { m.Sender = "" }},
		{"a body behind the byte limit", func(m *Mail) { m.Body = strings.Repeat("b", MaxMailBodyLength+1) }},
		{"too many attachments", func(m *Mail) { m.Items = append(attachments(1), attachments(MaxMailItems)...) }},
		{"an attachment without a record", func(m *Mail) { m.Items[0].ItemRecordID = 0 }},
		{"an attachment twice", func(m *Mail) { m.Items = []MailItem{m.Items[0], m.Items[0]} }},
	} {
		m := good
		m.Items = append([]MailItem(nil), good.Items...)
		tc.mod(&m)
		if err := s.SaveMail(ctx, c.ID, m); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: SaveMail = %v, want ErrInvalid", tc.name, err)
		}
	}
	status, items := mailRowStatus(t, s, created.ID)
	if status != 0 || len(items) != 1 || items[0].InstanceGUID != good.Items[0].InstanceGUID || items[0].Claimed {
		t.Errorf("after the refusals the row holds status %d and %+v; want it as it was", status, items)
	}
}

// The mails go with their character; an unknown or deleted character is NotFound for all three calls.
func TestMailsGoWithTheirCharacterAndAnUnknownOneIsNotFound(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a := newMailCharacter(t, s, acc, "cyb")
	b := newMailCharacter(t, s, acc, "dee")
	aMail, err := s.CreateMail(ctx, a.ID, NewMail{Sender: "Brannoc", Subject: "For cyb", Body: "", Items: attachments(1)})
	if err != nil {
		t.Fatal(err)
	}
	bMail, err := s.CreateMail(ctx, b.ID, NewMail{Sender: "Brannoc", Subject: "For dee", Body: ""})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Delete(ctx, acc, a.ID); err != nil {
		t.Fatal(err)
	}
	var mails, items int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1`, a.ID).Scan(&mails); err != nil || mails != 0 {
		t.Errorf("a deleted character keeps %d mails (%v), want none", mails, err)
	}
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items i JOIN character_mails m ON m.id = i.mail_id WHERE m.character_id = $1`, a.ID).Scan(&items); err != nil || items != 0 {
		t.Errorf("a deleted character keeps %d attachments (%v), want none", items, err)
	}
	got, err := s.Mails(ctx, b.ID)
	if err != nil || len(got) != 1 || got[0].ID != bMail.ID {
		t.Errorf("the other character's mailbox is %+v, %v; want its one mail", got, err)
	}
	for name, id := range map[string]string{"a deleted character": a.ID, "an unknown id": "0123456789abcdef0123456789abcdef", "a malformed id": "nope"} {
		if _, err := s.Mails(ctx, id); !errors.Is(err, ErrNotFound) {
			t.Errorf("%s: Mails = %v, want ErrNotFound", name, err)
		}
		if _, err := s.CreateMail(ctx, id, NewMail{Sender: "Brannoc", Subject: "Hello", Body: ""}); !errors.Is(err, ErrNotFound) {
			t.Errorf("%s: CreateMail = %v, want ErrNotFound", name, err)
		}
	}
	// a valid mail for an unknown character (its mail id is well formed; the character is not there)
	orphan := aMail
	if err := s.SaveMail(ctx, "0123456789abcdef0123456789abcdef", orphan); !errors.Is(err, ErrNotFound) {
		t.Errorf("a save for an unknown character: %v, want ErrNotFound", err)
	}
	// a well-formed mail id that no row holds, for a character that exists
	unknown := bMail
	unknown.ID = strings.Repeat("f", 32)
	if err := s.SaveMail(ctx, b.ID, unknown); !errors.Is(err, ErrNotFound) {
		t.Errorf("a save of a mail that is not there: %v, want ErrNotFound", err)
	}
}

// A stale whole-mail save cannot clear a stored mark or unclaim or drop a stored claimed item; it writes nothing, while the content and the unclaimed items may
// still be rewritten.
func TestAStaleMailSaveCannotUnclaimOrClearAMark(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	created, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Claim and mark", Body: "", Items: attachments(2)})
	if err != nil {
		t.Fatal(err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 1 || len(mails[0].Items) != 2 {
		t.Fatalf("Mails = %+v, %v; want one mail with two attachments", mails, err)
	}
	claimed := mails[0]
	claimed.Items[0].Claimed = true
	claimed.Status |= MailStatusRead | MailStatusDeleted
	if err := s.SaveMail(ctx, c.ID, claimed); err != nil {
		t.Fatalf("the save that claims and marks: %v", err)
	}
	if status, items := mailRowStatus(t, s, created.ID); status != MailStatusRead|MailStatusDeleted || len(items) != 2 || !items[0].Claimed {
		t.Fatalf("after the valid save the row holds status %d and %+v; want the marks and the claim", status, items)
	}
	// The second store is a second session: it holds the copy it read before the first session marked and claimed the mail.
	second := NewStore(s.pool)
	for _, tc := range []struct {
		name string
		mod  func(*Mail)
	}{
		{"a copy without the read mark", func(m *Mail) { m.Status = MailStatusDeleted }},
		{"a copy without the delete mark", func(m *Mail) { m.Status = MailStatusRead }},
		{"a copy without any mark", func(m *Mail) { m.Status = 0 }},
		{"a copy that unclaims the item", func(m *Mail) { m.Items[0].Claimed = false }},
		{"a copy that drops the claimed item", func(m *Mail) { m.Items = m.Items[1:] }},
		{"a copy that drops both items", func(m *Mail) { m.Items = nil }},
	} {
		stale := claimed
		stale.Items = append([]MailItem(nil), claimed.Items...)
		tc.mod(&stale)
		if err := second.SaveMail(ctx, c.ID, stale); !errors.Is(err, ErrMailStale) {
			t.Errorf("%s: SaveMail = %v, want ErrMailStale", tc.name, err)
		}
		if status, items := mailRowStatus(t, s, created.ID); status != MailStatusRead|MailStatusDeleted || len(items) != 2 || !items[0].Claimed {
			t.Errorf("%s: after the refused save the row holds status %d and %+v; want the marks and the claim", tc.name, status, items)
		}
	}
	// a save that keeps the marks and the claim may rewrite the content and add an unclaimed item
	grown := claimed
	grown.Sender, grown.Subject, grown.Body = "Mirelle", "A new title", "A new body."
	grown.Items = append(grown.Items, MailItem{ItemRecordID: 7010, InstanceGUID: guid(0x301), Quantity: 1, Quality: 1, CreatedAt: time.UnixMilli(1_780_000_000_500)})
	if err := second.SaveMail(ctx, c.ID, grown); err != nil {
		t.Fatalf("a save that keeps the marks and the claim: %v", err)
	}
	var sender, subject, body string
	if err := s.pool.QueryRow(ctx, `SELECT sender, subject, body FROM character_mails WHERE id = $1`, created.ID).Scan(&sender, &subject, &body); err != nil {
		t.Fatal(err)
	}
	status, items := mailRowStatus(t, s, created.ID)
	if sender != "Mirelle" || subject != "A new title" || body != "A new body." || status != MailStatusRead|MailStatusDeleted || len(items) != 3 || !items[0].Claimed {
		t.Errorf("after the accepted save: sender %q, subject %q, body %q, status %d, %d attachments; want the new content, the marks and 3 attachments",
			sender, subject, body, status, len(items))
	}
}

// The same whole-mail save twice changes nothing the second time.
func TestTheSameMailSaveTwiceChangesNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newMailCharacter(t, s, acc, "cyb")
	created, err := s.CreateMail(ctx, c.ID, NewMail{Sender: "Brannoc", Subject: "Once", Body: "", Items: attachments(1)})
	if err != nil {
		t.Fatal(err)
	}
	mails, err := s.Mails(ctx, c.ID)
	if err != nil || len(mails) != 1 {
		t.Fatalf("Mails = %d, %v; want 1", len(mails), err)
	}
	save := mails[0]
	save.Status |= MailStatusRead
	save.Items[0].Claimed = true
	for i := 0; i < 2; i++ {
		if err := s.SaveMail(ctx, c.ID, save); err != nil {
			t.Fatalf("save %d of the same mail: %v", i+1, err)
		}
		if status, items := mailRowStatus(t, s, created.ID); status != MailStatusRead || len(items) != 1 || !items[0].Claimed {
			t.Errorf("after save %d the row holds status %d and %+v; want the read mark and the one claim", i+1, status, items)
		}
	}
	var count int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1`, c.ID).Scan(&count); err != nil || count != 1 {
		t.Errorf("after saving the same mail twice there are %d rows (%v), want 1", count, err)
	}
}

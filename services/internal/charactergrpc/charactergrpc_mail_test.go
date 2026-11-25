// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package charactergrpc

// Mail (migration 0027) over gRPC: GetMails maps a character's mails and their attachments, SaveMail passes one mail whole and maps a stale save to
// FailedPrecondition, CreateMail passes the admin send's fields and returns the stored mail, a store without mails answers Unimplemented, and on the real store the
// whole path works: an admin send, a read with the detail, a read mark and a claim, a stale save refused, a delete that hides the mail, and NotFound for an unknown
// character or mail. GetCharacter with the items carries the unread count at the login.

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

const mailProtoGUID = "0123456789abcdef0123456789abcdef"

// mailFake is the fake store that also keeps the mails (the part of character.Store that MailStore names) and records what it was given.
type mailFake struct {
	*recStore
	mails       []character.Mail
	err         error
	mailsID     string
	saveID      string
	saved       character.Mail
	createID    string
	created     character.NewMail
	createRet   character.Mail
	asked       int
	mailAsked   int
	saveAsked   int
	creatAsked  int
	unread      int32
	unreadErr   error
	unreadID    string
	unreadAsked int
}

var _ MailStore = (*mailFake)(nil)

func (m *mailFake) Mails(_ context.Context, id string) ([]character.Mail, error) {
	m.asked++
	m.mailAsked++
	m.mailsID = id
	return m.mails, m.err
}

func (m *mailFake) UnreadMails(_ context.Context, id string) (int32, error) {
	m.asked++
	m.unreadAsked++
	m.unreadID = id
	return m.unread, m.unreadErr
}

func (m *mailFake) SaveMail(_ context.Context, id string, mail character.Mail) error {
	m.asked++
	m.saveAsked++
	m.saveID, m.saved = id, mail
	return m.err
}

func (m *mailFake) CreateMail(_ context.Context, id string, mail character.NewMail) (character.Mail, error) {
	m.asked++
	m.creatAsked++
	m.createID, m.created = id, mail
	return m.createRet, m.err
}

// protoItem and wantItem are one attachment on either side of the service, with every field set.
func protoItem(n int) *characterv1.MailItem {
	health, mana := float32(3210.5+float32(n)), float32(120.5+float32(n))
	return &characterv1.MailItem{
		ItemRecordId: 7001 + int64(n), InstanceGuid: fmtGUID(0x200 + n), Quantity: int32(1 + n), Quality: int32(2 + n), Durability: float32(10.5 + float64(n)),
		CreatedAtMs: 1_780_000_000_000 + int64(n), PersistedHealth: &health, PersistedMana: &mana, LockExpiration: 1_772_000_000 + int64(n), Claimed: n == 0,
	}
}

func wantItem(n int) character.MailItem {
	health, mana := float32(3210.5+float32(n)), float32(120.5+float32(n))
	return character.MailItem{
		ItemRecordID: 7001 + int64(n), InstanceGUID: fmtGUID(0x200 + n), Quantity: int32(1 + n), Quality: int32(2 + n), Durability: float32(10.5 + float64(n)),
		CreatedAt: time.UnixMilli(1_780_000_000_000 + int64(n)), PersistedHealth: &health, PersistedMana: &mana, LockExpiration: 1_772_000_000 + int64(n), Claimed: n == 0,
	}
}

// fullProtoMail and wantMail are the same mail on either side of the service: every field has its own value, so a swapped or dropped one is seen.
func fullProtoMail() *characterv1.Mail {
	return &characterv1.Mail{Id: mailProtoGUID, Sender: "Brannoc", Subject: "Welcome", Body: "Line one.\nLine two.", Status: 6,
		IssuedAtMs: 1_771_000_000_000, ExpiresAtMs: 1_771_600_000_000, Items: []*characterv1.MailItem{protoItem(0), protoItem(1)}}
}

func wantMail() character.Mail {
	return character.Mail{ID: mailProtoGUID, Sender: "Brannoc", Subject: "Welcome", Body: "Line one.\nLine two.", Status: 6,
		IssuedAtMs: 1_771_000_000_000, ExpiresAtMs: 1_771_600_000_000, Items: []character.MailItem{wantItem(0), wantItem(1)}}
}

func TestGetMailsMapsTheCharacterMails(t *testing.T) {
	second := wantMail()
	second.ID, second.Subject, second.Status, second.Items = fmtGUID(0x300), "No items", 0, nil
	st := &mailFake{recStore: &recStore{ret: fullCharacter()}, mails: []character.Mail{wantMail(), second}}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	got, err := c.GetMails(context.Background(), &characterv1.GetMailsRequest{CharacterId: id})
	if err != nil {
		t.Fatal(err)
	}
	if st.mailsID != id || st.mailAsked != 1 {
		t.Errorf("the store was asked for %q %d times; want the character id as sent, once", st.mailsID, st.mailAsked)
	}
	if got.GetMails() == nil || len(got.GetMails()) != 2 {
		t.Fatalf("GetMails returned %d mails, want 2", len(got.GetMails()))
	}
	want := []*characterv1.Mail{fullProtoMail(), {Id: fmtGUID(0x300), Sender: "Brannoc", Subject: "No items", Body: "Line one.\nLine two.",
		Status: 0, IssuedAtMs: 1_771_000_000_000, ExpiresAtMs: 1_771_600_000_000}}
	for i := range want {
		if !proto.Equal(got.GetMails()[i], want[i]) {
			t.Errorf("mail %d is %v, want %v", i, got.GetMails()[i], want[i])
		}
	}
	// a store with no mail answers an empty list
	empty := dialRec(t, &mailFake{recStore: &recStore{ret: fullCharacter()}})
	if got, err := empty.GetMails(context.Background(), &characterv1.GetMailsRequest{CharacterId: id}); err != nil || len(got.GetMails()) != 0 {
		t.Errorf("a character with no mail: %v, %v; want an empty list", got, err)
	}
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		failing := dialRec(t, &mailFake{recStore: &recStore{ret: fullCharacter()}, err: tc.err})
		if _, err := failing.GetMails(context.Background(), &characterv1.GetMailsRequest{CharacterId: id}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
}

func TestSaveMailPassesTheMailToTheStore(t *testing.T) {
	st := &mailFake{recStore: &recStore{ret: fullCharacter()}}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	if _, err := c.SaveMail(context.Background(), &characterv1.SaveMailRequest{CharacterId: id, Mail: fullProtoMail()}); err != nil {
		t.Fatalf("SaveMail = %v", err)
	}
	if st.saveID != id || st.saveAsked != 1 {
		t.Errorf("the store was given %q %d times; want the character id as sent, once", st.saveID, st.saveAsked)
	}
	if !sameMail(st.saved, wantMail()) {
		t.Errorf("the store was given %+v; want %+v", st.saved, wantMail())
	}
	// every failure is its own code; a stale save is FailedPrecondition
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"stale":     {character.ErrMailStale, codes.FailedPrecondition},
		"invalid":   {character.ErrInvalid, codes.InvalidArgument},
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		failing := dialRec(t, &mailFake{recStore: &recStore{ret: fullCharacter()}, err: tc.err})
		if _, err := failing.SaveMail(context.Background(), &characterv1.SaveMailRequest{CharacterId: id, Mail: fullProtoMail()}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
	// no mail is InvalidArgument and the store is not asked
	notAsked := &mailFake{recStore: &recStore{ret: fullCharacter()}}
	client := dialRec(t, notAsked)
	if _, err := client.SaveMail(context.Background(), &characterv1.SaveMailRequest{CharacterId: id}); status.Code(err) != codes.InvalidArgument {
		t.Errorf("a save without a mail: %v, want InvalidArgument", err)
	}
	if notAsked.saveAsked != 0 {
		t.Errorf("a save without a mail asked the store %d times, want none", notAsked.saveAsked)
	}
}

// sameMail compares a mail field by field (time.Time by instant, the optional stats by value).
func sameMail(got, want character.Mail) bool {
	if got.ID != want.ID || got.Sender != want.Sender || got.Subject != want.Subject || got.Body != want.Body || got.Status != want.Status ||
		got.IssuedAtMs != want.IssuedAtMs || got.ExpiresAtMs != want.ExpiresAtMs || len(got.Items) != len(want.Items) {
		return false
	}
	for i := range want.Items {
		a, b := got.Items[i], want.Items[i]
		if a.ItemRecordID != b.ItemRecordID || a.InstanceGUID != b.InstanceGUID || a.Quantity != b.Quantity || a.Quality != b.Quality || a.Durability != b.Durability ||
			!a.CreatedAt.Equal(b.CreatedAt) || a.LockExpiration != b.LockExpiration || a.Claimed != b.Claimed ||
			(a.PersistedHealth == nil) != (b.PersistedHealth == nil) || (a.PersistedHealth != nil && *a.PersistedHealth != *b.PersistedHealth) ||
			(a.PersistedMana == nil) != (b.PersistedMana == nil) || (a.PersistedMana != nil && *a.PersistedMana != *b.PersistedMana) {
			return false
		}
	}
	return true
}

func TestCreateMailAddsAMailThroughTheService(t *testing.T) {
	st := &mailFake{recStore: &recStore{ret: fullCharacter()}, createRet: wantMail()}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	got, err := c.CreateMail(context.Background(), &characterv1.CreateMailRequest{CharacterId: id, Sender: "Brannoc", Subject: "Welcome", Body: "Line one.\nLine two.",
		ExpiresInMs: 3_600_000, Items: []*characterv1.MailItem{protoItem(0), protoItem(1)}})
	if err != nil {
		t.Fatalf("CreateMail = %v", err)
	}
	if st.createID != id || st.creatAsked != 1 {
		t.Errorf("the store was given %q %d times; want the character id as sent, once", st.createID, st.creatAsked)
	}
	if st.created.Sender != "Brannoc" || st.created.Subject != "Welcome" || st.created.Body != "Line one.\nLine two." || st.created.ExpiresInMs != 3_600_000 ||
		len(st.created.Items) != 2 || !sameMailItem(st.created.Items[0], wantItem(0)) || !sameMailItem(st.created.Items[1], wantItem(1)) {
		t.Errorf("the store was given %+v; want the request's fields and items", st.created)
	}
	if got.GetMail() == nil || !proto.Equal(got.GetMail(), fullProtoMail()) {
		t.Errorf("the reply is %v, want the stored mail %v", got.GetMail(), fullProtoMail())
	}
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"invalid":   {character.ErrInvalid, codes.InvalidArgument},
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		failing := dialRec(t, &mailFake{recStore: &recStore{ret: fullCharacter()}, err: tc.err})
		if _, err := failing.CreateMail(context.Background(), &characterv1.CreateMailRequest{CharacterId: id, Sender: "Brannoc", Subject: "Welcome"}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
}

func sameMailItem(got, want character.MailItem) bool {
	return got.ItemRecordID == want.ItemRecordID && got.InstanceGUID == want.InstanceGUID && got.Quantity == want.Quantity && got.Quality == want.Quality &&
		got.Durability == want.Durability && got.CreatedAt.Equal(want.CreatedAt) && got.LockExpiration == want.LockExpiration && got.Claimed == want.Claimed &&
		(got.PersistedHealth == nil) == (want.PersistedHealth == nil) && (got.PersistedHealth == nil || *got.PersistedHealth == *want.PersistedHealth) &&
		(got.PersistedMana == nil) == (want.PersistedMana == nil) && (got.PersistedMana == nil || *got.PersistedMana == *want.PersistedMana)
}

// A store that keeps no mails answers Unimplemented, not a wrong answer.
func TestMailCallsOnAStoreWithoutMails(t *testing.T) {
	c := dialRec(t, &recStore{ret: fullCharacter()})
	ctx := context.Background()
	if _, err := c.GetMails(ctx, &characterv1.GetMailsRequest{CharacterId: mailProtoGUID}); status.Code(err) != codes.Unimplemented {
		t.Errorf("GetMails on a store without mails: %v, want Unimplemented", err)
	}
	if _, err := c.SaveMail(ctx, &characterv1.SaveMailRequest{CharacterId: mailProtoGUID, Mail: fullProtoMail()}); status.Code(err) != codes.Unimplemented {
		t.Errorf("SaveMail on a store without mails: %v, want Unimplemented", err)
	}
	if _, err := c.CreateMail(ctx, &characterv1.CreateMailRequest{CharacterId: mailProtoGUID, Sender: "Brannoc", Subject: "Welcome"}); status.Code(err) != codes.Unimplemented {
		t.Errorf("CreateMail on a store without mails: %v, want Unimplemented", err)
	}
}

// The unread count comes with the character load (with the items) as one count query, a failing count still answers the character with 0,
// and a store without mails sends none.
func TestGetCharacterCarriesTheUnreadMailCount(t *testing.T) {
	st := &mailFake{recStore: &recStore{ret: fullCharacter()}, unread: 2}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	got, err := c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id})
	if err != nil || got.GetCharacter().GetUnreadMailCount() != 0 || st.unreadAsked != 0 {
		t.Fatalf("without the items: %v, %v (%d count reads); want no count and no question", got, err, st.unreadAsked)
	}
	got, err = c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
	if err != nil {
		t.Fatal(err)
	}
	if got.GetCharacter().GetUnreadMailCount() != 2 || st.unreadID != id || st.unreadAsked != 1 || st.mailAsked != 0 {
		t.Errorf("with the items: count %d after %d count reads of %q (%d list reads); want 2 after one count read of %q and no list read",
			got.GetCharacter().GetUnreadMailCount(), st.unreadAsked, st.unreadID, st.mailAsked, id)
	}
	// a store that keeps no mails is still a character, with no count
	plain := dialRec(t, &recStore{ret: fullCharacter()})
	if got, err := plain.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true}); err != nil || got.GetCharacter().GetUnreadMailCount() != 0 {
		t.Errorf("a store without mails: %v, %v; want the character with no count", got, err)
	}
	// a count that fails is logged and leaves 0: the login is not failed for it
	failingStore := &mailFake{recStore: &recStore{ret: fullCharacter()}, unreadErr: errors.New("the count query is down")}
	failing := dialRec(t, failingStore)
	got, err = failing.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
	if err != nil || got.GetCharacter() == nil || got.GetCharacter().GetUnreadMailCount() != 0 {
		t.Errorf("a failing count: %v, %v; want the character with count 0", got, err)
	}
	if failingStore.unreadAsked != 1 {
		t.Errorf("a failing count was asked %d times, want 1", failingStore.unreadAsked)
	}
}

// The whole path on the real store: the admin send, the list, the detail, a read mark and a claim, a stale save, a delete, and NotFound for what is not there.
func TestMailsThroughTheServiceOnTheRealStore(t *testing.T) {
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
	sent, err := c.CreateMail(ctx, &characterv1.CreateMailRequest{CharacterId: id, Sender: "Brannoc", Subject: "Welcome", Body: "Line one.\nLine two.", ExpiresInMs: 3_600_000,
		Items: []*characterv1.MailItem{{ItemRecordId: 7001, Quantity: 2, Quality: 3, Durability: 1.5}, {ItemRecordId: 7002, Quantity: 1, Quality: 1, Durability: 0}}})
	if err != nil {
		t.Fatalf("CreateMail: %v", err)
	}
	mail := sent.GetMail()
	if mail.GetId() == "" || mail.GetStatus() != 0 || mail.GetIssuedAtMs() <= 0 || mail.GetExpiresAtMs() != mail.GetIssuedAtMs()+3_600_000 ||
		mail.GetSender() != "Brannoc" || mail.GetSubject() != "Welcome" || mail.GetBody() != "Line one.\nLine two." || len(mail.GetItems()) != 2 {
		t.Fatalf("the stored mail is %v; want its own id, status 0, the issue time, the absolute expiry and the two attachments", mail)
	}
	if mail.GetItems()[0].GetInstanceGuid() == "" || mail.GetItems()[0].GetInstanceGuid() == mail.GetItems()[1].GetInstanceGuid() {
		t.Errorf("the attachments carry guids %q and %q, want two fresh ones", mail.GetItems()[0].GetInstanceGuid(), mail.GetItems()[1].GetInstanceGuid())
	}
	read := func() *characterv1.Mail {
		t.Helper()
		list, err := c.GetMails(ctx, &characterv1.GetMailsRequest{CharacterId: id})
		if err != nil {
			t.Fatalf("GetMails: %v", err)
		}
		if len(list.GetMails()) != 1 {
			t.Fatalf("GetMails returned %d mails, want 1", len(list.GetMails()))
		}
		return list.GetMails()[0]
	}
	got := read()
	if !proto.Equal(got, mail) {
		t.Errorf("the mail read back is %v, want the stored %v", got, mail)
	}
	// mark it read and claim the first attachment
	got.Status = got.GetStatus() | character.MailStatusRead
	got.Items[0].Claimed = true
	if _, err := c.SaveMail(ctx, &characterv1.SaveMailRequest{CharacterId: id, Mail: got}); err != nil {
		t.Fatalf("SaveMail with the read mark and the claim: %v", err)
	}
	after := read()
	if after.GetStatus() != character.MailStatusRead || !after.GetItems()[0].GetClaimed() || after.GetItems()[1].GetClaimed() {
		t.Errorf("after the save the mail is %v; want the read mark and only the first attachment claimed", after)
	}
	// the old copy (unread, unclaimed) is stale and changes nothing
	stale := proto.Clone(mail).(*characterv1.Mail)
	if _, err := c.SaveMail(ctx, &characterv1.SaveMailRequest{CharacterId: id, Mail: stale}); status.Code(err) != codes.FailedPrecondition {
		t.Errorf("the stale save: %v, want FailedPrecondition", err)
	}
	if after := read(); after.GetStatus() != character.MailStatusRead || !after.GetItems()[0].GetClaimed() {
		t.Errorf("after the stale save the mail is %v; want the read mark and the claim kept", after)
	}
	// delete it: the row stays and GetMails leaves it out
	deleted := proto.Clone(after).(*characterv1.Mail)
	deleted.Status = deleted.GetStatus() | character.MailStatusDeleted
	if _, err := c.SaveMail(ctx, &characterv1.SaveMailRequest{CharacterId: id, Mail: deleted}); err != nil {
		t.Fatalf("SaveMail with the delete mark: %v", err)
	}
	if list, err := c.GetMails(ctx, &characterv1.GetMailsRequest{CharacterId: id}); err != nil || len(list.GetMails()) != 0 {
		t.Errorf("after the delete GetMails = %v, %v; want no mail", list, err)
	}
	var rows int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1`, id).Scan(&rows); err != nil || rows != 1 {
		t.Errorf("the deleted mail keeps %d rows (%v), want its 1 (the delete is a mark)", rows, err)
	}
	// an attachment sent as claimed, and a mailbox over its cap, are InvalidArgument
	claimed := proto.Clone(mail).(*characterv1.Mail)
	claimed.Items[0].Claimed = true
	if _, err := c.CreateMail(ctx, &characterv1.CreateMailRequest{CharacterId: id, Sender: "Brannoc", Subject: "Claimed", Items: []*characterv1.MailItem{claimed.GetItems()[0]}}); status.Code(err) != codes.InvalidArgument {
		t.Errorf("an attachment sent as claimed: %v, want InvalidArgument", err)
	}
	tooMany := make([]*characterv1.MailItem, 0, character.MaxMailItems+1)
	for i := 0; i < character.MaxMailItems+1; i++ {
		tooMany = append(tooMany, &characterv1.MailItem{ItemRecordId: 7001, Quantity: 1, Quality: 1})
	}
	if _, err := c.CreateMail(ctx, &characterv1.CreateMailRequest{CharacterId: id, Sender: "Brannoc", Subject: "Too many", Items: tooMany}); status.Code(err) != codes.InvalidArgument {
		t.Errorf("a mail with %d attachments: %v, want InvalidArgument", len(tooMany), err)
	}
	// what is not there is NotFound
	if _, err := c.GetMails(ctx, &characterv1.GetMailsRequest{CharacterId: "ffffffffffffffffffffffffffffffff"}); status.Code(err) != codes.NotFound {
		t.Errorf("GetMails for an unknown character: %v, want NotFound", err)
	}
	if _, err := c.CreateMail(ctx, &characterv1.CreateMailRequest{CharacterId: "ffffffffffffffffffffffffffffffff", Sender: "Brannoc", Subject: "Hello"}); status.Code(err) != codes.NotFound {
		t.Errorf("CreateMail for an unknown character: %v, want NotFound", err)
	}
	unknown := proto.Clone(mail).(*characterv1.Mail)
	unknown.Id = "ffffffffffffffffffffffffffffffff"
	if _, err := c.SaveMail(ctx, &characterv1.SaveMailRequest{CharacterId: id, Mail: unknown}); status.Code(err) != codes.NotFound {
		t.Errorf("SaveMail for an unknown mail: %v, want NotFound", err)
	}
}

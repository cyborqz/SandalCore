// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Mail (migration 0027): a character's mails with their items and status, read for one character and saved one mail at a time. A new mail is added with its
// sender, subject, body, an expiry duration and its attachments; the store gives it its id, its issue time, its absolute expiry and each new attachment's
// instance guid and creation time. The list is the live mails (without the delete bit), newest first; the unread count is the live mails without the read bit.

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/jackc/pgx/v5"
)

const (
	// MaxMails bounds a character's live mails (a deleted mail does not count): a bounded list and payload. The game data has no cap.
	MaxMails = 200
	// MaxMailItems bounds one mail's attachments: the client's attachment array is modelled; 20 keeps one mail small.
	MaxMailItems = 20
	// MaxMailSenderLength bounds the sender, in bytes (the table's octet_length).
	MaxMailSenderLength = 64
	// MaxMailSubjectLength bounds the subject (the list's title), in bytes.
	MaxMailSubjectLength = 128
	// MaxMailBodyLength bounds the body, in bytes.
	MaxMailBodyLength = 4096
	// MaxMailExpiryMs bounds an expiry duration: 10 years, so the issue time plus the duration stays in a bigint. The game data has no lifetime.
	MaxMailExpiryMs int64 = 10 * 365 * 24 * 60 * 60 * 1000
	// MailStatusDeleted and MailStatusRead are the client's own status bits; any other bit is kept as it was sent.
	MailStatusDeleted int32 = 2
	MailStatusRead    int32 = 4
)

// ErrMailStale is returned by SaveMail when the save would undo a mark or a claim the stored mail has: another save (a second session, a stale copy) wrote in
// between. The caller reads the mail again rather than writing over the change.
var ErrMailStale = errors.New("mail save is stale")

// MailItem is one attachment of a mail: the item's own data and whether it is claimed. An attachment has no container or slot until a claim moves it into the
// bags.
type MailItem struct {
	ItemRecordID    int64
	InstanceGUID    string // 32 lowercase hex digits, not all zero
	Quantity        int32
	Quality         int32
	Durability      float32
	CreatedAt       time.Time
	PersistedHealth *float32
	PersistedMana   *float32
	LockExpiration  int64 // Unix seconds; 0: not locked
	Claimed         bool
}

// Mail is one mail of a character: its sender, subject and body, its status, when it was issued and when it expires (0: none) and its attachments in the mail's
// order.
type Mail struct {
	ID          string // 32 lowercase hex digits, not all zero
	Sender      string
	Subject     string
	Body        string
	Status      int32
	IssuedAtMs  int64
	ExpiresAtMs int64
	Items       []MailItem
}

// NewMail is what CreateMail adds: the store gives the id, the issue time and the absolute expiry, and gives every attachment its instance guid and creation
// time (what the caller sent there is ignored). ExpiresInMs is the client's expiry duration; 0 means no expiry.
type NewMail struct {
	Sender      string
	Subject     string
	Body        string
	ExpiresInMs int64
	Items       []MailItem
}

// item is the attachment as the item's own checks see it: an attachment has no place until a claim moves it into the bags, so the storage is the backpack's
// first slot, which the checks accept for every attachment.
func (it MailItem) item() Item {
	return Item{Container: Backpack, Slot: 0, ItemRecordID: it.ItemRecordID, InstanceGUID: it.InstanceGUID, Quantity: it.Quantity, Quality: it.Quality,
		Durability: it.Durability, CreatedAt: it.CreatedAt, PersistedHealth: it.PersistedHealth, PersistedMana: it.PersistedMana, LockExpiration: it.LockExpiration}
}

// newItem is the attachment as it comes in a create request: its instance guid and creation time are the store's, so they are not checked.
func (it MailItem) newItem() Item {
	it.InstanceGUID = strings.Repeat("1", 32)
	it.CreatedAt = time.UnixMilli(minCreated)
	return it.item()
}

// validateMailText checks one name-like line of a mail: not empty, at most max bytes, valid UTF-8 and no control character, with no space at either end.
func validateMailText(what, text string, max int) error {
	switch {
	case text == "":
		return fmt.Errorf("%w: the %s is empty", ErrInvalid, what)
	case len(text) > max:
		return fmt.Errorf("%w: the %s is %d bytes (at most %d)", ErrInvalid, what, len(text), max)
	case !utf8.ValidString(text):
		return fmt.Errorf("%w: the %s is not valid UTF-8 text", ErrInvalid, what)
	case strings.TrimSpace(text) != text:
		return fmt.Errorf("%w: the %s has a space at one end: %q", ErrInvalid, what, text)
	}
	for _, r := range text {
		if r < 0x20 || r == 0x7f {
			return fmt.Errorf("%w: the %s has a control character: %q", ErrInvalid, what, text)
		}
	}
	return nil
}

// validateMailBody checks the body: valid UTF-8, at most MaxMailBodyLength bytes and no NUL (PostgreSQL refuses one).
func validateMailBody(body string) error {
	switch {
	case len(body) > MaxMailBodyLength:
		return fmt.Errorf("%w: the body is %d bytes (at most %d)", ErrInvalid, len(body), MaxMailBodyLength)
	case !utf8.ValidString(body):
		return fmt.Errorf("%w: the body is not valid UTF-8 text", ErrInvalid)
	case strings.IndexByte(body, 0) >= 0:
		return fmt.Errorf("%w: the body has a NUL", ErrInvalid)
	}
	return nil
}

// validateMailItems checks the attachments of a stored mail: each item's own fields and no instance guid twice.
func validateMailItems(items []MailItem) error {
	if len(items) > MaxMailItems {
		return fmt.Errorf("%w: %d attachments (at most %d)", ErrInvalid, len(items), MaxMailItems)
	}
	seen := make(map[string]bool, len(items))
	for i, it := range items {
		if err := it.item().Validate(); err != nil {
			return fmt.Errorf("attachment %d: %w", i, err)
		}
		if seen[it.InstanceGUID] {
			return fmt.Errorf("%w: attachment %d: instance guid %q again", ErrInvalid, i, it.InstanceGUID)
		}
		seen[it.InstanceGUID] = true
	}
	return nil
}

// ValidateMail checks one stored mail: its id, status and times, then its content and attachments.
func ValidateMail(m Mail) error {
	if !guidPattern.MatchString(m.ID) || m.ID == strings.Repeat("0", 32) {
		return fmt.Errorf("%w: mail id %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, m.ID)
	}
	if m.Status < 0 {
		return fmt.Errorf("%w: status %d is negative", ErrInvalid, m.Status)
	}
	if m.IssuedAtMs <= 0 {
		return fmt.Errorf("%w: issue time %d is not Unix milliseconds after 0", ErrInvalid, m.IssuedAtMs)
	}
	if m.ExpiresAtMs < 0 {
		return fmt.Errorf("%w: expiry %d is negative", ErrInvalid, m.ExpiresAtMs)
	}
	if err := validateMailText("sender", m.Sender, MaxMailSenderLength); err != nil {
		return err
	}
	if err := validateMailText("subject", m.Subject, MaxMailSubjectLength); err != nil {
		return err
	}
	if err := validateMailBody(m.Body); err != nil {
		return err
	}
	return validateMailItems(m.Items)
}

// ValidateNewMail checks what CreateMail was given: the content, the duration and the attachments. A new attachment's instance guid and creation time come
// from the store, so they are not checked; an attachment sent as claimed is refused.
func ValidateNewMail(n NewMail) error {
	if err := validateMailText("sender", n.Sender, MaxMailSenderLength); err != nil {
		return err
	}
	if err := validateMailText("subject", n.Subject, MaxMailSubjectLength); err != nil {
		return err
	}
	if err := validateMailBody(n.Body); err != nil {
		return err
	}
	if n.ExpiresInMs < 0 || n.ExpiresInMs > MaxMailExpiryMs {
		return fmt.Errorf("%w: the expiry duration %d ms is not 0 to %d", ErrInvalid, n.ExpiresInMs, MaxMailExpiryMs)
	}
	if len(n.Items) > MaxMailItems {
		return fmt.Errorf("%w: %d attachments (at most %d)", ErrInvalid, len(n.Items), MaxMailItems)
	}
	for i, it := range n.Items {
		if it.Claimed {
			return fmt.Errorf("%w: attachment %d is sent as claimed; a new attachment is claimable", ErrInvalid, i)
		}
		if err := it.newItem().Validate(); err != nil {
			return fmt.Errorf("attachment %d: %w", i, err)
		}
	}
	return nil
}

// UnreadCount is how many of the mails are live (no delete bit) and unread (no read bit).
func UnreadCount(mails []Mail) int32 {
	var n int32
	for _, m := range mails {
		if m.Status&(MailStatusDeleted|MailStatusRead) == 0 {
			n++
		}
	}
	return n
}

// UnreadMails is the number of a character's live, unread mails by one count query: the rule UnreadCount reads over the list
// (no delete bit, no read bit; an expired mail still counts until its status changes, as the list keeps it). ErrNotFound for an
// unknown character.
func (s *Store) UnreadMails(ctx context.Context, characterID string) (int32, error) {
	if !idPattern.MatchString(characterID) {
		return 0, ErrNotFound
	}
	var n int32
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_mails
		WHERE character_id = $1 AND (status & $2) = 0 AND (status & $3) = 0`, characterID, MailStatusDeleted, MailStatusRead).Scan(&n); err != nil {
		return 0, err
	}
	if n == 0 {
		var exists bool
		if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return 0, err
		}
		if !exists {
			return 0, ErrNotFound
		}
	}
	return n, nil
}

// Mails returns a character's live mails, newest first by issue time with the mail id breaking a tie, with their attachments in the mail's order. A deleted
// mail is left out; an unknown character is ErrNotFound.
func (s *Store) Mails(ctx context.Context, characterID string) ([]Mail, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	// One transaction, one snapshot: the mails and their attachments are read from the same state.
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	var out []Mail
	rows, err := tx.Query(ctx, `SELECT id, sender, subject, body, status, issued_at_ms, expires_at_ms FROM character_mails
		WHERE character_id = $1 AND (status & $2) = 0 ORDER BY issued_at_ms DESC, id`, characterID, MailStatusDeleted)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var m Mail
		if err := rows.Scan(&m.ID, &m.Sender, &m.Subject, &m.Body, &m.Status, &m.IssuedAtMs, &m.ExpiresAtMs); err != nil {
			rows.Close()
			return nil, err
		}
		out = append(out, m)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		var exists bool
		if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return nil, err
		}
		if !exists {
			return nil, ErrNotFound
		}
		return out, tx.Commit(ctx)
	}
	rows, err = tx.Query(ctx, `SELECT i.mail_id, i.item_record_id, i.instance_guid, i.quantity, i.quality, i.durability, i.created_at,
		i.persisted_health, i.persisted_mana, i.lock_expiration, i.claimed
		FROM character_mail_items i JOIN character_mails m ON m.id = i.mail_id
		WHERE m.character_id = $1 AND (m.status & $2) = 0 ORDER BY i.mail_id, i.position`, characterID, MailStatusDeleted)
	if err != nil {
		return nil, err
	}
	byID := make(map[string]int, len(out))
	for i := range out {
		byID[out[i].ID] = i
	}
	for rows.Next() {
		var mailID string
		var it MailItem
		if err := rows.Scan(&mailID, &it.ItemRecordID, &it.InstanceGUID, &it.Quantity, &it.Quality, &it.Durability, &it.CreatedAt,
			&it.PersistedHealth, &it.PersistedMana, &it.LockExpiration, &it.Claimed); err != nil {
			rows.Close()
			return nil, err
		}
		if i, ok := byID[mailID]; ok {
			out[i].Items = append(out[i].Items, it)
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return out, tx.Commit(ctx)
}

// SaveMail writes one mail whole: its content, its status and its attachments. It may rewrite the sender, subject, body, times and attachments, but not clear a
// stored status bit or unclaim or drop a stored claimed attachment (matched by its instance guid): that is ErrMailStale and nothing is written. It never claims: an
// attachment sent as claimed that is not claimed in the store is ErrInvalid and nothing is written (ClaimMailItems is the only way to claim, with the bags in the same
// transaction). ErrNotFound for an unknown character or mail; ErrInvalid for a mail the rules refuse.
func (s *Store) SaveMail(ctx context.Context, characterID string, m Mail) error {
	if err := ValidateMail(m); err != nil {
		return err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	if err := lockCharacter(ctx, tx, characterID); err != nil {
		return err
	}
	var stored int32
	if err := tx.QueryRow(ctx, `SELECT status FROM character_mails WHERE id = $1 AND character_id = $2 FOR UPDATE`, m.ID, characterID).Scan(&stored); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return ErrNotFound
		}
		return err
	}
	if m.Status&stored != stored {
		return fmt.Errorf("%w: the save has status %d and would clear a mark of the stored status %d", ErrMailStale, m.Status, stored)
	}
	rows, err := tx.Query(ctx, `SELECT instance_guid, claimed FROM character_mail_items WHERE mail_id = $1`, m.ID)
	if err != nil {
		return err
	}
	claimed := map[string]bool{}
	for rows.Next() {
		var guid string
		var isClaimed bool
		if err := rows.Scan(&guid, &isClaimed); err != nil {
			rows.Close()
			return err
		}
		if isClaimed {
			claimed[guid] = true
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	given := make(map[string]bool, len(m.Items))
	for _, it := range m.Items {
		if it.Claimed {
			given[it.InstanceGUID] = true
			// Only ClaimMailItems claims an attachment, together with the bags that hold it. A save that sets the flag would give the player nothing and leave the item
			// unclaimable, so it is refused whole (the other changes in it are not written either).
			if !claimed[it.InstanceGUID] {
				return fmt.Errorf("%w: the attachment %s is sent claimed, but a mail save never claims: only ClaimMailItems does", ErrInvalid, it.InstanceGUID)
			}
		}
	}
	for guid := range claimed {
		if !given[guid] {
			return fmt.Errorf("%w: the claimed attachment %s is sent unclaimed or left out", ErrMailStale, guid)
		}
	}
	if _, err := tx.Exec(ctx, `UPDATE character_mails SET sender = $2, subject = $3, body = $4, status = $5, issued_at_ms = $6, expires_at_ms = $7
		WHERE id = $1 AND character_id = $8`, m.ID, m.Sender, m.Subject, m.Body, m.Status, m.IssuedAtMs, m.ExpiresAtMs, characterID); err != nil {
		return itemsError(err)
	}
	if _, err := tx.Exec(ctx, `DELETE FROM character_mail_items WHERE mail_id = $1`, m.ID); err != nil {
		return err
	}
	if err := insertMailItems(ctx, tx, m.ID, m.Items); err != nil {
		return itemsError(err)
	}
	return tx.Commit(ctx)
}

// CreateMail adds one mail for a character: the store gives it its id and issue time, the expiry is the issue time plus the duration (0 stays 0), and every
// attachment gets a fresh instance guid and the issue time as its creation time. ErrNotFound for an unknown character; ErrInvalid for a mail the rules refuse
// or a full mailbox.
func (s *Store) CreateMail(ctx context.Context, characterID string, n NewMail) (Mail, error) {
	if err := ValidateNewMail(n); err != nil {
		return Mail{}, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Mail{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	if err := lockCharacter(ctx, tx, characterID); err != nil {
		return Mail{}, err
	}
	var live int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1 AND (status & $2) = 0`, characterID, MailStatusDeleted).Scan(&live); err != nil {
		return Mail{}, err
	}
	if live >= MaxMails {
		return Mail{}, fmt.Errorf("%w: the mailbox holds %d live mails (at most %d)", ErrInvalid, live, MaxMails)
	}
	id, err := freshMailGUID()
	if err != nil {
		return Mail{}, err
	}
	issued := time.Now().UnixMilli()
	expires := int64(0)
	if n.ExpiresInMs > 0 {
		expires = issued + n.ExpiresInMs
	}
	items := make([]MailItem, len(n.Items))
	for i, it := range n.Items {
		guid, err := freshMailGUID()
		if err != nil {
			return Mail{}, err
		}
		it.InstanceGUID, it.CreatedAt, it.Claimed = guid, time.UnixMilli(issued), false
		items[i] = it
	}
	if _, err := tx.Exec(ctx, `INSERT INTO character_mails (id, character_id, sender, subject, body, status, issued_at_ms, expires_at_ms)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`, id, characterID, n.Sender, n.Subject, n.Body, 0, issued, expires); err != nil {
		return Mail{}, itemsError(err)
	}
	if err := insertMailItems(ctx, tx, id, items); err != nil {
		return Mail{}, itemsError(err)
	}
	if err := tx.Commit(ctx); err != nil {
		return Mail{}, err
	}
	return Mail{ID: id, Sender: n.Sender, Subject: n.Subject, Body: n.Body, Status: 0, IssuedAtMs: issued, ExpiresAtMs: expires, Items: items}, nil
}

// insertMailItems adds a mail's attachments in their order, each at its place.
func insertMailItems(ctx context.Context, tx pgx.Tx, mailID string, items []MailItem) error {
	for i, it := range items {
		if _, err := tx.Exec(ctx, `INSERT INTO character_mail_items (mail_id, position, item_record_id, instance_guid, quantity, quality, durability,
			created_at, persisted_health, persisted_mana, lock_expiration, claimed) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)`,
			mailID, i, it.ItemRecordID, it.InstanceGUID, it.Quantity, it.Quality, it.Durability, it.CreatedAt, it.PersistedHealth, it.PersistedMana,
			it.LockExpiration, it.Claimed); err != nil {
			return err
		}
	}
	return nil
}

// freshMailGUID makes a random GUID for a mail or one of its attachments: 16 random bytes as 32 lowercase hex digits.
func freshMailGUID() (string, error) {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(raw[:]), nil
}

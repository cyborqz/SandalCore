-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Mail: a character's mails and the items attached to each of them.
--
-- TWO NEW TABLES. character_mails holds one row per mail: its GUID (the client's key), the recipient, the sender, the subject (the list's title), the body, the
-- client's status bits (2: deleted, 4: read; any other bit is kept as the client sent it) and when it was issued and expires (Unix milliseconds; an expiry of 0:
-- none). character_mail_items holds one row per attachment: the mail, its place in the mail's order (0 first), the item's own data (record, instance guid,
-- quantity, quality, durability, creation time, the mount state of migration 0007 and a lock) and whether the client has claimed it. An attachment has no
-- container or slot until a claim puts it into the bags; the claimed flag says it is no longer claimable. Both tables belong to the character in character_id
-- (the mail's recipient); deleting the character removes its mails and their items, and deleting a mail row removes its items. A claimed item moved into the
-- character's bags lives in character_items and is not touched here.
--
-- Nothing is backfilled: a character with no mail has no row. worldd reads a character's mails at the login and when the mail window opens (GetMails), saves one
-- mail whole after every change (SaveMail: the read and delete marks and the claimed attachments) and the admin send adds one (CreateMail). A deleted mail keeps
-- its row and its attachments and is not read again; a later piece that removes deleted rows is where they go.
--
-- The version is 0027. 0028 and 0029 stay free for this mail work's later migrations. The runner (internal/db/db.go) skips a version it has recorded, applies
-- every other one in version order and allows gaps, so a later 0028 or 0029 still applies.
--
-- This ADDS TWO TABLES AND ONLY WRITES TO THEM: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented
-- block in services/README.md ("Rolling back migration 0027").
--
-- The caps are the store's (mail.go): 200 live mails per character (a deleted mail does not count, so deleting frees a place), 20 attachments per mail, and the
-- text limits in bytes: a sender of 64, a subject of 128 and a body of 4096 UTF-8 bytes. The checks keep a row from holding a value the client's struct or the
-- store cannot carry; the database repeats them as a backstop.

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_mails (
    id            TEXT PRIMARY KEY
                  CONSTRAINT character_mails_id_hex CHECK (id ~ '^[0-9a-f]{32}$' AND id <> repeat('0', 32)),
    character_id  TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    sender        TEXT    NOT NULL
                  CONSTRAINT character_mails_sender CHECK (sender <> '' AND sender = btrim(sender) AND octet_length(sender) <= 64),
    subject       TEXT    NOT NULL
                  CONSTRAINT character_mails_subject CHECK (subject <> '' AND subject = btrim(subject) AND octet_length(subject) <= 128),
    body          TEXT    NOT NULL
                  CONSTRAINT character_mails_body CHECK (octet_length(body) <= 4096),
    status        INTEGER NOT NULL DEFAULT 0
                  CONSTRAINT character_mails_status CHECK (status >= 0),
    issued_at_ms  BIGINT  NOT NULL
                  CONSTRAINT character_mails_issued CHECK (issued_at_ms > 0),
    expires_at_ms BIGINT  NOT NULL DEFAULT 0
                  CONSTRAINT character_mails_expires CHECK (expires_at_ms >= 0)
);

-- The read of one character's live mails, newest first (issued_at_ms DESC, id), and the count of them for the mailbox cap.
CREATE INDEX character_mails_character ON character_mails (character_id, issued_at_ms DESC, id);

CREATE TABLE character_mail_items (
    mail_id          TEXT     NOT NULL REFERENCES character_mails (id) ON DELETE CASCADE,
    position         INTEGER  NOT NULL
                     CONSTRAINT character_mail_items_position CHECK (position >= 0 AND position < 20),
    item_record_id   BIGINT   NOT NULL
                     CONSTRAINT character_mail_items_record_positive CHECK (item_record_id > 0),
    instance_guid    TEXT     NOT NULL
                     CONSTRAINT character_mail_items_guid_hex CHECK (instance_guid ~ '^[0-9a-f]{32}$' AND instance_guid <> repeat('0', 32)),
    quantity         INTEGER  NOT NULL DEFAULT 1
                     CONSTRAINT character_mail_items_quantity CHECK (quantity BETWEEN 1 AND 1000000),
    quality          INTEGER  NOT NULL DEFAULT 1
                     CONSTRAINT character_mail_items_quality CHECK (quality BETWEEN 1 AND 200),
    durability       REAL     NOT NULL DEFAULT 0
                     CONSTRAINT character_mail_items_durability CHECK (durability >= 0 AND durability <= 1e9),   -- NaN is above 1e9 in PostgreSQL
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    persisted_health REAL,
    persisted_mana   REAL,
    lock_expiration  BIGINT   NOT NULL DEFAULT 0,
    claimed          BOOLEAN  NOT NULL DEFAULT false,
    CONSTRAINT character_mail_items_persisted_health CHECK (persisted_health IS NULL OR (persisted_health >= 0 AND persisted_health <= 1e9)),
    CONSTRAINT character_mail_items_persisted_mana   CHECK (persisted_mana   IS NULL OR (persisted_mana   >= 0 AND persisted_mana   <= 1e9)),
    CONSTRAINT character_mail_items_lock_expiration  CHECK (lock_expiration >= 0 AND lock_expiration <= 253402300799),   -- the end of the year 9999
    CONSTRAINT character_mail_items_pkey PRIMARY KEY (mail_id, position),
    CONSTRAINT character_mail_items_guid_unique UNIQUE (mail_id, instance_guid)
);

COMMENT ON TABLE character_mails IS 'Mail: one row per mail of a character (its GUID, the sender, the subject, the body, the client''s status bits and its issue and expiry times). worldd reads them at the login and when the mail window opens and saves one mail whole after every change (SaveMail).';
COMMENT ON COLUMN character_mails.id IS 'The mail''s GUID (the client''s key): 32 lower-case hex digits, not all zero.';
COMMENT ON COLUMN character_mails.character_id IS 'The recipient. Deleting the character removes its mails and their attachments.';
COMMENT ON COLUMN character_mails.sender IS 'The sender''s name, not empty, trimmed, at most 64 UTF-8 bytes.';
COMMENT ON COLUMN character_mails.subject IS 'The subject (the list''s title), not empty, trimmed, at most 128 UTF-8 bytes.';
COMMENT ON COLUMN character_mails.body IS 'The body the detail shows, at most 4096 UTF-8 bytes.';
COMMENT ON COLUMN character_mails.status IS 'The client''s status bits: 2 deleted, 4 read; any other bit is kept. A mail with the delete bit is not read again.';
COMMENT ON COLUMN character_mails.issued_at_ms IS 'When the mail was sent, Unix milliseconds.';
COMMENT ON COLUMN character_mails.expires_at_ms IS 'When the mail expires, Unix milliseconds; 0: no expiry.';

COMMENT ON TABLE character_mail_items IS 'Mail: one row per attachment of a mail, in the mail''s order, with the item''s own data and whether the client claimed it.';
COMMENT ON COLUMN character_mail_items.mail_id IS 'The mail. Deleting the mail row removes its attachments.';
COMMENT ON COLUMN character_mail_items.position IS 'The place in the mail, from 0 (the first attachment); below 20.';
COMMENT ON COLUMN character_mail_items.item_record_id IS 'The design database''s ItemRecord Guid.';
COMMENT ON COLUMN character_mail_items.instance_guid IS 'The item instance''s own GUID: 32 lower-case hex digits, not all zero; unique within one mail.';
COMMENT ON COLUMN character_mail_items.quantity IS 'The item''s count, 1 to 1000000 (as character_items).';
COMMENT ON COLUMN character_mail_items.quality IS 'The item''s quality, 1 to 200 (as character_items).';
COMMENT ON COLUMN character_mail_items.durability IS 'The item''s durability, 0 to 1e9 (as character_items).';
COMMENT ON COLUMN character_mail_items.created_at IS 'When the item was made.';
COMMENT ON COLUMN character_mail_items.persisted_health IS 'The mounts milestone: the Health a mount item persists, NULL for every other item.';
COMMENT ON COLUMN character_mail_items.persisted_mana IS 'The mounts milestone: the Mana a mount item persists, NULL for every other item.';
COMMENT ON COLUMN character_mail_items.lock_expiration IS 'The item''s lock time, Unix seconds (0: not locked).';
COMMENT ON COLUMN character_mail_items.claimed IS 'True once the client has claimed the attachment; it is no longer claimable.';

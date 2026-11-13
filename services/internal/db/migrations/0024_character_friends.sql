-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Social, step 7, "the friends list": the characters a character has as friends and the friend requests waiting for it.
--
-- Two NEW tables. character_friends holds one row per friend: the friend's character GUID and name, its level, class and subclass, its guild's name, whether it still counts this
-- character as a friend, and the friend's place in this character's list (oldest first). character_friend_requests holds one row per request this character has received and not
-- answered: the sender's GUID and name, the recipient's GUID, when the request expires and its place in this character's list (oldest first). Both tables belong to the character in
-- character_id, which is the recipient of a request and the owner of both lists; deleting the character removes its rows. worldd reads the lists with the character at the login
-- (GetCharacter, with the items) and saves them whole with the progress (SaveProgress with replace_friends and replace_friend_requests).
--
-- The thousands separator of an expiry: the store keeps Unix milliseconds (the game data's PendingFriendRequestTimeInterval is 1,209,600,000 for 14 days); the client's wire field is
-- Unix seconds, so the game server sends this value divided by 1,000.
--
-- Nothing is backfilled here: a character with no friends and no pending request has no row.
--
-- The version is 0024. 0020 is left free for the open account-citizenship change that owns that number, 0021 is the ignore list, and 0022 and 0023 are taken by a branch that is still in
-- progress. A database that branch has run records both versions, so the next number free on every branch is 0024. The runner
-- (internal/db/db.go) skips a version it has recorded, applies every other one in version order and allows gaps, so a 0020 that lands later still applies, to a database that holds
-- 0024 as well as to a new one, and a friends migration numbered 22 or 23 would never run on a database that branch has run. Two files with the same version are refused when the
-- services start.
--
-- This ADDS TWO TABLES AND ONLY WRITES TO THEM: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in
-- services/README.md ("Rolling back migration 0024").
--
-- The caps are the store's (friends.go: MaxFriends and MaxFriendRequests, 100 each). The place checks keep a row from holding a place outside a list of that size; a whole-list save
-- over either cap is refused by the store before anything is written.

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_friends (
    character_id  TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    friend_guid   TEXT    NOT NULL
                  CONSTRAINT character_friends_guid_hex CHECK (friend_guid ~ '^[0-9a-f]{32}$' AND friend_guid <> repeat('0', 32)),
    friend_name   TEXT    NOT NULL
                  CONSTRAINT character_friends_name CHECK (friend_name <> '' AND friend_name = btrim(friend_name) AND length(friend_name) <= 64),
    level         INTEGER NOT NULL
                  CONSTRAINT character_friends_level CHECK (level >= 1 AND level <= 25),
    class_id      INTEGER NOT NULL
                  CONSTRAINT character_friends_class CHECK (class_id >= 0 AND class_id <= 8),
    sub_archetype INTEGER NOT NULL
                  CONSTRAINT character_friends_sub_archetype CHECK (sub_archetype >= 0 AND sub_archetype <= 8),
    guild_name    TEXT    NOT NULL
                  CONSTRAINT character_friends_guild CHECK (guild_name = btrim(guild_name) AND length(guild_name) <= 64),
    still_friend  BOOLEAN NOT NULL,
    position      INTEGER NOT NULL
                  CONSTRAINT character_friends_position CHECK (position >= 0 AND position < 100),
    CONSTRAINT character_friends_pkey PRIMARY KEY (character_id, friend_guid),
    CONSTRAINT character_friends_order UNIQUE (character_id, position),
    CONSTRAINT character_friends_not_self CHECK (friend_guid <> character_id)
);

CREATE TABLE character_friend_requests (
    character_id   TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    sender_guid    TEXT    NOT NULL
                   CONSTRAINT character_friend_requests_sender_hex CHECK (sender_guid ~ '^[0-9a-f]{32}$' AND sender_guid <> repeat('0', 32)),
    sender_name    TEXT    NOT NULL
                   CONSTRAINT character_friend_requests_name CHECK (sender_name <> '' AND sender_name = btrim(sender_name) AND length(sender_name) <= 64),
    recipient_guid TEXT    NOT NULL
                   CONSTRAINT character_friend_requests_recipient_hex CHECK (recipient_guid ~ '^[0-9a-f]{32}$'),
    expires_at_ms  BIGINT  NOT NULL
                   CONSTRAINT character_friend_requests_expires CHECK (expires_at_ms > 0),
    position       INTEGER NOT NULL
                   CONSTRAINT character_friend_requests_position CHECK (position >= 0 AND position < 100),
    CONSTRAINT character_friend_requests_pkey PRIMARY KEY (character_id, sender_guid),
    CONSTRAINT character_friend_requests_order UNIQUE (character_id, position),
    CONSTRAINT character_friend_requests_not_self CHECK (sender_guid <> character_id),
    CONSTRAINT character_friend_requests_recipient CHECK (recipient_guid = character_id)
);

-- The read drops the requests that have expired for the character (friends.go) and the list keeps its oldest first; this index serves that read and the expiry column names it.
CREATE INDEX character_friend_requests_expiry ON character_friend_requests (character_id, expires_at_ms);

COMMENT ON TABLE character_friends IS 'Social, step 7: the characters a character has as friends, each with its place in the list (oldest first). worldd saves the list whole with the progress (SaveProgress).';
COMMENT ON COLUMN character_friends.character_id IS 'The character whose list this is. Deleting the character removes its friends.';
COMMENT ON COLUMN character_friends.friend_guid IS 'The friend''s character GUID: 32 lower-case hex digits, not all zero, and not the character itself.';
COMMENT ON COLUMN character_friends.friend_name IS 'The friend''s name as the character service has it; at most 64 characters.';
COMMENT ON COLUMN character_friends.level IS 'The friend''s level when the entry was written, 1 to 25.';
COMMENT ON COLUMN character_friends.class_id IS 'The friend''s ECharacterClass, 0 to 8 (0: a record whose class could not be derived).';
COMMENT ON COLUMN character_friends.sub_archetype IS 'The friend''s ECharacterClass subclass, 0 to 8 (0 in the game''s lists today).';
COMMENT ON COLUMN character_friends.guild_name IS 'The friend''s guild name; empty today, because the world has no guild system.';
COMMENT ON COLUMN character_friends.still_friend IS 'False once the friend removed this character: the entry stays until this character removes it too (a removal is one-way).';
COMMENT ON COLUMN character_friends.position IS 'The place in the character''s list, from 0, oldest first; below 100.';

COMMENT ON TABLE character_friend_requests IS 'Social, step 7: the friend requests a character has received and not answered, each with its place in the list (oldest first). worldd saves the list whole with the progress (SaveProgress).';
COMMENT ON COLUMN character_friend_requests.character_id IS 'The character the request is for; it is also the recipient_guid. Deleting the character removes its requests.';
COMMENT ON COLUMN character_friend_requests.sender_guid IS 'The request''s sender: 32 lower-case hex digits, not all zero, and not the character itself.';
COMMENT ON COLUMN character_friend_requests.sender_name IS 'The sender''s name as the character service has it; at most 64 characters.';
COMMENT ON COLUMN character_friend_requests.recipient_guid IS 'The character the request is for, the same as character_id.';
COMMENT ON COLUMN character_friend_requests.expires_at_ms IS 'When the request expires, Unix milliseconds; a request at or past this time is not read.';
COMMENT ON COLUMN character_friend_requests.position IS 'The place in the character''s list of pending requests, from 0, oldest first; below 100.';

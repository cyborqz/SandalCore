// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package character

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/jackc/pgx/v5"
)

// Friends (social, step 7, migration 0024): the characters one character has as friends, and the friend requests waiting for it, one row each in character_friends and
// character_friend_requests with their place in the character's list, so that the order they were added in (oldest first) is kept.  A request at or past its expiry is dropped when the
// lists are read.  worldd reads both lists with the character at the login and saves them whole with the progress (SaveProgress with replace_friends and replace_friend_requests).

const (
	// MaxFriends bounds a character's friends: the game data's MaximumFriendsListCount.
	MaxFriends = 100
	// MaxFriendRequests bounds the friend requests one character may have waiting: the owner's pick; the game data has no cap.
	MaxFriendRequests = 100
	// MaxFriendNameLength bounds a friend's or a sender's name, in bytes.  A character's name is 3 to 20 letters or digits; the bound is the table's, so a name the store takes is
	// stored whole.
	MaxFriendNameLength = 64
	// MaxGuildNameLength bounds a guild's name, in bytes; empty until the world has a guild system.
	MaxGuildNameLength = 64
	// FriendRequestLifetimeMs is how long a pending request stays: 14 days, the game data's PendingFriendRequestTimeInterval (1,209,600,000 ms).
	FriendRequestLifetimeMs int64 = 14 * 24 * 60 * 60 * 1000
	// FriendRequestExpirySeconds is that lifetime in the client's wire unit: the store keeps Unix milliseconds, the client's RequestExpiresTimestamp is Unix seconds.
	FriendRequestExpirySeconds int64 = FriendRequestLifetimeMs / 1000
)

// Friend is one friend of a character: the friend's character GUID, its name, level, class and subclass, its guild's name and whether it still counts the character as a friend
// (false: the friend removed the character; the entry stays until the character removes it too).  The online flag and the last online time are not stored.
type Friend struct {
	GUID         string // 32 lowercase hex digits, not all zero
	Name         string
	Level        int32 // 1-25
	ClassID      int32 // ECharacterClass 0-8 (0: a record whose class could not be derived)
	SubArchetype int32 // ECharacterClass 0-8 (0 in the game's lists today)
	GuildName    string
	StillFriend  bool
}

// FriendRequest is one friend request a character has received and not answered: its sender, the recipient and when it expires.  The recipient is the character the row belongs to.
type FriendRequest struct {
	SenderGUID    string // 32 lowercase hex digits, not all zero, and not the recipient
	SenderName    string
	RecipientGUID string // 32 lowercase hex digits, the character the request is for
	ExpiresAtMs   int64  // Unix milliseconds; at or past it the request is not read
}

// Friends is what a character has: its friends and the requests waiting for it, each list in the order it was added.
type Friends struct {
	Friends  []Friend
	Requests []FriendRequest
}

// ExpirySeconds is a pending request's expiry as the client's wire field carries it: the store keeps Unix milliseconds and the wire field is Unix seconds.
func ExpirySeconds(expiresAtMs int64) int64 { return expiresAtMs / 1000 }

// validateFriendName checks one name of a friend or a request: not empty, at most MaxFriendNameLength bytes, valid UTF-8 text, no control character and no space at either end.
func validateFriendName(what string, name string) error {
	switch {
	case name == "":
		return fmt.Errorf("%w: a %s is empty", ErrInvalid, what)
	case len(name) > MaxFriendNameLength:
		return fmt.Errorf("%w: a %s is %d bytes (at most %d)", ErrInvalid, what, len(name), MaxFriendNameLength)
	case !utf8.ValidString(name):
		return fmt.Errorf("%w: a %s is not valid UTF-8 text", ErrInvalid, what)
	case name[0] == ' ' || name[len(name)-1] == ' ':
		return fmt.Errorf("%w: a %s has a space at one end: %q", ErrInvalid, what, name)
	}
	for _, r := range name {
		if r < 0x20 || r == 0x7f {
			return fmt.Errorf("%w: a %s has a control character: %q", ErrInvalid, what, name)
		}
	}
	return nil
}

// validateGuid checks a friend or sender GUID: 32 lowercase hex digits, not all zero.
func validateGuid(what, guid string) error {
	if !guidPattern.MatchString(guid) || guid == strings.Repeat("0", 32) {
		return fmt.Errorf("%w: %s %q is not 32 lowercase hex digits (not all zero)", ErrInvalid, what, guid)
	}
	return nil
}

func validateFriend(i int, f Friend) error {
	at := fmt.Sprintf("friend %d", i)
	if err := validateGuid("its guid", f.GUID); err != nil {
		return fmt.Errorf("%s: %w", at, err)
	}
	if err := validateFriendName("friend name", f.Name); err != nil {
		return fmt.Errorf("%s: %w", at, err)
	}
	if f.Level < 1 || f.Level > MaxLevel {
		return fmt.Errorf("%w: %s has level %d (1 to %d)", ErrInvalid, at, f.Level, MaxLevel)
	}
	if f.ClassID < 0 || f.ClassID > 8 {
		return fmt.Errorf("%w: %s has class %d (0 to 8)", ErrInvalid, at, f.ClassID)
	}
	if f.SubArchetype < 0 || f.SubArchetype > 8 {
		return fmt.Errorf("%w: %s has subclass %d (0 to 8)", ErrInvalid, at, f.SubArchetype)
	}
	if len(f.GuildName) > MaxGuildNameLength || !utf8.ValidString(f.GuildName) || (f.GuildName != "" && (f.GuildName[0] == ' ' || f.GuildName[len(f.GuildName)-1] == ' ')) {
		return fmt.Errorf("%w: %s has a guild name of %d bytes that is not trimmed UTF-8 text (at most %d)", ErrInvalid, at, len(f.GuildName), MaxGuildNameLength)
	}
	for _, r := range f.GuildName {
		if r < 0x20 || r == 0x7f {
			return fmt.Errorf("%w: %s has a control character in its guild name: %q", ErrInvalid, at, f.GuildName)
		}
	}
	return nil
}

func validateFriendRequest(i int, r FriendRequest) error {
	at := fmt.Sprintf("request %d", i)
	if err := validateGuid("its sender guid", r.SenderGUID); err != nil {
		return fmt.Errorf("%s: %w", at, err)
	}
	if err := validateFriendName("sender name", r.SenderName); err != nil {
		return fmt.Errorf("%s: %w", at, err)
	}
	if err := validateGuid("its recipient guid", r.RecipientGUID); err != nil {
		return fmt.Errorf("%s: %w", at, err)
	}
	if r.ExpiresAtMs <= 0 {
		return fmt.Errorf("%w: %s has an expiry of %d (Unix milliseconds, positive)", ErrInvalid, at, r.ExpiresAtMs)
	}
	return nil
}

// ValidateFriends checks both lists: the caps, each friend's and each request's fields, and a friend or a sender twice.  The rules that depend on the character (not itself, the
// recipient being the character) are checked by SaveProgress, which knows it.
func ValidateFriends(f Friends) error {
	if len(f.Friends) > MaxFriends {
		return fmt.Errorf("%w: %d friends (at most %d)", ErrInvalid, len(f.Friends), MaxFriends)
	}
	seen := make(map[string]bool, len(f.Friends))
	for i, friend := range f.Friends {
		if err := validateFriend(i, friend); err != nil {
			return err
		}
		if seen[friend.GUID] {
			return fmt.Errorf("%w: friend %d: %q again", ErrInvalid, i, friend.GUID)
		}
		seen[friend.GUID] = true
	}
	if len(f.Requests) > MaxFriendRequests {
		return fmt.Errorf("%w: %d pending requests (at most %d)", ErrInvalid, len(f.Requests), MaxFriendRequests)
	}
	seen = make(map[string]bool, len(f.Requests))
	for i, request := range f.Requests {
		if err := validateFriendRequest(i, request); err != nil {
			return err
		}
		if seen[request.SenderGUID] {
			return fmt.Errorf("%w: request %d: sender %q again", ErrInvalid, i, request.SenderGUID)
		}
		seen[request.SenderGUID] = true
	}
	return nil
}

// validateFriendsFor checks what depends on the character: it cannot be its own friend, a request is for it and not from it, and no request is older than the store's clock.  The
// caller has checked the names and the caps already.
func validateFriendsFor(characterID string, friends []Friend, requests []FriendRequest) error {
	for i, friend := range friends {
		if friend.GUID == characterID {
			return fmt.Errorf("%w: friend %d is the character itself", ErrInvalid, i)
		}
	}
	for i, request := range requests {
		if request.SenderGUID == characterID {
			return fmt.Errorf("%w: request %d is from the character to itself", ErrInvalid, i)
		}
		if request.RecipientGUID != characterID {
			return fmt.Errorf("%w: request %d is for %q, not the character %q", ErrInvalid, i, request.RecipientGUID, characterID)
		}
	}
	return nil
}

// Friends returns the friends and the pending requests of a character, each list in the order it was added (oldest first).  Requests at or past their expiry are dropped here: they
// are deleted from the table, so the next login does not see them either.  A character with no lists has two empty ones; an unknown character is ErrNotFound.
func (s *Store) Friends(ctx context.Context, characterID string) (Friends, error) {
	if !idPattern.MatchString(characterID) {
		return Friends{}, ErrNotFound
	}
	// One transaction, one snapshot: the expired requests are deleted and both lists are read from the same state, so a save that commits between the
	// two SELECTs cannot give a login a friends/requests pair that never existed together (finding 4).
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead})
	if err != nil {
		return Friends{}, err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	now := time.Now().UnixMilli()
	if _, err := tx.Exec(ctx, `DELETE FROM character_friend_requests WHERE character_id = $1 AND expires_at_ms <= $2`, characterID, now); err != nil {
		return Friends{}, err
	}
	var out Friends
	rows, err := tx.Query(ctx, `SELECT friend_guid, friend_name, level, class_id, sub_archetype, guild_name, still_friend
		FROM character_friends WHERE character_id = $1 ORDER BY position`, characterID)
	if err != nil {
		return Friends{}, err
	}
	for rows.Next() {
		var f Friend
		if err := rows.Scan(&f.GUID, &f.Name, &f.Level, &f.ClassID, &f.SubArchetype, &f.GuildName, &f.StillFriend); err != nil {
			rows.Close()
			return Friends{}, err
		}
		out.Friends = append(out.Friends, f)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return Friends{}, err
	}
	rows, err = tx.Query(ctx, `SELECT sender_guid, sender_name, recipient_guid, expires_at_ms
		FROM character_friend_requests WHERE character_id = $1 ORDER BY position`, characterID)
	if err != nil {
		return Friends{}, err
	}
	for rows.Next() {
		var r FriendRequest
		if err := rows.Scan(&r.SenderGUID, &r.SenderName, &r.RecipientGUID, &r.ExpiresAtMs); err != nil {
			rows.Close()
			return Friends{}, err
		}
		out.Requests = append(out.Requests, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return Friends{}, err
	}
	if len(out.Friends) == 0 && len(out.Requests) == 0 {
		var exists bool
		if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return Friends{}, err
		}
		if !exists {
			return Friends{}, ErrNotFound
		}
	}
	return out, tx.Commit(ctx)
}

// replaceFriends makes the character's friends exactly `friends`, in this order, in tx.
func replaceFriends(ctx context.Context, tx pgx.Tx, characterID string, friends []Friend) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_friends WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	if len(friends) == 0 {
		return nil
	}
	guids := make([]string, len(friends))
	names := make([]string, len(friends))
	levels := make([]int32, len(friends))
	classes := make([]int32, len(friends))
	subs := make([]int32, len(friends))
	guilds := make([]string, len(friends))
	stills := make([]bool, len(friends))
	for i, f := range friends {
		guids[i], names[i], levels[i], classes[i], subs[i], guilds[i], stills[i] = f.GUID, f.Name, f.Level, f.ClassID, f.SubArchetype, f.GuildName, f.StillFriend
	}
	if _, err := tx.Exec(ctx, `INSERT INTO character_friends (character_id, friend_guid, friend_name, level, class_id, sub_archetype, guild_name, still_friend, position)
		SELECT $1, t.guid, t.name, t.level, t.class_id, t.sub_archetype, t.guild, t.still, t.n - 1
		FROM unnest($2::text[], $3::text[], $4::int[], $5::int[], $6::int[], $7::text[], $8::boolean[])
		     WITH ORDINALITY AS t(guid, name, level, class_id, sub_archetype, guild, still, n)`,
		characterID, guids, names, levels, classes, subs, guilds, stills); err != nil {
		return itemsError(err)
	}
	return nil
}

// replaceFriendRequests makes the character's pending requests exactly `requests`, in this order, in tx.
func replaceFriendRequests(ctx context.Context, tx pgx.Tx, characterID string, requests []FriendRequest) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_friend_requests WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	if len(requests) == 0 {
		return nil
	}
	senders := make([]string, len(requests))
	names := make([]string, len(requests))
	recipients := make([]string, len(requests))
	expiries := make([]int64, len(requests))
	for i, r := range requests {
		senders[i], names[i], recipients[i], expiries[i] = r.SenderGUID, r.SenderName, r.RecipientGUID, r.ExpiresAtMs
	}
	if _, err := tx.Exec(ctx, `INSERT INTO character_friend_requests (character_id, sender_guid, sender_name, recipient_guid, expires_at_ms, position)
		SELECT $1, t.sender, t.name, t.recipient, t.expires, t.n - 1
		FROM unnest($2::text[], $3::text[], $4::text[], $5::bigint[])
		     WITH ORDINALITY AS t(sender, name, recipient, expires, n)`,
		characterID, senders, names, recipients, expiries); err != nil {
		return itemsError(err)
	}
	return nil
}

// GetByName returns the character with this name without regard to the case of the letters (the unique index characters_name_lower is case-insensitive as well).  A name no
// character can have, an unknown name and a deleted character are ErrNotFound.
func (s *Store) GetByName(ctx context.Context, name string) (Character, error) {
	if !namePattern.MatchString(name) {
		return Character{}, ErrNotFound // no character has such a name; keep NUL and bad UTF-8 away from PostgreSQL
	}
	c, err := scan(s.pool.QueryRow(ctx, "SELECT "+columns+" FROM characters WHERE lower(name) = lower($1)", name))
	if errors.Is(err, pgx.ErrNoRows) {
		return Character{}, ErrNotFound
	}
	return c, err
}

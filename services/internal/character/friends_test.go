// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package character

// Social, step 7 (migration 0024): a character's friends and the friend requests waiting for it. They are saved with the progress, replaced whole, read back in the order they were added,
// kept apart for each character, and removed with it; a request at or past its expiry is dropped when the lists are read. The rules (the caps of 100, the fields, the self and recipient
// of a request, the expiry) have checks of their own that need no database.

import (
	"context"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// newFriendCharacter creates a Bard with a small kit, as the other social tests do, so that a save has somewhere to write.
func newFriendCharacter(t *testing.T, s *Store, acc int64, name string) Character {
	t.Helper()
	c, err := s.Create(context.Background(), New{AccountID: acc, Name: name, ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

// friendList makes n distinct friends, each with a different level, class and still-friend flag so that a field that slips is seen.
func friendList(n int) []Friend {
	out := make([]Friend, n)
	for i := range out {
		out[i] = Friend{
			GUID:         fmt.Sprintf("%032x", i+1),
			Name:         fmt.Sprintf("Friend%03d", i),
			Level:        int32(1 + i%MaxLevel),
			ClassID:      int32(1 + i%8),
			SubArchetype: int32(i % 9),
			GuildName:    "",
			StillFriend:  i%2 == 0,
		}
	}
	return out
}

// requestList makes n distinct requests for `characterID`, all expiring at `expiresAtMs`.
func requestList(characterID string, n int, expiresAtMs int64) []FriendRequest {
	out := make([]FriendRequest, n)
	for i := range out {
		out[i] = FriendRequest{
			SenderGUID:    fmt.Sprintf("%032x", 0x1000+i),
			SenderName:    fmt.Sprintf("Sender%03d", i),
			RecipientGUID: characterID,
			ExpiresAtMs:   expiresAtMs,
		}
	}
	return out
}

func futureMs() int64 { return time.Now().Add(24 * time.Hour).UnixMilli() }

func TestFriendsAndRequestsAreSavedWithTheProgressAndReadBackInOrder(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newFriendCharacter(t, s, acc, "cyb")
	if got, err := s.Friends(ctx, c.ID); err != nil || len(got.Friends) != 0 || len(got.Requests) != 0 {
		t.Fatalf("a new character has %+v, %v; want no friends and no requests", got, err)
	}
	friends := friendList(3)
	requests := requestList(c.ID, 2, futureMs())
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceFriends: true, Friends: friends, ReplaceFriendRequests: true, FriendRequests: requests}); err != nil {
		t.Fatalf("SaveProgress: %v", err)
	}
	got, err := s.Friends(ctx, c.ID)
	if err != nil || !reflect.DeepEqual(got, Friends{Friends: friends, Requests: requests}) {
		t.Errorf("the lists came back as %+v, %v; want %+v in the order they were given", got, err, Friends{Friends: friends, Requests: requests})
	}
	// a second save replaces both lists whole: the order is the new list's, and a friend or a request that is gone is gone
	second := Friends{Friends: friendList(1), Requests: requestList(c.ID, 1, futureMs())}
	second.Friends[0].Name = "Mirelle"
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceFriends: true, Friends: second.Friends, ReplaceFriendRequests: true, FriendRequests: second.Requests}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.Friends(ctx, c.ID); err != nil || !reflect.DeepEqual(got, second) {
		t.Errorf("after the second save the lists are %+v, %v; want %+v", got, err, second)
	}
	// an empty replacement empties both lists, and no row is left behind
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceFriends: true, ReplaceFriendRequests: true}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.Friends(ctx, c.ID); err != nil || len(got.Friends) != 0 || len(got.Requests) != 0 {
		t.Errorf("after an empty replacement the lists are %+v, %v; want none", got, err)
	}
	for _, table := range []string{"character_friends", "character_friend_requests"} {
		var rows int
		if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM `+table+` WHERE character_id = $1`, c.ID).Scan(&rows); err != nil || rows != 0 {
			t.Errorf("%d rows are left in %s after the empty replacement (%v)", rows, table, err)
		}
	}
}

// sqlTrace records every statement a pool sends, in order, so that a read's transaction can be seen.
type sqlTrace struct {
	mu         sync.Mutex
	statements []string
}

func (t *sqlTrace) TraceQueryStart(ctx context.Context, _ *pgx.Conn, data pgx.TraceQueryStartData) context.Context {
	t.mu.Lock()
	t.statements = append(t.statements, strings.TrimSpace(data.SQL))
	t.mu.Unlock()
	return ctx
}

func (t *sqlTrace) TraceQueryEnd(context.Context, *pgx.Conn, pgx.TraceQueryEndData) {}

// Friends reads the friends and the pending requests in one snapshot: both SELECTs (and the delete of the expired requests) are in one transaction, so a
// save that commits between them cannot give a login a pair of lists that never existed together.
func TestFriendsReadsBothListsInOneSnapshot(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newFriendCharacter(t, s, acc, "cyb")
	friends := friendList(2)
	requests := requestList(c.ID, 1, futureMs())
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceFriends: true, Friends: friends, ReplaceFriendRequests: true, FriendRequests: requests}); err != nil {
		t.Fatal(err)
	}
	// the same schema and search path, with every statement recorded
	cfg := s.pool.Config()
	trace := &sqlTrace{}
	cfg.ConnConfig.Tracer = trace
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	got, err := NewStore(pool).Friends(ctx, c.ID)
	if err != nil || !reflect.DeepEqual(got, Friends{Friends: friends, Requests: requests}) {
		t.Fatalf("the lists came back as %+v, %v; want %+v", got, err, Friends{Friends: friends, Requests: requests})
	}
	trace.mu.Lock()
	statements := append([]string(nil), trace.statements...)
	trace.mu.Unlock()
	begin, commit, friendsAt, requestsAt := -1, -1, -1, -1
	for i, statement := range statements {
		low := strings.ToLower(statement)
		switch {
		case strings.HasPrefix(low, "begin"):
			begin = i
		case strings.HasPrefix(low, "commit"):
			commit = i
		case strings.Contains(low, "select friend_guid"):
			friendsAt = i
		case strings.Contains(low, "select sender_guid"):
			requestsAt = i
		}
	}
	if begin < 0 || commit < 0 || begin > commit || friendsAt < begin || friendsAt > commit || requestsAt < begin || requestsAt > commit {
		t.Errorf("Friends sent %d statements (%q); want the friends and the requests read inside one transaction (begin before both SELECTs, commit after)",
			len(statements), statements)
	}
}

// The largest lists a character can have are saved and read back whole, in order.
func TestTheLargestFriendListsAreSavedWhole(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newFriendCharacter(t, s, acc, "cyb")
	friends := friendList(MaxFriends)
	requests := requestList(c.ID, MaxFriendRequests, futureMs())
	full := Friends{Friends: friends, Requests: requests}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceFriends: true, Friends: full.Friends, ReplaceFriendRequests: true, FriendRequests: full.Requests}); err != nil {
		t.Fatalf("SaveProgress of %d friends and %d requests: %v", len(full.Friends), len(full.Requests), err)
	}
	if got, err := s.Friends(ctx, c.ID); err != nil || !reflect.DeepEqual(got, full) {
		t.Errorf("the lists came back as %d friends and %d requests, %v; want the %d and %d that were saved, in order",
			len(got.Friends), len(got.Requests), err, len(full.Friends), len(full.Requests))
	}
}

// A save that would go over either cap, or holds something the rules refuse, changes nothing: not the lists, and not the rest of the progress it came with.
func TestARefusedSaveOfFriendsChangesNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newFriendCharacter(t, s, acc, "cyb")
	kept := Friends{Friends: friendList(2), Requests: requestList(c.ID, 1, futureMs())}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Level: 5, ReplaceFriends: true, Friends: kept.Friends, ReplaceFriendRequests: true, FriendRequests: kept.Requests}); err != nil {
		t.Fatal(err)
	}
	tooManyFriends := friendList(MaxFriends + 1)
	tooManyRequests := requestList(c.ID, MaxFriendRequests+1, futureMs())
	self := friendList(1)
	self[0].GUID = c.ID
	otherRecipient := requestList(c.ID, 1, futureMs())
	otherRecipient[0].RecipientGUID = "00000000000000000000000000000001"
	selfRequest := requestList(c.ID, 1, futureMs())
	selfRequest[0].SenderGUID = c.ID
	for name, p := range map[string]Progress{
		"101 friends":                     {Level: 9, ReplaceFriends: true, Friends: tooManyFriends},
		"101 requests":                    {Level: 9, ReplaceFriendRequests: true, FriendRequests: tooManyRequests},
		"the character as a friend":       {Level: 9, ReplaceFriends: true, Friends: self},
		"a friend twice":                  {Level: 9, ReplaceFriends: true, Friends: append(friendList(1), friendList(1)...)},
		"a request for another character": {Level: 9, ReplaceFriendRequests: true, FriendRequests: otherRecipient},
		"a request to itself":             {Level: 9, ReplaceFriendRequests: true, FriendRequests: selfRequest},
		"a friend without the flag":       {Level: 9, Friends: friendList(1)},
		"a request without the flag":      {Level: 9, FriendRequests: requestList(c.ID, 1, futureMs())},
	} {
		if _, err := s.SaveProgress(ctx, c.ID, p); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: SaveProgress = %v, want ErrInvalid", name, err)
		}
	}
	if got, err := s.Friends(ctx, c.ID); err != nil || !reflect.DeepEqual(got, kept) {
		t.Errorf("after the refusals the lists are %+v, %v; want %+v", got, err, kept)
	}
	if got, err := s.Get(ctx, c.ID); err != nil || got.Level != 5 {
		t.Errorf("after the refusals the level is %+v, %v; want 5: a refused save writes nothing", got, err)
	}
}

// A request at or past its expiry is dropped when the lists are read, and its row goes with it.
func TestAnExpiredFriendRequestIsDroppedWhenTheListsAreRead(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newFriendCharacter(t, s, acc, "cyb")
	live := time.Now().Add(24 * time.Hour).UnixMilli()
	requests := requestList(c.ID, 3, live)
	requests[1].ExpiresAtMs = time.Now().Add(-time.Hour).UnixMilli()
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceFriendRequests: true, FriendRequests: requests}); err != nil {
		t.Fatal(err)
	}
	got, err := s.Friends(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(got.Requests) != 2 || got.Requests[0].SenderGUID != requests[0].SenderGUID || got.Requests[1].SenderGUID != requests[2].SenderGUID {
		t.Errorf("the requests are %+v, %v; want the two that have not expired, in order", got.Requests, err)
	}
	var rows int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_friend_requests WHERE character_id = $1`, c.ID).Scan(&rows); err != nil || rows != 2 {
		t.Errorf("%d request rows are left (%v), want 2: the expired request is dropped", rows, err)
	}
}

// The name lookup finds a character without regard to the case of the letters and never finds a deleted one.
func TestGetByNameIgnoresTheCaseAndFindsNoDeletedCharacter(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a := newFriendCharacter(t, s, acc, "cyb")
	b := newFriendCharacter(t, s, acc, "dee")
	for name, want := range map[string]string{"cyb": a.ID, "CYB": a.ID, "CyB": a.ID, "Dee": b.ID, "DEE": b.ID} {
		got, err := s.GetByName(ctx, name)
		if err != nil || got.ID != want || got.Name != strings.ToLower(name) {
			t.Errorf("GetByName(%q) = %+v, %v; want %s (its name as stored)", name, got, err, want)
		}
	}
	for name, why := range map[string]string{
		"nobody":                "a name no character has",
		"":                      "an empty name",
		"c":                     "a name shorter than the rules allow",
		"cyb\x00":               "a name with a NUL",
		strings.Repeat("n", 21): "a name longer than the rules allow",
	} {
		if _, err := s.GetByName(ctx, name); !errors.Is(err, ErrNotFound) {
			t.Errorf("%s: GetByName = %v, want ErrNotFound", why, err)
		}
	}
	if err := s.Delete(ctx, acc, a.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := s.GetByName(ctx, "CYB"); !errors.Is(err, ErrNotFound) {
		t.Errorf("a deleted character was found: %v, want ErrNotFound", err)
	}
	if got, err := s.GetByName(ctx, "dee"); err != nil || got.ID != b.ID {
		t.Errorf("the other character after the delete: %+v, %v; want %s", got, err, b.ID)
	}
}

// The lists go with their character; an unknown one has none to read and cannot be saved.
func TestFriendsGoWithTheirCharacterAndAnUnknownOneIsNotFound(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newFriendCharacter(t, s, acc, "cyb")
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceFriends: true, Friends: friendList(2), ReplaceFriendRequests: true, FriendRequests: requestList(c.ID, 1, futureMs())}); err != nil {
		t.Fatal(err)
	}
	if err := s.Delete(ctx, acc, c.ID); err != nil {
		t.Fatal(err)
	}
	for _, table := range []string{"character_friends", "character_friend_requests"} {
		var rows int
		if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM `+table+` WHERE character_id = $1`, c.ID).Scan(&rows); err != nil || rows != 0 {
			t.Errorf("a deleted character keeps %d rows of %s (%v)", rows, table, err)
		}
	}
	for name, id := range map[string]string{"a deleted character": c.ID, "an unknown id": "0123456789abcdef0123456789abcdef", "a malformed id": "nope"} {
		if _, err := s.Friends(ctx, id); !errors.Is(err, ErrNotFound) {
			t.Errorf("%s: Friends = %v, want ErrNotFound", name, err)
		}
		if _, err := s.SaveProgress(ctx, id, Progress{ReplaceFriends: true, Friends: friendList(1)}); !errors.Is(err, ErrNotFound) {
			t.Errorf("%s: SaveProgress = %v, want ErrNotFound", name, err)
		}
	}
}

// Each character has lists of its own.
func TestFriendsOfOneCharacterAreNotAnotherCharacters(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a := newFriendCharacter(t, s, acc, "cyb")
	b := newFriendCharacter(t, s, acc, "dee")
	listA := Friends{Friends: friendList(2), Requests: requestList(a.ID, 1, futureMs())}
	listB := Friends{Friends: friendList(1), Requests: requestList(b.ID, 3, futureMs())}
	for id, list := range map[string]Friends{a.ID: listA, b.ID: listB} {
		if _, err := s.SaveProgress(ctx, id, Progress{ReplaceFriends: true, Friends: list.Friends, ReplaceFriendRequests: true, FriendRequests: list.Requests}); err != nil {
			t.Fatal(err)
		}
	}
	if got, err := s.Friends(ctx, a.ID); err != nil || !reflect.DeepEqual(got, listA) {
		t.Errorf("the first character's lists are %+v, %v; want %+v", got, err, listA)
	}
	if got, err := s.Friends(ctx, b.ID); err != nil || !reflect.DeepEqual(got, listB) {
		t.Errorf("the second character's lists are %+v, %v; want %+v", got, err, listB)
	}
	if _, err := s.SaveProgress(ctx, b.ID, Progress{ReplaceFriends: true, ReplaceFriendRequests: true}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.Friends(ctx, a.ID); err != nil || !reflect.DeepEqual(got, listA) {
		t.Errorf("after the second character emptied its lists the first has %+v, %v; want %+v", got, err, listA)
	}
}

func TestValidateFriends(t *testing.T) {
	friends := friendList(2)
	requests := requestList("bbec5c86eb101fe5f38c87f0fa22e7d0", 2, futureMs())
	longestName := strings.Repeat("n", MaxFriendNameLength)
	longestGuild := strings.Repeat("g", MaxGuildNameLength)
	for _, tc := range []struct {
		name string
		in   Friends
		bad  string // "" accepted, else a part of the reason
	}{
		{"nobody", Friends{}, ""},
		{"two friends and two requests", Friends{Friends: friends, Requests: requests}, ""},
		{"the cap: 100 friends and 100 requests", Friends{Friends: friendList(MaxFriends), Requests: requestList("bbec5c86eb101fe5f38c87f0fa22e7d0", MaxFriendRequests, futureMs())}, ""},
		{"the longest friend name", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: longestName, Level: 1, ClassID: 1}}}, ""},
		{"the longest guild name", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "Brannoc", Level: 1, ClassID: 1, GuildName: longestGuild}}}, ""},
		{"over the cap: 101 friends", Friends{Friends: friendList(MaxFriends + 1)}, "101"},
		{"over the cap: 101 requests", Friends{Requests: requestList("bbec5c86eb101fe5f38c87f0fa22e7d0", MaxFriendRequests+1, futureMs())}, "101"},
		{"the same friend twice", Friends{Friends: []Friend{friends[0], friends[0]}}, "again"},
		{"a friend guid that is not hex", Friends{Friends: []Friend{{GUID: "XYZ", Name: "Brannoc", Level: 1, ClassID: 1}}}, "guid"},
		{"the zero friend guid", Friends{Friends: []Friend{{GUID: strings.Repeat("0", 32), Name: "Brannoc", Level: 1, ClassID: 1}}}, "guid"},
		{"an empty friend name", Friends{Friends: []Friend{{GUID: friends[0].GUID, Level: 1, ClassID: 1}}}, "empty"},
		{"a friend name a byte over the limit", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: longestName + "n", Level: 1, ClassID: 1}}}, "at most"},
		{"a friend name with a space in front", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: " Brannoc", Level: 1, ClassID: 1}}}, "space"},
		{"a friend name with a newline", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "a\nb", Level: 1, ClassID: 1}}}, "control"},
		{"a friend name that is not UTF-8", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "El\xffta", Level: 1, ClassID: 1}}}, "UTF-8"},
		{"a friend level 0", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "Brannoc"}}}, "level"},
		{"a friend level 26", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "Brannoc", Level: MaxLevel + 1, ClassID: 1}}}, "level"},
		{"a friend class -1", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "Brannoc", Level: 1, ClassID: -1}}}, "class"},
		{"a friend class 9", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "Brannoc", Level: 1, ClassID: 9}}}, "class"},
		{"a friend subclass 9", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "Brannoc", Level: 1, ClassID: 1, SubArchetype: 9}}}, "subclass"},
		{"a guild name a byte over the limit", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "Brannoc", Level: 1, ClassID: 1, GuildName: longestGuild + "g"}}}, "guild"},
		{"a guild name with a space behind", Friends{Friends: []Friend{{GUID: friends[0].GUID, Name: "Brannoc", Level: 1, ClassID: 1, GuildName: "Sandal Makers "}}}, "guild"},
		{"a request sender that is not hex", Friends{Requests: []FriendRequest{{SenderGUID: "XYZ", SenderName: "Brannoc", RecipientGUID: requests[0].RecipientGUID, ExpiresAtMs: futureMs()}}}, "guid"},
		{"the zero request sender guid", Friends{Requests: []FriendRequest{{SenderGUID: strings.Repeat("0", 32), SenderName: "Brannoc", RecipientGUID: requests[0].RecipientGUID, ExpiresAtMs: futureMs()}}}, "guid"},
		{"an empty request sender name", Friends{Requests: []FriendRequest{{SenderGUID: requests[0].SenderGUID, RecipientGUID: requests[0].RecipientGUID, ExpiresAtMs: futureMs()}}}, "empty"},
		{"a request recipient that is not hex", Friends{Requests: []FriendRequest{{SenderGUID: requests[0].SenderGUID, SenderName: "Brannoc", RecipientGUID: "XYZ", ExpiresAtMs: futureMs()}}}, "guid"},
		{"a request expiry of 0", Friends{Requests: []FriendRequest{{SenderGUID: requests[0].SenderGUID, SenderName: "Brannoc", RecipientGUID: requests[0].RecipientGUID}}}, "expiry"},
		{"a request expiry before 1970", Friends{Requests: []FriendRequest{{SenderGUID: requests[0].SenderGUID, SenderName: "Brannoc", RecipientGUID: requests[0].RecipientGUID, ExpiresAtMs: -1}}}, "expiry"},
		{"the same sender twice", Friends{Requests: []FriendRequest{requests[0], requests[0]}}, "again"},
	} {
		err := ValidateFriends(tc.in)
		switch {
		case tc.bad == "" && err != nil:
			t.Errorf("%s: refused: %v", tc.name, err)
		case tc.bad != "" && (err == nil || !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), tc.bad)):
			t.Errorf("%s: %v, want ErrInvalid saying %q", tc.name, err, tc.bad)
		}
	}
}

// The progress checks the lists only when it is asked to replace them, and each list under its own flag.
func TestProgressValidatesFriendsOnlyWhenReplacing(t *testing.T) {
	friends := friendList(1)
	requests := requestList("bbec5c86eb101fe5f38c87f0fa22e7d0", 1, futureMs())
	if err := (Progress{Level: 3, ReplaceFriends: true, Friends: friends, ReplaceFriendRequests: true, FriendRequests: requests}).Validate(); err != nil {
		t.Errorf("a progress that replaces both lists: %v", err)
	}
	if err := (Progress{Level: 3, ReplaceFriends: true, ReplaceFriendRequests: true}).Validate(); err != nil {
		t.Errorf("a progress that empties both lists: %v", err)
	}
	if err := (Progress{Level: 3, Friends: friends}).Validate(); !errors.Is(err, ErrInvalid) {
		t.Errorf("friends without replace_friends: %v, want ErrInvalid", err)
	}
	if err := (Progress{Level: 3, FriendRequests: requests}).Validate(); !errors.Is(err, ErrInvalid) {
		t.Errorf("requests without replace_friend_requests: %v, want ErrInvalid", err)
	}
	if err := (Progress{Level: 3, ReplaceFriends: true, Friends: friendList(MaxFriends + 1)}).Validate(); !errors.Is(err, ErrInvalid) {
		t.Errorf("101 friends: %v, want ErrInvalid", err)
	}
}

// The store keeps the expiry in Unix milliseconds and the client's wire field is Unix seconds: 14 days are 1,209,600 s.
func TestFriendRequestExpiryConversion(t *testing.T) {
	if FriendRequestLifetimeMs != 14*24*60*60*1000 {
		t.Errorf("FriendRequestLifetimeMs = %d, want 14 days in milliseconds (1209600000)", FriendRequestLifetimeMs)
	}
	if FriendRequestExpirySeconds != 1209600 {
		t.Errorf("FriendRequestExpirySeconds = %d, want 1209600 (14 days in seconds)", FriendRequestExpirySeconds)
	}
	const now = int64(1771000000000) // a whole second: the wire does not carry the milliseconds
	if got := ExpirySeconds(now + FriendRequestLifetimeMs); got-now/1000 != 1209600 || got-ExpirySeconds(now) != FriendRequestExpirySeconds {
		t.Errorf("a request sent at %d expires on the wire at %d, %d s later; want 1209600 s", now, got, got-now/1000)
	}
	if ExpirySeconds(0) != 0 || ExpirySeconds(1999) != 1 {
		t.Errorf("ExpirySeconds did not cut the milliseconds: %d, %d", ExpirySeconds(0), ExpirySeconds(1999))
	}
}

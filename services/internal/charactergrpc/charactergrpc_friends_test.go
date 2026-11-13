// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package charactergrpc

// Social, step 7 over gRPC: SaveProgress carries a character's friends and pending requests to the store, GetCharacter (with the items) carries them back, GetCharacterByName answers the name
// check and the offline request without regard to the case of the letters, and on the real store the whole path works: saved, read back in order, replaced whole, left alone by a save that
// does not ask, refused with nothing changed, dropped once expired, and not found for a deleted character.

import (
	"context"
	"errors"
	"reflect"
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

// friendingStore is the recording store that also keeps the friends (the part of character.Store that FriendsStore names).
type friendingStore struct {
	*recStore
	friends character.Friends
	err     error
	asked   int
}

var _ FriendsStore = (*friendingStore)(nil)

func (s *friendingStore) Friends(_ context.Context, _ string) (character.Friends, error) {
	s.asked++
	return s.friends, s.err
}

// namingStore is the recording store that also looks a character up by name (the part that NameStore names).
type namingStore struct {
	*recStore
	byName character.Character
	err    error
	got    string
	asked  int
}

var _ NameStore = (*namingStore)(nil)

func (s *namingStore) GetByName(_ context.Context, name string) (character.Character, error) {
	s.asked++
	s.got = name
	return s.byName, s.err
}

func protoFriends() []*characterv1.Friend {
	return []*characterv1.Friend{
		{Guid: "0123456789abcdef0123456789abcdef", Name: "Brannoc", Level: 25, ClassId: 7, SubArchetype: 2, GuildName: "Sandal Makers", StillFriend: true},
		{Guid: "fedcba9876543210fedcba9876543210", Name: "Mirelle", Level: 1, ClassId: 1, SubArchetype: 0, GuildName: "", StillFriend: false},
	}
}

func wantFriends() []character.Friend {
	return []character.Friend{
		{GUID: "0123456789abcdef0123456789abcdef", Name: "Brannoc", Level: 25, ClassID: 7, SubArchetype: 2, GuildName: "Sandal Makers", StillFriend: true},
		{GUID: "fedcba9876543210fedcba9876543210", Name: "Mirelle", Level: 1, ClassID: 1, SubArchetype: 0, GuildName: "", StillFriend: false},
	}
}

func protoRequests() []*characterv1.FriendRequest {
	return []*characterv1.FriendRequest{
		{SenderGuid: "11111111111111111111111111111111", SenderName: "Brannoc", RecipientGuid: "0123456789abcdef0123456789abcdef", ExpiresAtMs: 1772209600000},
		{SenderGuid: "22222222222222222222222222222222", SenderName: "Mirelle", RecipientGuid: "0123456789abcdef0123456789abcdef", ExpiresAtMs: 1772209601000},
	}
}

func wantRequests() []character.FriendRequest {
	return []character.FriendRequest{
		{SenderGUID: "11111111111111111111111111111111", SenderName: "Brannoc", RecipientGUID: "0123456789abcdef0123456789abcdef", ExpiresAtMs: 1772209600000},
		{SenderGUID: "22222222222222222222222222222222", SenderName: "Mirelle", RecipientGUID: "0123456789abcdef0123456789abcdef", ExpiresAtMs: 1772209601000},
	}
}

func TestSaveProgressPassesTheFriendsToTheStore(t *testing.T) {
	st := &recStore{ret: fullCharacter()}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{
		Id: id, ReplaceFriends: true, Friends: protoFriends(), ReplaceFriendRequests: true, FriendRequests: protoRequests(),
	}); err != nil {
		t.Fatalf("SaveProgress = %v", err)
	}
	if !st.gotProgress.ReplaceFriends || !reflect.DeepEqual(st.gotProgress.Friends, wantFriends()) {
		t.Errorf("the store was given replace=%v, %+v; want replace=true, %+v", st.gotProgress.ReplaceFriends, st.gotProgress.Friends, wantFriends())
	}
	if !st.gotProgress.ReplaceFriendRequests || !reflect.DeepEqual(st.gotProgress.FriendRequests, wantRequests()) {
		t.Errorf("the store was given replace=%v, %+v; want replace=true, %+v", st.gotProgress.ReplaceFriendRequests, st.gotProgress.FriendRequests, wantRequests())
	}
	// replace with nothing empties the lists: the flag still reaches the store
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, ReplaceFriends: true, ReplaceFriendRequests: true}); err != nil {
		t.Fatal(err)
	}
	if !st.gotProgress.ReplaceFriends || len(st.gotProgress.Friends) != 0 || !st.gotProgress.ReplaceFriendRequests || len(st.gotProgress.FriendRequests) != 0 {
		t.Errorf("an empty replacement reached the store as %+v", st.gotProgress)
	}
	// a save that does not ask leaves the lists alone: no flag, and no entry
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, Level: 4}); err != nil {
		t.Fatal(err)
	}
	if st.gotProgress.ReplaceFriends || len(st.gotProgress.Friends) != 0 || st.gotProgress.ReplaceFriendRequests || len(st.gotProgress.FriendRequests) != 0 {
		t.Errorf("a save that does not ask reached the store as %+v", st.gotProgress)
	}
	// each list has its own flag: replacing one leaves the other alone
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, ReplaceFriends: true, Friends: protoFriends()}); err != nil {
		t.Fatal(err)
	}
	if !st.gotProgress.ReplaceFriends || st.gotProgress.ReplaceFriendRequests || len(st.gotProgress.FriendRequests) != 0 {
		t.Errorf("a save that replaces the friends alone reached the store as %+v", st.gotProgress)
	}
}

func TestGetCharacterCarriesTheFriendsWithTheItems(t *testing.T) {
	want := character.Friends{Friends: wantFriends(), Requests: wantRequests()}
	st := &friendingStore{recStore: &recStore{ret: fullCharacter()}, friends: want}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	got, err := c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id})
	if err != nil || len(got.GetCharacter().GetFriends()) != 0 || len(got.GetCharacter().GetFriendRequests()) != 0 || st.asked != 0 {
		t.Fatalf("without include_items: %v, %v (the store was asked %d times); want no lists and no question", got, err, st.asked)
	}
	got, err = c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
	if err != nil {
		t.Fatal(err)
	}
	if !sameFriends(got.GetCharacter().GetFriends(), protoFriends()) || !sameRequests(got.GetCharacter().GetFriendRequests(), protoRequests()) {
		t.Errorf("with include_items: friends %v, requests %v; want %v and %v", got.GetCharacter().GetFriends(), got.GetCharacter().GetFriendRequests(), protoFriends(), protoRequests())
	}
	// a store that keeps no lists (an older kind) is read without them, not refused
	plain := dialRec(t, &recStore{ret: fullCharacter()})
	if got, err := plain.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true}); err != nil || len(got.GetCharacter().GetFriends()) != 0 || len(got.GetCharacter().GetFriendRequests()) != 0 {
		t.Errorf("a store without lists: %v, %v; want the character with none", got, err)
	}
	// the store's failures are the caller's codes
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		failing := dialRec(t, &friendingStore{recStore: &recStore{ret: fullCharacter()}, err: tc.err})
		if _, err := failing.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
}

func TestGetCharacterByNamePassesTheNameAndReturnsTheRecord(t *testing.T) {
	want := fullCharacter()
	want.Name = "Zed"
	st := &namingStore{recStore: &recStore{ret: fullCharacter()}, byName: want}
	c := dialRec(t, st)
	got, err := c.GetCharacterByName(context.Background(), &characterv1.GetCharacterByNameRequest{Name: "ZED"})
	if err != nil {
		t.Fatal(err)
	}
	if st.got != "ZED" || st.asked != 1 {
		t.Errorf("the store was asked for %q %d times; want the name as sent, once", st.got, st.asked)
	}
	if got.GetCharacter().GetId() != want.ID || got.GetCharacter().GetName() != "Zed" || got.GetCharacter().GetLevel() != want.Level || got.GetCharacter().GetClassId() != want.ClassID {
		t.Errorf("the reply is %v; want the store's character (%s, Zed, level %d, class %d)", got.GetCharacter(), want.ID, want.Level, want.ClassID)
	}
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		failing := dialRec(t, &namingStore{recStore: &recStore{ret: fullCharacter()}, err: tc.err})
		if _, err := failing.GetCharacterByName(context.Background(), &characterv1.GetCharacterByNameRequest{Name: "Nobody"}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
	// a store without the lookup: Unimplemented, not a wrong answer
	if _, err := dialRec(t, &recStore{ret: fullCharacter()}).GetCharacterByName(context.Background(), &characterv1.GetCharacterByNameRequest{Name: "Zed"}); status.Code(err) != codes.Unimplemented {
		t.Errorf("a store without the by-name lookup: %v, want Unimplemented", err)
	}
}

// the whole path on the real store
func TestFriendsThroughTheServiceOnTheRealStore(t *testing.T) {
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
	other, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: account, Name: "dee", ClassId: 1, ClassName: "Ranger", AppearanceJson: "{}"})
	if err != nil {
		t.Fatal(err)
	}
	read := func() *characterv1.Character {
		t.Helper()
		got, err := c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
		if err != nil {
			t.Fatalf("GetCharacter: %v", err)
		}
		return got.GetCharacter()
	}
	if got := read(); len(got.GetFriends()) != 0 || len(got.GetFriendRequests()) != 0 {
		t.Fatalf("a new character has %d friends and %d requests", len(got.GetFriends()), len(got.GetFriendRequests()))
	}
	friend := &characterv1.Friend{Guid: other.GetCharacter().GetId(), Name: "dee", Level: 1, ClassId: 1, SubArchetype: 0, GuildName: "", StillFriend: true}
	request := &characterv1.FriendRequest{SenderGuid: "33333333333333333333333333333333", SenderName: "Mirelle", RecipientGuid: id, ExpiresAtMs: time.Now().Add(14 * 24 * time.Hour).UnixMilli()}
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, ReplaceFriends: true, Friends: []*characterv1.Friend{friend}, ReplaceFriendRequests: true, FriendRequests: []*characterv1.FriendRequest{request}}); err != nil {
		t.Fatalf("SaveProgress: %v", err)
	}
	if got := read(); !sameFriends(got.GetFriends(), []*characterv1.Friend{friend}) || !sameRequests(got.GetFriendRequests(), []*characterv1.FriendRequest{request}) {
		t.Errorf("the character has friends %v and requests %v; want %v and %v", got.GetFriends(), got.GetFriendRequests(), friend, request)
	}
	// the by-name lookup is case-insensitive and answers with the record and 404s for a deleted character
	if got, err := c.GetCharacterByName(ctx, &characterv1.GetCharacterByNameRequest{Name: "DEE"}); err != nil || got.GetCharacter().GetId() != other.GetCharacter().GetId() || got.GetCharacter().GetLevel() != 1 || got.GetCharacter().GetClassId() != 1 {
		t.Errorf("GetCharacterByName(DEE) = %v, %v; want dee's record", got, err)
	}
	if _, err := c.GetCharacterByName(ctx, &characterv1.GetCharacterByNameRequest{Name: "nobody"}); status.Code(err) != codes.NotFound {
		t.Errorf("GetCharacterByName(nobody) = %v, want NotFound", err)
	}
	if _, err := c.DeleteCharacter(ctx, &characterv1.DeleteCharacterRequest{Id: other.GetCharacter().GetId(), AccountId: account}); err != nil {
		t.Fatal(err)
	}
	if _, err := c.GetCharacterByName(ctx, &characterv1.GetCharacterByNameRequest{Name: "Dee"}); status.Code(err) != codes.NotFound {
		t.Errorf("a deleted character was found: %v, want NotFound", err)
	}
	// the lists of the account's characters and a read without the items do not carry them
	list, err := c.ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: account})
	if err != nil || len(list.GetCharacters()) != 1 || len(list.GetCharacters()[0].GetFriends()) != 0 {
		t.Errorf("ListCharacters = %v, %v", list, err)
	}
	if got, err := c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id}); err != nil || len(got.GetCharacter().GetFriends()) != 0 {
		t.Errorf("GetCharacter without the items = %v, %v", got, err)
	}
	// a save that does not ask leaves them alone
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, Level: 6}); err != nil {
		t.Fatal(err)
	}
	if got := read(); len(got.GetFriends()) != 1 || len(got.GetFriendRequests()) != 1 {
		t.Errorf("after a save of the level the lists are %v and %v; want them as they were", got.GetFriends(), got.GetFriendRequests())
	}
	// refusals: InvalidArgument and nothing changed, a character that is not there is NotFound
	tooMany := make([]*characterv1.Friend, 0, character.MaxFriends+1)
	for i := 0; i < character.MaxFriends+1; i++ {
		tooMany = append(tooMany, &characterv1.Friend{Guid: fmtGUID(i + 1), Name: "Friend", Level: 1, ClassId: 1})
	}
	for name, req := range map[string]*characterv1.SaveProgressRequest{
		"101 friends":              {Id: id, Level: 9, ReplaceFriends: true, Friends: tooMany},
		"the character as friend":  {Id: id, Level: 9, ReplaceFriends: true, Friends: []*characterv1.Friend{{Guid: id, Name: "cyb", Level: 1, ClassId: 2}}},
		"a friend without a flag":  {Id: id, Level: 9, Friends: []*characterv1.Friend{friend}},
		"a request to itself":      {Id: id, Level: 9, ReplaceFriendRequests: true, FriendRequests: []*characterv1.FriendRequest{{SenderGuid: id, SenderName: "cyb", RecipientGuid: id, ExpiresAtMs: 1772209600000}}},
		"a request without a flag": {Id: id, Level: 9, FriendRequests: []*characterv1.FriendRequest{request}},
	} {
		if _, err := c.SaveProgress(ctx, req); status.Code(err) != codes.InvalidArgument {
			t.Errorf("%s: %v, want InvalidArgument", name, err)
		}
	}
	if got := read(); len(got.GetFriends()) != 1 || len(got.GetFriendRequests()) != 1 || got.GetLevel() != 6 {
		t.Errorf("after the refusals: %d friends, %d requests, level %d; want 1, 1 and 6", len(got.GetFriends()), len(got.GetFriendRequests()), got.GetLevel())
	}
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: "ffffffffffffffffffffffffffffffff", ReplaceFriends: true, Friends: []*characterv1.Friend{friend}}); status.Code(err) != codes.NotFound {
		t.Errorf("an unknown character: %v, want NotFound", err)
	}
	// replaced whole, then emptied; the request that has expired is not sent again
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, ReplaceFriends: true, ReplaceFriendRequests: true}); err != nil {
		t.Fatal(err)
	}
	if got := read(); len(got.GetFriends()) != 0 || len(got.GetFriendRequests()) != 0 {
		t.Errorf("after the empty replacement the character has %v and %v", got.GetFriends(), got.GetFriendRequests())
	}
	expired := &characterv1.FriendRequest{SenderGuid: "44444444444444444444444444444444", SenderName: "Tavish", RecipientGuid: id, ExpiresAtMs: time.Now().Add(-time.Hour).UnixMilli()}
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, ReplaceFriendRequests: true, FriendRequests: []*characterv1.FriendRequest{expired}}); err != nil {
		t.Fatal(err)
	}
	if got := read(); len(got.GetFriendRequests()) != 0 {
		t.Errorf("an expired request is still sent: %v", got.GetFriendRequests())
	}
}

// fmtGUID is a 32-digit lower-case hex guid for the nth test record.
func fmtGUID(n int) string {
	const digits = "0123456789abcdef"
	b := make([]byte, 32)
	for i := 31; i >= 0; i-- {
		b[i] = digits[n&0xf]
		n >>= 4
	}
	return string(b)
}

// sameFriends and sameRequests compare proto messages by their fields: a message that came over gRPC holds internal state that reflect.DeepEqual would see.
func sameFriends(got, want []*characterv1.Friend) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if !proto.Equal(got[i], want[i]) {
			return false
		}
	}
	return true
}

func sameRequests(got, want []*characterv1.FriendRequest) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if !proto.Equal(got[i], want[i]) {
			return false
		}
	}
	return true
}

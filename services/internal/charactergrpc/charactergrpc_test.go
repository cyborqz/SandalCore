// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

import (
	"context"
	"errors"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/customization"
)

// acceptAll is a Checker that accepts every character: the transport tests are
// not about the rules (rules_test.go is).
type acceptAll struct{}

func (acceptAll) Check(customization.New) error { return nil }

// fakeStore keeps characters in memory for transport tests; listErr, when
// set, is what List fails with.
type fakeStore struct {
	byID    map[string]character.Character
	listErr error
	items   map[string][]character.Item
	xp      map[string][]character.Experience
	ranks   map[string][]character.TreeRanks // plan D1, stage 2
	bar     map[string][]character.ActionbarSlot
	profs   map[string][]character.Profession // Gathering stage 2
}

func (f *fakeStore) Create(_ context.Context, n character.New) (character.Character, error) {
	if err := character.ValidateName(n.Name); err != nil {
		return character.Character{}, err
	}
	for _, c := range f.byID {
		if c.Name == n.Name {
			return character.Character{}, character.ErrNameTaken
		}
	}
	c := character.Character{ID: "00000000000000000000000000000001", AccountID: n.AccountID, Name: n.Name,
		ArchetypeID: n.ArchetypeID, ClassName: n.ClassName, AppearanceJSON: "{}", Level: 1}
	f.byID[c.ID] = c
	return c, nil
}

func (f *fakeStore) List(_ context.Context, accountID int64) ([]character.Character, error) {
	if f.listErr != nil {
		return nil, f.listErr
	}
	var out []character.Character
	for _, c := range f.byID {
		if c.AccountID == accountID {
			out = append(out, c)
		}
	}
	return out, nil
}

func (f *fakeStore) Get(_ context.Context, id string) (character.Character, error) {
	if c, ok := f.byID[id]; ok {
		return c, nil
	}
	return character.Character{}, character.ErrNotFound
}

// built by C O A with Cyb

func (f *fakeStore) SaveState(_ context.Context, id string, st character.State) (character.Character, error) {
	c, ok := f.byID[id]
	if !ok {
		return character.Character{}, character.ErrNotFound
	}
	c.Position, c.LastRegion = st.Position, st.LastRegion
	f.byID[id] = c
	return c, nil
}

func (f *fakeStore) Delete(_ context.Context, accountID int64, id string) error {
	c, ok := f.byID[id]
	if !ok || c.AccountID != accountID {
		return character.ErrNotFound
	}
	delete(f.byID, id)
	return nil
}

func (f *fakeStore) Inventory(_ context.Context, id string) ([]character.Item, error) {
	if _, ok := f.byID[id]; !ok {
		return nil, character.ErrNotFound
	}
	return f.items[id], nil
}

func (f *fakeStore) SaveInventory(_ context.Context, id string, items []character.Item) error {
	if _, ok := f.byID[id]; !ok {
		return character.ErrNotFound
	}
	if f.items == nil {
		f.items = map[string][]character.Item{}
	}
	f.items[id] = items
	return nil
}

func (f *fakeStore) Experience(_ context.Context, id string) ([]character.Experience, error) {
	if _, ok := f.byID[id]; !ok {
		return nil, character.ErrNotFound
	}
	return f.xp[id], nil
}

func (f *fakeStore) SkillRanks(_ context.Context, id string) ([]character.TreeRanks, error) {
	if _, ok := f.byID[id]; !ok {
		return nil, character.ErrNotFound
	}
	return f.ranks[id], nil
}

func (f *fakeStore) Professions(_ context.Context, id string) ([]character.Profession, error) {
	if _, ok := f.byID[id]; !ok {
		return nil, character.ErrNotFound
	}
	return f.profs[id], nil
}

func (f *fakeStore) ActionBar(_ context.Context, id string) ([]character.ActionbarSlot, error) {
	if _, ok := f.byID[id]; !ok {
		return nil, character.ErrNotFound
	}
	return f.bar[id], nil
}

// (CoA | C-Y-B)

func (f *fakeStore) SaveProgress(_ context.Context, id string, p character.Progress) (character.Character, error) {
	c, ok := f.byID[id]
	if !ok {
		return character.Character{}, character.ErrNotFound
	}
	if err := p.Validate(); err != nil {
		return character.Character{}, err
	}
	if p.Level != 0 {
		c.Level = p.Level
	}
	if p.ClassID != 0 {
		c.ClassID = p.ClassID
	}
	f.byID[id] = c
	if p.ReplaceSkillRanks {
		if f.ranks == nil {
			f.ranks = map[string][]character.TreeRanks{}
		}
		f.ranks[id] = p.SkillRanks
	}
	if p.ReplaceActionBar {
		if f.bar == nil {
			f.bar = map[string][]character.ActionbarSlot{}
		}
		f.bar[id] = p.ActionBar
	}
	if f.xp == nil {
		f.xp = map[string][]character.Experience{}
	}
	for _, e := range p.Experience {
		replaced := false
		for i := range f.xp[id] {
			if f.xp[id][i].LevelingTypeGuid == e.LevelingTypeGuid {
				f.xp[id][i] = e
				replaced = true
			}
		}
		if !replaced {
			f.xp[id] = append(f.xp[id], e)
		}
	}
	return c, nil
}

func client(t *testing.T, store *fakeStore) characterv1.CharacterServiceClient {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	characterv1.RegisterCharacterServiceServer(srv, New(store, acceptAll{}))
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	conn, err := grpc.NewClient("passthrough:///bufnet",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return characterv1.NewCharacterServiceClient(conn)
}

func TestCharacterService(t *testing.T) {
	c := client(t, &fakeStore{byID: map[string]character.Character{}})
	ctx := context.Background()

	resp, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{
		AccountId: 5, Name: "cyb", ClassId: 2, ArchetypeId: 17747, ClassName: "Bard"})
	created := resp.GetCharacter()
	if err != nil || created.GetName() != "cyb" || created.GetAccountId() != 5 || created.GetPosition() != nil {
		t.Fatalf("CreateCharacter: %v %v", resp, err)
	}
	_, err = c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: 5, Name: "cyb", ClassName: "Bard"})
	if status.Code(err) != codes.AlreadyExists {
		t.Errorf("duplicate: %v, want AlreadyExists", err)
	}
	_, err = c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: 5, Name: "x", ClassName: "Bard"})
	if status.Code(err) != codes.InvalidArgument {
		t.Errorf("bad name: %v, want InvalidArgument", err)
	}

	list, err := c.ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: 5})
	if err != nil || len(list.Characters) != 1 {
		t.Fatalf("ListCharacters: %v %v", list, err)
	}

	saved, err := c.SaveCharacterState(ctx, &characterv1.SaveCharacterStateRequest{
		Id: created.GetId(), Position: &characterv1.Position{X: 1, Y: 2, Z: 3, Yaw: 4}, LastRegion: "Riverlands"})
	if err != nil || saved.GetCharacter().GetPosition().GetZ() != 3 || saved.GetCharacter().GetLastRegion() != "Riverlands" {
		t.Fatalf("SaveCharacterState: %v %v", saved, err)
	}

	got, err := c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: created.GetId()})
	if err != nil || got.GetCharacter().GetName() != "cyb" {
		t.Fatalf("GetCharacter: %v %v", got, err)
	}
	_, err = c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: "ffffffffffffffffffffffffffffffff"})
	if status.Code(err) != codes.NotFound {
		t.Errorf("unknown id: %v, want NotFound", err)
	}
}

// A store failure that is not one of the character errors is Internal, and
// its text (SQL, database host) stays in the service log. That includes a
// timeout inside the store while the caller's context is still alive.
func TestStoreFailureIsInternalWithoutDetail(t *testing.T) {
	for _, storeErr := range []error{
		errors.New("pq: secret detail"),
		fmt.Errorf("failed to connect to host=secret detail: %w", context.DeadlineExceeded),
	} {
		c := client(t, &fakeStore{byID: map[string]character.Character{}, listErr: storeErr})
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		_, err := c.ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: 5})
		cancel()
		if status.Code(err) != codes.Internal || strings.Contains(status.Convert(err).Message(), "secret detail") {
			t.Errorf("store error %q: %v, want Internal without the store's text", storeErr, err)
		}
	}
}

// The caller's own cancellation keeps its gRPC code. Over a real connection
// the client gives up first, so this calls the handler directly.
func TestCallerCancellationKeepsItsCode(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	s := New(&fakeStore{byID: map[string]character.Character{}, listErr: fmt.Errorf("query: %w", context.Canceled)}, acceptAll{})
	if _, err := s.ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: 5}); status.Code(err) != codes.Canceled {
		t.Errorf("ListCharacters: %v, want Canceled", err)
	}
}

func TestDeleteCharacter(t *testing.T) {
	store := &fakeStore{byID: map[string]character.Character{}}
	c := client(t, store)
	ctx := context.Background()
	resp, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: 5, Name: "cyb", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	id := resp.GetCharacter().GetId()
	_, err = c.DeleteCharacter(ctx, &characterv1.DeleteCharacterRequest{Id: id, AccountId: 6})
	if status.Code(err) != codes.NotFound {
		t.Errorf("another account: %v, want NotFound", err)
	}
	if _, err := c.DeleteCharacter(ctx, &characterv1.DeleteCharacterRequest{Id: id, AccountId: 5}); err != nil {
		t.Fatalf("DeleteCharacter: %v", err)
	}
	if _, ok := store.byID[id]; ok {
		t.Error("character still stored after DeleteCharacter")
	}
}

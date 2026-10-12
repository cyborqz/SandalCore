// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// Plan B2 over gRPC: a character's items come back with GetCharacter when asked for, SaveInventory replaces them, and a
// new character is created with its class's kit.

import (
	"context"
	"errors"
	"fmt"
	"net"
	"reflect"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

var itemTime = time.UnixMilli(1_790_000_000_123)

func guidOf(n int) string { return fmt.Sprintf("%032x", n) }

// every field of every item has a value of its own, so no two can be swapped unnoticed
func someItems() ([]character.Item, []*characterv1.Item) {
	return []character.Item{
		{Container: character.Backpack, Slot: 3, ItemRecordID: 6064630214426886486, InstanceGUID: guidOf(0xa1), Quantity: 5, Quality: 7, Durability: 11.5, CreatedAt: itemTime},
		{Container: character.Equipment, Slot: 15, ItemRecordID: 6064631131688402948, InstanceGUID: guidOf(0xb2), Quantity: 1, Quality: 51, Durability: 37.5, CreatedAt: itemTime.Add(time.Second)},
		{Container: character.Quest, Slot: 99, ItemRecordID: 9, InstanceGUID: guidOf(0xc3), Quantity: 2, Quality: 200, Durability: 0, CreatedAt: itemTime.Add(2 * time.Second)},
	}, []*characterv1.Item{
		{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 3, ItemRecordId: 6064630214426886486, InstanceGuid: guidOf(0xa1), Quantity: 5, Quality: 7, Durability: 11.5, CreatedAtMs: itemTime.UnixMilli()},
		{Container: characterv1.ItemContainer_ITEM_CONTAINER_EQUIPMENT, Slot: 15, ItemRecordId: 6064631131688402948, InstanceGuid: guidOf(0xb2), Quantity: 1, Quality: 51, Durability: 37.5, CreatedAtMs: itemTime.UnixMilli() + 1000},
		{Container: characterv1.ItemContainer_ITEM_CONTAINER_QUEST, Slot: 99, ItemRecordId: 9, InstanceGuid: guidOf(0xc3), Quantity: 2, Quality: 200, Durability: 0, CreatedAtMs: itemTime.UnixMilli() + 2000},
	}
}

// [C-O-A + C_Y_B]

func TestGetCharacterReturnsItemsOnlyWhenAskedFor(t *testing.T) {
	items, protoItems := someItems()
	// without the flag the store is never asked for the items (it would fail), and none are in the reply
	st := &recStore{ret: fullCharacter(), items: items}
	c := dialRec(t, st)
	got, err := c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef"})
	if err != nil || len(got.GetCharacter().GetItems()) != 0 {
		t.Fatalf("GetCharacter = %v, %v; want no items", got, err)
	}
	wantProtoEqual(t, "the character without items", got.GetCharacter(), fullProto())

	got, err = c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef", IncludeItems: true})
	if err != nil {
		t.Fatal(err)
	}
	want := fullProto()
	want.Items = protoItems
	wantProtoEqual(t, "the character with its items", got.GetCharacter(), want)
	if st.gotID != "0123456789abcdef0123456789abcdef" {
		t.Errorf("the store was asked for %q", st.gotID)
	}

	// a character with none has none
	empty := dialRec(t, &recStore{ret: fullCharacter()})
	got, err = empty.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef", IncludeItems: true})
	if err != nil || len(got.GetCharacter().GetItems()) != 0 {
		t.Errorf("a character with no items: %v, %v", got, err)
	}
	// an unknown character is NotFound whichever call finds out
	if _, err := dialRec(t, &recStore{err: character.ErrNotFound}).GetCharacter(context.Background(),
		&characterv1.GetCharacterRequest{Id: "ffffffffffffffffffffffffffffffff", IncludeItems: true}); status.Code(err) != codes.NotFound {
		t.Errorf("unknown id: %v", err)
	}
}

func TestSaveInventoryPassesEveryFieldToTheStore(t *testing.T) {
	items, protoItems := someItems()
	st := &recStore{}
	c := dialRec(t, st)
	resp, err := c.SaveInventory(context.Background(), &characterv1.SaveInventoryRequest{Id: "0123456789abcdef0123456789abcdef", Items: protoItems})
	if err != nil || resp.GetSaved() != 3 {
		t.Fatalf("SaveInventory = %v, %v", resp, err)
	}
	if st.gotID != "0123456789abcdef0123456789abcdef" || len(st.gotItems) != 3 {
		t.Fatalf("the store got %q and %d items", st.gotID, len(st.gotItems))
	}
	for i, want := range items {
		if got := st.gotItems[i]; got != want {
			t.Errorf("item %d reached the store as %+v, want %+v", i, got, want)
		}
	}
}

func TestSaveInventoryRefusesAContainerThatIsNone(t *testing.T) {
	_, protoItems := someItems()
	for name, container := range map[string]characterv1.ItemContainer{
		"unspecified": characterv1.ItemContainer_ITEM_CONTAINER_UNSPECIFIED, "past the enum": characterv1.ItemContainer(9), "negative": characterv1.ItemContainer(-1)} {
		st := &recStore{}
		bad := proto.Clone(protoItems[1]).(*characterv1.Item)
		bad.Container = container
		_, err := dialRec(t, st).SaveInventory(context.Background(), &characterv1.SaveInventoryRequest{
			Id: "0123456789abcdef0123456789abcdef", Items: []*characterv1.Item{protoItems[0], bad}})
		if status.Code(err) != codes.InvalidArgument || st.gotItems != nil {
			t.Errorf("%s: %v (the store got %v), want InvalidArgument and no call", name, err, st.gotItems)
		}
	}
	// a nil item in the list is an item with every field at its zero: the container is none
	st := &recStore{}
	if _, err := dialRec(t, st).SaveInventory(context.Background(), &characterv1.SaveInventoryRequest{Id: "x", Items: []*characterv1.Item{{}}}); status.Code(err) != codes.InvalidArgument {
		t.Errorf("an empty item: %v", err)
	}
}

// recKits records the class it was asked for and makes items for it.
type recKits struct {
	asked []int32
	err   error
}

func (k *recKits) Kit(class int32) ([]character.Item, error) {
	k.asked = append(k.asked, class)
	if k.err != nil {
		return nil, k.err
	}
	items, _ := someItems()
	return items, nil
}

func dialWith(t *testing.T, st Store, kits Kits) characterv1.CharacterServiceClient {
	t.Helper()
	return dialServer(t, New(st, acceptAll{}, WithKits(kits)))
}

func dialServer(t *testing.T, s *Server) characterv1.CharacterServiceClient {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	characterv1.RegisterCharacterServiceServer(srv, s)
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

func TestCreateCharacterStartsWithTheClassKit(t *testing.T) {
	items, protoItems := someItems()
	st := &recStore{ret: fullCharacter()}
	kits := &recKits{}
	c := dialWith(t, st, kits)
	resp, err := c.CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{AccountId: 41, Name: "Zed", ClassId: 7, AppearanceJson: "{}"})
	if err != nil {
		t.Fatal(err)
	}
	if len(kits.asked) != 1 || kits.asked[0] != 7 {
		t.Errorf("the kit was made for classes %v, want [7]", kits.asked)
	}
	if len(st.gotNew.Items) != 3 || st.gotNew.Items[1] != items[1] {
		t.Errorf("the store was given %+v, want the kit", st.gotNew.Items)
	}
	if got := resp.GetCharacter().GetItems(); len(got) != 3 || !proto.Equal(got[1], protoItems[1]) {
		t.Errorf("the reply carries %v, want the kit it made", got)
	}
	// a character the store refuses (a taken name) is a kit nobody gets, and the error is the store's
	st2 := &recStore{err: character.ErrNameTaken}
	if _, err := dialWith(t, st2, &recKits{}).CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{AccountId: 1, Name: "Zed", ClassId: 2}); status.Code(err) != codes.AlreadyExists {
		t.Errorf("a taken name: %v", err)
	}
	// no kit maker: a character with no items, as before plan B2
	st3 := &recStore{ret: fullCharacter()}
	resp, err = dialRec(t, st3).CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{AccountId: 1, Name: "Zed", ClassId: 2})
	if err != nil || len(st3.gotNew.Items) != 0 || len(resp.GetCharacter().GetItems()) != 0 {
		t.Errorf("without kits: %v, %v, %+v", resp, err, st3.gotNew.Items)
	}
}

// C Y B with CoA

func TestCreateCharacterIsRefusedWhenItsKitCannotBeMade(t *testing.T) {
	st := &recStore{ret: fullCharacter()}
	c := dialWith(t, st, &recKits{err: errors.New("class 2 has no starting kit")})
	_, err := c.CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{AccountId: 41, Name: "Zed", ClassId: 2})
	if status.Code(err) != codes.Internal || st.gotNew.Name != "" {
		t.Errorf("CreateCharacter = %v (the store was given %q), want Internal and no character stored", err, st.gotNew.Name)
	}
}

// the whole path on the real store: kit, load, save, refusals
func TestItemsThroughTheServiceOnTheRealStore(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var account int64
	if err := pool.QueryRow(ctx, `INSERT INTO accounts (username, password_hash) VALUES ('Owner', 'x') RETURNING id`).Scan(&account); err != nil {
		t.Fatal(err)
	}
	_, protoItems := someItems()
	c := dialWith(t, character.NewStore(pool), &recKits{})
	created, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: account, Name: "cyb", ClassId: 2, ClassName: "Bard", AppearanceJson: "{}"})
	if err != nil {
		t.Fatal(err)
	}
	id := created.GetCharacter().GetId()
	if len(created.GetCharacter().GetItems()) != 3 {
		t.Errorf("the create reply carries %d items", len(created.GetCharacter().GetItems()))
	}
	got, err := c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
	if err != nil || len(got.GetCharacter().GetItems()) != 3 {
		t.Fatalf("GetCharacter = %v, %v", got, err)
	}
	for i, want := range protoItems {
		if !proto.Equal(got.GetCharacter().GetItems()[i], want) {
			t.Errorf("item %d came back as %v, want %v", i, got.GetCharacter().GetItems()[i], want)
		}
	}
	// the list does not carry items
	list, err := c.ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: account})
	if err != nil || len(list.GetCharacters()) != 1 || len(list.GetCharacters()[0].GetItems()) != 0 {
		t.Errorf("ListCharacters = %v, %v", list, err)
	}

	// the chest goes from the bag to the Equipment: a save that replaces the set
	moved := []*characterv1.Item{proto.Clone(protoItems[0]).(*characterv1.Item)}
	moved[0].Container, moved[0].Slot = characterv1.ItemContainer_ITEM_CONTAINER_EQUIPMENT, 2
	if resp, err := c.SaveInventory(ctx, &characterv1.SaveInventoryRequest{Id: id, Items: moved}); err != nil || resp.GetSaved() != 1 {
		t.Fatalf("SaveInventory = %v, %v", resp, err)
	}
	got, _ = c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
	if items := got.GetCharacter().GetItems(); len(items) != 1 || !proto.Equal(items[0], moved[0]) {
		t.Errorf("after the save: %v", items)
	}

	// refusals: InvalidArgument and nothing changed; an unknown character is NotFound
	bad := proto.Clone(moved[0]).(*characterv1.Item)
	bad.Slot = 83
	for name, req := range map[string]*characterv1.SaveInventoryRequest{
		"no items":            {Id: id},
		"a slot out of range": {Id: id, Items: []*characterv1.Item{bad}},
		"two in a slot":       {Id: id, Items: []*characterv1.Item{moved[0], moved[0]}},
	} {
		if _, err := c.SaveInventory(ctx, req); status.Code(err) != codes.InvalidArgument {
			t.Errorf("%s: %v, want InvalidArgument", name, err)
		}
	}
	if _, err := c.SaveInventory(ctx, &characterv1.SaveInventoryRequest{Id: "ffffffffffffffffffffffffffffffff", Items: moved}); status.Code(err) != codes.NotFound {
		t.Errorf("an unknown character: %v, want NotFound", err)
	}
	got, _ = c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
	if items := got.GetCharacter().GetItems(); len(items) != 1 || !proto.Equal(items[0], moved[0]) {
		t.Errorf("the refusals changed the items: %v", items)
	}
}

// (C-O-A : C | Y | B)

// The mounts milestone (migration 0007): a mount item's persisted Health, Mana and lock time cross the boundary both ways, and an item that keeps none
// stays as it was (nil, nil and 0).
func TestMountItemStateCrossesTheGrpcBoundary(t *testing.T) {
	health, mana, dead := float32(3572), float32(1), float32(0)
	items := []character.Item{
		{Container: character.Backpack, Slot: 5, ItemRecordID: 6064631121016127508, InstanceGUID: guidOf(0xd4), Quantity: 1, Quality: 51, CreatedAt: itemTime,
			PersistedHealth: &health, PersistedMana: &mana, LockExpiration: 1770131735},
		{Container: character.Backpack, Slot: 6, ItemRecordID: 6064634325298708480, InstanceGUID: guidOf(0xd5), Quantity: 1, Quality: 51, CreatedAt: itemTime,
			PersistedHealth: &dead, PersistedMana: &mana, LockExpiration: 1790003600},
		{Container: character.Backpack, Slot: 7, ItemRecordID: 7001, InstanceGUID: guidOf(0xd6), Quantity: 1, Quality: 1, CreatedAt: itemTime},
	}
	protoItems := []*characterv1.Item{
		{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 5, ItemRecordId: 6064631121016127508, InstanceGuid: guidOf(0xd4), Quantity: 1, Quality: 51, CreatedAtMs: itemTime.UnixMilli(),
			PersistedHealth: proto.Float32(3572), PersistedMana: proto.Float32(1), LockExpiration: 1770131735},
		{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 6, ItemRecordId: 6064634325298708480, InstanceGuid: guidOf(0xd5), Quantity: 1, Quality: 51, CreatedAtMs: itemTime.UnixMilli(),
			PersistedHealth: proto.Float32(0), PersistedMana: proto.Float32(1), LockExpiration: 1790003600},
		{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 7, ItemRecordId: 7001, InstanceGuid: guidOf(0xd6), Quantity: 1, Quality: 1, CreatedAtMs: itemTime.UnixMilli()},
	}
	// into the store: a persisted Health of 0 is a Health (a dead mount), not an absent one
	st := &recStore{}
	if _, err := dialRec(t, st).SaveInventory(context.Background(), &characterv1.SaveInventoryRequest{Id: "0123456789abcdef0123456789abcdef", Items: protoItems}); err != nil {
		t.Fatal(err)
	}
	if len(st.gotItems) != 3 || !reflect.DeepEqual(st.gotItems, items) {
		t.Errorf("the store got %+v, want %+v", st.gotItems, items)
	}
	if st.gotItems[1].PersistedHealth == nil || *st.gotItems[1].PersistedHealth != 0 || st.gotItems[2].PersistedHealth != nil || st.gotItems[2].PersistedMana != nil {
		t.Errorf("a dead mount's Health of 0 and an item that keeps none: %+v", st.gotItems)
	}
	// out of the store
	got, err := dialRec(t, &recStore{ret: fullCharacter(), items: items}).GetCharacter(context.Background(),
		&characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef", IncludeItems: true})
	if err != nil {
		t.Fatal(err)
	}
	want := fullProto()
	want.Items = protoItems
	wantProtoEqual(t, "the character with its mounts", got.GetCharacter(), want)
	if got.GetCharacter().GetItems()[1].PersistedHealth == nil || got.GetCharacter().GetItems()[2].PersistedHealth != nil {
		t.Error("the dead mount's Health of 0 is present, the plain item's absent")
	}
}

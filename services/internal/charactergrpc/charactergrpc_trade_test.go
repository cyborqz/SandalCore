// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// Player trading (migration 0032) over gRPC: CommitPlayerTrade forwards both character snapshots
// and transfer counts to TradeStore, maps errors to gRPC codes, and works end-to-end against PostgreSQL.

import (
	"context"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

type tradeFake struct {
	*recStore
	req   character.CommitTradeRequest
	ret   character.CommitTradeResult
	err   error
	asked int
}

var _ TradeStore = (*tradeFake)(nil)

func (f *tradeFake) CommitPlayerTrade(_ context.Context, r character.CommitTradeRequest) (character.CommitTradeResult, error) {
	f.asked++
	f.req = r
	return f.ret, f.err
}

func TestCommitPlayerTradeUnimplementedAndMissingSides(t *testing.T) {
	// Store without TradeStore -> Unimplemented
	cPlain := dialRec(t, &recStore{ret: fullCharacter()})
	if _, err := cPlain.CommitPlayerTrade(context.Background(), &characterv1.CommitPlayerTradeRequest{
		TradeId: fmtGUID(0x11), SideA: &characterv1.SaveInventoryRequest{Id: fmtGUID(0x1)}, SideB: &characterv1.SaveInventoryRequest{Id: fmtGUID(0x2)},
		CopperAToB: 10, CommittedAtMs: 1_780_000_000_000,
	}); status.Code(err) != codes.Unimplemented {
		t.Fatalf("without TradeStore err = %v, want Unimplemented", err)
	}

	// Missing side -> InvalidArgument
	f := &tradeFake{recStore: &recStore{ret: fullCharacter()}}
	c := dialRec(t, f)
	if _, err := c.CommitPlayerTrade(context.Background(), &characterv1.CommitPlayerTradeRequest{
		TradeId: fmtGUID(0x11), SideA: &characterv1.SaveInventoryRequest{Id: fmtGUID(0x1)}, CopperAToB: 10, CommittedAtMs: 1_780_000_000_000,
	}); status.Code(err) != codes.InvalidArgument {
		t.Fatalf("missing side_b err = %v, want InvalidArgument", err)
	}

	// ErrTradeCommitted -> FailedPrecondition
	f.err = character.ErrTradeCommitted
	if _, err := c.CommitPlayerTrade(context.Background(), &characterv1.CommitPlayerTradeRequest{
		TradeId: fmtGUID(0x11), SideA: &characterv1.SaveInventoryRequest{Id: fmtGUID(0x1)}, SideB: &characterv1.SaveInventoryRequest{Id: fmtGUID(0x2)},
		CopperAToB: 10, CommittedAtMs: 1_780_000_000_000,
	}); status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("ErrTradeCommitted code = %v, want FailedPrecondition", err)
	}
}

func TestCommitPlayerTradeThroughTheServiceOnTheRealStore(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var accountID int64
	if err := pool.QueryRow(ctx, `INSERT INTO accounts (username, password_hash) VALUES ('TradeOwner', 'x') RETURNING id`).Scan(&accountID); err != nil {
		t.Fatal(err)
	}
	store := character.NewStore(pool)
	c := dialWith(t, store, &recKits{})

	makeChar := func(name string) string {
		created, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: accountID, Name: name, ClassId: 2, ClassName: "Bard", AppearanceJson: "{}"})
		if err != nil {
			t.Fatal(err)
		}
		return created.GetCharacter().GetId()
	}
	alice, bob, carol := makeChar("trgrpca"), makeChar("trgrpcb"), makeChar("trgrpcc")

	itemA := &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 0, ItemRecordId: 7010, InstanceGuid: fmtGUID(0x8101), Quantity: 5, Quality: 2, Durability: 15, CreatedAtMs: 1_780_000_000_000}
	itemB := &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 0, ItemRecordId: 7020, InstanceGuid: fmtGUID(0x8201), Quantity: 3, Quality: 4, Durability: 20, CreatedAtMs: 1_780_000_000_000}
	whA := &characterv1.WarehouseItem{Area: character.WarehouseItems, NodeGuid: 0, ContainerGuid: 9001, Slot: 0, ItemRecordId: 7030, InstanceGuid: fmtGUID(0x8102), Quantity: 1, Quality: 1, Durability: 10, CreatedAtMs: 1_780_000_000_000}

	req := &characterv1.CommitPlayerTradeRequest{
		TradeId: fmtGUID(0x9a01),
		SideA: &characterv1.SaveInventoryRequest{
			Id: alice, Items: []*characterv1.Item{itemB},
			ReplaceWallet: true, Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 4500}},
			ReplaceWarehouse: true, WarehouseItems: []*characterv1.WarehouseItem{whA},
		},
		SideB: &characterv1.SaveInventoryRequest{
			Id: bob, Items: []*characterv1.Item{itemA},
			ReplaceWallet: true, Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 1500}},
			ReplaceWarehouse: true,
		},
		CopperAToB:    500,
		ItemsAToB:     1,
		ItemsBToA:     1,
		CommittedAtMs: 1_780_000_010_000,
	}
	resp, err := c.CommitPlayerTrade(ctx, req)
	if err != nil || resp.GetReplayed() {
		t.Fatalf("CommitPlayerTrade = %v, %v; want replayed=false", resp, err)
	}

	// Idempotent replay -> Replayed=true
	again, err := c.CommitPlayerTrade(ctx, req)
	if err != nil || !again.GetReplayed() {
		t.Fatalf("replay CommitPlayerTrade = %v, %v; want replayed=true", again, err)
	}

	// Same trade_id for a different pair -> FailedPrecondition
	conflict := &characterv1.CommitPlayerTradeRequest{
		TradeId:       fmtGUID(0x9a01),
		SideA:         &characterv1.SaveInventoryRequest{Id: alice, Items: []*characterv1.Item{itemB}, ReplaceWallet: true, Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 10}}},
		SideB:         &characterv1.SaveInventoryRequest{Id: carol, Items: nil, ReplaceWallet: true, Wallet: []*characterv1.WalletEntry{{CurrencyGuid: character.CopperCurrency, Amount: 10}}},
		CopperAToB:    10,
		CommittedAtMs: 1_780_000_011_000,
	}
	if _, err := c.CommitPlayerTrade(ctx, conflict); status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("conflict CommitPlayerTrade err = %v, want FailedPrecondition", err)
	}

	// Cross-character duplicate InstanceGUID -> InvalidArgument
	dup := &characterv1.CommitPlayerTradeRequest{
		TradeId:       fmtGUID(0x9a02),
		SideA:         &characterv1.SaveInventoryRequest{Id: alice, Items: []*characterv1.Item{itemB}, ReplaceWarehouse: true, WarehouseItems: []*characterv1.WarehouseItem{whA}},
		SideB:         &characterv1.SaveInventoryRequest{Id: bob, Items: []*characterv1.Item{itemA, itemB}, ReplaceWarehouse: true},
		ItemsAToB:     1,
		CommittedAtMs: 1_780_000_012_000,
	}
	if _, err := c.CommitPlayerTrade(ctx, dup); status.Code(err) != codes.InvalidArgument {
		t.Fatalf("duplicate guid CommitPlayerTrade err = %v, want InvalidArgument", err)
	}

	// Unknown character -> NotFound
	unknown := &characterv1.CommitPlayerTradeRequest{
		TradeId:       fmtGUID(0x9a03),
		SideA:         &characterv1.SaveInventoryRequest{Id: alice, Items: []*characterv1.Item{itemB}},
		SideB:         &characterv1.SaveInventoryRequest{Id: "ffffffffffffffffffffffffffffffff", Items: nil},
		CopperAToB:    5,
		CommittedAtMs: 1_780_000_013_000,
	}
	if _, err := c.CommitPlayerTrade(ctx, unknown); status.Code(err) != codes.NotFound {
		t.Fatalf("unknown character CommitPlayerTrade err = %v, want NotFound", err)
	}

	// Verify persisted state of Alice and Bob.
	gotA, err := store.Inventory(ctx, alice)
	if err != nil || len(gotA) != 1 || gotA[0].InstanceGUID != itemB.GetInstanceGuid() {
		t.Fatalf("alice inventory = %+v (%v)", gotA, err)
	}
	gotB, err := store.Inventory(ctx, bob)
	if err != nil || len(gotB) != 1 || gotB[0].InstanceGUID != itemA.GetInstanceGuid() {
		t.Fatalf("bob inventory = %+v (%v)", gotB, err)
	}
}

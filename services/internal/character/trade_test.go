// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Player trading (migration 0032): atomic two-character inventory, wallet, jobs, recipes and
// warehouse swaps in one transaction against live PostgreSQL.

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"
)

func TestCommitPlayerTradeAtomicSwapAndEmptyBagGift(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()

	alice := newMailCharacter(t, s, acc, "tradealice")
	bob := newMailCharacter(t, s, acc, "tradebob")

	// Seed Alice with 1 backpack item + 1 resource item + 1 warehouse item + 1 job + 1 recipe + 5000 copper.
	// Seed Bob with 1 backpack item + 1000 copper.
	now := time.UnixMilli(1_771_299_474_000).UTC()
	aliceItem := Item{Container: Backpack, Slot: 0, ItemRecordID: 7010, InstanceGUID: guid(0x4101), Quantity: 5, Quality: 3, Durability: 88.5, CreatedAt: now}
	aliceRes := Item{Container: Resource0, Slot: 0, ItemRecordID: 7020, InstanceGUID: guid(0x4102), Quantity: 10, Quality: 2, Durability: 10, CreatedAt: now}
	aliceWh := WarehouseItem{Area: WarehouseItems, NodeGUID: 0, ContainerGUID: 9001, Slot: 0, Item: Item{ItemRecordID: 7030, InstanceGUID: guid(0x4103), Quantity: 1, Quality: 1, Durability: 10, CreatedAt: now}}
	aliceJob := ProcessingJob{JobGUID: guid(0x4104), StationKey: "0000000000000001", RecipeGUID: 5001, Quantity: 1, QueuedAt: now, StartedAt: now, DurationMs: 5000, ClientTxn: guid(0x4105)}
	bobItem := Item{Container: Backpack, Slot: 0, ItemRecordID: 7040, InstanceGUID: guid(0x4201), Quantity: 1, Quality: 5, Durability: 42.25, CreatedAt: now}
	hp := float32(750.0)
	mp := float32(120.0)
	bobItem.PersistedHealth = &hp
	bobItem.PersistedMana = &mp
	bobItem.LockExpiration = 1_800_000_000

	if err := s.SaveInventoryEverything(ctx, alice.ID, []Item{aliceItem, aliceRes},
		[]WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 5000}}, true,
		[]ProcessingJob{aliceJob}, true,
		[]int64{8001}, true,
		[]WarehouseItem{aliceWh}, true); err != nil {
		t.Fatalf("seed alice: %v", err)
	}
	if err := s.SaveInventoryAndWallet(ctx, bob.ID, []Item{bobItem},
		[]WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1000}}); err != nil {
		t.Fatalf("seed bob: %v", err)
	}

	// Swap: Alice sends aliceItem + aliceRes + 1200 copper to Bob; Bob sends bobItem + 200 copper to Alice.
	bobAfterA := aliceItem
	bobAfterA.Slot = 0
	bobAfterRes := aliceRes
	bobAfterRes.Slot = 0
	aliceAfterB := bobItem
	aliceAfterB.Slot = 0

	res, err := s.CommitPlayerTrade(ctx, CommitTradeRequest{
		TradeID: guid(0xaa01),
		SideA: TradeSide{
			ID: alice.ID,
			Bags: ClaimBags{
				Items:            []Item{aliceAfterB},
				Wallet:           []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 4000}},
				ReplaceWallet:    true,
				Jobs:             []ProcessingJob{aliceJob},
				ReplaceJobs:      true,
				KnownRecipes:     []int64{8001, 8002},
				ReplaceKnown:     true,
				Warehouse:        []WarehouseItem{aliceWh},
				ReplaceWarehouse: true,
			},
		},
		SideB: TradeSide{
			ID: bob.ID,
			Bags: ClaimBags{
				Items:            []Item{bobAfterA, bobAfterRes},
				Wallet:           []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 2000}},
				ReplaceWallet:    true,
				Jobs:             nil,
				ReplaceJobs:      true,
				KnownRecipes:     nil,
				ReplaceKnown:     true,
				Warehouse:        nil,
				ReplaceWarehouse: true,
			},
		},
		CopperAToB:    1200,
		CopperBToA:    200,
		ItemsAToB:     2,
		ItemsBToA:     1,
		CommittedAtMs: 1_771_300_000_000,
	})
	if err != nil {
		t.Fatalf("CommitPlayerTrade: %v", err)
	}
	if res.Replayed {
		t.Fatal("first commit returned Replayed=true")
	}

	// Verify Alice's persisted state (including mount metadata, wallet, jobs, recipes, warehouse).
	gotAliceItems, err := s.Inventory(ctx, alice.ID)
	if err != nil || len(gotAliceItems) != 1 || gotAliceItems[0].InstanceGUID != bobItem.InstanceGUID ||
		gotAliceItems[0].Durability != 42.25 || gotAliceItems[0].PersistedHealth == nil || *gotAliceItems[0].PersistedHealth != 750.0 ||
		gotAliceItems[0].PersistedMana == nil || *gotAliceItems[0].PersistedMana != 120.0 || gotAliceItems[0].LockExpiration != 1_800_000_000 {
		t.Fatalf("alice inventory = %+v (%v)", gotAliceItems, err)
	}
	if w := walletOf(t, s, alice); w != 4000 {
		t.Fatalf("alice wallet = %d, want 4000", w)
	}
	if recipes, err := s.KnownRecipes(ctx, alice.ID); err != nil || len(recipes) != 2 {
		t.Fatalf("alice recipes = %v (%v)", recipes, err)
	}
	if jobs, err := s.ProcessingJobs(ctx, alice.ID); err != nil || len(jobs) != 1 || jobs[0].JobGUID != aliceJob.JobGUID {
		t.Fatalf("alice jobs = %v (%v)", jobs, err)
	}
	if wh, err := s.WarehouseItems(ctx, alice.ID); err != nil || len(wh) != 1 || wh[0].InstanceGUID != aliceWh.InstanceGUID {
		t.Fatalf("alice warehouse = %v (%v)", wh, err)
	}

	// Verify Bob's persisted state.
	gotBobItems, err := s.Inventory(ctx, bob.ID)
	if err != nil || len(gotBobItems) != 2 {
		t.Fatalf("bob inventory = %+v (%v)", gotBobItems, err)
	}
	if w := walletOf(t, s, bob); w != 2000 {
		t.Fatalf("bob wallet = %d, want 2000", w)
	}

	// Verify character_trades ledger row.
	var ca, cb string
	var cAToB, cBToA int64
	var iAToB, iBToA int32
	if err := s.pool.QueryRow(ctx, `SELECT char_a_id, char_b_id, copper_a_to_b, copper_b_to_a, items_a_to_b, items_b_to_a FROM character_trades WHERE id = $1`, guid(0xaa01)).
		Scan(&ca, &cb, &cAToB, &cBToA, &iAToB, &iBToA); err != nil {
		t.Fatalf("character_trades row missing: %v", err)
	}
	if ca != alice.ID || cb != bob.ID || cAToB != 1200 || cBToA != 200 || iAToB != 2 || iBToA != 1 {
		t.Fatalf("ledger mismatch: %s %s %d %d %d %d", ca, cb, cAToB, cBToA, iAToB, iBToA)
	}

	// One-sided gift of Alice's very last item leaving Alice with 0 items!
	giftedToBob := aliceAfterB
	giftedToBob.Slot = 5
	resGift, err := s.CommitPlayerTrade(ctx, CommitTradeRequest{
		TradeID: guid(0xaa02),
		SideA: TradeSide{
			ID: alice.ID,
			Bags: ClaimBags{
				Items:            nil, // 0 items post-trade!
				Wallet:           []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 4000}},
				ReplaceWallet:    true,
				Warehouse:        []WarehouseItem{aliceWh},
				ReplaceWarehouse: true,
			},
		},
		SideB: TradeSide{
			ID: bob.ID,
			Bags: ClaimBags{
				Items:            []Item{bobAfterA, bobAfterRes, giftedToBob},
				Wallet:           []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 2000}},
				ReplaceWallet:    true,
				Warehouse:        nil,
				ReplaceWarehouse: true,
			},
		},
		ItemsAToB:     1,
		CommittedAtMs: 1_771_300_005_000,
	})
	if err != nil || resGift.Replayed {
		t.Fatalf("last-item gift commit = %+v, %v", resGift, err)
	}
	if gotEmpty, err := s.Inventory(ctx, alice.ID); err != nil || len(gotEmpty) != 0 {
		t.Fatalf("alice inventory after gifting last item = %+v (%v), want empty", gotEmpty, err)
	}
	if gotBob3, err := s.Inventory(ctx, bob.ID); err != nil || len(gotBob3) != 3 {
		t.Fatalf("bob inventory after gift = %+v (%v), want 3 items", gotBob3, err)
	}
}

func TestCommitPlayerTradeRollbackAndDuplicateGUIDRejection(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()

	alice := newMailCharacter(t, s, acc, "rollbacka")
	bob := newMailCharacter(t, s, acc, "rollbackb")

	now := time.UnixMilli(1_771_299_474_000).UTC()
	aItem := Item{Container: Backpack, Slot: 0, ItemRecordID: 7010, InstanceGUID: guid(0x5101), Quantity: 2, Quality: 1, Durability: 10, CreatedAt: now}
	bItem := Item{Container: Backpack, Slot: 0, ItemRecordID: 7020, InstanceGUID: guid(0x5201), Quantity: 2, Quality: 1, Durability: 10, CreatedAt: now}
	bWh := WarehouseItem{Area: WarehouseItems, NodeGUID: 0, ContainerGUID: 9001, Slot: 0, Item: Item{ItemRecordID: 7030, InstanceGUID: guid(0x5202), Quantity: 1, Quality: 1, Durability: 10, CreatedAt: now}}

	if err := s.SaveInventoryAndWallet(ctx, alice.ID, []Item{aItem}, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 500}}); err != nil {
		t.Fatal(err)
	}
	if err := s.SaveInventoryEverything(ctx, bob.ID, []Item{bItem}, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 500}}, true, nil, false, nil, false, []WarehouseItem{bWh}, true); err != nil {
		t.Fatal(err)
	}

	// 1. Invalid item on side B must fail and leave side A untouched.
	badItem := bItem
	badItem.Quantity = 0
	_, err := s.CommitPlayerTrade(ctx, CommitTradeRequest{
		TradeID:       guid(0xab01),
		SideA:         TradeSide{ID: alice.ID, Bags: ClaimBags{Items: []Item{bItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 999}}, ReplaceWallet: true}},
		SideB:         TradeSide{ID: bob.ID, Bags: ClaimBags{Items: []Item{badItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1}}, ReplaceWallet: true}},
		CopperBToA:    499,
		CommittedAtMs: 1_771_300_010_000,
	})
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("invalid side B err = %v, want ErrInvalid", err)
	}
	if w := walletOf(t, s, alice); w != 500 {
		t.Fatalf("alice wallet mutated on rollback: %d", w)
	}

	// 2. Duplicate InstanceGUID across side A and side B bags rejected with ErrInvalid, zero DB change.
	dupOnB := aItem
	dupOnB.Slot = 1
	_, err = s.CommitPlayerTrade(ctx, CommitTradeRequest{
		TradeID:       guid(0xab02),
		SideA:         TradeSide{ID: alice.ID, Bags: ClaimBags{Items: []Item{aItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 400}}, ReplaceWallet: true}},
		SideB:         TradeSide{ID: bob.ID, Bags: ClaimBags{Items: []Item{bItem, dupOnB}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 600}}, ReplaceWallet: true}},
		CopperAToB:    100,
		CommittedAtMs: 1_771_300_011_000,
	})
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("cross-character duplicate bag guid err = %v, want ErrInvalid", err)
	}

	// 3. Duplicate InstanceGUID between side A bag and side B warehouse (both replace_warehouse=true and false).
	dupWhInA := Item{Container: Backpack, Slot: 2, ItemRecordID: 7030, InstanceGUID: bWh.InstanceGUID, Quantity: 1, Quality: 1, Durability: 10, CreatedAt: now}
	_, err = s.CommitPlayerTrade(ctx, CommitTradeRequest{
		TradeID:       guid(0xab03),
		SideA:         TradeSide{ID: alice.ID, Bags: ClaimBags{Items: []Item{aItem, dupWhInA}, ReplaceWarehouse: true}},
		SideB:         TradeSide{ID: bob.ID, Bags: ClaimBags{Items: []Item{bItem}, Warehouse: []WarehouseItem{bWh}, ReplaceWarehouse: true}},
		ItemsBToA:     1,
		CommittedAtMs: 1_771_300_012_000,
	})
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("cross-character bag/warehouse duplicate guid err = %v, want ErrInvalid", err)
	}
	_, err = s.CommitPlayerTrade(ctx, CommitTradeRequest{
		TradeID:       guid(0xab04),
		SideA:         TradeSide{ID: alice.ID, Bags: ClaimBags{Items: []Item{aItem, dupWhInA}}},
		SideB:         TradeSide{ID: bob.ID, Bags: ClaimBags{Items: []Item{bItem}, ReplaceWarehouse: false}},
		ItemsBToA:     1,
		CommittedAtMs: 1_771_300_013_000,
	})
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("cross-character bag/stored-warehouse duplicate guid err = %v, want ErrInvalid", err)
	}
	// Verify DB completely untouched by all failed attempts.
	if w := walletOf(t, s, alice); w != 500 {
		t.Fatalf("alice wallet changed after rejected trades: %d", w)
	}
	if w := walletOf(t, s, bob); w != 500 {
		t.Fatalf("bob wallet changed after rejected trades: %d", w)
	}
}

func TestCommitPlayerTradeIdempotentReplayAndDifferentPairConflict(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()

	alice := newMailCharacter(t, s, acc, "replaya")
	bob := newMailCharacter(t, s, acc, "replayb")
	carol := newMailCharacter(t, s, acc, "replayc")

	now := time.UnixMilli(1_771_299_474_000).UTC()
	aItem := Item{Container: Backpack, Slot: 0, ItemRecordID: 7010, InstanceGUID: guid(0x6101), Quantity: 1, Quality: 1, Durability: 10, CreatedAt: now}
	bItem := Item{Container: Backpack, Slot: 0, ItemRecordID: 7020, InstanceGUID: guid(0x6201), Quantity: 1, Quality: 1, Durability: 10, CreatedAt: now}
	cItem := Item{Container: Backpack, Slot: 0, ItemRecordID: 7030, InstanceGUID: guid(0x6301), Quantity: 1, Quality: 1, Durability: 10, CreatedAt: now}
	if err := s.SaveInventoryAndWallet(ctx, alice.ID, []Item{aItem}, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1000}}); err != nil {
		t.Fatal(err)
	}
	if err := s.SaveInventoryAndWallet(ctx, bob.ID, []Item{bItem}, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1000}}); err != nil {
		t.Fatal(err)
	}
	if err := s.SaveInventoryAndWallet(ctx, carol.ID, []Item{cItem}, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1000}}); err != nil {
		t.Fatal(err)
	}

	tradeID := guid(0xac01)
	req := CommitTradeRequest{
		TradeID:       tradeID,
		SideA:         TradeSide{ID: alice.ID, Bags: ClaimBags{Items: []Item{aItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 900}}, ReplaceWallet: true}},
		SideB:         TradeSide{ID: bob.ID, Bags: ClaimBags{Items: []Item{bItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1100}}, ReplaceWallet: true}},
		CopperAToB:    100,
		CommittedAtMs: 1_771_300_020_000,
	}
	res, err := s.CommitPlayerTrade(ctx, req)
	if err != nil || res.Replayed {
		t.Fatalf("initial commit = %+v, %v", res, err)
	}

	// Simulate later gameplay save modifying Alice's wallet to 7777.
	if err := s.SaveInventoryAndWallet(ctx, alice.ID, []Item{aItem}, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 7777}}); err != nil {
		t.Fatal(err)
	}

	// Replaying the same trade_id (both same order and reversed A/B order) returns Replayed=true without clobbering 7777!
	replay1, err := s.CommitPlayerTrade(ctx, req)
	if err != nil || !replay1.Replayed {
		t.Fatalf("same-order replay = %+v, %v; want Replayed=true", replay1, err)
	}
	swapped := req
	swapped.SideA, swapped.SideB = req.SideB, req.SideA
	replay2, err := s.CommitPlayerTrade(ctx, swapped)
	if err != nil || !replay2.Replayed {
		t.Fatalf("reversed-order replay = %+v, %v; want Replayed=true", replay2, err)
	}
	if w := walletOf(t, s, alice); w != 7777 {
		t.Fatalf("replay clobbered later save: alice wallet = %d, want 7777", w)
	}

	// Reusing trade_id for a different pair (Alice, Carol) fails with ErrTradeCommitted.
	conflictReq := CommitTradeRequest{
		TradeID:       tradeID,
		SideA:         TradeSide{ID: alice.ID, Bags: ClaimBags{Items: []Item{aItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 1}}, ReplaceWallet: true}},
		SideB:         TradeSide{ID: carol.ID, Bags: ClaimBags{Items: []Item{cItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 2}}, ReplaceWallet: true}},
		CopperAToB:    1,
		CommittedAtMs: 1_771_300_021_000,
	}
	_, err = s.CommitPlayerTrade(ctx, conflictReq)
	if !errors.Is(err, ErrTradeCommitted) {
		t.Fatalf("reused trade_id for different pair err = %v, want ErrTradeCommitted", err)
	}
	if w := walletOf(t, s, carol); w != 1000 {
		t.Fatalf("carol wallet changed on conflict: %d", w)
	}
}

func TestCommitPlayerTradeConcurrentOppositeOrderAndValidationErrors(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()

	alice := newMailCharacter(t, s, acc, "conca")
	bob := newMailCharacter(t, s, acc, "concb")

	now := time.UnixMilli(1_771_299_474_000).UTC()
	aItem := Item{Container: Backpack, Slot: 0, ItemRecordID: 7010, InstanceGUID: guid(0x7101), Quantity: 1, Quality: 1, Durability: 10, CreatedAt: now}
	bItem := Item{Container: Backpack, Slot: 0, ItemRecordID: 7020, InstanceGUID: guid(0x7201), Quantity: 1, Quality: 1, Durability: 10, CreatedAt: now}
	if err := s.SaveInventoryAndWallet(ctx, alice.ID, []Item{aItem}, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 5000}}); err != nil {
		t.Fatal(err)
	}
	if err := s.SaveInventoryAndWallet(ctx, bob.ID, []Item{bItem}, []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 5000}}); err != nil {
		t.Fatal(err)
	}

	// Self-trade -> ErrInvalid
	if _, err := s.CommitPlayerTrade(ctx, CommitTradeRequest{
		TradeID: guid(0xad01), SideA: TradeSide{ID: alice.ID}, SideB: TradeSide{ID: alice.ID}, CopperAToB: 10, CommittedAtMs: 1_771_300_030_000,
	}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("self-trade err = %v, want ErrInvalid", err)
	}

	// 0-for-0 empty trade -> ErrInvalid
	if _, err := s.CommitPlayerTrade(ctx, CommitTradeRequest{
		TradeID: guid(0xad02), SideA: TradeSide{ID: alice.ID}, SideB: TradeSide{ID: bob.ID}, CommittedAtMs: 1_771_300_030_000,
	}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("empty trade err = %v, want ErrInvalid", err)
	}

	// Unknown character -> ErrNotFound
	if _, err := s.CommitPlayerTrade(ctx, CommitTradeRequest{
		TradeID: guid(0xad03), SideA: TradeSide{ID: alice.ID}, SideB: TradeSide{ID: "ffffffffffffffffffffffffffffffff"}, CopperAToB: 10, CommittedAtMs: 1_771_300_030_000,
	}); !errors.Is(err, ErrNotFound) {
		t.Fatalf("unknown character err = %v, want ErrNotFound", err)
	}

	// Concurrent opposite-order swaps (A,B) vs (B,A): canonical lock ordering guarantees zero deadlock!
	const rounds = 8
	var wg sync.WaitGroup
	errs := make(chan error, rounds*2)
	for i := 0; i < rounds; i++ {
		i := i
		wg.Add(2)
		go func() {
			defer wg.Done()
			_, err := s.CommitPlayerTrade(ctx, CommitTradeRequest{
				TradeID:       guid(0xae00 + i*2 + 1),
				SideA:         TradeSide{ID: alice.ID, Bags: ClaimBags{Items: []Item{aItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 4900}}, ReplaceWallet: true}},
				SideB:         TradeSide{ID: bob.ID, Bags: ClaimBags{Items: []Item{bItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 5100}}, ReplaceWallet: true}},
				CopperAToB:    100,
				CommittedAtMs: 1_771_300_040_000 + int64(i),
			})
			errs <- err
		}()
		go func() {
			defer wg.Done()
			_, err := s.CommitPlayerTrade(ctx, CommitTradeRequest{
				TradeID:       guid(0xae00 + i*2 + 2),
				SideA:         TradeSide{ID: bob.ID, Bags: ClaimBags{Items: []Item{bItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 5000}}, ReplaceWallet: true}},
				SideB:         TradeSide{ID: alice.ID, Bags: ClaimBags{Items: []Item{aItem}, Wallet: []WalletEntry{{CurrencyGuid: CopperCurrency, Amount: 5000}}, ReplaceWallet: true}},
				CopperBToA:    100,
				CommittedAtMs: 1_771_300_050_000 + int64(i),
			})
			errs <- err
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatalf("concurrent opposite-order commit failed: %v", err)
		}
	}
}

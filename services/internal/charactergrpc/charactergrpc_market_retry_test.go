// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

package charactergrpc

// Marketplace (migration 0030) over gRPC: a listing the seller already has is answered AlreadyExists (the store's ErrListingSaved), not InvalidArgument, so worldd can tell
// a save that committed from one the rules refused.

import (
	"context"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
)

func TestSaveInventoryAndListingMapsAnAlreadySavedListingToAlreadyExists(t *testing.T) {
	st := &marketFake{recStore: &recStore{ret: fullCharacter()}, saveErr: character.ErrListingSaved}
	c := dialRec(t, st)
	item := &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 1, ItemRecordId: 7001, InstanceGuid: fmtGUID(0x10),
		Quantity: 1, Quality: 2, Durability: 3, CreatedAtMs: 1_771_000_000_000}
	_, err := c.SaveInventoryAndListing(context.Background(), &characterv1.SaveInventoryAndListingRequest{CharacterId: "0123456789abcdef0123456789abcdef",
		Items: []*characterv1.Item{item}, Listing: protoMarketListing()})
	if status.Code(err) != codes.AlreadyExists {
		t.Errorf("an already saved listing: %v, want AlreadyExists", err)
	}
	if st.saveAsked != 1 {
		t.Errorf("the store was asked %d times, want 1", st.saveAsked)
	}
}

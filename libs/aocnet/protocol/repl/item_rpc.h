// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/item_rpc.h
//
//  The client's item-use RPCs of plan B2 in a typed form: ServerItemUsed and ServerActivateItem, both on the pawn
//  (APlayerCharacter), with the same four parameters
//
//      Storage             EItemLocation (the exe's enum: Backpack 3, Equipment 4 ...), a byte
//      Slot                int32, the slot of that storage
//      ExpectedItem        an FGuid (four int32): the instance the client believes is there
//      RequestedEquipSlot  uint32, an EEquipSlot, or 0xFFFFFFFF ("any": the server picks the slot)
//
//  What the retail client sends [observed, 141 equips and 100 unequips of the retail captures]: both RPCs, in the same
//  instant and with the same parameters, for an item that can be worn (double click on it in the bag: Storage 3 and the bag
//  slot, which equips it; on the equipment slot: Storage 4 and the slot, which takes it off); a consumable sends
//  ServerItemUsed only.  RequestedEquipSlot is 0xFFFFFFFF in every one of the 141 equips.  The server answers an equip with
//  two or more ClientReceiveStorageEvent, in one packet with the HeldItems and AppearanceIDs deltas.
//
//  A parameter left at its default is not on the wire (a 0 send bit): Storage 0, Slot 0, the zero FGuid and RequestedEquipSlot 0
//  read as those values.
// ============================================================================
#pragma once

#include "protocol/repl/rpc.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// RequestedEquipSlot's "any slot".
constexpr uint32_t kAnyEquipSlot = 0xFFFFFFFFu;

struct ItemUse {
    uint8_t   storage = 0;                      // EItemLocation: ItemLocation::Backpack 3, Equipment 4
    int32_t   slot = 0;
    GuidValue expected_item;                    // the instance's guid
    uint32_t  requested_equip_slot = kAnyEquipSlot;   // an EEquipSlot, or kAnyEquipSlot
    bool operator==(const ItemUse&) const = default;
};

/// ServerItemUsed from a decoded call; nullopt for another RPC, an undecoded call or a parameter of another kind.
std::optional<ItemUse> parse_item_used(const RpcCall& call);

/// ServerActivateItem from a decoded call (the same four parameters); nullopt as above.
std::optional<ItemUse> parse_activate_item(const RpcCall& call);

}}}  // namespace aoc::protocol::repl

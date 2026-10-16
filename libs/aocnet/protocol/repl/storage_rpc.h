// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/storage_rpc.h
//
//  The storage RPCs of plan B2 in a typed form: the contents of a storage
//  (ClientReceiveStoragePayload) and the events that change one
//  (ClientReceiveStorageEvent, ServerRequestStorageEvent), on top of rpc.h's
//  parameter sets and item_codec.h's FItemInstance / FItemStorageEvent codecs.
//
//  ClientReceiveStoragePayload(TargetStorage, NewStorageData) is on the pawn
//  (APlayerCharacter) and on the PlayerController (AAoCPlayerController): two
//  schema entries with the same parameters, and the capture's payloads are
//  bit-identical on both.  NewStorageData is an FItemStorageData flattened into
//  13 commands: ContainerName, MaximumWeight, MaximumItems, bLootFountainOnDeath,
//  ItemDropOnDestructionPercent, MaximumItemsToDropPercent, MinimumImpulse,
//  MaximumImpulse (FVectors of three doubles), bRandomizeAngular, Storage (an
//  array: per slot ItemData, Slot, TransactionIDLock (an FGuid as four int32),
//  bCheckItemLootPermission, ItemLootPermission (an array of FGuids)),
//  bClientRetrieveOnly, Dimension (an FVector2D), OverrideMatrix (an array of
//  columns, each an array of int32).  What the captures show (the login of a
//  level 20 Ranger and the original server's Bard):
//
//    * Storage holds one element per slot, MaximumItems of them.  An empty slot is
//      all zeros (the empty item, Slot 0, no lock, 178 bits); a filled slot has
//      Slot = its index.  Backpack 70, Equipment 83 (slot = EEquipSlot), Quest
//      100, one resource storage per equipped bag (24, 40 and 30 slots).
//    * The container name is empty and the weights, percents, impulses and loot
//      flags are 0 in every capture.  A resource bag has a Dimension (6x4, 8x5,
//      6x5); the 8x5 and 6x5 bags an OverrideMatrix of 8 and 6 columns of five
//      cells, each -1 or -2 (what the two mean is not known), the 6x4 one none.
//    * The original server's four payloads of the Bard: an empty Backpack,
//      Equipment with the 13 starter items, empty Quest and resource storages.
//
//  An empty Backpack is 13,297 bits (a header, 178 bits per empty slot, a tail of
//  145), which a default StoragePayload with 70 default slots writes exactly.
//
//  ClientReceiveStorageEvent(ItemEvent) and ServerRequestStorageEvent(StorageEvent)
//  each take one FItemStorageEvent.  An equip is two events (Remove from the
//  backpack, Add to the equipment) and an unequip the reverse; the Add of an
//  equip carries the character's GUID as `instigator`.
// ============================================================================
#pragma once

#include "protocol/repl/rpc.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// EItemStorageEventType (the exe's enum).
enum class StorageEventType : uint8_t { Add = 0, Remove = 1, Move = 2, Fill = 3, Swap = 4 };

/// EItemStorageErrors::ItemStorage_NumErrors: the status byte of every captured container ("no error").
constexpr uint8_t kStorageNoError = 14;

/// EItemLocation values the storages use (the exe's enum; ServerItemUsed's Storage, ItemData.Location).
enum class ItemLocation : uint8_t {
    Backpack = 3, Equipment = 4, Vanity = 5, Bank = 6, Resource0 = 11, Resource1 = 12, Resource2 = 13, Quest = 18,
    cCount = 24,
};

/// One element of FItemStorageData::Storage.
struct StorageSlot {
    ItemInstance           item;                 // the empty item for an empty slot
    int32_t                slot = 0;             // the slot index for a filled slot, 0 for an empty one
    GuidValue              transaction_lock;     // TransactionIDLock
    bool                   check_loot_permission = false;
    std::vector<GuidValue> loot_permission;      // ItemLootPermission
    bool operator==(const StorageSlot&) const = default;
};
// C Y B with C O A

/// ClientReceiveStoragePayload's parameters.  Floats compare by bit pattern.
struct StoragePayload {
    emit::FIntrepidNetworkGUID target;           // TargetStorage: the storage component (null: left at the default)
    std::string                container_name;
    float                      maximum_weight = 0;
    int32_t                    maximum_items = 0;
    bool                       loot_fountain_on_death = false;
    float                      item_drop_on_destruction_percent = 0;
    float                      maximum_items_to_drop_percent = 0;
    Vec3d                      minimum_impulse, maximum_impulse;
    bool                       randomize_angular = false;
    std::vector<StorageSlot>   storage;
    bool                       client_retrieve_only = false;
    Vec2d                      dimension;
    std::vector<std::vector<int32_t>> override_matrix;   // TetrisColumn: per column its Column array

    bool operator==(const StoragePayload& o) const;
};

/// The slots of a storage that hold an item (`item.has_item`).
size_t filled_slots(const StoragePayload& p);

/// The parameters of a ClientReceiveStoragePayload call on `rpc` (the pawn's or the PlayerController's schema entry).
/// nullopt when `rpc` is not that RPC or a value does not fit (an item that does not, a count above 65,535, a string that
/// is not an FString).
std::optional<RpcParams> storage_payload_params(const schema::RpcSchema& rpc, const StoragePayload& payload);

/// The payload of a decoded ClientReceiveStoragePayload call; nullopt for another RPC, an undecoded call or parameters
/// that are not a whole payload (no NewStorageData).
std::optional<StoragePayload> parse_storage_payload(const RpcCall& call);
std::optional<StoragePayload> parse_storage_payload(const schema::RpcSchema& rpc, const RpcParams& params);

/// The parameters of a call of an RPC whose one parameter is an FItemStorageEvent (ClientReceiveStorageEvent,
/// ServerRequestStorageEvent); nullopt for any other RPC or an event that does not fit.
std::optional<RpcParams> storage_event_params(const schema::RpcSchema& rpc, const ItemStorageEvent& event);

/// The event of a decoded call of such an RPC; nullopt for another RPC, an undecoded call or no event (left at the default).
std::optional<ItemStorageEvent> parse_storage_event(const RpcCall& call);
std::optional<ItemStorageEvent> parse_storage_event(const schema::RpcSchema& rpc, const RpcParams& params);

// ---- plan E1, stage 3, part 2: the loot window ----------------------------------------------------------------------------------
//
//  A corpse's loot is shown in a window the client opens when it is sent ClientReceiveLootStoragePayload(LootActor, NewStorageData,
//  LootAllocationMethods): the same FItemStorageData as a storage payload (research/combat 2: one slot per distinct item and quality, bClientRetrieveOnly 1, a
//  slot's bCheckItemLootPermission 1 with the looting player's CharacterGuid in its ItemLootPermission), then the client takes each item with
//  ServerRequestLootStorageEvents(ItemEvents): one FItemStorageEvent of type 3 per slot (MoveSourceContainer = the loot container's slot, TargetContainers = the
//  bag) and is answered with ClientReceiveStorageEvent calls and, when the corpse holds nothing more, ClientRemoveListenerFromStorage(LootActor).  The
//  retail captures show LootAllocationMethods {Default 1, every rarity 0}.

struct LootAllocation {
    uint8_t default_method = 1;
    uint8_t by_rarity[7] = {0, 0, 0, 0, 0, 0, 0};   // Common, Uncommon, Rare, Heroic, Epic, Legendary, Artifact
    bool operator==(const LootAllocation&) const = default;
};

struct LootPayload {
    emit::FIntrepidNetworkGUID actor;       // LootActor: the corpse
    StoragePayload             data;        // `data.target` is `actor` (the first parameter)
    LootAllocation             allocation;
};

/// The parameters of a ClientReceiveLootStoragePayload call on `rpc` (the PlayerController's schema entry); nullopt for another RPC or a value that does not fit.
std::optional<RpcParams> loot_payload_params(const schema::RpcSchema& rpc, const LootPayload& payload);
std::optional<LootPayload> parse_loot_payload(const RpcCall& call);

/// The events of a decoded ServerRequestLootStorageEvents call (none when the array is empty); nullopt for another RPC, an undecoded call or an array of
/// more than 256 events.
std::optional<std::vector<ItemStorageEvent>> parse_loot_events(const RpcCall& call);

/// The parameters of ClientRemoveListenerFromStorage(LootActor); nullopt for another RPC.
std::optional<RpcParams> remove_listener_params(const schema::RpcSchema& rpc, const emit::FIntrepidNetworkGUID& actor);

// ---- Gathering stage 2, "Harvesting": a tool's wear ---------------------------------------------------------------------------------
//
//  ClientUpdateItemDurability(ItemDurabilityUpdate) on the PlayerController tells the client the new durability of items of one container.  A harvest sends one, in the
//  frame of its result, before the storage events [R: 1427 packet 673: the Equipment storage, slot 33 (the Sickle), 165 -> 164]: the struct FItemDurabilityUpdate holds the
//  container (TargetContainer, an object) and an array of {ItemGuid (an FGuid), Slot (int32), Durability (float)}.

struct ItemDurability {
    GuidValue item;            // the instance guid
    int32_t   slot = 0;        // its slot in the container
    float     durability = 0;
    bool operator==(const ItemDurability& o) const { return item == o.item && slot == o.slot && same_bits(durability, o.durability); }
};

struct ItemDurabilityUpdate {
    emit::FIntrepidNetworkGUID container;      // TargetContainer: the storage component
    std::vector<ItemDurability> items;
    bool operator==(const ItemDurabilityUpdate& o) const { return container == o.container && items == o.items; }
};

/// The parameters of a ClientUpdateItemDurability call on `rpc`; nullopt for another RPC or an update with no item.
std::optional<RpcParams> item_durability_params(const schema::RpcSchema& rpc, const ItemDurabilityUpdate& update);
/// The update of a decoded call; nullopt for another RPC, an undecoded call or parameters that are no update.
std::optional<ItemDurabilityUpdate> parse_item_durability(const RpcCall& call);
std::optional<ItemDurabilityUpdate> parse_item_durability(const schema::RpcSchema& rpc, const RpcParams& params);

}}}  // namespace aoc::protocol::repl

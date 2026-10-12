// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/inventory.h
//
//  A player's items (plan B2): the three storages the client shows, what is where, and the rules that move an item between
//  them.  Pure logic: no sockets, no NetGUIDs, no database but the ItemCatalog it is asked about; the session turns an
//  Outcome's events into ClientReceiveStorageEvent calls and the pawn's visible gear.
//
//  The storages (the client's own, ClientReceiveStoragePayload's MaximumItems): the Backpack has 70 slots, the Equipment 83
//  (the slot is the EEquipSlot: 2 Chest, 15 Primary ...) and the Quest storage 100.  An item is an svc::InventoryItem (what the
//  character service stores); to_instance makes the FItemInstance the wire carries from it.  One slot holds one item, and one
//  instance guid names one item.
//
//  What the retail server does with a request, read from the research of 2026-10-02 (about 100 equip and unequip captures and
//  the storage_move ones; the events are the order the client receives them in) and what this does the same:
//
//    equip    (ServerItemUsed on a bag slot, RequestedEquipSlot always 0xFFFFFFFF: the server picks the slot)
//               Remove the item from its bag slot; for each item it displaces (see below), in order, Add it to the lowest free
//               bag slot and Remove it from its equipment slot; Add the item to its equipment slot.  A plain equip is two
//               events, an empty equip slot; a two-hander over a main hand and an off hand six (capture 1204: Remove 2H, Add
//               mace at bag slot 4, Remove mace, Add focus at bag slot 5, Remove focus, Add 2H).
//    unequip  (ServerItemUsed on an equipment slot) Add the item to the lowest free bag slot, Remove it from the equipment slot.
//    move     (ServerRequestStorageEvent type Move, bag to bag, a whole stack) Remove it from its slot, Add it to the target.
//    swap     (type Swap onto an occupied slot, both whole stacks) Remove A, Remove B, Add B at A's slot, Add A at B's slot.
//
//  Which slot an item is worn in.  The item's record lists the slots it may be worn in (content.db item_equip_slot, in the
//  record's order); the client names none, and the server takes the first of them no worn item covers, else the first.  The
//  retail captures fit: a first ring goes to RingRight 10, a second to RingLeft 11 (the record's order is 10, 11 for most rings),
//  and an earring goes to the free one of its two slots.  An item flagged two-handed covers every slot of its list: worn, it
//  takes them all (the item sits in one, and the others stay empty); an item that is worn into a slot a two-hander covers, or a
//  two-hander worn over anything in its slots, displaces what is there to the bag.  A request that names a slot the record does
//  not list is refused.
//
//  What is refused, and changes nothing: an item that is not in the slot the client says (empty, or another instance's guid),
//  an item the catalog does not know or whose record lists no slot, a bag with too few free slots for what the equip displaces,
//  a storage other than the Backpack (to use) or the Backpack to the Backpack (to move), a partial stack (a split), a move
//  onto an occupied slot that is not a swap, and every number out of range.  There is no class rule: the design data has none
//  (every class's kit holds weapons of another class's line, and a retail level 20 character wears level 26 gear), so
//  "the class may equip it" is true for all, and the record's bCanEquip is not a rule either (ItemRecord::can_equip).
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/value.h"
#include "svc/services.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::items {

namespace repl = ::aoc::protocol::repl;

inline constexpr int kBackpackSlots = 70;
inline constexpr int kEquipmentSlots = 83;
inline constexpr int kQuestSlots = 100;
inline constexpr int kMaxItems = kBackpackSlots + kEquipmentSlots + kQuestSlots;

/// The number of slots of a storage.
int slot_count(svc::ItemContainer c);
const char* container_name(svc::ItemContainer c);

/// EEquipSlot values the rules and the visible gear name (the exe's enum).
namespace equip {
inline constexpr uint8_t Helmet = 1, Chest = 2, Wrists = 3, Hands = 4, Legs = 5, Feet = 6, Shoulders = 7, Back = 8, Belt = 9;
inline constexpr uint8_t RingRight = 10, RingLeft = 11, Necklace = 12, EarRight = 13, EarLeft = 14;
inline constexpr uint8_t Primary = 15, Secondary = 16, Ranged = 17, RangedOffHand = 18;
inline constexpr uint8_t Shirt = 19, ResourceBag0 = 23, Pickaxe = 26, Axe = 29, Sickle = 33, FishingRod = 35, Bow = 38, Crowbar = 41;
inline constexpr uint8_t Costume = 71, PrimaryCosmetic = 75, CloakCosmetic = 79;
}  // namespace equip
// Cyb.C.O.A

// ---- items as the wire carries them ---------------------------------------------------------------------------------------

/// The stats a mount item persists (the Health and Mana StatTypeDef records: retail's login mounts carry exactly these two) .
inline constexpr uint64_t kPersistedHealthStat = 108986352686712ull;
inline constexpr uint64_t kPersistedManaStat = 109183576135288ull;

/// The instance guid's 32 hex digits <-> the FGuid's four words (A first, big-endian digits: the capture's
/// 9cb881dd7fbb492eb9e6b0e19677c0f7 is A 0x9cb881dd ...).  nullopt for anything else, the zero guid included.
std::optional<repl::GuidValue> guid_of(const std::string& hex);
std::string guid_hex(const repl::GuidValue& g);

/// .NET ticks (FDateTime: 100 ns since 0001-01-01) of a Unix millisecond time, and back (sub-millisecond ticks are dropped).
int64_t ticks_of_ms(int64_t unix_ms);
int64_t ms_of_ticks(int64_t ticks);

/// The FItemInstance of an item: the record, quantity, quality, durability, instance guid and creation time, every other member
/// at its default, as the original server's starter items are (NodeGuid, CrateGuid, LockExpiration, stats ... all zero), except what a mount item
/// persists (the mounts milestone): its Health and Mana as the two persisted stats (kPersistedHealthStat, kPersistedManaStat, in that order, as retail's
/// login shows them) and its LockExpiration.
/// `item.instance_guid` must be 32 hex digits (Inventory only holds such items).
repl::ItemInstance to_instance(const svc::InventoryItem& item);
/// The item a decoded instance is, in `container` slot `slot`: nullopt for an empty instance or one the table cannot hold (a guid
/// of zero words, a quantity or quality out of its range).
std::optional<svc::InventoryItem> item_of(const repl::ItemInstance& instance, svc::ItemContainer container, int slot);

// ---- the catalog ---------------------------------------------------------------------------------------------------------

/// What the equip rules need of an item record.
struct ItemInfo {
    std::vector<uint8_t> equip_slots;       // the EEquipSlot values it may be worn in, in the record's order
    bool                 two_handed = false;   // worn, it covers every slot of equip_slots
    bool wearable() const { return !equip_slots.empty(); }
};

class ItemCatalog {
public:
    virtual ~ItemCatalog() = default;
    /// The record's facts; nullopt for a record the catalog does not know.
    virtual std::optional<ItemInfo> info(int64_t record) const = 0;
};

/// The catalog worldd runs: content.db's item and item_equip_slot.  A query that fails reads as "unknown" and is logged once.
class ContentItemCatalog : public ItemCatalog {
public:
    explicit ContentItemCatalog(const content::ContentDb& db) : db_(db) {}
    std::optional<ItemInfo> info(int64_t record) const override;

private:
    const content::ContentDb& db_;
    mutable bool              failure_logged_ = false;
};

// ---- the inventory --------------------------------------------------------------------------------------------------------

/// What happened to a slot, in the order the client is told.
struct Event {
    repl::StorageEventType type = repl::StorageEventType::Add;   // Add or Remove
    svc::ItemContainer     container = svc::ItemContainer::Backpack;
    int                    slot = 0;
    svc::InventoryItem     item;                                  // the instance added to / removed from the slot
    /// Whether the event carries the character's guid as its instigator.  The retail server's do for an equip's Add and for
    /// both events of a move or swap, and not for an equip's Remove or an unequip's Remove (the zero guid; research A1).
    bool                   instigated = true;
};

/// The result of a request: whether it was applied, why not, and what the client is told.
struct Outcome {
    bool               ok = false;
    std::string        refusal;           // why not (for the log); empty when ok
    std::vector<Event> events;            // empty when refused
    bool               equipment_changed = false;   // the Equipment storage changed: the visible gear may have
};

class Inventory {
public:
    Inventory() = default;

    /// The inventory holding `items` (in any order).  nullopt, with `error` saying which item and why, for a slot outside its
    /// storage, two items in a slot, an instance guid that is not 32 hex digits or is used twice, a record of 0 or less, a
    /// quantity or quality out of its range (what the character service's table checks) or more than kMaxItems.
    static std::optional<Inventory> from_items(const std::vector<svc::InventoryItem>& items, std::string& error);

    /// The item in a slot, nullptr when it is empty or the slot is outside the storage.
    const svc::InventoryItem* at(svc::ItemContainer container, int slot) const;
    size_t count(svc::ItemContainer container) const;
    size_t size() const { return count(svc::ItemContainer::Backpack) + count(svc::ItemContainer::Equipment) + count(svc::ItemContainer::Quest); }
    /// The lowest empty slot of a storage, -1 when it is full.
    int first_free(svc::ItemContainer container) const;
    /// Every item: the Backpack's by slot, then the Equipment's, then the Quest storage's.
    std::vector<svc::InventoryItem> items() const;
    /// The item record of every item, in the order of items() (a cheap look for a session that asks every tick which records it carries).
    std::vector<int64_t> records() const;

    /// ServerItemUsed: the item in `storage` slot `slot`, which the client says is the instance `expected_guid` (the 32 hex digits;
    /// a zero or malformed guid matches nothing), is worn (a bag slot: into `requested_equip_slot`, or the slot the server picks when that
    /// is 0xFFFFFFFF) or taken off (an equipment slot: into the lowest free bag slot).  Applied only when every check passes.
    Outcome use(svc::ItemContainer storage, int slot, const std::string& expected_guid, uint32_t requested_equip_slot,
                const ItemCatalog& catalog);

    /// ServerRequestStorageEvent, Move: the whole stack of `count` items of the instance `guid` from `from_slot` to the empty `to_slot`,
    /// both in the Backpack.
    Outcome move(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot);
    /// ServerRequestStorageEvent, Swap: the two whole stacks of the Backpack change places; `to_guid` and `to_count` are what the
    /// client says is in the target slot.
    Outcome swap(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot,
                 const std::string& to_guid, int32_t to_count);

    /// Plan D1: a use consumed `count` of the stack in `storage` slot `slot` (the instance `guid`): the stack shrinks (a Remove,
    /// then an Add of the stack with its new count, the same instance) or goes when nothing is left (a Remove). Refused, changing
    /// nothing, when the slot does not hold that instance or holds fewer than `count`. Modelled: no retail capture shows a stack
    /// losing one (the consumables retail used were single items, removed with one Remove event).
    Outcome consume(svc::ItemContainer storage, int slot, const std::string& guid, int32_t count);

    /// Plan D1: items granted into the Backpack (a GiveItem reward): each goes to the lowest free bag slot, an Add event each
    /// (the character as the instigator, as the join's Adds), in the order given; `items` carry their record, quantity, quality,
    /// durability, a fresh instance guid and creation time, their container and slot are set here. Refused, changing nothing,
    /// when the bag has too few free slots for all of them, an item breaks the table's rules or its guid is one the inventory
    /// has. The bag's Add is what the retail server sent for every item that reached a bag (the join's, an unequip's).
    Outcome grant(std::vector<svc::InventoryItem> items);

    /// The mounts milestone, stage 3: a mount item keeps its Health and its LockExpiration (retail persists them on the item instance).  Sets them on the
    /// item of the instance `guid` (in the Backpack or the Equipment); `health` unset leaves the Health, `lock_expiration` negative leaves the lock.  The
    /// outcome lists the item's Remove and Add (the client is told the item changed, as a stack that shrinks is), or nothing when `events` is false.
    /// Refused, changing nothing, when no item holds the instance or the values are out of range.
    Outcome update_mount(const std::string& guid, std::optional<float> health, int64_t lock_expiration, bool events);
    /// The item of an instance guid (the Backpack, the Equipment, the Quest storage); nullptr when none.
    const svc::InventoryItem* find_guid(const std::string& guid) const;

    bool operator==(const Inventory& o) const { return slots_ == o.slots_; }

private:
    using Key = std::pair<uint8_t, int>;
    const svc::InventoryItem* find(svc::ItemContainer c, int slot) const;
    Outcome equip(int bag_slot, const ItemCatalog& catalog, uint32_t requested);
    Outcome unequip(int equip_slot);
    /// Whether a worn item covers `slot` (it is in it, or a two-hander of the Equipment lists it).
    bool covered(int slot, const ItemCatalog& catalog) const;

    std::map<Key, svc::InventoryItem> slots_;   // (container, slot) -> the item
};

// ---- the visible gear ----------------------------------------------------------------------------------------------------

/// The keys of BaseCharacterInfo.HeldItems, a fast array of the 23 slots the client's UPlayerInfo has, in the order of its
/// ReplicationIDs 1..23 (the captured Bard's, pkt#107): the weapons and the artisan tools and the cosmetic weapons.  The value
/// of a key is the record of the item worn in that slot, 0 when none.
inline constexpr std::array<uint8_t, 23> kHeldSlots = {15, 16, 17, 18, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 41, 75, 76, 77, 78};

/// Whether the item worn in `slot` is part of Character Appearance.AppearanceIDs: Helmet to Belt (1-9), Costume and CloakCosmetic
/// (71, 79). Rings, earrings, the necklace, the artisan shirt and trousers, the tool belt, the lure and the mounts never are
/// (research A1: 309 retail pawn records).
bool shows_in_appearance(int slot);

/// The records worn in kHeldSlots, in that order (0: nothing in the slot).
std::array<int64_t, 23> held_records(const Inventory& inventory);
/// The AppearanceIDs array: the worn armor, costume and cloak, by ascending slot (ItemRecord TypeId 2616523457551378613,
/// enchantment 0, as the captured Bard's).
std::vector<repl::AppearanceId> appearance_ids(const Inventory& inventory);

/// The TypeId of an ItemRecord reference (AppearanceIDs' TypeId).
inline constexpr uint64_t kItemRecordTypeId = 2616523457551378613ull;

}  // namespace aoc::worldd::items

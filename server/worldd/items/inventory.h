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
//
//  Gathering stage 3, "Resource storage": the three resource containers (Resource0 to Resource2, the resource bags worn in Equipment slots 23, 24 and 25).  An item goes to a
//  resource container iff its gameplay tags hold a child of item.resource (ItemInfo::resource); every other item goes to the Backpack, and the Backpack and the Equipment
//  refuse a resource item as the resource containers refuse anything else [exe: UItemStorageComponent's item check, UItemMovementSubsystem::ValidateTargetContainers].  A
//  container is a grid (resource_grid.h, from the bag's ResourceBagDef): an item takes inv_w x inv_h cells, its slot is the index of the top-left cell, a new stack goes to the
//  first free rectangle (rows, then columns).  grant() is THE routing: a caller never picks a container.  A grant tops up the stacks of the same record and quality (up to the
//  record's MaxStackSize, the bag's stat modifiers are not replicated) before it starts a new one, in the first container with room, in index order [R: capture 1198's
//  Resource containers hold one stack of 15 Weeping Willow and one of 10 Halcyonite that harvests of 5 grew; the retail event of a top-up is not captured, the Remove and Add of
//  the same instance that consume() sends is modelled].  The grids are the ones of the bags worn when the inventory was built: a container opens at the next login for a bag worn
//  later, and a bag whose container is open cannot be taken off or replaced during the session.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "items/resource_grid.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/value.h"
#include "svc/services.h"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::items {

namespace repl = ::aoc::protocol::repl;

inline constexpr int kBackpackSlots = 70;
inline constexpr int kEquipmentSlots = 83;
inline constexpr int kQuestSlots = 100;
/// Gathering stage 3: a resource container has the cells of its bag (at most kMaxResourceCells), and the character service stores a slot below that.
inline constexpr int kResourceContainers = 3;
inline constexpr int kMaxItems = kBackpackSlots + kEquipmentSlots + kQuestSlots + kResourceContainers * kMaxResourceCells;

inline bool is_resource(svc::ItemContainer c) { return c >= svc::ItemContainer::Resource0; }
/// 0, 1 or 2 for Resource0 to Resource2.
inline int resource_index(svc::ItemContainer c) { return static_cast<int>(c) - static_cast<int>(svc::ItemContainer::Resource0); }
inline svc::ItemContainer resource_container(int index) { return static_cast<svc::ItemContainer>(static_cast<int>(svc::ItemContainer::Resource0) + index); }

/// The number of slots of a storage (a resource container: the most any bag has; Inventory::grid has the bag's own).
int slot_count(svc::ItemContainer c);
const char* container_name(svc::ItemContainer c);

/// EEquipSlot values the rules and the visible gear name (the exe's enum).
namespace equip {
inline constexpr uint8_t Helmet = 1, Chest = 2, Wrists = 3, Hands = 4, Legs = 5, Feet = 6, Shoulders = 7, Back = 8, Belt = 9;
inline constexpr uint8_t RingRight = 10, RingLeft = 11, Necklace = 12, EarRight = 13, EarLeft = 14;
inline constexpr uint8_t Primary = 15, Secondary = 16, Ranged = 17, RangedOffHand = 18;
inline constexpr uint8_t Shirt = 19, ResourceBag0 = 23, ResourceBag1 = 24, ResourceBag2 = 25, Pickaxe = 26, Axe = 29, Sickle = 33, FishingRod = 35, Bow = 38, Crowbar = 41;
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
    /// Gathering stage 3: the item is stored in a resource container (its gameplay tags hold a child of item.resource), takes inv_w x inv_h cells of its grid and stacks to max_stack.
    bool                 resource = false;
    int                  inv_w = 1;
    int                  inv_h = 1;
    int                  max_stack = 1;
    /// The grid a resource bag gives its container, set for the items that are resource bags.
    std::optional<ResourceGrid> bag;
    bool wearable() const { return !equip_slots.empty(); }
};

class ItemCatalog {
public:
    virtual ~ItemCatalog() = default;
    /// The record's facts; nullopt for a record the catalog does not know.
    virtual std::optional<ItemInfo> info(int64_t record) const = 0;
};

/// The catalog worldd runs: content.db's item, item_equip_slot and resource_bag.  A query that fails reads as "unknown" and is logged once.  What it found is kept (a grant asks about
/// every item of a container): a record is read from the file once per catalog.
class ContentItemCatalog : public ItemCatalog {
public:
    explicit ContentItemCatalog(const content::ContentDb& db) : db_(db) {}
    std::optional<ItemInfo> info(int64_t record) const override;

private:
    const content::ContentDb&                                   db_;
    mutable std::mutex                                          mutex_;
    mutable std::unordered_map<int64_t, std::optional<ItemInfo>> kept_;
    mutable bool                                                failure_logged_ = false;
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

/// What grant_what_fits did: the events of what it placed, and what it could not (the quantity left of an item that topped up a stack or filled a container in part comes back
/// as an item of its own with the rest, under the instance guid the caller gave it).
struct GrantResult {
    Outcome                         outcome;   // ok unless an item breaks the table's rules or its guid is one the inventory has (then nothing was placed and `rest` is every item)
    std::vector<svc::InventoryItem> rest;
};

/// The resource items found in the Backpack at the login, and where they went (Inventory::repair_resources).
struct ResourceRepair {
    std::vector<svc::InventoryItem> moved;    // now in a resource container, with their new container and slot
    std::vector<svc::InventoryItem> stayed;   // left in the Backpack: no container had room for them
};

class Inventory {
public:
    Inventory() = default;

    /// The inventory holding `items` (in any order).  nullopt, with `error` saying which item and why, for a slot outside its
    /// storage, two items in a slot, an instance guid that is not 32 hex digits or is used twice, a record of 0 or less, a
    /// quantity or quality out of its range (what the character service's table checks) or more than kMaxItems.
    /// With a `catalog` the resource containers' grids are the ones of the bags worn in Equipment slots 23 to 25, and an item in a resource container must lie inside its grid, on
    /// no blocked cell and on no other item (an error says which); without one a resource item is only checked for its slot number (the grids stay empty).
    static std::optional<Inventory> from_items(const std::vector<svc::InventoryItem>& items, std::string& error, const ItemCatalog* catalog = nullptr);

    /// The grid of a resource container (Resource0 to Resource2; an empty grid for any other storage and for a container whose bag is not worn).
    const ResourceGrid& grid(svc::ItemContainer container) const;
    /// The slot of the first free `w` x `h` rectangle of a resource container, -1 when there is none (or the storage is no resource container).
    int place(svc::ItemContainer container, int w, int h, const ItemCatalog& catalog) const;

    /// The item in a slot, nullptr when it is empty or the slot is outside the storage.
    const svc::InventoryItem* at(svc::ItemContainer container, int slot) const;
    size_t count(svc::ItemContainer container) const;
    size_t size() const { return slots_.size(); }
    /// The lowest empty slot of a storage, -1 when it is full (a resource container: -1, its free places depend on the item's dimension: place()).
    int first_free(svc::ItemContainer container) const;
    /// Every item: the Backpack's by slot, then the Equipment's, the Quest storage's and the three resource containers'.
    std::vector<svc::InventoryItem> items() const;
    /// The item record of every item, in the order of items() (a cheap look for a session that asks every tick which records it carries).
    std::vector<int64_t> records() const;

    /// ServerItemUsed: the item in `storage` slot `slot`, which the client says is the instance `expected_guid` (the 32 hex digits;
    /// a zero or malformed guid matches nothing), is worn (a bag slot: into `requested_equip_slot`, or the slot the server picks when that
    /// is 0xFFFFFFFF) or taken off (an equipment slot: into the lowest free bag slot).  Applied only when every check passes.
    Outcome use(svc::ItemContainer storage, int slot, const std::string& expected_guid, uint32_t requested_equip_slot,
                const ItemCatalog& catalog);
// (C | Y | B ~ CoA)

    /// ServerRequestStorageEvent, Move: the whole stack of `count` items of the instance `guid` from `from_slot` to the empty `to_slot`,
    /// both in the Backpack; or, with a `catalog`, both in resource containers (the same one or two): the stack is moved to the top-left cell `to_slot` of its rectangle, which must lie
    /// in the grid on free cells (its own old cells are free).  The Backpack and a resource container never exchange items, a move onto a stack of the same item (Fill) is refused.
    Outcome move(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot, const ItemCatalog* catalog = nullptr);
    /// ServerRequestStorageEvent, Swap: the two whole stacks of the Backpack change places; `to_guid` and `to_count` are what the
    /// client says is in the target slot.  A resource container's items are not swapped (refused).
    Outcome swap(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot,
                 const std::string& to_guid, int32_t to_count);

    /// Plan D1: a use consumed `count` of the stack in `storage` slot `slot` (the instance `guid`): the stack shrinks (a Remove,
    /// then an Add of the stack with its new count, the same instance) or goes when nothing is left (a Remove). Refused, changing
    /// nothing, when the slot does not hold that instance or holds fewer than `count`. Modelled: no retail capture shows a stack
    /// losing one (the consumables retail used were single items, removed with one Remove event).
    Outcome consume(svc::ItemContainer storage, int slot, const std::string& guid, int32_t count);

    /// Plan D1: items granted (a GiveItem reward, a purchase, a use): THE routing of Gathering stage 3 -- each item goes where its kind says (see the top of this file): a resource item
    /// tops up the stacks of its record and quality in the resource containers and starts new ones at the first free rectangle of the first container with room, any other item takes
    /// the lowest free bag slot.  An Add event each (the character as the instigator, as the join's Adds; a top-up is a Remove and an Add of the same instance), in the order given;
    /// `items` carry their record, quantity, quality, durability, a fresh instance guid and creation time, their container and slot are set here.  Refused, changing nothing, when the
    /// storages have no room for all of them, an item breaks the table's rules or its guid is one the inventory has.
    Outcome grant(std::vector<svc::InventoryItem> items, const ItemCatalog& catalog);
    /// The same routing, for a caller that keeps what does not fit (a harvest: the rest goes to a pile on the ground): everything that fits is placed and the events say how, the rest
    /// comes back in `rest`.  An item that breaks the table's rules or whose guid the inventory has refuses the whole call (nothing placed).
    GrantResult grant_what_fits(std::vector<svc::InventoryItem> items, const ItemCatalog& catalog);
    /// Whether grant(items) would be applied (it changes nothing).
    bool can_grant(const std::vector<svc::InventoryItem>& items, const ItemCatalog& catalog) const;
    /// The login's repair: the resource items found in the Backpack (the harvests of Gathering stage 2 went there) move to the resource containers, first free rectangle each, in the
    /// order of their Backpack slots.  Those no container has room for stay where they are.  No events (the join sends the repaired layout).
    ResourceRepair repair_resources(const ItemCatalog& catalog);

    /// The mounts milestone, stage 3: a mount item keeps its Health and its LockExpiration (retail persists them on the item instance).  Sets them on the
    /// item of the instance `guid` (in the Backpack or the Equipment); `health` unset leaves the Health, `lock_expiration` negative leaves the lock.  The
    /// outcome lists the item's Remove and Add (the client is told the item changed, as a stack that shrinks is), or nothing when `events` is false.
    /// Refused, changing nothing, when no item holds the instance or the values are out of range.
    Outcome update_mount(const std::string& guid, std::optional<float> health, int64_t lock_expiration, bool events);
    /// The item of an instance guid (the Backpack, the Equipment, the Quest storage); nullptr when none.
    const svc::InventoryItem* find_guid(const std::string& guid) const;

    /// Gathering stage 2, "Harvesting": a tool's wear.  Sets the durability of the item in `container` slot `slot` that holds the instance `guid` (no storage event: the client is
    /// told by ClientUpdateItemDurability).  False, changing nothing, when the slot holds another item or the value is not a number from 0 up.
    bool set_durability(svc::ItemContainer container, int slot, const std::string& guid, float durability);

    bool operator==(const Inventory& o) const { return slots_ == o.slots_; }

private:
    using Key = std::pair<uint8_t, int>;
    const svc::InventoryItem* find(svc::ItemContainer c, int slot) const;
    /// The cells a resource container's items cover (one flag per cell), `ignore` aside.
    std::vector<bool> covered_cells(svc::ItemContainer container, const ItemCatalog& catalog, const std::string* ignore = nullptr) const;
    /// grant_what_fits for one item that is a resource: tops up, then starts stacks; `item.quantity` is what is left.
    void grant_resource(svc::InventoryItem item, const ItemInfo& info, const ItemCatalog& catalog, GrantResult& out);
    Outcome move_resource(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot, const ItemCatalog& catalog);
    Outcome equip(int bag_slot, const ItemCatalog& catalog, uint32_t requested);
    Outcome unequip(int equip_slot);
    /// Whether a worn item covers `slot` (it is in it, or a two-hander of the Equipment lists it).
    bool covered(int slot, const ItemCatalog& catalog) const;

    std::map<Key, svc::InventoryItem> slots_;   // (container, slot) -> the item
    std::array<ResourceGrid, kResourceContainers> grids_;   // the grids of the resource bags worn when the inventory was built
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

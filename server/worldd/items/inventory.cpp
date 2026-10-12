// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/inventory.cpp
//
//  See inventory.h.
// ============================================================================
#include "items/inventory.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aoc::worldd::items {

namespace {

constexpr int64_t kTicksAtUnixEpoch = 621355968000000000ll;   // 1970-01-01 as FDateTime ticks
constexpr int64_t kTicksPerMs = 10000;
constexpr int32_t kMaxQuantity = 1000000;
constexpr int32_t kMaxQuality = 200;
constexpr int64_t kMaxMs = 9'000'000'000'000ll;   // year 2255: ticks stay far inside an int64
constexpr int64_t kMaxLockExpiration = 253402300799ll;   // Unix seconds: the end of the year 9999 (character service migration 0007)

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template <class T>
bool contains(const std::vector<T>& v, T x) { return std::find(v.begin(), v.end(), x) != v.end(); }
// (C-O-A ~ Cyb)

bool valid_container(svc::ItemContainer c) { return c == svc::ItemContainer::Backpack || c == svc::ItemContainer::Equipment || c == svc::ItemContainer::Quest; }

/// What is wrong with an item, "" when nothing: the constraints of the character service's table.
std::string item_problem(const svc::InventoryItem& it) {
    if (!valid_container(it.container)) return "its storage is none of the Backpack, the Equipment and the Quest storage";
    if (it.slot < 0 || it.slot >= slot_count(it.container)) {
        return "slot " + std::to_string(it.slot) + " is outside the " + container_name(it.container) + " (0 to " +
               std::to_string(slot_count(it.container) - 1) + ")";
    }
    if (it.item_record_id <= 0) return "record " + std::to_string(it.item_record_id) + " is none";
    if (const auto g = guid_of(it.instance_guid); !g || guid_hex(*g) != it.instance_guid) {
        return "instance guid '" + it.instance_guid + "' is not 32 lowercase hex digits (or is the zero guid)";
    }
    if (it.quantity < 1 || it.quantity > kMaxQuantity) return "quantity " + std::to_string(it.quantity) + " is not 1 to " + std::to_string(kMaxQuantity);
    if (it.quality < 1 || it.quality > kMaxQuality) return "quality " + std::to_string(it.quality) + " is not 1 to " + std::to_string(kMaxQuality);
    if (!std::isfinite(it.durability) || it.durability < 0) return "durability is not a number from 0";
    if (it.created_at_ms < 0 || it.created_at_ms > kMaxMs) return "creation time " + std::to_string(it.created_at_ms) + " ms is out of range";
    for (const std::optional<float>* p : {&it.persisted_health, &it.persisted_mana}) {
        if (*p && (!std::isfinite(**p) || **p < 0 || **p > 1e9f)) return "a persisted Health or Mana is not a number from 0 to 1e9";
    }
    if (it.lock_expiration < 0 || it.lock_expiration > kMaxLockExpiration) return "lock expiration " + std::to_string(it.lock_expiration) + " is out of range";
    return {};
}

}  // namespace

int slot_count(svc::ItemContainer c) {
    switch (c) {
        case svc::ItemContainer::Backpack: return kBackpackSlots;
        case svc::ItemContainer::Equipment: return kEquipmentSlots;
        case svc::ItemContainer::Quest: return kQuestSlots;
    }
    return 0;
}

const char* container_name(svc::ItemContainer c) {
    switch (c) {
        case svc::ItemContainer::Backpack: return "Backpack";
        case svc::ItemContainer::Equipment: return "Equipment";
        case svc::ItemContainer::Quest: return "Quest storage";
    }
    return "?";
}

// ---- items as the wire carries them --------------------------------------------------------------------------------------

std::optional<repl::GuidValue> guid_of(const std::string& hex) {
    if (hex.size() != 32) return std::nullopt;
    uint32_t w[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 32; ++i) {
        const int d = hex_digit(hex[i]);
        if (d < 0) return std::nullopt;
        w[i / 8] = (w[i / 8] << 4) | static_cast<uint32_t>(d);
    }
    if (!(w[0] | w[1] | w[2] | w[3])) return std::nullopt;
    return repl::GuidValue{w[0], w[1], w[2], w[3]};
}

std::string guid_hex(const repl::GuidValue& g) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", g.a, g.b, g.c, g.d);
    return buf;
}

int64_t ticks_of_ms(int64_t unix_ms) { return unix_ms * kTicksPerMs + kTicksAtUnixEpoch; }
int64_t ms_of_ticks(int64_t ticks) { return (ticks - kTicksAtUnixEpoch) / kTicksPerMs; }

repl::ItemInstance to_instance(const svc::InventoryItem& item) {
    repl::ItemInstance i;
    i.has_item = true;
    i.record_id = static_cast<uint64_t>(item.item_record_id);
    i.quantity = static_cast<uint32_t>(item.quantity);
    i.quality = static_cast<uint32_t>(item.quality);
    i.durability = item.durability;
    if (const auto g = guid_of(item.instance_guid)) i.guid = *g;
    i.creation_time = ticks_of_ms(item.created_at_ms);
    // what a mount item persists, in the order retail's login has it (Health, then Mana)
    if (item.persisted_health) i.stats.push_back({kPersistedHealthStat, *item.persisted_health});
    if (item.persisted_mana) i.stats.push_back({kPersistedManaStat, *item.persisted_mana});
    i.lock_expiration = item.lock_expiration;
    return i;
}

std::optional<svc::InventoryItem> item_of(const repl::ItemInstance& instance, svc::ItemContainer container, int slot) {
    if (!instance.has_item) return std::nullopt;
    svc::InventoryItem it;
    it.container = container;
    it.slot = slot;
    it.item_record_id = static_cast<int64_t>(instance.record_id);
    it.instance_guid = guid_hex(instance.guid);
    it.quantity = static_cast<int32_t>(instance.quantity);
    it.quality = static_cast<int32_t>(instance.quality);
    it.durability = instance.durability;
    it.created_at_ms = ms_of_ticks(instance.creation_time);
    for (const repl::PersistedStat& s : instance.stats) {
        if (s.stat_id == kPersistedHealthStat) it.persisted_health = s.value;
        if (s.stat_id == kPersistedManaStat) it.persisted_mana = s.value;
    }
    it.lock_expiration = instance.lock_expiration;
    if (!item_problem(it).empty()) return std::nullopt;
    return it;
}

// ---- the catalog ---------------------------------------------------------------------------------------------------------
// ~ C Y B :: C O A ~

std::optional<ItemInfo> ContentItemCatalog::info(int64_t record) const {
    std::string error;
    const auto rec = db_.item(record, error);
    if (!rec) {
        if (!error.empty() && !failure_logged_) {
            failure_logged_ = true;
            spdlog::error("[items] {}: items are read as unknown from now on (further failures are not logged)", error);
        }
        return std::nullopt;
    }
    ItemInfo i;
    i.equip_slots = rec->equip_slots;
    i.two_handed = rec->occupies_all_equip_slots;
    return i;
}

// ---- the inventory --------------------------------------------------------------------------------------------------------

std::optional<Inventory> Inventory::from_items(const std::vector<svc::InventoryItem>& items, std::string& error) {
    Inventory inv;
    if (items.size() > static_cast<size_t>(kMaxItems)) {
        error = std::to_string(items.size()) + " items, a character holds at most " + std::to_string(kMaxItems);
        return std::nullopt;
    }
    std::map<std::string, int> guids;
    for (size_t n = 0; n < items.size(); ++n) {
        const svc::InventoryItem& it = items[n];
        if (const std::string why = item_problem(it); !why.empty()) {
            error = "item " + std::to_string(n) + ": " + why;
            return std::nullopt;
        }
        const Key key{static_cast<uint8_t>(it.container), it.slot};
        if (!inv.slots_.emplace(key, it).second) {
            error = "item " + std::to_string(n) + ": two items in " + container_name(it.container) + " slot " + std::to_string(it.slot);
            return std::nullopt;
        }
        if (!guids.emplace(it.instance_guid, static_cast<int>(n)).second) {
            error = "item " + std::to_string(n) + ": instance guid " + it.instance_guid + " is the guid of item " + std::to_string(guids[it.instance_guid]);
            return std::nullopt;
        }
    }
    return inv;
}

const svc::InventoryItem* Inventory::find(svc::ItemContainer c, int slot) const {
    const auto it = slots_.find({static_cast<uint8_t>(c), slot});
    return it == slots_.end() ? nullptr : &it->second;
}

const svc::InventoryItem* Inventory::at(svc::ItemContainer container, int slot) const {
    if (!valid_container(container) || slot < 0 || slot >= slot_count(container)) return nullptr;
    return find(container, slot);
}

size_t Inventory::count(svc::ItemContainer container) const {
    size_t n = 0;
    for (const auto& [key, item] : slots_) n += key.first == static_cast<uint8_t>(container) ? 1 : 0;
    return n;
}

int Inventory::first_free(svc::ItemContainer container) const {
    for (int s = 0; s < slot_count(container); ++s) {
        if (!find(container, s)) return s;
    }
    return -1;
}

std::vector<svc::InventoryItem> Inventory::items() const {
    std::vector<svc::InventoryItem> out;
    out.reserve(slots_.size());
    for (const auto& [key, item] : slots_) out.push_back(item);   // the map orders by storage (Backpack, Equipment, Quest) and slot
    return out;
}

std::vector<int64_t> Inventory::records() const {
    std::vector<int64_t> out;
    out.reserve(slots_.size());
    for (const auto& [key, item] : slots_) out.push_back(item.item_record_id);
    return out;
}

Outcome Inventory::use(svc::ItemContainer storage, int slot, const std::string& expected_guid, uint32_t requested_equip_slot,
                       const ItemCatalog& catalog) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    if (storage != svc::ItemContainer::Backpack && storage != svc::ItemContainer::Equipment) {
        return refuse(std::string("items of the ") + (valid_container(storage) ? container_name(storage) : "unknown storage") + " are not used");
    }
    const svc::InventoryItem* item = at(storage, slot);
    if (!item) return refuse(std::string("no item in ") + container_name(storage) + " slot " + std::to_string(slot));
    if (!guid_of(expected_guid) || item->instance_guid != expected_guid) {
        return refuse(std::string("the item in ") + container_name(storage) + " slot " + std::to_string(slot) +
                      " is not the instance the client named (" + expected_guid + ")");
    }
    return storage == svc::ItemContainer::Backpack ? equip(slot, catalog, requested_equip_slot) : unequip(slot);
}

Outcome Inventory::equip(int bag_slot, const ItemCatalog& catalog, uint32_t requested) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    const svc::InventoryItem item = *find(svc::ItemContainer::Backpack, bag_slot);
// == C.O.A | C | Y | B ==

    std::map<int64_t, std::optional<ItemInfo>> cache;   // a record is asked of the catalog once per request
    const auto info_of = [&](int64_t record) -> const std::optional<ItemInfo>& {
        auto it = cache.find(record);
        if (it == cache.end()) it = cache.emplace(record, catalog.info(record)).first;
        return it->second;
    };
    const std::optional<ItemInfo>& info = info_of(item.item_record_id);
    if (!info) return refuse("item record " + std::to_string(item.item_record_id) + " is not in content.db");
    if (!info->wearable()) return refuse("item record " + std::to_string(item.item_record_id) + " is not worn in any slot");

    // a slot is covered when a worn item is in it, or a worn two-hander lists it
    const auto covered = [&](int s) {
        if (find(svc::ItemContainer::Equipment, s)) return true;
        for (const auto& [key, worn] : slots_) {
            if (key.first != static_cast<uint8_t>(svc::ItemContainer::Equipment)) continue;
            const auto& w = info_of(worn.item_record_id);
            if (w && w->two_handed && std::find(w->equip_slots.begin(), w->equip_slots.end(), static_cast<uint8_t>(s)) != w->equip_slots.end()) return true;
        }
        return false;
    };

    int target = -1;
    if (requested != 0xFFFFFFFFu) {
        if (requested > 255 || !contains(info->equip_slots, static_cast<uint8_t>(requested))) {
            return refuse("slot " + std::to_string(requested) + " is not one of the item's slots");
        }
        target = static_cast<int>(requested);
    } else if (info->two_handed) {
        target = info->equip_slots.front();   // a two-hander sits in its first slot and covers the others
    } else {
        for (const uint8_t s : info->equip_slots) {
            if (!covered(s)) {
                target = s;
                break;
            }
        }
        if (target < 0) target = info->equip_slots.front();
    }

    // what it covers once worn, and what that displaces to the bag: the items in those slots first, in the item's order, then any
    // two-hander elsewhere that covers one of them, by slot
    std::vector<int> footprint;
    if (info->two_handed) footprint.assign(info->equip_slots.begin(), info->equip_slots.end());
    else footprint.push_back(target);
    std::vector<int> displaced;
    for (const int s : footprint) {
        if (find(svc::ItemContainer::Equipment, s) && !contains(displaced, s)) displaced.push_back(s);
    }
    for (int e = 0; e < kEquipmentSlots; ++e) {
        const svc::InventoryItem* worn = find(svc::ItemContainer::Equipment, e);
        if (!worn || contains(displaced, e)) continue;
        const auto& w = info_of(worn->item_record_id);
        if (!w || !w->two_handed) continue;
        for (const uint8_t s : w->equip_slots) {
            if (contains(footprint, static_cast<int>(s))) {
                displaced.push_back(e);
                break;
            }
        }
    }
    const size_t free_after = static_cast<size_t>(kBackpackSlots) - count(svc::ItemContainer::Backpack) + 1;   // the item leaves its slot
    if (displaced.size() > free_after) {
        return refuse("the bag has " + std::to_string(free_after) + " free slot(s) and wearing the item takes off " + std::to_string(displaced.size()) + " item(s)");
    }

    // apply, in the order the client is told
    const auto remove = [&](svc::ItemContainer c, int s) {
        Event e;
        e.type = repl::StorageEventType::Remove;
        e.container = c;
        e.slot = s;
        e.item = *find(c, s);
        e.instigated = false;   // the retail Remove of an equip names nobody
        slots_.erase({static_cast<uint8_t>(c), s});
        out.events.push_back(std::move(e));
    };
    const auto add = [&](svc::ItemContainer c, int s, svc::InventoryItem it) {
        it.container = c;
        it.slot = s;
        Event e;
        e.type = repl::StorageEventType::Add;
        e.container = c;
        e.slot = s;
        e.item = it;
        slots_[{static_cast<uint8_t>(c), s}] = std::move(it);
        out.events.push_back(std::move(e));
    };
    remove(svc::ItemContainer::Backpack, bag_slot);
    for (const int d : displaced) {
        const svc::InventoryItem moved = *find(svc::ItemContainer::Equipment, d);
        add(svc::ItemContainer::Backpack, first_free(svc::ItemContainer::Backpack), moved);
        remove(svc::ItemContainer::Equipment, d);
    }
    add(svc::ItemContainer::Equipment, target, item);
    out.ok = true;
    out.equipment_changed = true;
    return out;
}

Outcome Inventory::unequip(int equip_slot) {
    Outcome out;
    const svc::InventoryItem item = *find(svc::ItemContainer::Equipment, equip_slot);
    const int bag = first_free(svc::ItemContainer::Backpack);
    if (bag < 0) {
        out.refusal = "the bag is full";
        return out;
    }
    svc::InventoryItem moved = item;
    moved.container = svc::ItemContainer::Backpack;
    moved.slot = bag;
    Event add;
    add.type = repl::StorageEventType::Add;
    add.container = svc::ItemContainer::Backpack;
    add.slot = bag;
    add.item = moved;
    Event remove;
    remove.type = repl::StorageEventType::Remove;
    remove.container = svc::ItemContainer::Equipment;
    remove.slot = equip_slot;
    remove.item = item;
    remove.instigated = false;
    slots_.erase({static_cast<uint8_t>(svc::ItemContainer::Equipment), equip_slot});
    slots_[{static_cast<uint8_t>(svc::ItemContainer::Backpack), bag}] = moved;
    out.events = {std::move(add), std::move(remove)};
    out.ok = true;
    out.equipment_changed = true;
    return out;
}
// -- C Y B x C.O.A --

// ---- plan D1: consuming and granting ---------------------------------------------------------------------------------------

Outcome Inventory::consume(svc::ItemContainer storage, int slot, const std::string& guid, int32_t count) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    const svc::InventoryItem* item = at(storage, slot);
    if (!item) return refuse(std::string("no item in ") + container_name(storage) + " slot " + std::to_string(slot));
    if (!guid_of(guid) || item->instance_guid != guid) {
        return refuse(std::string("the item in ") + container_name(storage) + " slot " + std::to_string(slot) + " is not the instance named (" + guid + ")");
    }
    if (count < 1 || count > item->quantity) {
        return refuse("the stack in " + std::string(container_name(storage)) + " slot " + std::to_string(slot) + " holds " + std::to_string(item->quantity) +
                      ", not " + std::to_string(count) + " to consume");
    }
    const svc::InventoryItem before = *item;
    Event remove;
    remove.type = repl::StorageEventType::Remove;
    remove.container = storage;
    remove.slot = slot;
    remove.item = before;
    remove.instigated = true;
    out.events.push_back(std::move(remove));
    slots_.erase({static_cast<uint8_t>(storage), slot});
    if (before.quantity > count) {
        svc::InventoryItem left = before;
        left.quantity = before.quantity - count;
        Event add;
        add.type = repl::StorageEventType::Add;
        add.container = storage;
        add.slot = slot;
        add.item = left;
        add.instigated = true;
        slots_[{static_cast<uint8_t>(storage), slot}] = left;
        out.events.push_back(std::move(add));
    }
    out.ok = true;
    out.equipment_changed = storage == svc::ItemContainer::Equipment;
    return out;
}

const svc::InventoryItem* Inventory::find_guid(const std::string& guid) const {
    for (const auto& [key, item] : slots_) {
        if (item.instance_guid == guid) return &item;
    }
    return nullptr;
}

Outcome Inventory::update_mount(const std::string& guid, std::optional<float> health, int64_t lock_expiration, bool events) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    if (health && (!std::isfinite(*health) || *health < 0 || *health > 1e9f)) return refuse("a Health that is not a number from 0 to 1e9");
    if (lock_expiration > kMaxLockExpiration) return refuse("lock expiration " + std::to_string(lock_expiration) + " is out of range");
    for (auto& [key, item] : slots_) {
        if (item.instance_guid != guid) continue;
        const svc::InventoryItem before = item;
        if (health) item.persisted_health = *health;
        if (lock_expiration >= 0) item.lock_expiration = lock_expiration;
        out.ok = true;
        out.equipment_changed = false;
        if (events && !(before == item)) {
            Event remove;
            remove.type = repl::StorageEventType::Remove;
            remove.container = item.container;
            remove.slot = item.slot;
            remove.item = before;
            remove.instigated = true;
            out.events.push_back(std::move(remove));
            Event add;
            add.type = repl::StorageEventType::Add;
            add.container = item.container;
            add.slot = item.slot;
            add.item = item;
            add.instigated = true;
            out.events.push_back(std::move(add));
        }
        return out;
    }
    return refuse("no item holds the instance " + guid);
}
// C | O | A and Cyb

Outcome Inventory::grant(std::vector<svc::InventoryItem> items) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    if (items.empty()) return refuse("nothing to grant");
    const size_t free = static_cast<size_t>(kBackpackSlots) - count(svc::ItemContainer::Backpack);
    if (items.size() > free) {
        return refuse("the bag has " + std::to_string(free) + " free slot(s) and the grant is " + std::to_string(items.size()) + " item(s)");
    }
    if (slots_.size() + items.size() > static_cast<size_t>(kMaxItems)) return refuse("the character would hold more than " + std::to_string(kMaxItems) + " items");
    std::set<std::string> guids;
    for (const auto& [key, held] : slots_) guids.insert(held.instance_guid);
    for (size_t n = 0; n < items.size(); ++n) {
        svc::InventoryItem probe = items[n];
        probe.container = svc::ItemContainer::Backpack;
        probe.slot = 0;
        if (const std::string why = item_problem(probe); !why.empty()) return refuse("granted item " + std::to_string(n) + ": " + why);
        if (!guids.insert(probe.instance_guid).second) return refuse("granted item " + std::to_string(n) + ": its instance guid is one the character has");
    }
    for (svc::InventoryItem& it : items) {
        it.container = svc::ItemContainer::Backpack;
        it.slot = first_free(svc::ItemContainer::Backpack);
        Event add;
        add.type = repl::StorageEventType::Add;
        add.container = it.container;
        add.slot = it.slot;
        add.item = it;
        add.instigated = true;
        slots_[{static_cast<uint8_t>(it.container), it.slot}] = it;
        out.events.push_back(std::move(add));
    }
    out.ok = true;
    return out;
}

Outcome Inventory::move(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.refusal = std::move(why);
        return out;
    };
    if (from != svc::ItemContainer::Backpack || to != svc::ItemContainer::Backpack) return refuse("only moves inside the Backpack are made");
    if (from_slot == to_slot) return refuse("the item is moved onto its own slot");
    const svc::InventoryItem* item = at(from, from_slot);
    if (!item) return refuse("no item in Backpack slot " + std::to_string(from_slot));
    if (to_slot < 0 || to_slot >= kBackpackSlots) return refuse("Backpack slot " + std::to_string(to_slot) + " does not exist");
    if (!guid_of(guid) || item->instance_guid != guid) return refuse("the item in Backpack slot " + std::to_string(from_slot) + " is not the instance the client named");
    if (count != item->quantity) return refuse("the move names " + std::to_string(count) + " of a stack of " + std::to_string(item->quantity) + " (splitting is not made)");
    if (find(to, to_slot)) return refuse("Backpack slot " + std::to_string(to_slot) + " is not empty (a move onto an item is a swap)");
    svc::InventoryItem moved = *item;
    Event remove;
    remove.type = repl::StorageEventType::Remove;
    remove.container = from;
    remove.slot = from_slot;
    remove.item = moved;
    moved.slot = to_slot;
    Event add;
    add.type = repl::StorageEventType::Add;
    add.container = to;
    add.slot = to_slot;
    add.item = moved;
    slots_.erase({static_cast<uint8_t>(from), from_slot});
    slots_[{static_cast<uint8_t>(to), to_slot}] = moved;
    out.events = {std::move(remove), std::move(add)};
    out.ok = true;
    return out;
}
// [C.O.A x Cyb]

Outcome Inventory::swap(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot,
                        const std::string& to_guid, int32_t to_count) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.refusal = std::move(why);
        return out;
    };
    if (from != svc::ItemContainer::Backpack || to != svc::ItemContainer::Backpack) return refuse("only swaps inside the Backpack are made");
    if (from_slot == to_slot) return refuse("the item is swapped with itself");
    const svc::InventoryItem* a = at(from, from_slot);
    const svc::InventoryItem* b = at(to, to_slot);
    if (!a) return refuse("no item in Backpack slot " + std::to_string(from_slot));
    if (!b) return refuse("no item in Backpack slot " + std::to_string(to_slot) + " to swap with");
    if (!guid_of(guid) || a->instance_guid != guid) return refuse("the item in Backpack slot " + std::to_string(from_slot) + " is not the instance the client named");
    if (!guid_of(to_guid) || b->instance_guid != to_guid) return refuse("the item in Backpack slot " + std::to_string(to_slot) + " is not the instance the client named");
    if (count != a->quantity || to_count != b->quantity) return refuse("a swap names whole stacks only");
    svc::InventoryItem na = *a, nb = *b;   // as they were
    Event remove_a{repl::StorageEventType::Remove, from, from_slot, na};
    Event remove_b{repl::StorageEventType::Remove, to, to_slot, nb};
    svc::InventoryItem a_at_to = na, b_at_from = nb;
    a_at_to.slot = to_slot;
    b_at_from.slot = from_slot;
    Event add_b{repl::StorageEventType::Add, from, from_slot, b_at_from};
    Event add_a{repl::StorageEventType::Add, to, to_slot, a_at_to};
    slots_[{static_cast<uint8_t>(from), from_slot}] = b_at_from;
    slots_[{static_cast<uint8_t>(to), to_slot}] = a_at_to;
    out.events = {std::move(remove_a), std::move(remove_b), std::move(add_b), std::move(add_a)};
    out.ok = true;
    return out;
}

// ---- the visible gear ----------------------------------------------------------------------------------------------------

bool shows_in_appearance(int slot) { return (slot >= 1 && slot <= 9) || slot == equip::Costume || slot == equip::CloakCosmetic; }

std::array<int64_t, 23> held_records(const Inventory& inventory) {
    std::array<int64_t, 23> out{};
    for (size_t i = 0; i < kHeldSlots.size(); ++i) {
        if (const svc::InventoryItem* it = inventory.at(svc::ItemContainer::Equipment, kHeldSlots[i])) out[i] = it->item_record_id;
    }
    return out;
}

std::vector<repl::AppearanceId> appearance_ids(const Inventory& inventory) {
    std::vector<repl::AppearanceId> out;
    for (int s = 0; s < kEquipmentSlots; ++s) {
        if (!shows_in_appearance(s)) continue;
        const svc::InventoryItem* it = inventory.at(svc::ItemContainer::Equipment, s);
        if (!it) continue;
        repl::AppearanceId id;
        id.record_guid = static_cast<uint64_t>(it->item_record_id);
        id.type_id = kItemRecordTypeId;
        id.enchantment_level = 0;
        id.item_equip_slot = static_cast<uint8_t>(s);
        out.push_back(id);
    }
    return out;
}

}  // namespace aoc::worldd::items

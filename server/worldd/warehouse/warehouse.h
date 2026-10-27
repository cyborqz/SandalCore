// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  warehouse/warehouse.h  (storage clerks, step 0, "put it in, take it out"; header only)
//
//  A character's warehouse as the storage clerk's window shows it.  Pure logic over the design data and the character's items: no network, no clock, no database.
//
//  The containers.  A warehouse has three kinds, each a container the window lists:
//    * the item chunks (area Items): the same in every town, so a character's chunk is kept by its record and no town.  The free base chunk ("Default") holds 40 slots, a plain
//      list: an item takes one slot, its slot is its index.  It holds items (gear, tools, usables ...), never a material.
//    * the material tabs (area Materials): one set for each town.  The free base tab ("BaseStorage") is a grid of 8 x 16 cells, laid out like a material bag: an item takes inv_w x inv_h
//      cells and its slot is the row-major index of its top-left cell.  It holds materials (and Glint, which is one), never anything else.
//    * the cargo tab (area Cargo): one for each town of the Village level and up, a grid of 6 x 8.  It holds crates, and no crate exists yet, so nothing goes in.
//  What a character owns is the free ones; buying more is the next step.
//
//  A move.  The window asks the server to move a stack between a bag (the item bag or a material bag) and a warehouse container, or inside one container.  The server takes the whole
//  stack to an empty place (a stack is never split or merged: a move onto a stack of the same item is refused, as in the bags), or swaps two whole stacks of list containers.  What is
//  refused changes nothing and tells the client nothing but the storages as they are.  The answer is a Remove from the place the stack left and an Add where it arrived; the bag's side
//  carries the item toast, the warehouse's does not.
//
//  The rules of what may go where are the client's own (a material only in a material container, anything else only in an item container) and the owner's of the bags (the item bag
//  holds no materials).  A container the window does not have open, another town's tab and a place outside the container are refused.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "dialogue/expression.h"
#include "items/inventory.h"
#include "items/resource_grid.h"
#include "protocol/repl/warehouse_rpc.h"
#include "svc/services.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc::worldd::warehouse {

namespace repl = ::aoc::protocol::repl;

/// The kinds of container (the values of the character service's area column, by name).
enum class Area : uint8_t { Items, Materials, Cargo };

inline const char* area_name(Area a) { return a == Area::Items ? "Items" : a == Area::Materials ? "Materials" : "Cargo"; }
inline std::optional<Area> area_of(const std::string& name) {
    if (name == "Items") return Area::Items;
    if (name == "Materials") return Area::Materials;
    if (name == "Cargo") return Area::Cargo;
    return std::nullopt;
}

/// A container of a character's warehouse: its kind, the town (0 for an item chunk) and the record of the chunk or tab.
struct Key {
    Area    area = Area::Items;
    int64_t node = 0;
    int64_t record = 0;
    auto operator<=>(const Key&) const = default;
};

/// A container as the window shows it: its place in the data and its shape.  `columns` x `rows` is the grid of a tab; a chunk has none (columns 0).
struct Spec {
    Key  key;
    int  slots = 0;
    int  columns = 0, rows = 0;
    int  index = 0;           // a tab's ResourceStorageComponentIndex
    bool member = false;      // a tab only a citizen may have
    bool grid() const { return columns > 0; }
    bool operator==(const Spec&) const = default;
};

// ---- the facts the data's expressions ask about ---------------------------------------------------------------------------------------------------------------------

/// What an availability or cost expression is asked: the town the window is for and its level (the ENodeLevel ordinal), and whether the character is a citizen of it.  The functions are
/// the ones the data's warehouse expressions use; any other ends the evaluation with no value, and the expression does not hold.
class Facts final : public dialogue::ExprHost {
public:
    int64_t node = 0;
    int     level = 0;
    bool    citizen = false;   // there is no citizenship yet: nobody is

    std::optional<dialogue::ExprValue> call(const std::string& name, const std::vector<dialogue::ExprValue>& args) const override {
        using dialogue::ExprValue;
        constexpr int64_t kPlayer = 1;
        if (name == "GetPlayer") return ExprValue::of(kPlayer);
        if (name == "GetCurrentNode" || name == "Node_GetActorCityNode") return args.size() == 1 ? std::optional<ExprValue>(ExprValue::of(node)) : std::nullopt;
        if (name == "Node_GetLevel") return args.size() == 1 ? std::optional<ExprValue>(ExprValue::of(level)) : std::nullopt;
        if (name == "Node_IsActorInNodeName") return ExprValue::of(1);
        if (name == "Node_IsPlayerCitizenOfNode") return ExprValue::of(citizen ? 1 : 0);
        if (name == "GetNodeToPlayerReputationRank") {
            ExprValue v;
            v.text = "ENodeReputationRank::None";   // no reputation tier exists yet: the character is nobody's citizen, noble or liege
            return v;
        }
        if (name == "EnumInt") {
            if (args.size() != 1) return std::nullopt;
            if (args[0].text.empty()) return ExprValue::of(args[0].i);
            const int level_of = level_named(args[0].text);
            if (level_of < 0) return std::nullopt;
            return ExprValue::of(level_of);
        }
        return std::nullopt;
    }
    int64_t random_range(int64_t lo, int64_t) const override { return lo; }

    /// The ENodeLevel ordinal of "ENodeLevel::Village" (or "Village"); -1 for a name that is none.
    static int level_named(const std::string& text) {
        static constexpr const char* kLevels[] = {"Wilderness", "Crossroads", "Encampment", "Village", "Town", "City", "Metropolis"};
        const size_t colons = text.rfind("::");
        const std::string name = colons == std::string::npos ? text : text.substr(colons + 2);
        for (int i = 0; i < 7; ++i) {
            if (name == kLevels[i]) return i;
        }
        return -1;
    }
};

/// Whether an availability expression holds (the empty text always does; one that does not read or calls an unknown function does not).
inline bool holds(const std::string& expression, const Facts& facts) {
    if (expression.find_first_not_of(" \t\r\n") == std::string::npos) return true;
    const auto v = dialogue::evaluate_expression(expression, facts);
    return v && v->i != 0;
}

/// The amount of a cost: the value expression times the multiplier (an empty multiplier is 1), in the currency's own units.  nullopt when an expression does not read.
inline std::optional<double> cost_amount(const content::WarehouseCostRow& cost, const Facts& facts) {
    const auto value = dialogue::evaluate_expression(cost.value, facts);
    if (!value) return std::nullopt;
    double mult = 1.0;
    if (cost.multiplier.find_first_not_of(" \t\r\n") != std::string::npos) {
        const auto m = dialogue::evaluate_expression(cost.multiplier, facts);
        if (!m) return std::nullopt;
        mult = static_cast<double>(m->i);
    }
    return static_cast<double>(value->i) * mult;
}

// ---- the book: the data -----------------------------------------------------------------------------------------------------------------------------------------

class Book {
public:
    Book() = default;
    explicit Book(content::WarehouseData data) : d_(std::move(data)) {}

    size_t nodes() const { return d_.nodes.size(); }
    size_t chunks() const { return d_.chunks.size(); }
    size_t tabs() const { return d_.tabs.size(); }
    const content::WarehouseData::Node* node(int64_t guid) const {
        const auto it = d_.nodes.find(guid);
        return it == d_.nodes.end() ? nullptr : &it->second;
    }

    /// The summary the window asks for: the two defs the town names and, in the defs' order, every item chunk, material tab and cargo tab with whether the character may buy it and what it
    /// costs (the free ones list no cost).  nullopt for a town that has no warehouse.
    std::optional<repl::WarehouseSummary> summary(int64_t node_guid, const Facts& facts) const {
        const content::WarehouseData::Node* n = node(node_guid);
        if (!n) return std::nullopt;
        repl::WarehouseSummary s;
        s.id = repl::WarehouseId{repl::kWarehouseAreaNode, node_guid};
        s.resource_def = n->resource_def;
        s.crate_def = n->crate_def;
        if (const auto it = d_.chunk_defs.find(n->chunks_def); it != d_.chunk_defs.end()) {
            for (const int64_t guid : it->second) {
                const auto c = d_.chunks.find(guid);
                if (c == d_.chunks.end()) continue;
                s.chunks.push_back(entry(guid, c->second.requirement, c->second.costs, facts));
            }
        }
        const auto tabs_of = [&](int64_t def, std::vector<repl::WarehouseSummaryEntry>& out) {
            const auto d = d_.defs.find(def);
            if (d == d_.defs.end()) return;
            for (const int64_t guid : d->second.tabs) {
                const auto t = d_.tabs.find(guid);
                if (t == d_.tabs.end()) continue;
                out.push_back(entry(guid, t->second.requirement, t->second.costs, facts));
            }
        };
        tabs_of(n->resource_def, s.tabs);
        tabs_of(n->crate_def, s.crates);
        return s;
    }

    /// The containers a character has in a town's warehouse: the free base chunk, the free base tab and, where its requirement holds, the free cargo tab.  Empty for a town that has no
    /// warehouse.  (Buying more is the next step; what a character owns is the first of each def that costs nothing.)
    std::vector<Spec> owned(int64_t node_guid, const Facts& facts) const {
        std::vector<Spec> out;
        const content::WarehouseData::Node* n = node(node_guid);
        if (!n) return out;
        if (const auto it = d_.chunk_defs.find(n->chunks_def); it != d_.chunk_defs.end()) {
            for (const int64_t guid : it->second) {
                const auto c = d_.chunks.find(guid);
                if (c == d_.chunks.end() || !c->second.costs.empty() || !holds(c->second.requirement, facts)) continue;
                Spec s;
                s.key = Key{Area::Items, 0, guid};
                s.slots = c->second.size;
                out.push_back(s);
                break;   // the first free one: the character's base chunk
            }
        }
        const auto first_free_tab = [&](int64_t def, Area area) {
            const auto d = d_.defs.find(def);
            if (d == d_.defs.end()) return;
            for (const int64_t guid : d->second.tabs) {
                const auto t = d_.tabs.find(guid);
                if (t == d_.tabs.end() || !t->second.costs.empty() || t->second.member || !holds(t->second.requirement, facts)) continue;
                if ((area == Area::Cargo) != (t->second.storage_type == 1)) continue;
                Spec s;
                s.key = Key{area, node_guid, guid};
                s.columns = d->second.columns;
                s.rows = d->second.rows;
                s.slots = d->second.columns * d->second.rows;
                s.index = 0;
                s.member = t->second.member;
                out.push_back(s);
                break;
            }
        };
        first_free_tab(n->resource_def, Area::Materials);
        first_free_tab(n->crate_def, Area::Cargo);
        return out;
    }

    /// The character's base item chunk, the same in every town: the first free chunk of a town's chunk def (the item chunks do not depend on the town, so any town gives it).  It is made
    /// and listed when the character joins, as retail's login does, not when a window opens.  nullopt when the data holds none.
    std::optional<Spec> base_chunk() const {
        const Facts facts;   // (a base chunk asks nothing of the town)
        for (const auto& [guid, n] : d_.nodes) {
            for (const Spec& s : owned(guid, facts)) {
                if (s.key.area == Area::Items) return s;
            }
        }
        return std::nullopt;
    }

private:
    repl::WarehouseSummaryEntry entry(int64_t guid, const std::string& requirement, const std::vector<content::WarehouseCostRow>& costs, const Facts& facts) const {
        repl::WarehouseSummaryEntry e;
        e.record = guid;
        e.available = holds(requirement, facts);
        for (const content::WarehouseCostRow& c : costs) {
            const auto amount = cost_amount(c, facts);
            e.costs.push_back({c.currency, c.type_id, amount ? *amount : 0.0});
        }
        return e;
    }

    content::WarehouseData d_;
};

// ---- the character's stored items -------------------------------------------------------------------------------------------------------------------------------

/// What a character keeps in its warehouse: the items of every container by place.  A place holds one stack; an instance is one item (the character service checks that it is not also in a
/// bag).
class Storage {
public:
    /// Loads the rows the character service sent (a row of an area or town this build does not know is kept: it is saved back as it was).
    void load(const std::vector<svc::WarehouseItem>& rows) {
        items_.clear();
        for (const svc::WarehouseItem& w : rows) {
            const auto area = area_of(w.area);
            if (!area) continue;
            items_[{Key{*area, w.node_guid, w.container_guid}, w.item.slot}] = w.item;
        }
    }

    std::vector<svc::WarehouseItem> rows() const {
        std::vector<svc::WarehouseItem> out;
        out.reserve(items_.size());
        for (const auto& [place, item] : items_) {
            svc::WarehouseItem w;
            w.area = area_name(place.first.area);
            w.node_guid = place.first.node;
            w.container_guid = place.first.record;
            w.item = item;
            w.item.container = svc::ItemContainer::Backpack;
            w.item.slot = place.second;
            out.push_back(std::move(w));
        }
        return out;
    }

    size_t size() const { return items_.size(); }
    const svc::InventoryItem* at(const Key& key, int slot) const {
        const auto it = items_.find({key, slot});
        return it == items_.end() ? nullptr : &it->second;
    }
    /// The items of one container, by slot.
    std::vector<svc::InventoryItem> items_of(const Key& key) const {
        std::vector<svc::InventoryItem> out;
        for (auto it = items_.lower_bound({key, 0}); it != items_.end() && it->first.first == key; ++it) out.push_back(it->second);
        return out;
    }
    bool holds_guid(const std::string& guid) const {
        for (const auto& [place, item] : items_) {
            if (item.instance_guid == guid) return true;
        }
        return false;
    }
    void put(const Key& key, int slot, svc::InventoryItem item) {
        item.container = svc::ItemContainer::Backpack;
        item.slot = slot;
        items_[{key, slot}] = std::move(item);
    }
    void erase(const Key& key, int slot) { items_.erase({key, slot}); }

private:
    std::map<std::pair<Key, int>, svc::InventoryItem> items_;
};

// ---- a move ------------------------------------------------------------------------------------------------------------------------------------------------------

/// One end of a request: a bag (the item bag or a material bag) or a warehouse container, and the slot.
struct End {
    bool               in_warehouse = false;
    svc::ItemContainer bag = svc::ItemContainer::Backpack;
    Key                key;
    int                slot = 0;
};

struct Request {
    repl::StorageEventType type = repl::StorageEventType::Move;   // Move, Fill or Swap
    End                    from, to;
    int32_t                count = 0;      // what the client says the source slot holds
    std::string            guid;           // the instance in the source slot
    std::string            to_guid;        // a Swap: the instance in the target slot
    int32_t                to_count = 0;
};

/// One event the client is told, in order: an Add or a Remove at a bag or a warehouse place.
struct Step {
    repl::StorageEventType type = repl::StorageEventType::Add;
    bool                   in_warehouse = false;
    svc::ItemContainer     bag = svc::ItemContainer::Backpack;
    Key                    key;
    int                    slot = 0;
    svc::InventoryItem     item;
};

struct Result {
    bool              ok = false;
    std::string       refusal;       // why not (for the log); empty when ok
    std::vector<Step> steps;         // empty when refused
    bool              bags_changed = false;       // a bag's items changed (the inventory is to be saved)
};

/// What the window has open: the containers of the town it was opened for.
struct OpenWindow {
    int64_t           node = 0;
    std::vector<Spec> specs;
    const Spec* find(const Key& key) const {
        for (const Spec& s : specs) {
            if (s.key == key) return &s;
        }
        return nullptr;
    }
};

namespace detail {

inline bool is_bag(svc::ItemContainer c) { return c == svc::ItemContainer::Backpack || items::is_resource(c); }

/// The cells a grid container's items cover, `ignore` aside.
inline std::vector<bool> covered(const Spec& spec, const Storage& wh, const items::ResourceGrid& grid, const items::ItemCatalog& catalog, const std::string* ignore = nullptr) {
    std::vector<bool> cells(static_cast<size_t>(grid.cells()), false);
    for (const svc::InventoryItem& it : wh.items_of(spec.key)) {
        if (ignore && it.instance_guid == *ignore) continue;
        const auto info = catalog.info(it.item_record_id);
        grid.cover(cells, it.slot, info ? info->inv_w : 1, info ? info->inv_h : 1);
    }
    return cells;
}

/// Whether `item` may be put in warehouse place (`spec`, `slot`) when the stack `ignore` (the one that is moving) does not count.  Empty when it may, else why not.
inline std::string why_not_here(const Spec& spec, int slot, const svc::InventoryItem& item, const Storage& wh, const items::ItemCatalog& catalog, const std::string* ignore) {
    const auto info = catalog.info(item.item_record_id);
    if (!info) return "the item's record is not one this server knows";
    switch (spec.key.area) {
        case Area::Items:
            if (info->resource) return "a material is not put in an item chunk (materials go to a material tab)";
            break;
        case Area::Materials:
            if (!info->resource) return "only a material is put in a material tab";
            break;
        case Area::Cargo:
            return "the cargo tab holds crates, and no crate exists yet";
    }
    if (slot < 0 || slot >= spec.slots) return "slot " + std::to_string(slot) + " is not a slot of the container";
    if (spec.grid()) {
        const items::ResourceGrid grid(spec.columns, spec.rows, false);
        if (!grid.fits(slot, info->inv_w, info->inv_h, covered(spec, wh, grid, catalog, ignore))) {
            return "a " + std::to_string(info->inv_w) + " x " + std::to_string(info->inv_h) + " item does not fit slot " + std::to_string(slot) + " (outside the tab or on another item)";
        }
    } else {
        const svc::InventoryItem* there = wh.at(spec.key, slot);
        if (there && !(ignore && there->instance_guid == *ignore)) return "slot " + std::to_string(slot) + " is not empty (a move onto an item is a swap, and a stack is not merged)";
    }
    return {};
}

}  // namespace detail

/// Applies a request to the bags and the warehouse, or refuses it and changes nothing.  `open` is what the window has open.
inline Result transfer(items::Inventory& bags, Storage& wh, const items::ItemCatalog& catalog, const OpenWindow& open, const Request& r) {
    Result out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.steps.clear();
        return out;
    };
    if (r.type != repl::StorageEventType::Move && r.type != repl::StorageEventType::Fill && r.type != repl::StorageEventType::Swap) {
        return refuse("an event of type " + std::to_string(static_cast<int>(r.type)) + " is not made in the warehouse");
    }
    // the ends: a bag is the item bag or a material bag, a warehouse container is one the window has open
    const auto check_end = [&](const End& e) -> std::string {
        if (e.in_warehouse) {
            if (!open.find(e.key)) return "that container is not open in the warehouse window";
        } else if (!detail::is_bag(e.bag)) {
            return "only the item bag and the material bags exchange items with the warehouse";
        }
        return {};
    };
    if (std::string why = check_end(r.from); !why.empty()) return refuse("the source: " + why);
    if (std::string why = check_end(r.to); !why.empty()) return refuse("the target: " + why);
    if (!r.from.in_warehouse && !r.to.in_warehouse) return refuse("a move between two bags is the bags' own");
    if (r.from.in_warehouse == r.to.in_warehouse && r.from.in_warehouse && r.from.key == r.to.key && r.from.slot == r.to.slot) return refuse("the item is moved onto its own slot");
    if (r.from.in_warehouse && r.to.in_warehouse && r.from.key != r.to.key) return refuse("an item does not move from one warehouse container to another");

    // the stack that moves
    const svc::InventoryItem* source = r.from.in_warehouse ? wh.at(r.from.key, r.from.slot) : bags.at(r.from.bag, r.from.slot);
    if (!source) return refuse("no item in the source slot");
    if (items::guid_of(r.guid) == std::nullopt || source->instance_guid != r.guid) return refuse("the item in the source slot is not the instance the client named");
    if (r.count != source->quantity) return refuse("the move names " + std::to_string(r.count) + " of a stack of " + std::to_string(source->quantity) + " (splitting is not made)");
    const svc::InventoryItem moving = *source;

    if (r.type == repl::StorageEventType::Swap) {
        // two whole stacks of list containers change places: the item bag and an item chunk
        const auto is_list = [&](const End& e) { return e.in_warehouse ? !open.find(e.key)->grid() : e.bag == svc::ItemContainer::Backpack; };
        if (!is_list(r.from) || !is_list(r.to)) return refuse("only the stacks of the item bag and the item chunks are swapped");
        const svc::InventoryItem* other = r.to.in_warehouse ? wh.at(r.to.key, r.to.slot) : bags.at(r.to.bag, r.to.slot);
        if (!other) return refuse("no item in the target slot to swap with");
        if (items::guid_of(r.to_guid) == std::nullopt || other->instance_guid != r.to_guid) return refuse("the item in the target slot is not the instance the client named");
        if (r.to_count != other->quantity) return refuse("a swap names whole stacks only");
        const svc::InventoryItem swapped = *other;
        // (the item bag and an item chunk hold items and no materials, so each stack is allowed where the other one lies)
        // apply: the four events are the bags' own order (Remove A, Remove B, Add B where A was, Add A where B was)
        Step ra, rb, ab, aa;
        const auto fill = [&](Step& s, repl::StorageEventType type, const End& e, const svc::InventoryItem& item) {
            s.type = type;
            s.in_warehouse = e.in_warehouse;
            s.bag = e.bag;
            s.key = e.key;
            s.slot = e.slot;
            s.item = item;
        };
        fill(ra, repl::StorageEventType::Remove, r.from, moving);
        fill(rb, repl::StorageEventType::Remove, r.to, swapped);
        svc::InventoryItem b_at_from = swapped, a_at_to = moving;
        b_at_from.slot = r.from.slot;
        a_at_to.slot = r.to.slot;
        fill(ab, repl::StorageEventType::Add, r.from, b_at_from);
        fill(aa, repl::StorageEventType::Add, r.to, a_at_to);
        // the bags: a swap of the item bag's stack with a chunk's goes through the inventory's own pieces
        if (!r.from.in_warehouse || !r.to.in_warehouse) {
            const End& bag_end = r.from.in_warehouse ? r.to : r.from;
            const End& wh_end = r.from.in_warehouse ? r.from : r.to;
            const svc::InventoryItem& bag_item = r.from.in_warehouse ? swapped : moving;
            const svc::InventoryItem& wh_item = r.from.in_warehouse ? moving : swapped;
            items::Inventory trial = bags;
            const items::Outcome removed = trial.consume(bag_end.bag, bag_end.slot, bag_item.instance_guid, bag_item.quantity);
            if (!removed.ok) return refuse(removed.refusal);
            svc::InventoryItem arriving = wh_item;
            const items::Outcome added = trial.add_at(bag_end.bag, bag_end.slot, arriving, catalog);
            if (!added.ok) return refuse(added.refusal);
            bags = std::move(trial);
            wh.put(wh_end.key, wh_end.slot, bag_item);
            out.bags_changed = true;
        } else {
            wh.put(r.from.key, r.from.slot, swapped);
            wh.put(r.to.key, r.to.slot, moving);
        }
        out.steps = {ra, rb, ab, aa};
        out.ok = true;
        return out;
    }

    // Move and Fill: the whole stack to an empty place
    if (r.to.in_warehouse) {
        const Spec* spec = open.find(r.to.key);
        const std::string* ignore = r.from.in_warehouse ? &moving.instance_guid : nullptr;
        const std::string why = detail::why_not_here(*spec, r.to.slot, moving, wh, catalog, ignore);
        if (!why.empty()) return refuse(why);
        // it leaves a bag, or another place of the same container
        Step remove, add;
        remove.type = repl::StorageEventType::Remove;
        remove.in_warehouse = r.from.in_warehouse;
        remove.bag = r.from.bag;
        remove.key = r.from.key;
        remove.slot = r.from.slot;
        remove.item = moving;
        add.type = repl::StorageEventType::Add;
        add.in_warehouse = true;
        add.key = r.to.key;
        add.slot = r.to.slot;
        add.item = moving;
        add.item.slot = r.to.slot;
        if (r.from.in_warehouse) {
            wh.erase(r.from.key, r.from.slot);
        } else {
            const items::Outcome removed = bags.consume(r.from.bag, r.from.slot, moving.instance_guid, moving.quantity);
            if (!removed.ok) return refuse(removed.refusal);
            out.bags_changed = true;
        }
        wh.put(r.to.key, r.to.slot, moving);
        out.steps = {remove, add};
        out.ok = true;
        return out;
    }
    // out of the warehouse into a bag
    {
        svc::InventoryItem arriving = moving;
        const items::Outcome added = bags.add_at(r.to.bag, r.to.slot, arriving, catalog);
        if (!added.ok) return refuse(added.refusal);
        wh.erase(r.from.key, r.from.slot);
        Step remove, add;
        remove.type = repl::StorageEventType::Remove;
        remove.in_warehouse = true;
        remove.key = r.from.key;
        remove.slot = r.from.slot;
        remove.item = moving;
        add.type = repl::StorageEventType::Add;
        add.in_warehouse = false;
        add.bag = r.to.bag;
        add.slot = r.to.slot;
        add.item = moving;
        add.item.container = r.to.bag;
        add.item.slot = r.to.slot;
        out.bags_changed = true;
        out.steps = {remove, add};
        out.ok = true;
        return out;
    }
}

/// The payload a container sends the client: its slots (one element for each, an empty one where nothing lies) and its shape.
inline repl::StoragePayload payload_of(const Spec& spec, const Storage& wh) {
    repl::StoragePayload p;
    p.maximum_items = spec.slots;
    p.dimension = repl::Vec2d{static_cast<double>(spec.columns), static_cast<double>(spec.rows)};
    p.storage.resize(static_cast<size_t>(spec.slots));
    for (const svc::InventoryItem& it : wh.items_of(spec.key)) {
        if (it.slot < 0 || it.slot >= spec.slots) continue;
        repl::StorageSlot& slot = p.storage[static_cast<size_t>(it.slot)];
        slot.item = items::to_instance(it);
        slot.slot = it.slot;
    }
    return p;
}

}  // namespace aoc::worldd::warehouse

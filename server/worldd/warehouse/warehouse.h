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
//  A move.  The window asks the server to move a stack between a bag (the item bag or a material bag) and a warehouse container, or inside one container.  The server moves the whole
//  stack to an empty place or splits part of it there, joins a stack onto a stack it passes the joining rule with (the target keeps its guid and quality), or swaps two whole stacks.  What is
//  refused changes nothing and tells the client nothing but the storages as they are.  The answer is a Remove from the place the stack left and an Add where it arrived; the bag's side
//  carries the item toast, the warehouse's does not.  A swap answers with four events: the two Removes, then the two Adds.
//
//  The quick-send.  A Fill may name more than one target slot: the stacks it tops up and a free slot for what is left.  It is judged whole (every slot must take its part, or nothing
//  changes), and its answer is one Remove from the source and one Add for each target slot, in the request's order: an Add onto a stack carries that stack's guid and the part it took,
//  an Add into a free slot the new stack with a guid of its own.
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

/// One target slot of a Fill that names more than one: where the part goes, how many, and the stack it joins (empty for a free slot).
struct Target {
    End         to;
    int32_t     count = 0;
    std::string to_guid;      // the stack in the slot (empty: a free slot)
};

/// What the window asks for.  With `targets` empty the request has the one target `to` (its answer: one Remove and one Add, or a swap's four events); a Fill with `targets` names each
/// target slot there (its answer: one Remove, then one Add for each of them).
struct Request {
    repl::StorageEventType type = repl::StorageEventType::Move;   // Move, Fill or Swap
    End                    from, to;
    int32_t                count = 0;      // what the client says the source slot holds
    std::string            guid;           // the instance in the source slot
    std::string            to_guid;        // a Swap: the instance in the target slot
    int32_t                to_count = 0;
    std::vector<Target>    targets;        // a Fill that names more than one target slot: the slots and what goes in each (empty: the single `to`)
};

/// One event the client is told, in order: an Add or a Remove at a bag or a warehouse place.
struct Step {
    repl::StorageEventType type = repl::StorageEventType::Add;
    bool                   in_warehouse = false;
    svc::ItemContainer     bag = svc::ItemContainer::Backpack;
    Key                    key;
    int                    slot = 0;
    svc::InventoryItem     item;
    int32_t                count = 0;   // a Remove of part of a stack: how many leave, `item` being the stack as it was (0: the whole of it)
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

/// The cells a grid container's items cover, `ignore` and `ignore2` aside.
inline std::vector<bool> covered(const Spec& spec, const Storage& wh, const items::ResourceGrid& grid, const items::ItemCatalog& catalog, const std::string* ignore = nullptr,
                                 const std::string* ignore2 = nullptr) {
    std::vector<bool> cells(static_cast<size_t>(grid.cells()), false);
    for (const svc::InventoryItem& it : wh.items_of(spec.key)) {
        if (ignore && it.instance_guid == *ignore) continue;
        if (ignore2 && it.instance_guid == *ignore2) continue;
        const auto info = catalog.info(it.item_record_id);
        grid.cover(cells, it.slot, info ? info->inv_w : 1, info ? info->inv_h : 1);
    }
    return cells;
}

/// Whether `item` may be put in warehouse place (`spec`, `slot`) when the stacks `ignore` and `ignore2` (the ones that are moving) do not count.  Empty when it may, else why not.
inline std::string why_not_here(const Spec& spec, int slot, const svc::InventoryItem& item, const Storage& wh, const items::ItemCatalog& catalog, const std::string* ignore,
                                const std::string* ignore2 = nullptr) {
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
        if (!grid.fits(slot, info->inv_w, info->inv_h, covered(spec, wh, grid, catalog, ignore, ignore2))) {
            return "a " + std::to_string(info->inv_w) + " x " + std::to_string(info->inv_h) + " item does not fit slot " + std::to_string(slot) + " (outside the tab or on another item)";
        }
    } else {
        const svc::InventoryItem* there = wh.at(spec.key, slot);
        if (there && !(ignore && there->instance_guid == *ignore) && !(ignore2 && there->instance_guid == *ignore2)) {
            return "slot " + std::to_string(slot) + " is not empty (a move onto an item is a swap)";
        }
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
    if (r.count < 1 || r.count > source->quantity) return refuse("the move names " + std::to_string(r.count) + " of a stack of " + std::to_string(source->quantity));
    const svc::InventoryItem moving = *source;
    const svc::InventoryItem* target = r.to.in_warehouse ? wh.at(r.to.key, r.to.slot) : bags.at(r.to.bag, r.to.slot);
    const bool target_named = items::guid_of(r.to_guid).has_value();
    const Spec* from_spec = r.from.in_warehouse ? open.find(r.from.key) : nullptr;
    const Spec* to_spec = r.to.in_warehouse ? open.find(r.to.key) : nullptr;
    const auto is_list = [&](const End& e) { return e.in_warehouse ? !open.find(e.key)->grid() : e.bag == svc::ItemContainer::Backpack; };
    const auto info = catalog.info(moving.item_record_id);
    Storage trial_wh = wh;
    items::Inventory trial_bags = bags;
    // takes `count` off a stack on the copies: a part of it, or the whole stack
    const auto take_from = [&](const End& e, const svc::InventoryItem& item, int32_t count) -> std::string {
        if (e.in_warehouse) {
            if (count >= item.quantity) trial_wh.erase(e.key, e.slot);
            else {
                svc::InventoryItem left = item;
                left.quantity -= count;
                trial_wh.put(e.key, e.slot, left);
            }
            return {};
        }
        const items::Outcome removed = trial_bags.consume(e.bag, e.slot, item.instance_guid, count);
        return removed.ok ? std::string() : removed.refusal;
    };
    // puts a stack at an end on the copies
    const auto put_at = [&](const End& e, svc::InventoryItem item) -> std::string {
        if (e.in_warehouse) {
            trial_wh.put(e.key, e.slot, item);
            return {};
        }
        item.container = e.bag;
        const items::Outcome added = trial_bags.add_at(e.bag, e.slot, item, catalog);
        return added.ok ? std::string() : added.refusal;
    };
    const auto fill = [&](Step& s, repl::StorageEventType type, const End& e, const svc::InventoryItem& item, int32_t count) {
        s.type = type;
        s.in_warehouse = e.in_warehouse;
        s.bag = e.bag;
        s.key = e.key;
        s.slot = e.slot;
        s.item = item;
        s.count = count;
    };

    if (r.type == repl::StorageEventType::Swap) {
        // two whole stacks change places: both list places (the item bag and an item chunk) or both grids (a material bag and a material tab)
        if (!target) return refuse("no item in the target slot to swap with");
        if (!target_named || target->instance_guid != r.to_guid) return refuse("the item in the target slot is not the instance the client named");
        if (r.count != moving.quantity || r.to_count != target->quantity) return refuse("a swap names whole stacks only");
        if (is_list(r.from) != is_list(r.to)) return refuse("only stacks of the item bag and the item chunks, or of two grids, are swapped");
        if (r.from.in_warehouse && r.to.in_warehouse && r.from.key == r.to.key && to_spec->grid()) {
            // the final places are judged together: the moving stack's new place, then the target's against it and every other item (each checked on its own
            // with both ignored would let two rectangles of one grid land on one cell)
            const std::string why = detail::why_not_here(*to_spec, r.to.slot, moving, trial_wh, catalog, &target->instance_guid, &moving.instance_guid);
            if (!why.empty()) return refuse("the source: " + why);
            const items::ResourceGrid grid(to_spec->columns, to_spec->rows, false);
            std::vector<bool> cells = detail::covered(*to_spec, trial_wh, grid, catalog, &moving.instance_guid, &target->instance_guid);
            grid.cover(cells, r.to.slot, info->inv_w, info->inv_h);
            const auto target_info = catalog.info(target->item_record_id);
            if (!target_info) return refuse("the target: the item's record is not one this server knows");
            if (!grid.fits(r.from.slot, target_info->inv_w, target_info->inv_h, cells)) {
                return refuse("the target: a " + std::to_string(target_info->inv_w) + " x " + std::to_string(target_info->inv_h) + " item does not fit slot " +
                              std::to_string(r.from.slot) + " (outside the tab or on another item)");
            }
        } else {
            if (r.to.in_warehouse) {
                const std::string why = detail::why_not_here(*to_spec, r.to.slot, moving, trial_wh, catalog, &target->instance_guid, &moving.instance_guid);
                if (!why.empty()) return refuse("the source: " + why);
            }
            if (r.from.in_warehouse) {
                const std::string why = detail::why_not_here(*from_spec, r.from.slot, *target, trial_wh, catalog, &moving.instance_guid, &target->instance_guid);
                if (!why.empty()) return refuse("the target: " + why);
            }
        }
        if (std::string why = take_from(r.from, moving, moving.quantity); !why.empty()) return refuse(why);
        if (std::string why = take_from(r.to, *target, target->quantity); !why.empty()) return refuse(why);
        svc::InventoryItem b_at_from = *target;
        b_at_from.slot = r.from.slot;
        if (std::string why = put_at(r.from, b_at_from); !why.empty()) return refuse(why);
        svc::InventoryItem a_at_to = moving;
        a_at_to.slot = r.to.slot;
        if (std::string why = put_at(r.to, a_at_to); !why.empty()) return refuse(why);
        Step ra, rb, ab, aa;
        fill(ra, repl::StorageEventType::Remove, r.from, moving, 0);
        fill(rb, repl::StorageEventType::Remove, r.to, *target, 0);
        fill(ab, repl::StorageEventType::Add, r.from, b_at_from, 0);
        fill(aa, repl::StorageEventType::Add, r.to, a_at_to, 0);
        bags = std::move(trial_bags);
        wh = std::move(trial_wh);
        out.steps = {ra, rb, ab, aa};
        out.bags_changed = !r.from.in_warehouse || !r.to.in_warehouse;
        out.ok = true;
        return out;
    }

    if (!r.targets.empty()) {
        // a Fill that names more than one target slot: the existing stacks to top up, then the free slot for the rest (the client's quick-send)
        if (r.type != repl::StorageEventType::Fill) return refuse("a move that names more than one target slot is a Fill");
        if (!info) return refuse("the item's record is not one this server knows");
        const int32_t max_stack = std::max(1, info->max_stack);
        int64_t total = 0;
        for (const Target& t : r.targets) {
            if (t.count < 1) return refuse("a target slot names no amount");
            total += t.count;
        }
        if (total != r.count) return refuse("the move names " + std::to_string(r.count) + " but its target slots name " + std::to_string(total));
        for (size_t i = 0; i < r.targets.size(); ++i) {
            const Target& t = r.targets[i];
            for (size_t j = 0; j < i; ++j) {
                const End& earlier = r.targets[j].to;
                if (earlier.in_warehouse == t.to.in_warehouse && earlier.bag == t.to.bag && earlier.key == t.to.key && earlier.slot == t.to.slot) {
                    return refuse("the request names the same target slot twice");
                }
            }
            if (t.to.in_warehouse != r.to.in_warehouse || t.to.bag != r.to.bag || t.to.key != r.to.key) {
                return refuse("the target slots are not all in the target container");
            }
            if (t.to.in_warehouse == r.from.in_warehouse && t.to.in_warehouse && t.to.key == r.from.key && t.to.slot == r.from.slot) {
                return refuse("the item is moved onto its own slot");
            }
            if (!t.to.in_warehouse && !r.from.in_warehouse && t.to.bag == r.from.bag && t.to.slot == r.from.slot) {
                return refuse("the item is moved onto its own slot");
            }
        }
        // the whole request is judged on the copies: every target slot must take its part before anything is taken from the source, and each accepted target is applied
        // to the copies in turn, so a later target sees the raised stack or the cells a new stack took
        std::vector<Step> adds;
        adds.reserve(r.targets.size());
        for (const Target& t : r.targets) {
            const svc::InventoryItem* there = t.to.in_warehouse ? trial_wh.at(t.to.key, t.to.slot) : trial_bags.at(t.to.bag, t.to.slot);
            if (there) {
                if (!items::guid_of(t.to_guid) || there->instance_guid != t.to_guid) return refuse("the target slot holds an item, and the client did not name it");
                if (!items::stacks_with(moving, *there, info->rarity)) return refuse("the target holds another stack (a move onto an item is a swap)");
                const int32_t room = max_stack > there->quantity ? max_stack - there->quantity : 0;
                if (t.count > room) {
                    return refuse("the move names " + std::to_string(t.count) + " of a stack of " + std::to_string(moving.quantity) + "; only " + std::to_string(room) +
                                  " fit onto the target");
                }
                Step add;
                fill(add, repl::StorageEventType::Add, t.to, *there, 0);
                add.item.quantity = t.count;
                adds.push_back(std::move(add));
                // the top-up raises the target stack on the copies, so a later target sees it
                svc::InventoryItem merged = *there;
                merged.quantity = there->quantity + t.count;
                if (!t.to.in_warehouse) {
                    if (std::string why = take_from(t.to, *there, there->quantity); !why.empty()) return refuse(why);
                }
                if (std::string why = put_at(t.to, merged); !why.empty()) return refuse(why);
            } else {
                if (items::guid_of(t.to_guid).has_value()) return refuse("the target slot is empty, but the client named an item in it");
                // (a source stack larger than its record allows is bad data: its part for a free slot may not make another stack of that kind)
                if (t.count > max_stack) return refuse("the move names " + std::to_string(t.count) + " for a free slot; a stack of this item holds " + std::to_string(max_stack));
                if (t.to.in_warehouse) {
                    const std::string* ignore = r.from.in_warehouse && total >= moving.quantity ? &moving.instance_guid : nullptr;
                    const std::string why = detail::why_not_here(*to_spec, t.to.slot, moving, trial_wh, catalog, ignore);
                    if (!why.empty()) return refuse(why);
                }
                svc::InventoryItem arriving = moving;
                arriving.quantity = t.count;
                arriving.slot = t.to.slot;
                if (!t.to.in_warehouse) arriving.container = t.to.bag;
                if (total < moving.quantity || r.targets.size() > 1) {
                    do {
                        arriving.instance_guid = bags.fresh_guid();
                    } while (!items::guid_of(arriving.instance_guid) || bags.find_guid(arriving.instance_guid) || wh.holds_guid(arriving.instance_guid));
                }
                Step add;
                fill(add, repl::StorageEventType::Add, t.to, arriving, 0);
                adds.push_back(std::move(add));
                // the new stack takes its cells on the copies, so a later target sees them
                if (std::string why = put_at(t.to, arriving); !why.empty()) return refuse(why);
            }
        }
        if (std::string why = take_from(r.from, moving, static_cast<int32_t>(total)); !why.empty()) return refuse(why);
        Step remove;
        fill(remove, repl::StorageEventType::Remove, r.from, moving, total < moving.quantity ? static_cast<int32_t>(total) : 0);
        bags = std::move(trial_bags);
        wh = std::move(trial_wh);
        out.steps.push_back(std::move(remove));
        for (Step& s : adds) out.steps.push_back(std::move(s));
        out.bags_changed = !r.from.in_warehouse || !r.to.in_warehouse;
        out.ok = true;
        return out;
    }

    if (!target) {
        // an empty place: the whole stack moves, or a part of it becomes a new stack there (the client names no item in the place)
        if (target_named) return refuse("the target slot is empty, but the client named an item in it");
        const bool split = r.count < moving.quantity;
        // a whole stack leaves its cells, so a same-container move may slide over itself; a split leaves the source in place
        if (r.to.in_warehouse) {
            const std::string* ignore = r.from.in_warehouse && !split ? &moving.instance_guid : nullptr;
            const std::string why = detail::why_not_here(*to_spec, r.to.slot, moving, trial_wh, catalog, ignore);
            if (!why.empty()) return refuse(why);
        }
        svc::InventoryItem arriving = moving;
        if (split) {
            if (!info) return refuse("the item's record is not one this server knows");
            arriving.quantity = r.count;
            do {
                arriving.instance_guid = bags.fresh_guid();
            } while (!items::guid_of(arriving.instance_guid) || bags.find_guid(arriving.instance_guid) || wh.holds_guid(arriving.instance_guid));
        }
        arriving.slot = r.to.slot;
        if (!r.to.in_warehouse) arriving.container = r.to.bag;
        if (std::string why = take_from(r.from, moving, r.count); !why.empty()) return refuse(why);
        if (std::string why = put_at(r.to, arriving); !why.empty()) return refuse(why);
        Step remove, add;
        fill(remove, repl::StorageEventType::Remove, r.from, moving, split ? r.count : 0);
        fill(add, repl::StorageEventType::Add, r.to, arriving, 0);
        bags = std::move(trial_bags);
        wh = std::move(trial_wh);
        out.steps = {remove, add};
        out.bags_changed = !r.from.in_warehouse || !r.to.in_warehouse;
        out.ok = true;
        return out;
    }

    // onto a stack: it joins when the two stacks pass the rule, and the target keeps its own guid and quality
    if (!target_named || target->instance_guid != r.to_guid) return refuse("the target slot holds an item, and the client did not name it (a Move or a Fill to a free slot names no item)");
    if (!info) return refuse("the item's record is not one this server knows");
    if (!items::stacks_with(moving, *target, info->rarity)) return refuse("the target holds another stack (a move onto an item is a swap)");
    const int32_t max_stack = std::max(1, info->max_stack);
    const int32_t room = max_stack > target->quantity ? max_stack - target->quantity : 0;
    if (r.count > room) return refuse("the move names " + std::to_string(r.count) + " of a stack of " + std::to_string(moving.quantity) + "; only " + std::to_string(room) + " fit onto the target");
    if (std::string why = take_from(r.from, moving, r.count); !why.empty()) return refuse(why);
    svc::InventoryItem merged = *target;
    merged.quantity += r.count;
    if (!r.to.in_warehouse) {
        if (std::string why = take_from(r.to, *target, target->quantity); !why.empty()) return refuse(why);
    }
    if (std::string why = put_at(r.to, merged); !why.empty()) return refuse(why);
    Step remove, add;
    fill(remove, repl::StorageEventType::Remove, r.from, moving, r.count < moving.quantity ? r.count : 0);
    svc::InventoryItem added = *target;
    added.quantity = r.count;
    fill(add, repl::StorageEventType::Add, r.to, added, 0);
    bags = std::move(trial_bags);
    wh = std::move(trial_wh);
    out.steps = {remove, add};
    out.bags_changed = !r.from.in_warehouse || !r.to.in_warehouse;
    out.ok = true;
    return out;
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

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_resource.cpp: Gathering stage 3, part "Resource storage" (items/resource_grid.h, items/inventory.h)
//
//  the grid           the Tetris rule against the 12 slots of five retail captures (1198's Resource1 and Resource2 lists, 1469, 1470, 1471), the Sanctus bag's 6 x 4, the matrices
//                     of capture 1198's Mining and Lumberjacking bags
//  the catalog        ContentItemCatalog on the fixture: the dimension, the stack size, the resource flag and the bag grids of content.db v14
//  the inventory      the grids of the worn bags, what from_items refuses, ONE routing (a resource item to Resource0 at the first free rectangle, everything else to the Backpack,
//                     the Backpack untouched by a harvest), the top-up of a stack (same record and quality), a full container, a full Backpack that blocks nothing, moves inside
//                     and between the containers and every refusal, the bags that stay, a partial consume, the login's repair, a round trip through from_items
//  the real file      with AOC_CONTENT_DB (else dist/content/content.db of the working tree; "[skip]" without it): every one of the 10,568 items routed to exactly one kind (1,824
//                     resource), the 64 worn bags with a grid, none of the items larger than the Sanctus bag, ContentDb::resource_bag on the three bags of capture 1198
// ============================================================================
#include "content_db_fixture.h"
#include "items/inventory.h"
#include "items/resource_grid.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace fs = std::filesystem;
namespace cf = content_fixture;
namespace fi = content_fixture::item;
namespace fr = content_fixture::resource;
using svc::ItemContainer;
using Inv = items::Inventory;
using items::ResourceGrid;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const ItemContainer Bag = ItemContainer::Backpack, Worn = ItemContainer::Equipment, R0 = ItemContainer::Resource0, R1 = ItemContainer::Resource1,
                           R2 = ItemContainer::Resource2;

static std::string guid_n(uint32_t n) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xA0000000u + n, 0x1000u + n, 0x2000u + n, n);
    return buf;
}

static svc::InventoryItem make_item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1, int32_t quality = 1) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_n(n);
    it.quantity = quantity;
    it.quality = quality;
    it.durability = 0;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

// ---- the grid ---------------------------------------------------------------------------------------------------------------------
// built by C-O-A with C:Y:B

/// Places the items in order on a grid, as retail's server did: each at the first free rectangle.
static std::vector<int> place_all(const ResourceGrid& g, const std::vector<std::pair<int, int>>& dims) {
    std::vector<bool> covered;
    std::vector<int> slots;
    for (const auto& [w, h] : dims) {
        const int slot = g.place(w, h, covered);
        slots.push_back(slot);
        if (slot >= 0) g.cover(covered, slot, w, h);
    }
    return slots;
}

static void test_grid() {
    std::printf("the grid: the rule against the retail captures\n");
    const ResourceGrid sanctus(6, 4, false, fr::kSanctusMatrix);
    CHECK(sanctus.cells() == 24 && sanctus.usable_cells() == 24 && sanctus.override_matrix().empty() && !sanctus.blocked(1, 0),
          "the Sanctus bag: 6 x 4 = 24 cells, none blocked (its def's four-column matrix does not apply), no override matrix is sent");
    const ResourceGrid mining(8, 5, true, fr::kMiningMatrix);
    CHECK(mining.cells() == 40 && mining.usable_cells() == 24 && mining.override_matrix() == fr::kMiningMatrix && mining.blocked(2, 0) && mining.blocked(0, 2) && !mining.blocked(0, 0),
          "capture 1198's Mining T2 bag: 8 x 5 = 40 cells, 24 usable, the matrix as the client was sent it");
    const ResourceGrid lumber(6, 5, true, fr::kLumberMatrix);
    CHECK(lumber.cells() == 30 && lumber.usable_cells() == 18 && lumber.blocked(0, 0) && !lumber.blocked(1, 0) && lumber.blocked(1, 3), "capture 1198's Lumberjacking T2 bag: 6 x 5 = 30 cells, 18 usable");
    CHECK(ResourceGrid().empty() && ResourceGrid().cells() == 0 && ResourceGrid().place(1, 1, {}) == -1, "no bag: no cells, nothing fits");
    CHECK(ResourceGrid(0, 4, false).empty() && ResourceGrid(8, 33, false).empty() && !ResourceGrid(8, 30, false).empty(), "a grid has 1 to 256 cells (the Livestream bag's 8 x 30 = 240 is the largest)");

    // capture 1198, Resource1 (the Mining bag): Bass 2 x 1 at slot 0, the Bear carcass 2 x 2 at 3, Halcyonite 2 x 2 at 6, Bluegill 2 x 1 at 8
    CHECK(place_all(mining, {{2, 1}, {2, 2}, {2, 2}, {2, 1}}) == std::vector<int>({0, 3, 6, 8}), "1198 Resource1: the four captured slots 0, 3, 6, 8");
    // capture 1198, Resource2 (the Lumberjacking bag): Braidwood 1 x 3 at slot 1, the Willow stacks at 3 and 5
    CHECK(place_all(lumber, {{1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3}}) == std::vector<int>({1, 3, 5, 12, 14, 16}),
          "1198 Resource2: the three captured slots 1, 3, 5, then the next free 1 x 3 are rows 2 to 4 of the other columns");
    // capture 1470 (a Mining bag): Limestone 2 x 2 at slot 0, Coal 1 x 1 at 3, Sapphire 2 x 2 at 6 (columns 6-7: (3, 0) is taken)
    CHECK(place_all(mining, {{2, 2}, {1, 1}, {2, 2}}) == std::vector<int>({0, 3, 6}), "1470: Limestone at 0, Coal at 3, Sapphire at 6");
    // captures 1469 and 1471 (slots 24 and 27 of a Mining bag that held other items): free 2 x 2 rectangles of the grid
    CHECK(mining.fits(24, 2, 2, {}) && mining.fits(27, 2, 2, {}), "1469 slot 24 = (0, 3) and 1471 slot 27 = (3, 3) are free 2 x 2 rectangles of the Mining bag");
    CHECK(!mining.fits(2, 1, 1, {}) && !mining.fits(1, 2, 1, {}) && !mining.fits(7, 2, 1, {}) && !mining.fits(19, 1, 1, {}) && !mining.fits(40, 1, 1, {}) && !mining.fits(-1, 1, 1, {}),
          "a blocked cell, a rectangle across a blocked column, past the right edge, on a blocked row, outside the grid and a negative slot are no place");
    // the Sanctus bag: the 1 x 2 plants of captures 1427, 1429, 1454 and 1455 go left to right, then the next row of rectangles
    CHECK(place_all(sanctus, {{1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}}) == std::vector<int>({0, 1, 2, 3, 4, 5, 12}), "Sanctus: six plants 1 x 2 in the first row, the seventh two rows down");
    // a covered cell blocks, a rectangle in the last row cannot be taller
    std::vector<bool> covered;
    sanctus.cover(covered, 0, 6, 1);
    CHECK(sanctus.place(1, 1, covered) == 6 && sanctus.place(1, 4, covered) == -1 && sanctus.place(1, 3, covered) == 6 && sanctus.place(7, 1, covered) == -1,
          "the first row covered: the next free cell is slot 6; nothing 4 tall or 7 wide fits");
}

// ---- the fixture's catalog ----------------------------------------------------------------------------------------------------

static std::unique_ptr<content::ContentDb> g_db;
static std::unique_ptr<items::ContentItemCatalog> g_catalog;

static bool open_fixture(const fs::path& dir) {
    const fs::path path = dir / "resource.db";
    if (!cf::make_db(path)) return false;
    std::string error;
    auto db = content::ContentDb::open(path.string(), error);
    if (!db) {
        std::printf("  [FAIL] test setup: %s\n", error.c_str());
        return false;
    }
    g_db = std::make_unique<content::ContentDb>(std::move(*db));
    g_catalog = std::make_unique<items::ContentItemCatalog>(*g_db);
    return true;
}

static void test_catalog() {
    std::printf("the catalog: content.db v14 on the fixture\n");
    const items::ItemCatalog& c = *g_catalog;
    const auto oak = c.info(fr::kOak);
    CHECK(oak && oak->resource && oak->inv_w == 1 && oak->inv_h == 3 && oak->max_stack == 20 && !oak->bag && !oak->wearable(), "Resource_Raw_Wood_Oak: a resource, 1 x 3, stacks to 20");
    const auto glint = c.info(fr::kGlint);
    CHECK(glint && glint->resource && glint->inv_w == 1 && glint->max_stack == 10, "Certificate_T1_Glint: a resource that stacks to 10");
    const auto potion = c.info(fi::kPotion);
    CHECK(potion && !potion->resource && potion->inv_w == 1 && potion->inv_h == 1, "a potion is no resource and takes one cell");
    CHECK(!c.info(424242), "a record the file does not have is unknown");
    const auto sanctus = c.info(fi::kBag);
    CHECK(sanctus && !sanctus->resource && sanctus->bag && *sanctus->bag == ResourceGrid(6, 4, false, fr::kSanctusMatrix) && sanctus->bag->cells() == 24 && sanctus->wearable(),
          "Bag_Resource_Sanctus: a worn item that is no resource, with the 6 x 4 grid");
    const auto mining = c.info(fr::kBagMining);
    CHECK(mining && mining->bag && *mining->bag == ResourceGrid(8, 5, true, fr::kMiningMatrix), "the Mining T2 bag: the 8 x 5 grid with its matrix");
    const auto lumber = c.info(fr::kBagLumber);
    CHECK(lumber && lumber->bag && *lumber->bag == ResourceGrid(6, 5, true, fr::kLumberMatrix), "the Lumberjacking T2 bag: the 6 x 5 grid with its matrix");
    CHECK(!c.info(fi::kChest)->bag && !c.info(fi::kScepter)->resource, "gear has no grid and is no resource");
    // the file is read once per record
    const auto again = c.info(fr::kOak);
    CHECK(again && again->inv_h == 3, "a second ask for a record answers the same (from what was kept)");

    // ContentDb itself
    std::string error;
    const auto rec = g_db->item(fr::kBearCarcass, error);
    CHECK(rec && rec->resource && rec->inv_w == 2 && rec->inv_h == 2 && rec->max_stack_size == 20, "ContentDb::item reads inv_w, inv_h and resource");
    const auto named = g_db->item(fr::kOak, error);
    const auto unnamed = g_db->item(fr::kCoal, error);
    CHECK(named && named->display_name == "Oak Wood" && unnamed && unnamed->display_name.empty() && unnamed->name == "Resource_Raw_Stone_Coal",
          "ContentDb::item reads display_name (contentc version 16: the pick-up popup's name), empty for a record that has none");
    const auto bag = g_db->resource_bag(fr::kBagMining, error);
    CHECK(bag && bag->columns == 8 && bag->rows == 5 && bag->custom && bag->def == fr::kDefMining && bag->matrix.size() == 8 && bag->matrix[2][0] == -2 && bag->matrix[0][0] == -1,
          "ContentDb::resource_bag: the Mining bag's grid and matrix");
    const auto plain = g_db->resource_bag(fi::kBag, error);
    CHECK(plain && !plain->custom && plain->columns == 6 && plain->matrix.size() == 4, "ContentDb::resource_bag: the Sanctus bag is not custom and its matrix is kept");
    error.clear();
    CHECK(!g_db->resource_bag(fi::kPotion, error) && error.empty(), "an item that is no bag has no grid, and that is no error");
}

// ---- the inventory --------------------------------------------------------------------------------------------------------------

/// A character wearing the Sanctus bag (and what a test adds).
static std::vector<svc::InventoryItem> worn_kit(bool mining_in_24 = false, bool lumber_in_25 = false) {
    std::vector<svc::InventoryItem> v;
    v.push_back(make_item(Worn, 23, fi::kBag, 900));
    if (mining_in_24) v.push_back(make_item(Worn, 24, fr::kBagMining, 901));
    if (lumber_in_25) v.push_back(make_item(Worn, 25, fr::kBagLumber, 902));
    return v;
}

static std::optional<Inv> build(std::vector<svc::InventoryItem> v, std::string* why = nullptr) {
    std::string error;
    auto inv = Inv::from_items(v, error, g_catalog.get());
    if (!inv && why) *why = error;
    return inv;
}

static std::vector<svc::InventoryItem> with(std::vector<svc::InventoryItem> base, std::initializer_list<svc::InventoryItem> more) {
    for (const svc::InventoryItem& m : more) base.push_back(m);
    return base;
}

static size_t adds_to(const items::Outcome& o, ItemContainer c) {
    size_t n = 0;
    for (const items::Event& e : o.events) n += e.type == aoc::protocol::repl::StorageEventType::Add && e.container == c ? 1 : 0;
    return n;
}

static void test_from_items() {
    std::printf("from_items: the grids of the worn bags, and what a resource container holds\n");
    auto one = build(worn_kit());
    CHECK(one && one->grid(R0) == ResourceGrid(6, 4, false, fr::kSanctusMatrix) && one->grid(R1).empty() && one->grid(R2).empty() && one->grid(Bag).empty(),
          "a character with the Sanctus bag in slot 23 has Resource0, 6 x 4, and no other container");
    auto three = build(worn_kit(true, true));
    CHECK(three && three->grid(R0).cells() == 24 && three->grid(R1).cells() == 40 && three->grid(R2).cells() == 30 && three->grid(R1).override_matrix() == fr::kMiningMatrix &&
              three->grid(R2).override_matrix() == fr::kLumberMatrix,
          "capture 1198's three bags: containers of 24, 40 and 30 cells, the last two with their matrices");

    // items in the containers, as 1198's login lists: Bass, Bear, Halcyonite and Bluegill in Resource1; Braidwood and two Willow stacks in Resource2
    auto held = build(with(worn_kit(true, true), {make_item(R1, 0, fr::kBass, 1, 2), make_item(R1, 3, fr::kBearCarcass, 2), make_item(R1, 6, fr::kHalcyonite, 3, 10),
                                                  make_item(R1, 8, fr::kBluegill, 4), make_item(R2, 1, fr::kBraidwood, 5, 5), make_item(R2, 3, fr::kWillow, 6, 5, 6),
                                                  make_item(R2, 5, fr::kWillow, 7, 15), make_item(R0, 0, fr::kRune, 8, 3, 7), make_item(R0, 1, fr::kRune, 9, 1, 17)}));
    CHECK(held && held->count(R1) == 4 && held->count(R2) == 3 && held->count(R0) == 2 && held->size() == 12, "1198's resource lists load on the three grids");
// C:Y:B : C | O | A

    std::string why;
    CHECK(!build(with(worn_kit(), {make_item(R1, 0, fr::kOak, 1)}), &why) && why.find("Resource1") != std::string::npos && why.find("no resource bag") != std::string::npos,
          "an item in a container whose bag is not worn is refused, naming the container");
    CHECK(!build(with(worn_kit(), {make_item(R0, 24, fr::kRune, 1)}), &why) && why.find("does not fit") != std::string::npos, "a slot outside the 24 cells is refused");
    CHECK(!build(with(worn_kit(), {make_item(R0, 5, fr::kLimestone, 1)}), &why) && why.find("2 x 2") != std::string::npos, "a 2 x 2 item whose rectangle runs past the right edge is refused");
    CHECK(!build(with(worn_kit(true), {make_item(R1, 2, fr::kCoal, 1)}), &why) && why.find("blocked") != std::string::npos, "an item on a blocked cell is refused");
    CHECK(!build(with(worn_kit(), {make_item(R0, 0, fr::kOak, 1), make_item(R0, 6, fr::kDaffodil, 2)}), &why) && why.find("another item") != std::string::npos,
          "a daffodil (1 x 2) at (0, 1) on the oak (1 x 3 at (0, 0)) is refused");
    CHECK(build(with(worn_kit(), {make_item(R0, 0, fr::kOak, 1), make_item(R0, 1, fr::kOak, 2, 1, 6), make_item(R0, 18, fr::kRune, 3)})).has_value(), "neighbouring rectangles are fine");
    // without a catalog the grids stay empty and only the slot number is checked
    std::string error;
    auto bare = Inv::from_items(with(worn_kit(), {make_item(R2, 255, fr::kOak, 1)}), error);
    CHECK(bare && bare->grid(R0).empty(), "from_items without a catalog keeps the items and opens no grid");
    CHECK(!Inv::from_items({make_item(R0, 256, fr::kOak, 1)}, error) && error.find("slot 256") != std::string::npos, "...and still refuses a slot past 255");
    // a resource item in the Backpack still loads (the login repairs it)
    CHECK(build(with(worn_kit(), {make_item(Bag, 0, fr::kOak, 1)})).has_value(), "a resource item in the Backpack loads: the login's repair moves it");
}

static void test_routing() {
    std::printf("grant: one routing\n");
    auto inv = build(worn_kit());
    // an Oak stack and a potion in one grant: the oak to Resource0 slot 0, the potion to the lowest Backpack slot
    auto o = inv->grant({make_item(Bag, 0, fr::kOak, 1, 6), make_item(Bag, 0, fi::kPotion, 2, 3)}, *g_catalog);
    CHECK(o.ok && o.events.size() == 2 && o.events[0].container == R0 && o.events[0].slot == 0 && o.events[0].item.quantity == 6 && o.events[1].container == Bag && o.events[1].slot == 0,
          "an oak goes to Resource0 slot 0 and a potion to Backpack slot 0, as Add events in the order given");
    CHECK(o.events[0].type == aoc::protocol::repl::StorageEventType::Add && o.events[0].instigated && inv->count(R0) == 1 && inv->count(Bag) == 1, "...instigated, one in each storage");
    // a second oak stack of another quality: Resource0 slot 1; a daffodil next to it: (2, 0)
    o = inv->grant({make_item(Bag, 0, fr::kOak, 3, 5, 6), make_item(Bag, 0, fr::kDaffodil, 4, 3)}, *g_catalog);
    CHECK(o.ok && o.events.size() == 2 && o.events[0].slot == 1 && o.events[1].slot == 2, "the next rectangles in row-major order: slots 1 and 2");
    // with three bags the harvest goes to Resource0 first
    auto three = build(worn_kit(true, true));
    o = three->grant({make_item(Bag, 0, fr::kBass, 5, 2)}, *g_catalog);
    CHECK(o.ok && o.events.size() == 1 && o.events[0].container == R0, "with three bags the first container with room (Resource0) takes the item");
    // the kit's other kinds never go to a resource container, and a record the file does not know goes to the Backpack
    o = three->grant({make_item(Bag, 0, 424242, 6), make_item(Bag, 0, fi::kChest, 7)}, *g_catalog);
    CHECK(o.ok && adds_to(o, Bag) == 2 && three->count(R0) == 1, "an unknown record and a chest go to the Backpack");
    // refusals change nothing
    const auto before = inv->items();
    CHECK(!inv->grant({}, *g_catalog).ok, "nothing to grant is refused");
    auto bad = make_item(Bag, 0, fr::kOak, 1);   // the guid of an oak already held
    o = inv->grant({bad}, *g_catalog);
    CHECK(!o.ok && o.events.empty() && o.refusal.find("guid") != std::string::npos && inv->items() == before, "a guid the character already has is refused, nothing changed");
    auto zero = make_item(Bag, 0, fr::kOak, 8, 0);
    CHECK(!inv->grant({zero}, *g_catalog).ok && inv->items() == before, "a quantity of 0 is refused");
    // a grant to a character with no bag: a resource item has no home, a potion still does
    auto nobag = build({make_item(Worn, 2, fi::kChest, 1)});
    const auto r = nobag->grant_what_fits({make_item(Bag, 0, fr::kOak, 2, 4), make_item(Bag, 0, fi::kPotion, 3)}, *g_catalog);
    CHECK(r.outcome.ok && r.outcome.events.size() == 1 && r.outcome.events[0].container == Bag && r.rest.size() == 1 && r.rest[0].item_record_id == fr::kOak && r.rest[0].quantity == 4,
          "no bag worn: the oak comes back in `rest`, the potion is placed");
    CHECK(!nobag->grant({make_item(Bag, 0, fr::kOak, 4)}, *g_catalog).ok, "...and grant() refuses it whole");
}

static void test_merge() {
    std::printf("grant: topping up a stack\n");
    // an Oak stack of 15 at quality 1: ten more fill it to 20 (a Remove and an Add of the same instance) and start a stack of 5
    auto inv = build(with(worn_kit(), {make_item(R0, 0, fr::kOak, 1, 15)}));
    auto o = inv->grant({make_item(Bag, 0, fr::kOak, 2, 10)}, *g_catalog);
    CHECK(o.ok && o.events.size() == 3, "ten oak on a stack of 15: three events");
    const bool shape = o.events.size() == 3 && o.events[0].type == aoc::protocol::repl::StorageEventType::Remove && o.events[0].slot == 0 && o.events[0].item.quantity == 15 &&
                       o.events[1].type == aoc::protocol::repl::StorageEventType::Add && o.events[1].slot == 0 && o.events[1].item.quantity == 20 &&
                       o.events[1].item.instance_guid == o.events[0].item.instance_guid && o.events[2].type == aoc::protocol::repl::StorageEventType::Add && o.events[2].slot == 1 &&
                       o.events[2].item.quantity == 5 && o.events[2].item.instance_guid == guid_n(2);
    CHECK(shape, "Remove the stack of 15, Add it again with 20 (the same instance), Add a new stack of 5 at the next free rectangle");
    CHECK(inv->at(R0, 0)->quantity == 20 && inv->at(R0, 1)->quantity == 5 && inv->count(R0) == 2, "the inventory holds 20 and 5");
    // another quality does not merge, nor does a full stack
    inv = build(with(worn_kit(), {make_item(R0, 0, fr::kOak, 1, 15)}));
    o = inv->grant({make_item(Bag, 0, fr::kOak, 2, 5, 6)}, *g_catalog);
    CHECK(o.ok && o.events.size() == 1 && o.events[0].slot == 1 && inv->at(R0, 0)->quantity == 15, "an oak of quality 6 does not join a stack of quality 1");
    inv = build(with(worn_kit(), {make_item(R0, 0, fr::kOak, 1, 20)}));
    o = inv->grant({make_item(Bag, 0, fr::kOak, 2, 5)}, *g_catalog);
    CHECK(o.ok && o.events.size() == 1 && o.events[0].slot == 1, "a stack that is full is not topped up");
    // another record does not merge
    inv = build(with(worn_kit(), {make_item(R0, 0, fr::kOak, 1, 5)}));
    o = inv->grant({make_item(Bag, 0, fr::kWillow, 2, 5)}, *g_catalog);
    CHECK(o.ok && o.events.size() == 1 && o.events[0].item.item_record_id == fr::kWillow, "another record starts its own stack");
    // the stack that has room is found in any container: Coal x19 in Resource1 is topped up, the 4 left go to the first container with room, Resource0
    inv = build(with(worn_kit(true), {make_item(R1, 0, fr::kCoal, 1, 19)}));
    o = inv->grant({make_item(Bag, 0, fr::kCoal, 2, 5)}, *g_catalog);
    CHECK(o.ok && o.events.size() == 3 && o.events[0].container == R1 && o.events[1].container == R1 && o.events[1].item.quantity == 20 && o.events[2].container == R0 &&
              o.events[2].item.quantity == 4,
          "a stack with room in Resource1 is topped up first, the rest of the grant starts a stack in Resource0");
    // a stack of an item that stacks to 10: the Glint certificates
    inv = build(with(worn_kit(), {make_item(R0, 0, fr::kGlint, 1, 8)}));
    o = inv->grant({make_item(Bag, 0, fr::kGlint, 2, 5)}, *g_catalog);
    CHECK(o.ok && o.events.size() == 3 && o.events[1].item.quantity == 10 && o.events[2].item.quantity == 3, "a record that stacks to 10 stops at 10");
    // several items of one grant meet the same stack: 8 + 5 + 5 certificates on an empty bag are 10, 8
    inv = build(worn_kit());
    o = inv->grant({make_item(Bag, 0, fr::kGlint, 1, 8), make_item(Bag, 0, fr::kGlint, 2, 5), make_item(Bag, 0, fr::kGlint, 3, 5)}, *g_catalog);
    std::vector<int32_t> held;
    for (int s = 0; s < 24; ++s) {
        if (const svc::InventoryItem* it = inv->at(R0, s)) held.push_back(it->quantity);
    }
    CHECK(o.ok && held == std::vector<int32_t>({10, 8}), "18 certificates in one grant: a stack of 10 and a stack of 8");
}

static void test_full() {
    std::printf("grant: a full container, a full Backpack\n");
    auto inv = build(worn_kit());
    // 1 x 3 oaks in a 6 x 4 grid: one per column (rows 0 to 2), six in all
    std::vector<svc::InventoryItem> eight;
    for (uint32_t i = 1; i <= 8; ++i) eight.push_back(make_item(Bag, 0, fr::kOak, i, 20, static_cast<int32_t>(i)));   // distinct qualities: none joins another
    const auto r = inv->grant_what_fits(eight, *g_catalog);
    CHECK(r.outcome.ok && r.outcome.events.size() == 6 && inv->count(R0) == 6 && r.rest.size() == 2 && r.rest[0].quality == 7 && r.rest[1].quality == 8, "eight oak stacks: six fit the 6 x 4 bag, two are returned");
    bool slots_ok = true;
    for (size_t i = 0; i < r.outcome.events.size(); ++i) slots_ok = slots_ok && r.outcome.events[i].slot == static_cast<int>(i);
    CHECK(slots_ok, "...at slots 0 to 5");
    // grant() is all or nothing
    inv = build(worn_kit());
    const auto before = inv->items();
    const auto o = inv->grant(eight, *g_catalog);
    CHECK(!o.ok && o.events.empty() && inv->items() == before && o.refusal.find("no room") != std::string::npos, "grant of eight stacks is refused whole: nothing changed");
    CHECK(inv->can_grant({eight[0], eight[1]}, *g_catalog) && !inv->can_grant(eight, *g_catalog) && inv->items() == before, "can_grant says so and changes nothing");
    // a Backpack full of other items blocks nothing of a resource grant, and a resource container full blocks nothing of the Backpack's
    std::vector<svc::InventoryItem> full = worn_kit();
    for (uint32_t s = 0; s < 70; ++s) full.push_back(make_item(Bag, static_cast<int>(s), fi::kPotion, 100 + s));
    auto packed = build(full);
    const auto oak = packed->grant({make_item(Bag, 0, fr::kOak, 1, 5)}, *g_catalog);
    CHECK(oak.ok && oak.events.size() == 1 && oak.events[0].container == R0 && packed->count(Bag) == 70, "a Backpack full of potions does not block a harvest: the oak goes to Resource0");
    CHECK(!packed->grant({make_item(Bag, 0, fi::kPotion, 2)}, *g_catalog).ok, "...but one more potion has no room");
    // a mixed grant that fits in one kind only: all or nothing
    const auto mixed = packed->grant({make_item(Bag, 0, fr::kDaffodil, 3, 2), make_item(Bag, 0, fi::kPotion, 4)}, *g_catalog);
    CHECK(!mixed.ok && packed->count(R0) == 1, "an oak/potion grant that does not fit as a whole is refused without placing the part that fits");
    const auto part = packed->grant_what_fits({make_item(Bag, 0, fr::kDaffodil, 3, 2), make_item(Bag, 0, fi::kPotion, 4)}, *g_catalog);
    CHECK(part.outcome.ok && part.outcome.events.size() == 1 && part.rest.size() == 1 && part.rest[0].item_record_id == fi::kPotion, "grant_what_fits places the daffodil and returns the potion");
    // an item larger than a stack: the first piece is placed, the rest has no guid of its own
    auto big = build(worn_kit());
    const auto bigr = big->grant_what_fits({make_item(Bag, 0, fr::kCoal, 1, 25)}, *g_catalog);
    CHECK(bigr.outcome.events.size() == 1 && bigr.outcome.events[0].item.quantity == 20 && bigr.rest.size() == 1 && bigr.rest[0].quantity == 5 && bigr.rest[0].instance_guid.empty(),
          "25 coal for a stack of 20: 20 are placed, 5 come back without the instance guid the placed stack uses");
}

static void test_move() {
    std::printf("move: inside and between the containers\n");
    auto inv = build(with(worn_kit(true), {make_item(R0, 0, fr::kDaffodil, 1, 3), make_item(R0, 1, fr::kOak, 2, 4), make_item(R0, 12, fr::kCoal, 3, 7)}));
    const std::string daffodil = guid_n(1), oak = guid_n(2), coal = guid_n(3);
    auto o = inv->move(R0, 0, 3, daffodil, R0, 2, g_catalog.get());
    CHECK(o.ok && o.events.size() == 2 && o.events[0].type == aoc::protocol::repl::StorageEventType::Remove && o.events[0].slot == 0 && o.events[1].type == aoc::protocol::repl::StorageEventType::Add &&
              o.events[1].slot == 2 && o.events[1].container == R0 && o.events[1].item.instance_guid == daffodil && inv->at(R0, 2) && !inv->at(R0, 0),
          "a daffodil moves from slot 0 to slot 2: a Remove and an Add");
    std::vector<svc::InventoryItem> snapshot = inv->items();
    const auto refuse = [&](const items::Outcome& r, const char* what) {
        const bool ok = !r.ok && r.events.empty() && !r.refusal.empty() && inv->items() == snapshot;
        if (!ok) std::printf("    (%s: ok %d, refusal '%s')\n", what, r.ok ? 1 : 0, r.refusal.c_str());
        return ok;
    };
    CHECK(refuse(inv->move(R0, 2, 3, daffodil, R0, 1, g_catalog.get()), "onto the oak"), "onto another item: refused (a swap or a fill are not made)");
    CHECK(refuse(inv->move(R0, 2, 3, daffodil, R0, 24, g_catalog.get()), "past the grid"), "to a slot past the 24 cells: refused");
    CHECK(refuse(inv->move(R0, 2, 3, daffodil, R0, 18, g_catalog.get()), "too tall"), "to the last row, where a 1 x 2 item does not fit: refused");
    CHECK(refuse(inv->move(R0, 2, 3, daffodil, R0, 2, g_catalog.get()), "own slot"), "onto its own slot: refused");
    CHECK(refuse(inv->move(R0, 2, 2, daffodil, R0, 3, g_catalog.get()), "split"), "a part of a stack: refused");
    CHECK(refuse(inv->move(R0, 2, 3, guid_n(77), R0, 3, g_catalog.get()), "guid"), "a guid that is not the stack's: refused");
    CHECK(refuse(inv->move(R0, 5, 3, daffodil, R0, 3, g_catalog.get()), "empty slot"), "from an empty slot: refused");
    CHECK(refuse(inv->move(R0, 2, 3, daffodil, R0, 3), "no catalog"), "without a catalog the dimensions are unknown: refused");
    CHECK(refuse(inv->move(R0, 2, 3, daffodil, Bag, 0, g_catalog.get()), "to the bag"), "a resource item to the Backpack: refused");
    CHECK(refuse(inv->move(Bag, 0, 1, daffodil, R0, 3, g_catalog.get()), "from the bag"), "...and the Backpack to a resource container");
    CHECK(refuse(inv->swap(R0, 2, 3, daffodil, R0, 1, oak, 4), "swap"), "a swap of resource items: refused");
    CHECK(refuse(inv->move(R0, 2, 3, daffodil, R2, 0, g_catalog.get()), "a container with no bag"), "to Resource2, whose bag is not worn: refused");
    // sliding over its own cells is allowed: the daffodil at slot 2 (rows 0-1 of column 2) down one row to slot 8
    o = inv->move(R0, 2, 3, daffodil, R0, 8, g_catalog.get());
    CHECK(o.ok && inv->at(R0, 8) && !inv->at(R0, 2), "a stack may slide over its own old cells");
    snapshot = inv->items();
    // between containers: the coal from Resource0 to Resource1 (the Mining bag): (3, 0) is free, (2, 0) is blocked
    CHECK(refuse(inv->move(R0, 12, 7, coal, R1, 2, g_catalog.get()), "blocked"), "to a blocked cell of the Mining bag: refused");
    o = inv->move(R0, 12, 7, coal, R1, 3, g_catalog.get());
    CHECK(o.ok && o.events.size() == 2 && o.events[0].container == R0 && o.events[1].container == R1 && o.events[1].slot == 3 && inv->at(R1, 3) && !inv->at(R0, 12) && inv->count(R1) == 1,
          "the coal moves from Resource0 to Resource1 slot 3");
}
// [C.Y.B ~ CoA]

static void test_bags_stay() {
    std::printf("bags: a bag whose container is open stays\n");
    auto inv = build(with(worn_kit(), {make_item(Bag, 0, fr::kBagMining, 1), make_item(R0, 0, fr::kOak, 2, 4)}));
    const auto before = inv->items();
    auto o = inv->use(Worn, 23, guid_n(900), 0xFFFFFFFFu, *g_catalog);
    CHECK(!o.ok && o.refusal.find("container open") != std::string::npos && inv->items() == before, "taking off the Sanctus bag is refused while its container is open (even an empty one)");
    o = inv->use(Bag, 0, guid_n(1), 23, *g_catalog);
    CHECK(!o.ok && o.refusal.find("container open") != std::string::npos && inv->items() == before, "wearing the Mining bag over it is refused");
    // the server picks the first free bag slot: 24, whose container does not exist this session
    o = inv->use(Bag, 0, guid_n(1), 0xFFFFFFFFu, *g_catalog);
    CHECK(o.ok && inv->at(Worn, 24) && inv->grid(R1).empty() && inv->grid(R0).cells() == 24, "wearing it in the free slot 24 works; its container opens at the next login (no grid this session)");
    auto relog = build(inv->items());
    CHECK(relog && relog->grid(R1).cells() == 40, "...and the next login builds it");
    // the stack in a container is not used
    o = inv->use(R0, 0, guid_n(2), 0xFFFFFFFFu, *g_catalog);
    CHECK(!o.ok && o.refusal.find("Resource0") != std::string::npos, "a stack in a resource container is not used");
}

static void test_consume() {
    std::printf("consume in a container\n");
    auto inv = build(with(worn_kit(), {make_item(R0, 0, fr::kOak, 1, 20)}));
    auto o = inv->consume(R0, 0, guid_n(1), 8);
    CHECK(o.ok && o.events.size() == 2 && o.events[0].type == aoc::protocol::repl::StorageEventType::Remove && o.events[0].item.quantity == 20 && o.events[1].item.quantity == 12 &&
              o.events[1].slot == 0 && o.events[1].container == R0,
          "taking 8 of a stack of 20: a Remove and an Add with 12, in the same slot");
    o = inv->consume(R0, 0, guid_n(1), 12);
    CHECK(o.ok && o.events.size() == 1 && !inv->at(R0, 0), "taking the rest empties the slot");
}

static void test_repair() {
    std::printf("the login's repair: resource items found in the Backpack\n");
    std::vector<svc::InventoryItem> v = worn_kit();
    for (uint32_t i = 0; i < 7; ++i) v.push_back(make_item(Bag, static_cast<int>(i), fr::kOak, 10 + i, 20, static_cast<int32_t>(i + 1)));
    v.push_back(make_item(Bag, 7, fi::kPotion, 30));
    v.push_back(make_item(Bag, 8, fr::kDaffodil, 31, 3));
    auto inv = build(v);
    const auto r = inv->repair_resources(*g_catalog);
    CHECK(r.moved.size() == 6 && r.stayed.size() == 2, "seven oaks and a daffodil: six oaks fit the Sanctus bag (one per column), the seventh oak and the daffodil stay");
    bool at_slots = r.moved.size() == 6;
    for (size_t i = 0; i < r.moved.size(); ++i) at_slots = at_slots && r.moved[i].container == R0 && r.moved[i].slot == static_cast<int>(i) && r.moved[i].instance_guid == guid_n(10 + static_cast<uint32_t>(i));
    CHECK(at_slots, "the moved oaks keep their guids and take slots 0 to 5 of Resource0, in the order of their Backpack slots");
    CHECK(inv->count(R0) == 6 && inv->count(Bag) == 3 && inv->at(Bag, 6)->item_record_id == fr::kOak && inv->at(Bag, 7)->item_record_id == fi::kPotion, "the Backpack keeps the seventh oak, the potion and the daffodil, in their slots");
    auto again = build(inv->items());
    CHECK(again && *again == *inv, "the repaired inventory loads again and equals itself");
    const auto second = inv->repair_resources(*g_catalog);
    CHECK(second.moved.empty() && second.stayed.size() == 2, "a second repair moves nothing");
    auto empty = build(worn_kit());
    CHECK(empty->repair_resources(*g_catalog).moved.empty(), "an inventory with no resource item in the Backpack changes nothing");
}

static void test_round_trip() {
    std::printf("a relog: items() builds the same inventory\n");
    auto inv = build(worn_kit(true, true));
    for (uint32_t i = 0; i < 12; ++i) {
        inv->grant({make_item(Bag, 0, i % 2 ? fr::kOak : fr::kHalcyonite, 100 + i, 7 + static_cast<int32_t>(i), 1 + static_cast<int32_t>(i % 3))}, *g_catalog);
    }
    inv->grant({make_item(Bag, 0, fi::kPotion, 200, 4)}, *g_catalog);
    const auto all = inv->items();
    auto again = build(all);
    CHECK(again && *again == *inv && again->items() == all && again->grid(R1).cells() == 40, "the items of an inventory with grants in two containers load into an equal inventory");
    // the order of items(): storage by storage, by slot
    bool ordered = true;
    for (size_t i = 1; i < all.size(); ++i) {
        ordered = ordered && (static_cast<int>(all[i - 1].container) < static_cast<int>(all[i].container) ||
                              (all[i - 1].container == all[i].container && all[i - 1].slot < all[i].slot));
    }
    CHECK(ordered, "items(): the Backpack's, the Equipment's, then Resource0, Resource1 and Resource2 by slot");
}

// ---- the real file ----------------------------------------------------------------------------------------------------------------

static const char* real_db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef RESOURCE_CONTENT_DB_PATH
    if (fs::exists(RESOURCE_CONTENT_DB_PATH)) return RESOURCE_CONTENT_DB_PATH;
#endif
    return nullptr;
}

using Matrix32 = std::vector<std::vector<int32_t>>;
static Matrix32 matrix32(const std::vector<std::vector<int>>& m) {
    Matrix32 out;
    for (const auto& col : m) out.emplace_back(col.begin(), col.end());
    return out;
}

static void test_real() {
    std::printf("the real content.db\n");
    const char* path = real_db_path();
    if (!path) {
        std::printf("  [skip] no content.db (AOC_CONTENT_DB, or dist/content/content.db of the working tree)\n");
        return;
    }
    std::string error;
    auto db = content::ContentDb::open(path, error);
    if (!db) {
        std::printf("  [FAIL] %s\n", error.c_str());
        g_fail++;
        return;
    }
    items::ContentItemCatalog catalog(*db);
    sqlite3* raw = nullptr;
    sqlite3_open_v2(path, &raw, SQLITE_OPEN_READONLY, nullptr);
    std::vector<int64_t> guids;
    std::map<int64_t, bool> tagged;
    {
        sqlite3_stmt* q = nullptr;
        sqlite3_prepare_v2(raw, "SELECT guid FROM item ORDER BY guid", -1, &q, nullptr);
        while (sqlite3_step(q) == SQLITE_ROW) guids.push_back(sqlite3_column_int64(q, 0));
        sqlite3_finalize(q);
        sqlite3_prepare_v2(raw, "SELECT DISTINCT item_guid FROM item_tag WHERE lower(tag) LIKE 'item.resource%'", -1, &q, nullptr);
        while (sqlite3_step(q) == SQLITE_ROW) tagged[sqlite3_column_int64(q, 0)] = true;
        sqlite3_finalize(q);
    }
    sqlite3_close(raw);
    CHECK(guids.size() == 10568 && tagged.size() == 1824, "10,568 items, 1,824 of them carry a child of item.resource");

    // six resource reagents have a DefaultQuality of -3 or -2 in the data: they read with quality 1 (they were unknown records, and an unknown record goes to the Backpack)
    const auto ashlar = db->item(6064634087359578287ll, error);   // Resource_Reagent_Ashlar_EarthsongPrism, -3
    const auto sugar = db->item(6064634088104264020ll, error);    // Resource_Reagent_Seed_SugarCane, -2
    CHECK(ashlar && ashlar->resource && ashlar->default_quality == 1 && sugar && sugar->resource && sugar->default_quality == 1, "the reagents whose DefaultQuality is -3 or -2 read with quality 1 and are resources");

    // the names the retail pick-up text shows (the 17 items of the 24 captured harvest events; contentc's TestRealItemDisplayNames checks all 17 in the file itself)
    {
        const std::pair<int64_t, const char*> names[] = {{6064633254017630211ll, "Coconut Wood"}, {109162169652924ll, "Oak Wood"}, {6064629184431849472ll, "Daffodil"},
                                                         {6064630406148390932ll, "Bear Carcass Portion"}, {6064629326302543894ll, "Wolf"}, {6064629252182507530ll, "Rough Sapphire"}};
        size_t ok = 0;
        for (const auto& [guid, want] : names) {
            const auto r = db->item(guid, error);
            ok += r && r->display_name == want ? 1 : 0;
        }
        CHECK(ok == std::size(names), "ContentDb::item on the real file: display_name reads the retail text's names (Coconut Wood, Oak Wood, Daffodil, Bear Carcass Portion, Wolf, Rough Sapphire)");
    }

    // one grant of every item into a character with the Sanctus bag and nothing else: the item goes where its kind says, exactly one storage each
    size_t resource = 0, backpack = 0, other = 0, tag_mismatch = 0, too_big = 0;
    for (const int64_t g : guids) {
        const auto info = catalog.info(g);
        if (!info) {
            ++other;
            continue;
        }
        if (info->resource != (tagged.count(g) > 0)) ++tag_mismatch;
        if (info->inv_w > 6 || info->inv_h > 4 || info->inv_w < 1 || info->inv_h < 1) ++too_big;
        std::vector<svc::InventoryItem> kit = {make_item(Worn, 23, 6064629382556287782ll, 1)};   // Bag_Resource_Sanctus
        auto inv = Inv::from_items(kit, error, &catalog);
        if (!inv) {
            ++other;
            continue;
        }
        const auto o = inv->grant({make_item(Bag, 0, g, 2, 1)}, catalog);
        if (!o.ok || o.events.size() != 1) {
            ++other;
            continue;
        }
        (o.events[0].container == R0 ? resource : o.events[0].container == Bag ? backpack : other)++;
        if ((o.events[0].container == R0) != info->resource) ++tag_mismatch;
    }
    CHECK(resource == 1824 && backpack == 10568 - 1824 && other == 0, "every one of the 10,568 items is granted to exactly one storage: 1,824 to Resource0, the rest to the Backpack");
    CHECK(tag_mismatch == 0, "the catalog's flag is the item_tag rule for every item, and the routing follows the flag");
    CHECK(too_big == 0, "no item takes more than the Sanctus bag's 6 x 4, or less than a cell");
// -- CoA x C-Y-B --

    // the 70 bags: the 64 worn ones have a grid, the six warehouse tab definitions are no worn item
    size_t with_grid = 0, bags = 0;
    {
        sqlite3* r2 = nullptr;
        sqlite3_open_v2(path, &r2, SQLITE_OPEN_READONLY, nullptr);
        sqlite3_stmt* q = nullptr;
        sqlite3_prepare_v2(r2, "SELECT item_guid FROM resource_bag", -1, &q, nullptr);
        while (sqlite3_step(q) == SQLITE_ROW) {
            ++bags;
            const auto info = catalog.info(sqlite3_column_int64(q, 0));
            with_grid += info && info->bag ? 1 : 0;
        }
        sqlite3_finalize(q);
        sqlite3_close(r2);
    }
    CHECK(bags == 70 && with_grid == 64, "70 resource_bag rows, 64 of them worn items the catalog gives a grid");

    // capture 1198's three bags, to the cell
    const auto sanctus = db->resource_bag(6064629382556287782ll, error);
    const auto mining = db->resource_bag(6064632218973765644ll, error);
    const auto lumber = db->resource_bag(6064632219108704288ll, error);
    CHECK(sanctus && sanctus->columns == 6 && sanctus->rows == 4 && !sanctus->custom, "Bag_Resource_Sanctus: 6 x 4, not custom");
    CHECK(mining && mining->columns == 8 && mining->rows == 5 && mining->custom && mining->matrix == matrix32(fr::kMiningMatrix), "Bag_Resource_T2_Mining_0: capture 1198's 8 x 5 matrix, cell by cell");
    CHECK(lumber && lumber->columns == 6 && lumber->rows == 5 && lumber->custom && lumber->matrix == matrix32(fr::kLumberMatrix), "Bag_Resource_T2_Lumberjacking_0: capture 1198's 6 x 5 matrix, cell by cell");

    // the real items on the real grids: capture 1198's lists load into the three bags
    auto inv = Inv::from_items({make_item(Worn, 23, 6064629382556287782ll, 1), make_item(Worn, 24, 6064632218973765644ll, 2), make_item(Worn, 25, 6064632219108704288ll, 3),
                                make_item(R1, 0, 6064629326234910728ll, 4, 2), make_item(R1, 3, 6064630406148390932ll, 5), make_item(R1, 6, 6064629252186439693ll, 6, 10),
                                make_item(R1, 8, 6064629326176452611ll, 7), make_item(R2, 1, 6064631481844695080ll, 8, 5), make_item(R2, 3, 6064631481841025063ll, 9, 5, 6),
                                make_item(R2, 5, 6064631481841025063ll, 10, 15)},
                               error, &catalog);
    CHECK(inv && inv->grid(R1).cells() == 40 && inv->grid(R2).cells() == 30 && inv->count(R1) == 4 && inv->count(R2) == 3, "capture 1198's Resource1 and Resource2 lists load on the real grids");
}

int main() {
    const fs::path dir = cf::make_temp_dir("worldd-resource-");
    std::printf("test_worldd_resource\n");
    test_grid();
    if (open_fixture(dir)) {
        test_catalog();
        test_from_items();
        test_routing();
        test_merge();
        test_full();
        test_move();
        test_bags_stay();
        test_consume();
        test_repair();
        test_round_trip();
    } else {
        g_fail++;
    }
    test_real();
    g_catalog.reset();
    g_db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tailoring_review_tests.inc: what the review of the Tailoring milestone added to the pure tests
//
//  The six outpost vendors (the Textile Mill, Hideworks and Smithy vendors of Aven's End and of Briarmoor Workshops) stand in their outposts' Riverlands zones, so the shops'
//  GetNodeBiome conditions hold for them; the biome is the shopkeeper's, not the player's.  Of the Textile Mill vendor's 39 recipe items the Riverlands shop sells 33 (the six
//  Emberwake pieces are listed for a shopkeeper in the Tabletop Mountains), and of the Hideworks vendor's 12 and the Smithy vendor's 10 half.  The window and the cart ask the
//  same predicates of the same facts, and the window hides what the cart refuses.
//  Included by test_worldd_professions.cpp after tailoring_tests.inc, before main().
// ============================================================================
namespace tlr {
struct Keeper {
    const char* what;
    double      x, y;      // cm, where the node population puts the shopkeeper
    const char* node;      // the record name of the outpost's node
};
constexpr Keeper kKeepers[] = {
    {"the Textile Mill vendor of Aven's End", -875046.9, 606173.9, "Verra_RVR_ArtisanPOI_AvensEnd"},
    {"the Hideworks vendor of Aven's End", -864590.2, 615141.2, "Verra_RVR_ArtisanPOI_AvensEnd"},
    {"the Smithy vendor of Aven's End", -863672.7, 602298.3, "Verra_RVR_ArtisanPOI_AvensEnd"},
    {"the Textile Mill vendor of Briarmoor Workshops", -700515.5, 583081.0, "Verra_RVR_ArtisanPOI_BriarmoorWorkshops"},
    {"the Hideworks vendor of Briarmoor Workshops", -663929.3, 559755.5, "Verra_RVR_ArtisanPOI_BriarmoorWorkshops"},
    {"the Smithy vendor of Briarmoor Workshops", -682507.9, 551384.2, "Verra_RVR_ArtisanPOI_BriarmoorWorkshops"},
};
constexpr int64_t kSmithy = 6064630643960643621ll, kWoodshop = 6064630643964837928ll;
}  // namespace tlr

/// How many of the recipe items a shop lists `holds` lets through (nullopt for a shop of the data that is not there), and the names of those it hides.
static size_t tlr_recipe_items_sold(const content::ContentDb& db, const vendor::TownShops& shops, int64_t shop, const vendor::TownShops::Holds& holds, std::vector<std::string>* hidden) {
    size_t sold = 0;
    const vendor::TaxRate rate = shops.default_rate(shop);
    const auto prices = shops.data().prices.find(shop);
    if (prices == shops.data().prices.end()) return 0;
    for (const auto& [item, rows] : prices->second) {
        std::string error;
        const auto learn = db.item_learn(item, error);
        if (!learn || learn->empty() || rows.empty()) continue;
        repl::VendorPurchaseCart cart;
        cart.vendor = shop;
        cart.items.push_back({item, 1, std::nullopt, std::nullopt});
        if (shops.quote(shop, cart, rate, holds).ok) {
            ++sold;
        } else if (hidden) {
            const auto rec = db.item(item, error);
            hidden->push_back(rec ? rec->name : std::to_string(item));
        }
    }
    return sold;
}

/// The outposts' shopkeepers, the node that decides, and what the shops sell in the Riverlands.
static void test_tlr_shopkeepers(const content::ContentDb& db) {
    std::printf("the outposts' shopkeepers stand in their Riverlands zones; the shops ask the shopkeeper's place; what they sell there\n");
    std::string error;
    auto nodes = world::CityNodes::load(db.path(), error);
    CHECK_SHOWN(nodes != nullptr, "the city nodes read");
    if (!nodes) return;
    for (const tlr::Keeper& k : tlr::kKeepers) {
        const world::NodeSite site = nodes->node_at(k.x, k.y);
        const world::CityNodeInfo* node = nodes->by_rank(site.node_rank);
        vendor::ShopFacts f;
        f.nodes = &*nodes;
        f.player_x = k.x + 300;
        f.player_y = k.y;
        f.has_npc = true;
        f.npc_x = k.x;
        f.npc_y = k.y;
        const auto river = dialogue::evaluate_expression("GetNodeBiome() == EBiomeType::Riverlands", f);
        const auto table = dialogue::evaluate_expression("GetNodeBiome() == EBiomeType::TabletopMountains", f);
        CHECK_SHOWN(node && node->name == k.node && river && river->i == 1 && table && table->i == 0, (std::string(k.what) + " stands in its outpost's Riverlands zone: its shop's GetNodeBiome holds").c_str());
    }

    // the shopkeeper's node decides: a player in a Tabletop Mountains node, the shopkeeper at Aven's End, and the other way round; with no shopkeeper it is the player's
    const world::CityNodeInfo* tabletop = nullptr;
    for (const world::CityNodeInfo& n : nodes->nodes()) {
        if (!tabletop && nodes->node_at(n.x, n.y).biome == 17) tabletop = &n;   // EBiomeType::TabletopMountains
    }
    CHECK_SHOWN(tabletop != nullptr, "a Tabletop Mountains node is in the data");
    if (!tabletop) return;
    const tlr::Keeper& aven = tlr::kKeepers[0];
    const auto biome_is = [&](const vendor::ShopFacts& f, const char* biome) {
        const auto v = dialogue::evaluate_expression(std::string("GetNodeBiome() == EBiomeType::") + biome, f);
        return v && v->i == 1;
    };
    vendor::ShopFacts keeper_river;   // the player is in the mountains, the shopkeeper at Aven's End
    keeper_river.nodes = &*nodes;
    keeper_river.player_x = tabletop->x;
    keeper_river.player_y = tabletop->y;
    keeper_river.has_npc = true;
    keeper_river.npc_x = aven.x;
    keeper_river.npc_y = aven.y;
    vendor::ShopFacts keeper_table = keeper_river;   // the player is at Aven's End, the shopkeeper in the mountains
    keeper_table.player_x = aven.x;
    keeper_table.player_y = aven.y;
    keeper_table.npc_x = tabletop->x;
    keeper_table.npc_y = tabletop->y;
    vendor::ShopFacts alone = keeper_river;          // no shopkeeper: the player's own place
    alone.has_npc = false;
    CHECK_SHOWN(biome_is(keeper_river, "Riverlands") && !biome_is(keeper_river, "TabletopMountains"), "the player in the mountains and the shopkeeper at Aven's End: the shop's biome is the Riverlands");
    CHECK_SHOWN(biome_is(keeper_table, "TabletopMountains") && !biome_is(keeper_table, "Riverlands"), "the player at Aven's End and the shopkeeper in the mountains: the shop's biome is the Tabletop Mountains");
    CHECK_SHOWN(biome_is(alone, "TabletopMountains"), "with no shopkeeper the biome is the player's");

    // what the three shops sell at Aven's End's shopkeepers
    std::string vendor_error;
    auto data = db.vendor_data(vendor_error);
    CHECK_SHOWN(data.has_value(), "the vendor tables read");
    if (!data) return;
    const vendor::TownShops shops(std::move(*data));
    const auto holds_at = [&](const tlr::Keeper& k) {
        vendor::ShopFacts f;
        f.nodes = &*nodes;
        f.player_x = k.x;
        f.player_y = k.y;
        f.has_npc = true;
        f.npc_x = k.x;
        f.npc_y = k.y;
        return vendor::TownShops::Holds([f](const content::VendorPredicateRow& p) {
            const auto v = dialogue::evaluate_expression(p.expression, f);
            return v && v->i != 0;
        });
    };
    std::vector<std::string> hidden;
    const size_t textile = tlr_recipe_items_sold(db, shops, tl::kTextileMill, holds_at(tlr::kKeepers[0]), &hidden);
    bool all_emberwake = true;
    for (const std::string& name : hidden) all_emberwake = all_emberwake && name.find("Emberwake") != std::string::npos;
    CHECK_SHOWN(textile == 33 && hidden.size() == 6 && all_emberwake,
                "of the Textile Mill vendor's 39 recipe items the shop sells 33 at Aven's End: the six Emberwake pieces are listed only for a shopkeeper in the Tabletop Mountains");
    CHECK_SHOWN(tlr_recipe_items_sold(db, shops, tl::kHideworks, holds_at(tlr::kKeepers[1]), nullptr) == 6, "of the Hideworks vendor's 12 recipe items the shop sells 6 at Aven's End");
    CHECK_SHOWN(tlr_recipe_items_sold(db, shops, tlr::kSmithy, holds_at(tlr::kKeepers[2]), nullptr) == 5, "of the Smithy vendor's 10 Armorsmithing recipe items the shop sells 5 at Aven's End");
    // ... and the same at Briarmoor Workshops
    CHECK_SHOWN(tlr_recipe_items_sold(db, shops, tl::kTextileMill, holds_at(tlr::kKeepers[3]), nullptr) == 33 &&
                    tlr_recipe_items_sold(db, shops, tl::kHideworks, holds_at(tlr::kKeepers[4]), nullptr) == 6 &&
                    tlr_recipe_items_sold(db, shops, tlr::kSmithy, holds_at(tlr::kKeepers[5]), nullptr) == 5,
                "Briarmoor Workshops' three vendors sell the same 33, 6 and 5");
    SAYOK("the outposts' shopkeepers");
}

/// The window and the cart ask the same predicates of the same facts: what the window hides is exactly what the cart refuses, listing by listing.
static void test_tlr_window(const content::ContentDb& db) {
    std::printf("the window hides what the cart refuses\n");
    std::string error;
    auto nodes = world::CityNodes::load(db.path(), error);
    auto data = db.vendor_data(error);
    CHECK_SHOWN(nodes != nullptr && data.has_value(), "the city nodes and the vendor tables read");
    if (!nodes || !data) return;
    const vendor::TownShops shops(std::move(*data));
    struct Case {
        int64_t           shop;
        const tlr::Keeper* keeper;
        size_t            hidden;   // the listings the window hides at that shopkeeper
    };
    const Case cases[] = {{tl::kTextileMill, &tlr::kKeepers[0], 6}, {tl::kHideworks, &tlr::kKeepers[1], 6}, {tlr::kSmithy, &tlr::kKeepers[2], 5}, {tlr::kWoodshop, nullptr, 0}};
    for (const Case& c : cases) {
        const content::VendorData::Def* def = shops.def(c.shop);
        CHECK_SHOWN(def != nullptr, "the shop is in the data");
        if (!def) continue;
        const double kx = c.keeper ? c.keeper->x : tlr::kKeepers[0].x, ky = c.keeper ? c.keeper->y : tlr::kKeepers[0].y;
        bool unreadable = false;
        const repl::UnavailableList list = vendor::window_unavailable(shops, c.shop, &*nodes, kx + 300, ky, kx, ky, &unreadable);
        std::set<std::pair<int64_t, int>> hidden;   // (the tab's inventory, the listing's index)
        bool plain = !unreadable;
        for (const repl::UnavailableTab& tab : list) {
            plain = plain && !tab.inventory.hidden && tab.inventory.index >= 0 && static_cast<size_t>(tab.inventory.index) < def->inventories.size();
            if (!plain) break;
            for (const repl::UnavailableEntry& l : tab.listings) {
                plain = plain && l.hidden;   // (the shops' conditions hide; none greys out)
                hidden.insert({def->inventories[static_cast<size_t>(tab.inventory.index)], l.index});
            }
        }
        CHECK_SHOWN(plain && hidden.size() == c.hidden, (def->name + "'s window hides " + std::to_string(c.hidden) + " listing(s) at its shopkeeper").c_str());
        // every priced listing: sold by the cart exactly when the window does not hide it
        const auto prices = shops.data().prices.find(c.shop);
        size_t listings = 0, agree = 0;
        const vendor::TownShops::Holds holds = [&](const content::VendorPredicateRow& p) {
            vendor::ShopFacts f;
            f.nodes = &*nodes;
            f.player_x = kx + 300;
            f.player_y = ky;
            f.has_npc = true;
            f.npc_x = kx;
            f.npc_y = ky;
            const auto v = dialogue::evaluate_expression(p.expression, f);
            return v && v->i != 0;
        };
        if (prices == shops.data().prices.end()) continue;
        for (const auto& [item, rows] : prices->second) {
            for (const content::VendorPriceRow& r : rows) {
                repl::VendorPurchaseCart cart;
                cart.vendor = c.shop;
                cart.items.push_back({item, 1, r.variation != 0 ? std::optional<int64_t>(r.variation) : std::nullopt, r.offer != 0 ? std::optional<int64_t>(r.offer) : std::nullopt});
                ++listings;
                const bool sold = shops.quote(c.shop, cart, shops.default_rate(c.shop), holds).ok;
                agree += sold == (hidden.count({r.inventory, r.ord}) == 0) ? 1 : 0;
            }
        }
        CHECK_SHOWN(listings > 0 && agree == listings, (def->name + ": the cart sells a listing exactly when its window shows it (" + std::to_string(agree) + " of " + std::to_string(listings) + ")").c_str());
    }
    SAYOK("the window and the cart");
}

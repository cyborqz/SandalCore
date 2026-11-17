// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tailoring_review_session_tests.inc: what the review of the Tailoring milestone added to the session tests
//
//  The outpost vendors have no conversation: the player's use of the NPC opens the shop's window directly.  That window must carry the same list of hidden listings the
//  cart is checked with (the shops' GetNodeBiome conditions hide their Tabletop Mountains listings in the Riverlands), for the Textile Mill, Hideworks and Smithy vendors and
//  for a second player; the Woodshop vendor has no condition and sends no list.  The Smithy vendor's Riverlands recipe items are bought and learned, its Tabletop ones are
//  refused; a recipe item used twice is refused the second time and kept; a craft with too few materials or a stack the recipe does not take is refused with nothing taken;
//  the Simple Rucksack, a Novice recipe that makes a bag, lands in the item bag.
//  Included by test_worldd_items_session.cpp after tailoring_session_tests.inc; its tests are called from main().
// ============================================================================
#include "protocol/repl/rpc_fields.h"
#include "vendor/town_shops.h"

namespace tlr {
constexpr int64_t kSmithy = 6064630643960643621ll, kSmithyAsset = 6064630656572981249ll;
constexpr int64_t kWoodshopAsset = 6064630656441778203ll;
constexpr int64_t kSwordShouldersItem = 6064630643519389699ll, kSwordShouldersRecipe = 6064629386974855192ll;   // Armorsmithing, Riverlands
constexpr int64_t kDeepwardForearmsItem = 6064633921832550412ll;                                                // Armorsmithing, Tabletop Mountains
constexpr int64_t kEmberwakeBackItem = 6064633921833206320ll;                                                   // Tailoring, Tabletop Mountains
constexpr int64_t kCoarseThread = 6064634778922778975ll, kArtisanBagReagent = 6064634673791172631ll;
constexpr int64_t kRucksackRecipe = 6064634778920747358ll, kRucksackItem = 6064634778838630730ll;
constexpr int64_t kT3 = 6064630554071793707ll;   // CraftingStation_Tailoring_T3
constexpr int64_t kArcaneChestRecipe = 6064629387023482925ll, kArcaneChestItem = 6064629376874446886ll, kBoltLinen = 6064630417662214277ll, kTannedHideWolf = 6064630417661886573ll;
constexpr int64_t kAlchemyShirtRecipe = 6064632236037243152ll, kAlchemyShirtItem = 6064632236008079422ll, kPreparedYardageLinen = 6064634086453084186ll, kPowderEmerald = 6064632105660121095ll;
constexpr int64_t kArtisanShirtReagent = 6064634673789141007ll;
}  // namespace tlr

/// One number of a read-back parameter: signed or unsigned, whichever the decoder gave.
static std::optional<int64_t> tlr_number(const repl::Value* v, int64_t fallback) {
    if (!v) return fallback;
    if (const int64_t* i = v->get_if<int64_t>()) return *i;
    if (const uint64_t* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return std::nullopt;
}

/// The tabs and listings a ClientOpenVendor call lists as unavailable, read back the way the encoder wrote them: nothing on the wire is the empty list, nullopt is a list
/// that does not read.
static std::optional<repl::UnavailableList> tlr_unavailable_of(const repl::RpcCall& c) {
    const schema::RpcSchema& rpc = *c.rpc;
    repl::UnavailableList out;
    const repl::Value* v = repl::rpcf::value_of(rpc, c.params, "VendorInventoryUnavailableList");
    if (!v) return out;
    const repl::ArrayValue* tabs = repl::rpcf::as<repl::ArrayValue>(v);
    const schema::RepCommand* list = repl::rpc_command(rpc, "VendorInventoryUnavailableList");
    if (!tabs || !list) return std::nullopt;
    const auto tab_elems = repl::rpcf::elements_of(rpc, *list);
    const uint16_t h_listings = repl::rpcf::local(tab_elems, "VendorInventoryUnavailableList.UnavailableListings");
    const uint16_t h_index = repl::rpcf::local(tab_elems, "VendorInventoryUnavailableList.UnavailableInventory.Index");
    const uint16_t h_failures = repl::rpcf::local(tab_elems, "VendorInventoryUnavailableList.UnavailableInventory.PredicateFailures");
    const uint16_t h_hidden = repl::rpcf::local(tab_elems, "VendorInventoryUnavailableList.UnavailableInventory.bHidden");
    if (!h_listings || !h_index || !h_failures || !h_hidden) return std::nullopt;
    const auto listing_elems = repl::rpcf::elements_of(rpc, tab_elems[h_listings - 1]);
    const uint16_t l_index = repl::rpcf::local(listing_elems, "UnavailableListings.Index");
    const uint16_t l_failures = repl::rpcf::local(listing_elems, "UnavailableListings.PredicateFailures");
    const uint16_t l_hidden = repl::rpcf::local(listing_elems, "UnavailableListings.bHidden");
    if (!l_index || !l_failures || !l_hidden) return std::nullopt;
    const auto bytes_of = [](const repl::Value* b) {
        std::vector<uint8_t> bytes;
        if (const repl::ArrayValue* a = repl::rpcf::as<repl::ArrayValue>(b)) {
            for (const repl::ElementState& e : a->elements) {
                const auto n = tlr_number(repl::rpcf::element_value(e, 1), 0);
                bytes.push_back(static_cast<uint8_t>(n.value_or(0)));
            }
        }
        return bytes;
    };
    for (const repl::ElementState& tab : tabs->elements) {
        repl::UnavailableTab t;
        const auto index = tlr_number(repl::rpcf::element_value(tab, h_index), 0);
        const auto hidden = repl::rpcf::bool_or(repl::rpcf::element_value(tab, h_hidden), false);
        if (!index || !hidden) return std::nullopt;
        t.inventory.index = static_cast<int32_t>(*index);
        t.inventory.hidden = *hidden;
        t.inventory.failures = bytes_of(repl::rpcf::element_value(tab, h_failures));
        if (const repl::ArrayValue* listings = repl::rpcf::as<repl::ArrayValue>(repl::rpcf::element_value(tab, h_listings))) {
            for (const repl::ElementState& l : listings->elements) {
                repl::UnavailableEntry e;
                const auto li = tlr_number(repl::rpcf::element_value(l, l_index), 0);
                const auto lh = repl::rpcf::bool_or(repl::rpcf::element_value(l, l_hidden), false);
                if (!li || !lh) return std::nullopt;
                e.index = static_cast<int32_t>(*li);
                e.hidden = *lh;
                e.failures = bytes_of(repl::rpcf::element_value(l, l_failures));
                t.listings.push_back(std::move(e));
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

/// The unavailable lists of the ClientOpenVendor calls the PlayerController channel `channel` got in `got[from..]`, one for each call.
static std::vector<std::optional<repl::UnavailableList>> tlr_open_lists(const std::vector<fake::Delivered>& got, size_t from, uint32_t channel) {
    std::vector<std::optional<repl::UnavailableList>> out;
    for (size_t i = from; i < got.size(); ++i) {
        if (got[i].channel != channel) continue;
        const auto blocks = repl::read_content_blocks(got[i].content.data(), got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            size_t p = b.payload_pos;
            const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), got[i].content.data(), b.payload_pos + b.payload_bits, p);
            if (!calls) continue;
            for (const repl::RpcCall& c : *calls) {
                if (c.decoded && std::string(c.rpc->name) == "ClientOpenVendor") out.push_back(tlr_unavailable_of(c));
            }
        }
    }
    return out;
}

/// The listing indexes of the one tab an unavailable list names, all hidden (empty when the list is not exactly that).
static std::vector<int> tlr_hidden_listings(const std::optional<repl::UnavailableList>& list) {
    std::vector<int> out;
    if (!list || list->size() != 1 || (*list)[0].inventory.hidden || !(*list)[0].inventory.failures.empty()) return out;
    for (const repl::UnavailableEntry& e : (*list)[0].listings) {
        if (!e.hidden || !e.failures.empty()) return {};
        out.push_back(e.index);
    }
    return out;
}

/// The window of each outpost vendor, opened by using the NPC: the hidden listings are on the wire, for the first player and for a second one.
static void test_tlr_window_lists() {
    std::printf("the window an outpost vendor opens hides the listings its shop hides in the Riverlands\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Tailoring session tests are skipped\n");
        return;
    }
    Harness h(tls_character(2, {}, {}, "cyb", 100000, 25), true, true, ws_real_db());
    CHECK(h.s != nullptr, "(joined)");
    if (!h.s) return;
    const TlsVendor textile = tls_add_vendor(h, tls::kTextileMillAsset, "Textile Mill Vendor", 400, 0);
    const TlsVendor hideworks = tls_add_vendor(h, tls::kHideworksAsset, "Hideworks Vendor", 0, 400);
    const TlsVendor smithy = tls_add_vendor(h, tlr::kSmithyAsset, "Smithy Vendor", -400, 0);
    const TlsVendor woodshop = tls_add_vendor(h, tlr::kWoodshopAsset, "Woodshop Vendor", 0, -400);
    CHECK(textile.id != 0 && hideworks.id != 0 && smithy.id != 0 && woodshop.id != 0, "the four outpost vendors stand 4 m from the player");
    if (textile.id == 0 || hideworks.id == 0 || smithy.id == 0 || woodshop.id == 0) return;
    ws_phase_in(h);
    const auto open = [&](const TlsVendor& v) {
        const size_t from = h.got.size();
        h.request(ws_interact(v.component), h.pawn_ch);
        return tlr_open_lists(h.got, from, h.pc_ch);
    };
    // what the shop hides at a Riverlands shopkeeper, worked out from the data's predicates
    const vendor::TownShops* shops = h.fx.world->town_shops();
    CHECK(shops != nullptr, "(the world has the town shops)");
    if (!shops) return;

    const auto lists = open(textile);
    CHECK(lists.size() == 1 && lists[0].has_value(), "the Textile Mill vendor's window is one ClientOpenVendor, and its list reads");
    CHECK(lists.size() == 1 && tlr_hidden_listings(lists[0]) == std::vector<int>({6, 7, 8, 9, 10, 11}),
          "it hides the six Tabletop Mountains recipe items of the Tailoring tab (listings 6 to 11) and nothing else");
    CHECK(h.s->vendor_def() == tls::kTextileMill && h.s->vendor_opens() == 1, "(the window is the Textile Mill vendor's)");

    const auto hide = open(hideworks);
    CHECK(hide.size() == 1 && tlr_hidden_listings(hide[0]) == std::vector<int>({6, 7, 8, 9, 10, 11}), "the Hideworks vendor's window hides its six Tabletop Mountains recipe items (listings 6 to 11)");
    const auto smith = open(smithy);
    CHECK(smith.size() == 1 && tlr_hidden_listings(smith[0]) == std::vector<int>({5, 6, 7, 8, 9}), "the Smithy vendor's window hides its five Tabletop Mountains recipe items (listings 5 to 9)");
    const auto wood = open(woodshop);
    CHECK(wood.size() == 1 && wood[0].has_value() && wood[0]->empty(), "the Woodshop vendor's shop has no condition: its window sends no list");
    CHECK(h.s->vendor_opens() == 4 && h.mirror.unreadable == 0, "(four windows, every chain read)");

    // a second player at the same vendor sees the same window
    WsPeer second;
    CHECK(second.join(*h.fx.world, tls_character(0, {}, {}, "cyb2", 100000, 1)), "a second character joins the same world");
    if (!second.s) return;
    ws_peer_phase_in(second);
    const size_t from = second.got.size();
    second.request(ws_interact(textile.component), second.pawn_ch);
    const auto again = tlr_open_lists(second.got, from, second.pc_ch);
    CHECK(again.size() == 1 && tlr_hidden_listings(again[0]) == std::vector<int>({6, 7, 8, 9, 10, 11}), "a second player at the Textile Mill vendor is sent the same hidden listings");
    second.request(tls_purchase(tls_cart(tls::kTextileMill, {{tls::kLightBack, 1}})), second.pc_ch);
    CHECK(second.s->vendor_purchases() == 1 && second.s->wallet().copper() == 100000 - 12 && h.s->wallet().copper() == 100000, "he pays 12 copper from his own purse for a reagent; the first player's purse is as it was");
}

/// The Smithy vendor's Armorsmithing tab: a Riverlands recipe item is bought and learned, a Tabletop Mountains one is refused, and a second use of a known recipe is refused.
static void test_tlr_smithy_recipes() {
    std::printf("the Smithy vendor's Riverlands recipe items are bought and learned, its Tabletop Mountains ones are refused\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Tailoring session tests are skipped\n");
        return;
    }
    Harness h(tls_character(0, {}, {}, "cyb", 100000, 25), true, true, ws_real_db());
    CHECK(h.s != nullptr, "(joined)");
    if (!h.s) return;
    const TlsVendor smithy = tls_add_vendor(h, tlr::kSmithyAsset, "Smithy Vendor", 400, 0);
    if (smithy.id == 0) return;
    ws_phase_in(h);
    h.request(ws_interact(smithy.component), h.pawn_ch);
    CHECK(h.s->vendor_def() == tlr::kSmithy && h.s->vendor_opens() == 1, "the Smithy vendor's window opens");
    // a Tabletop Mountains listing: refused, nothing taken, nothing given
    h.request(tls_purchase(tls_cart(tlr::kSmithy, {{tlr::kDeepwardForearmsItem, 1}})), h.pc_ch);
    CHECK(h.s->vendor_purchases() == 0 && h.s->vendor_refused() >= 1 && h.s->wallet().copper() == 100000 && !tls_held(*h.s, tlr::kDeepwardForearmsItem).has_value(),
          "the Tabletop Mountains recipe item is refused: no copper leaves and no item arrives");
    CHECK(g_log.str().find("is not for sale to this player here") != std::string::npos, "the log says it is not for sale here");
    // a Riverlands one: two of it, 65 copper each with the node's tax
    h.request(tls_purchase(tls_cart(tlr::kSmithy, {{tlr::kSwordShouldersItem, 2}})), h.pc_ch);
    CHECK(h.s->vendor_purchases() == 1 && h.s->wallet().copper() == 100000 - 130, "two Riverlands recipe items cost 65 copper each");
    const auto item = tls_in_bag(*h.s, tlr::kSwordShouldersItem);
    CHECK(item.has_value() && item->quantity == 2, "both are in the bag");
    if (!item) return;
    h.request(item_used(3, item->slot, item->instance_guid), h.pawn_ch);
    CHECK(std::find(h.s->taught_recipes().begin(), h.s->taught_recipes().end(), tlr::kSwordShouldersRecipe) != h.s->taught_recipes().end(), "using one teaches the Armorsmithing recipe");
    const auto left = tls_in_bag(*h.s, tlr::kSwordShouldersItem);
    CHECK(left.has_value() && left->quantity == 1, "one of the two is used up");
    // the recipe is known now: a second use is refused and the item stays
    if (left) {
        h.request(item_used(3, left->slot, left->instance_guid), h.pawn_ch);
        const auto kept = tls_in_bag(*h.s, tlr::kSwordShouldersItem);
        CHECK(kept.has_value() && kept->quantity == 1, "a known recipe's item is refused and kept");
        CHECK(g_log.str().find("is known already") != std::string::npos, "the log says the recipe is known already");
    }
}

/// The refusals of a start that name too little or the wrong stack, and the Simple Rucksack.
static void test_tlr_craft_orders() {
    std::printf("Tailoring: too few materials, a stack the recipe does not take, and the Simple Rucksack\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Tailoring session tests are skipped\n");
        return;
    }
    // too few: the Back takes 2 Snowdrop thread and the bag holds 1
    Harness few(tls_character(0, {tls::kBackRecipe}, {tls_stack(0, tls::kThreadSnowdrop, 1), tls_stack(1, tls::kSkinGrem, 1), tls_stack(2, tls::kLightBack, 1)}), true, true, ws_real_db());
    const WsStation few_station = tls_station(few, tls::kT1, "CraftingStation_Tailoring_T1");
    if (few.s == nullptr || few_station.id == 0) return;
    tls_start(few, few_station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)});
    CHECK(few.s->quick_refused() >= 1 && !few.s->quick_active(), "a start that names 2 thread when the bag holds 1 is refused");
    CHECK(tls_total(*few.s, tls::kThreadSnowdrop) == 1 && tls_total(*few.s, tls::kSkinGrem) == 1 && few.s->wallet().copper() == 100000, "nothing was taken and nothing paid");

    // the wrong stack: Daffodil thread for a recipe that takes Snowdrop thread
    Harness wrong(tls_character(0, {tls::kBackRecipe}, {tls_stack(0, tls::kThreadDaffodil, 2), tls_stack(1, tls::kSkinGrem, 1), tls_stack(2, tls::kLightBack, 1)}), true, true,
                  ws_real_db());
    const WsStation wrong_station = tls_station(wrong, tls::kT1, "CraftingStation_Tailoring_T1");
    if (wrong.s == nullptr || wrong_station.id == 0) return;
    tls_start(wrong, wrong_station, tls::kBackRecipe, {wss_resource(tls::kThreadDaffodil, 2), wss_resource(tls::kSkinGrem, 1)});
    CHECK(wrong.s->quick_refused() >= 1 && !wrong.s->quick_active(), "a stack the recipe does not take (Daffodil thread for the Back) is refused");
    CHECK(tls_total(*wrong.s, tls::kThreadDaffodil) == 2 && tls_total(*wrong.s, tls::kSkinGrem) == 1 && wrong.s->wallet().copper() == 100000, "nothing was taken and nothing paid");

    // the Simple Rucksack: 5 Coarse thread and the bag reagent make a bag, which lands in the item bag
    Harness bag(tls_character(2, {}, {tls_resource(0, tlr::kCoarseThread, 5), tls_resource(1, tlr::kArtisanBagReagent, 1)}, "cyb", 100000, 25), true, true, ws_real_db());
    const WsStation bag_station = tls_station(bag, tls::kT1, "CraftingStation_Tailoring_T1");
    if (bag.s == nullptr || bag_station.id == 0) return;
    tls_start(bag, bag_station, tlr::kRucksackRecipe, {wss_resource(tlr::kCoarseThread, 5)});
    CHECK(bag.s->quick_active(), "the Simple Rucksack (a Novice recipe nobody has to learn) begins");
    ws_run(bag, 2400);
    bag.settle();
    CHECK(bag.s->quick_made() == 1, "the Simple Rucksack is made");
    const auto rucksack = tls_in_bag(*bag.s, tlr::kRucksackItem);
    CHECK(rucksack.has_value() && rucksack->quality == 1 && rucksack->quantity == 1, "it is a quality 1 bag in the item bag");
    CHECK(!tls_held(*bag.s, tlr::kCoarseThread).has_value() && !tls_held(*bag.s, tlr::kArtisanBagReagent).has_value() && bag.s->wallet().copper() == 100000 - 75,
          "the 5 thread and the reagent left the bags and 75 copper left the purse");
    CHECK(bag.s->professions().state(tls::kTailoring).xp == 400LL * 25 * 25 + 120, "Tailoring paid 120 experience (100 x the bag's 1.2 slot factor)");
}

/// The Apprentice and Journeyman recipes at their own stations, as the owner's Journeyman crafts them: the price with the station's tax, the piece's quality and durability, the experience.
static void test_tlr_apprentice_journeyman() {
    std::printf("Tailoring: an Apprentice chest at Tailoring - Apprentice and a Journeyman shirt at Tailoring - Journeyman\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Tailoring session tests are skipped\n");
        return;
    }
    // the Arcane Aegis Chest: 6 Linen Bolt, 4 Tanned Wolf Hide and 9 Cloth Gauges: Chest; 1,000 copper and half again in tax
    Harness arc(tls_character(2, {tlr::kArcaneChestRecipe}, {tls_resource(0, tlr::kBoltLinen, 6), tls_resource(1, tlr::kTannedHideWolf, 4), tls_resource(2, tls::kLightChest, 9)}, "cyb", 100000, 25),
                true, true, ws_real_db());
    const WsStation arc_station = tls_station(arc, tls::kT2, "CraftingStation_Tailoring_T2");
    if (arc.s == nullptr || arc_station.id == 0) return;
    tls_start(arc, arc_station, tlr::kArcaneChestRecipe, {wss_resource(tlr::kBoltLinen, 6), wss_resource(tlr::kTannedHideWolf, 4)});
    CHECK(arc.s->quick_active(), "the Arcane Aegis Chest begins at the Apprentice station (a Journeyman may craft an Apprentice recipe)");
    ws_run(arc, 2400);
    arc.settle();
    CHECK(arc.s->quick_made() == 1, "the chest is made");
    const auto chest = tls_in_bag(*arc.s, tlr::kArcaneChestItem);
    CHECK(chest.has_value() && chest->quality == 1 && std::fabs(chest->durability - 21.0) < 0.01, "it is quality 1 with durability 21");
    CHECK(tls_total(*arc.s, tlr::kBoltLinen) == 0 && tls_total(*arc.s, tlr::kTannedHideWolf) == 0 && tls_total(*arc.s, tls::kLightChest) == 0, "the 6 bolts, the 4 hides and the 9 reagents left the bags");
    CHECK(arc.s->wallet().copper() == 100000 - 1500, "1,500 copper left the purse (1,000 and half again in tax)");
    CHECK(arc.s->professions().state(tls::kTailoring).xp == 400LL * 25 * 25 + 400, "Tailoring paid 400 experience (200 for an Apprentice recipe x the chest's 2.0 slot factor)");

    // the Journeyman's Alchemist's Shirt: 15 Linen Prepared Yardage, 3 Emerald Powder and 23 Tailor's Collar Fastenings; 2,500 copper and half again
    Harness jm(tls_character(2, {tlr::kAlchemyShirtRecipe},
                             {tls_resource(0, tlr::kPreparedYardageLinen, 15), tls_resource(1, tlr::kPowderEmerald, 3), tls_resource(2, tlr::kArtisanShirtReagent, 23)}, "cyb", 100000, 25),
               true, true, ws_real_db());
    const WsStation jm_station = tls_station(jm, tlr::kT3, "CraftingStation_Tailoring_T3");
    if (jm.s == nullptr || jm_station.id == 0) return;
    tls_start(jm, jm_station, tlr::kAlchemyShirtRecipe, {wss_resource(tlr::kPreparedYardageLinen, 15), wss_resource(tlr::kPowderEmerald, 3)});
    CHECK(jm.s->quick_active(), "the Journeyman Alchemist's Shirt begins at the Journeyman station");
    ws_run(jm, 2400);
    jm.settle();
    CHECK(jm.s->quick_made() == 1, "the shirt is made");
    const auto shirt = tls_in_bag(*jm.s, tlr::kAlchemyShirtItem);
    CHECK(shirt.has_value() && shirt->quality == 1 && std::fabs(shirt->durability - 35.0) < 0.01, "it is quality 1 with durability 35");
    CHECK(jm.s->wallet().copper() == 100000 - 3750, "3,750 copper left the purse (2,500 and half again in tax)");
    CHECK(jm.s->professions().state(tls::kTailoring).xp == 400LL * 25 * 25 + 450, "Tailoring paid 450 experience (300 for a Journeyman recipe x the shirt's 1.5 slot factor)");
}

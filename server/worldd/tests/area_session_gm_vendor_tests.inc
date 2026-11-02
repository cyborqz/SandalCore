// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_gm_vendor_tests.inc: the GM vendor's purchases of materials
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs it through aoc_tests::run; it is included from area_session_registered.inc)
// ============================================================================

// The item stacks of a cart count against the item bag's free slots; the stacks of materials go to the material bags. A full item bag does not stop a purchase of materials, and a cart
// the material bag cannot hold is refused whole.
static void test_gm_materials_with_a_full_item_bag() {
    std::printf("the GM vendor: materials go to the material bag when the item bag is full\n");
    std::vector<svc::InventoryItem> full = kit();
    full.erase(std::remove_if(full.begin(), full.end(), [](const svc::InventoryItem& i) { return i.container == Bag; }), full.end());
    for (int i = 0; i < items::kBackpackSlots; ++i) full.push_back(make_item(Bag, i, fi::kPotion, 500 + static_cast<uint32_t>(i)));
    VendorHarness f(cyb_record(full));
    {   // the materials vendor sells a material: the fixture's Coal (1 x 1, stacks of 20)
        std::vector<vendor::GmVendor> defs;
        vendor::GmVendor gear;
        gear.guid = f.gear_def;
        gear.name = "PTR_ONLY_Vendor_Gear_0-9";
        gear.kind = "Gear";
        gear.label = "GM Gear 0-9";
        gear.items = {fi::kMace1H, fi::kPotion};
        gear.listings = 2;
        gear.tabs = 1;
        defs.push_back(gear);
        vendor::GmVendor mats;
        mats.guid = f.mats_def;
        mats.name = "PTR_ONLY_Vendor_Materials";
        mats.kind = "Materials";
        mats.label = "GM Materials";
        mats.items = {cf::resource::kCoal};
        mats.listings = 1;
        mats.tabs = 1;
        defs.push_back(mats);
        f.fx.world->set_gm_vendors(vendor::GmVendors::from_defs(std::move(defs)));
    }
    f.play();
    f.run(milliseconds(1500));
    f.talk_to(f.gm2);
    CHECK(f.s->vendor_actor() == f.gm2 && bag_items(f) == 70, "the materials vendor's window is open and the item bag holds 70 stacks: no free slot");
    f.client_request(purchase(cart_of(f.mats_def, {{cf::resource::kCoal, 41}})), f.pc_ch);
    CHECK(f.s->vendor_purchases() == 1 && f.s->vendor_refused() == 0, "41 Coal (three stacks) are sold although the item bag has no free slot");
    CHECK(f.s->inventory().count(ItemContainer::Resource0) == 3 && bag_items(f) == 70, "they are in the material bag, and the item bag is as it was");
    // a cart that the material bag cannot hold: 500 more Coal are 25 stacks and the bag has 21 free cells
    f.run(milliseconds(2200));
    const size_t materials_before = f.s->inventory().count(ItemContainer::Resource0);
    f.client_request(purchase(cart_of(f.mats_def, {{cf::resource::kCoal, 500}})), f.pc_ch);
    CHECK(f.s->vendor_purchases() == 1 && f.s->vendor_refused() == 1 && f.s->inventory().count(ItemContainer::Resource0) == materials_before && bag_items(f) == 70,
          "a cart the material bag cannot hold is refused whole: nothing is added");
}
AOC_REGISTER_TEST(test_gm_materials_with_a_full_item_bag);

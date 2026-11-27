// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_market_window_tests.inc: the Marketplace window as the game's own client keeps it
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc)
//
//    the pages         the window's open is followed by the storefront pages: 100 storefronts a page, bComplete on the last, the player's own storefront left out,
//                      one empty complete page for a market nobody sells at
//    the story         a player lists an item, the Listings tab shows it; the player logs out and in again and the tab shows it; the world restarts and the tab shows it;
//                      nothing is lost or doubled
//    one save          a second listing while the first one's save is in flight is refused as in progress; a stuck save holds up no other character's; a service that is down
//                      leaves the listing waiting, bounded, with nothing given back; the world says a character is saving until its saves are answered and its bags written
//    the refusal       a refused listing resyncs the client's bags and its own listings; only the listed containers are listed from
//    the log-in        a seller's log-in waits for his listing's save, and for a listing and a mail claim at once (one bound for both), never sees a gap after a refused save of an
//                      ended session, is served beside another character's, and a listing of a character whose log-in has begun is refused (both looks of the mark)
//
//  WindowClient plays the client's rules for this window: the menu learns the market from the open, the Listings tab learns it only from a storefront page that is the last
//  one, a click on the tab asks for the data with the market the tab knows and the character's own guid, and the tab shows an answer only when it names the character
//  itself, every listing of it keyed by the listing's guid; a registration answer adds its listing.
// ============================================================================
#include "login/login_worker.h"
#include "protocol/repl/market_rpc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>

namespace market_window_test {

using ListingKey = std::array<uint32_t, 4>;

/// One storefront page of the open, as the test reads it.
struct PageSeen {
    size_t   storefronts = 0;
    bool     complete = false;
    int64_t  node = 0;
    uint64_t version = 0;
    uint64_t rarity = 0;
};

/// What the client of this window knows and shows (a new client for each client process).
struct WindowClient {
    repl::GuidValue       self;
    repl::GuidValue       menu_market;   // from the open
    repl::GuidValue       tab_market;    // from the last storefront page; zero until one comes
    std::set<ListingKey>  shown;         // the listings the Listings tab shows
    std::vector<PageSeen> pages;
    std::vector<repl::GuidValue> storefront_guids;
    size_t                answers = 0;   // the data answers that came

    explicit WindowClient(const repl::GuidValue& own) : self(own) {}

    static ListingKey key_of(const repl::GuidValue& g) { return {g.a, g.b, g.c, g.d}; }

    void hear(const std::vector<repl::RpcCall>& calls) {
        for (const repl::RpcCall& c : calls) {
            if (!c.rpc || !c.decoded) continue;
            const std::string name = c.rpc->name;
            if (name == "ClientOpenMarketplaceUI") {
                menu_market = {word_of(c, "MarketplaceGuid.A").value_or(0), word_of(c, "MarketplaceGuid.B").value_or(0), word_of(c, "MarketplaceGuid.C").value_or(0),
                               word_of(c, "MarketplaceGuid.D").value_or(0)};
            } else if (name == "ClientReceiveMarketplaceHeaders") {
                PageSeen page;
                const repl::ArrayValue* list = array_of(c, "Headers");
                page.storefronts = list ? list->elements.size() : 0;
                if (list) {
                    for (const repl::ElementState& e : list->elements) {
                        storefront_guids.push_back({member_word(c, "Headers", e, "Headers.PlayerGuid.A").value_or(0), member_word(c, "Headers", e, "Headers.PlayerGuid.B").value_or(0),
                                                    member_word(c, "Headers", e, "Headers.PlayerGuid.C").value_or(0), member_word(c, "Headers", e, "Headers.PlayerGuid.D").value_or(0)});
                    }
                }
                const auto* complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*c.rpc, c.params, "bComplete"));
                page.complete = complete && *complete;
                page.node = number_of(c, "NodeId").value_or(0);
                page.version = enum_of(c, "HeaderVersion").value_or(0);
                page.rarity = enum_of(c, "ItemRarity").value_or(99);
                pages.push_back(page);
                if (page.complete) tab_market = menu_market;
            } else if (name == "ClientReceiveMarketplaceData") {
                ++answers;
                const repl::GuidValue named{word_of(c, "PlayerGuid.A").value_or(0), word_of(c, "PlayerGuid.B").value_or(0), word_of(c, "PlayerGuid.C").value_or(0),
                                            word_of(c, "PlayerGuid.D").value_or(0)};
                if (!(named == self)) continue;   // the tab drops an answer that names another character
                shown.clear();
                if (const repl::ArrayValue* list = array_of(c, "MarketplaceItemData")) {
                    for (const repl::ElementState& e : list->elements) {
                        shown.insert(key_of({member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.MarketplaceItemGuid.A").value_or(0),
                                             member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.MarketplaceItemGuid.B").value_or(0),
                                             member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.MarketplaceItemGuid.C").value_or(0),
                                             member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.MarketplaceItemGuid.D").value_or(0)}));
                    }
                }
            } else if (name == "ClientMarketplaceItemRegistrationResponse") {
                if (enum_of(c, "Response").value_or(1) == 0) shown.insert(key_of(listing_guid(c)));
            }
        }
    }

    /// The click on the Listings tab.
    BunchWriter click_listings_tab() const { return data_request(tab_market, self, true); }
};

/// The session's own step (the tick that takes a save's answer), until `done` or `milliseconds` of real time have passed.
static bool step_session_until(MarketHarness& h, const std::function<bool()>& done, int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        h.step();
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/// A listing of a stranger at the test market: `n` makes the seller, the listing and the day.
static market::Listing stranger_listing(uint32_t n) {
    market::Listing l;
    l.node = kNode;
    l.seller = repl::GuidValue{0x51000000u + n, 7u, 11u, 13u};
    l.seller_name = "seller " + std::to_string(n);
    l.guid = repl::GuidValue{0x52000000u + n, 7u, 11u, 13u};
    l.item = make_item(Bag, 1, fi::kPotion, 5000 + n, 1);
    l.item_name = "potion";
    l.price_per_unit = kPrice;
    l.issue_date = 1000.0 + n;
    l.expiration_date = l.issue_date + kLifeSeconds;
    return l;
}

}  // namespace market_window_test

using namespace market_window_test;

/// The open is followed by the storefront pages of the market: 100 storefronts a page, bComplete on the last, the player's own storefront left out.
static void test_marketplace_open_pushes_the_storefront_pages() {
    std::printf("the Marketplace: the window's open is followed by the storefront pages\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    for (uint32_t n = 1; n <= 230; ++n) h.fx.world->market_listings()->add(stranger_listing(n));
    market::Listing mine = stranger_listing(900);
    mine.seller = character_guid(h);
    mine.seller_name = "cyb";
    h.fx.world->market_listings()->add(mine);   // the player's own storefront is in the world's cache too
    WindowClient client(character_guid(h));
    const size_t from = h.got.size();
    h.open_market(h.manager);
    client.hear(pc_calls_since(h, from));
    CHECK(client.pages.size() == 3, "230 other storefronts come as three pages");
    if (client.pages.size() == 3) {
        CHECK(client.pages[0].storefronts == 100 && client.pages[1].storefronts == 100 && client.pages[2].storefronts == 30, "the pages hold 100, 100 and 30 storefronts");
        CHECK(!client.pages[0].complete && !client.pages[1].complete && client.pages[2].complete, "only the last page is complete");
        for (const PageSeen& p : client.pages) {
            CHECK(p.node == kNode && p.version == 1843 && p.rarity == 7, "a page names the market's node, the header version and the any-rarity filter");
        }
    }
    bool own = false;
    for (const repl::GuidValue& g : client.storefront_guids) own = own || g == character_guid(h);
    CHECK(!own, "the player's own storefront is not among the pages");
    CHECK(client.storefront_guids.size() == 230, "every other storefront is on a page once");
    CHECK(!(client.tab_market == repl::GuidValue{}) && client.tab_market == client.menu_market, "the Listings tab knows the market once the last page came");
    CHECK(count_calls(pc_calls_since(h, from), "ClientReceiveMarketplaceData") == 0, "the open sends no seller data");
}

/// A market nobody sells at still gets its one empty, complete page: the Listings tab needs the last page to learn the market.
static void test_marketplace_open_sends_one_empty_page() {
    std::printf("the Marketplace: a market with no storefront gets one empty complete page\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    WindowClient client(character_guid(h));
    const size_t from = h.got.size();
    h.open_market(h.manager);
    client.hear(pc_calls_since(h, from));
    CHECK(client.pages.size() == 1 && client.pages[0].storefronts == 0 && client.pages[0].complete, "one empty complete page");
    CHECK(client.tab_market == client.menu_market && !(client.tab_market == repl::GuidValue{}), "the tab knows the market");
}

/// The player's story: list, relog, restart; the Listings tab shows the listing every time, once.
static void test_marketplace_listings_tab_story() {
    std::printf("the Marketplace: a listing shows in the Listings tab, after a relog and after a restart\n");
    MarketSaveSink sink;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const repl::GuidValue self = character_guid(h);

    // the first client lists two potions; the tab shows the listing, and shows it after a click too
    WindowClient first(self);
    size_t from = h.got.size();
    h.open_market(h.manager);
    first.hear(pc_calls_since(h, from));
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
    first.hear(pc_calls_since(h, from));
    CHECK(first.shown.size() == 1, "the tab shows the new listing");
    from = h.got.size();
    h.client_request(first.click_listings_tab(), h.pc_ch);
    first.hear(pc_calls_since(h, from));
    CHECK(first.answers == 1, "a click on the Listings tab is answered");
    CHECK(first.shown.size() == 1, "the tab still shows the one listing after the click");
    const std::set<ListingKey> listed = first.shown;
    CHECK(sink.market_saves.size() == 1, "the listing was saved once");
    CHECK(h.s->inventory().at(Bag, 1) && h.s->inventory().at(Bag, 1)->quantity == 3, "the bag holds the three units that stayed");

    // the player logs out and in again on the same world: a new client, the same listing
    h.s->finish("the client left");
    h.s.reset();
    MarketPeer peer(h, cyb_record());
    peer.play();
    WindowClient second(self);
    from = peer.got.size();
    peer.request(interact(market_component(h.fx.world->registry(), h.manager)), peer.pawn_ch);
    second.hear(market_calls_since(peer.got, peer.pc_ch, from));
    from = peer.got.size();
    peer.request(second.click_listings_tab(), peer.pc_ch);
    second.hear(market_calls_since(peer.got, peer.pc_ch, from));
    CHECK(second.answers == 1, "after the relog a click on the Listings tab is answered");
    CHECK(second.shown == listed, "after the relog the tab shows the listing, once");
    peer.s->finish("the client left");
    peer.s.reset();
    CHECK(h.fx.world->market_listings()->size() == 1, "the world holds the one listing");

    // the world restarts on the saved rows: a new client, the same listing
    MarketSaveSink after;
    after.saved_listings.status = svc::CallStatus::Ok;
    after.saved_listings.listings = sink.market_saves;
    MarketHarness restarted(cyb_record(), &after);
    restarted.play();
    restarted.run(milliseconds(1500));
    CHECK(restarted.fx.world->market_listings()->size() == 1, "the restarted world read the one saved listing");
    WindowClient third(character_guid(restarted));
    CHECK(third.self == self, "the character has the same guid after the restart");
    from = restarted.got.size();
    restarted.open_market(restarted.manager);
    third.hear(pc_calls_since(restarted, from));
    from = restarted.got.size();
    restarted.client_request(third.click_listings_tab(), restarted.pc_ch);
    third.hear(pc_calls_since(restarted, from));
    CHECK(third.answers == 1, "after the restart a click on the Listings tab is answered");
    CHECK(third.shown == listed, "after the restart the tab shows the saved listing, once");
    CHECK(after.market_saves.empty(), "the restart saved no listing again");
}

/// A second listing while the first one's save is in flight is refused as in progress, takes nothing and never reaches the service; the first one lands.
static void test_marketplace_one_save_in_flight() {
    std::printf("the Marketplace: a second listing while the first one's save is in flight waits\n");
    MarketSaveSink sink;
    sink.hold = true;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    const svc::InventoryItem* other = h.s->inventory().at(Bag, 0);
    CHECK(held && other && held->instance_guid != other->instance_guid, "the bag holds two stacks");
    if (!held || !other) {
        sink.release();
        return;
    }
    const std::string other_guid = other->instance_guid;
    const int32_t other_units = other->quantity;
    sink.reset_started();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 1, kPrice), true)});
    CHECK(sink.wait_started(3000), "the first listing's save is in flight");
    const int64_t purse = h.s->wallet().copper();
    const size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*other), 1, kPrice), true)});
    for (int i = 0; i < 3; ++i) h.step();
    const repl::RpcCall* answer = registration_answer(pc_calls_since(h, from));
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 3, "the second listing is refused with REQUEST_ALREADY_IN_PROGRESS (3)");
    const svc::InventoryItem* still = h.s->inventory().find_guid(other_guid);
    CHECK(still && still->quantity == other_units && h.s->wallet().copper() == purse, "the second listing took no unit and no fee");
    {
        std::lock_guard<std::mutex> lock(sink.mu);
        CHECK(sink.market_saves.size() == 1, "the service was asked for one save only");
    }
    sink.release();
    CHECK(step_session_until(h, [&] { return h.fx.world->market_listings()->size() == 1; }, 3000), "the first listing lands");
    CHECK(h.s->market_pending() == 0, "and nothing is in flight any more");
}

/// The world counts a character's saves that have no answer taken, and the bags of a session that ended with one: a log-in reads that count and waits.
static void test_marketplace_log_in_barrier_follows_the_saves() {
    std::printf("the Marketplace: the world says a character is saving from the listing's save until the ended session's bags are written\n");
    MarketSaveSink sink;
    sink.hold = true;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const std::string id = cyb_record().id;
    ::aoc::worldd::market::MarketStore* store = h.fx.world->market_listings();
    CHECK(!store->saving(id), "nothing is saving before a listing");
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held != nullptr, "the bag holds the stack");
    if (!held) {
        sink.release();
        return;
    }
    sink.reset_started();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 1, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save is in flight");
    CHECK(store->saving(id), "the character is saving while the save has no answer");
    CHECK(!store->saving("00000000000000000000000000000001"), "another character is not");
    // the session ends with the save in flight: the world holds it and the bags
    const repl::GuidValue seller = character_guid(h);
    h.s->finish("the client left");
    h.s.reset();
    const size_t bags_before = sink.inventories.size();
    CHECK(store->saving(id), "the character is still saving after the session ended");
    sink.release();
    CHECK(step_world_until(h, [&] { return !store->saving(id); }, 3000), "the character stops saving once the answer is taken and the bags are written");
    CHECK(sink.inventories.size() > bags_before, "the bags were written before the log-in barrier opened");
    CHECK(store->count(seller) == 1, "the listing is in the world");
}

/// A session that stays and takes its answer stops saving at once.
static void test_marketplace_log_in_barrier_opens_at_the_answer() {
    std::printf("the Marketplace: the world says a character stops saving at the listing's answer\n");
    MarketSaveSink sink;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    if (!held) return;
    h.client_request(sell_request(market, items::to_instance(*held), 1, kPrice), h.pc_ch);
    CHECK(h.fx.world->market_listings()->size() == 1, "the listing landed");
    CHECK(!h.fx.world->market_listings()->saving(cyb_record().id), "the character is not saving after the answer");
}

/// The service fails one character's save for good: that save is asked for again at a growing pace, nothing is given back or removed, the other character's listing lands,
/// and the tick is never held up.
static void test_marketplace_stuck_save_does_not_hold_others() {
    std::printf("the Marketplace: a save the service keeps failing does not hold up another character's listing\n");
    MarketSaveSink sink;
    sink.retry_ms = 5;
    sink.stuck_character = cyb_record().id;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const int64_t purse = h.s->wallet().copper();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    for (int i = 0; i < 5; ++i) h.step();

    svc::Character second = with_start_purse(cyb_record());
    second.id = "0000000000000000000000000000c0b0";
    second.name = "bob";
    MarketPeer peer(h, second);
    peer.play();
    peer.request(interact(market_component(h.fx.world->registry(), h.manager)), peer.pawn_ch);
    const svc::InventoryItem* peer_held = peer.s->inventory().at(Bag, 1);
    CHECK(peer_held != nullptr, "the second seller's bag holds a stack");
    if (!peer_held) return;
    peer.client_sends({peer.client.actor(peer.pc_ch, sell_request(market, items::to_instance(*peer_held), 1, kPrice + 1), true)});
    long slowest_ms = 0;
    for (int i = 0; i < 600 && h.fx.world->market_listings()->size() == 0; ++i) {
        const auto started = std::chrono::steady_clock::now();
        h.step();
        peer.step();
        slowest_ms = std::max<long>(slowest_ms, static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (int i = 0; i < 20; ++i) {
        h.step();
        peer.step();
    }
    CHECK(h.fx.world->market_listings()->size() == 1 && h.fx.world->market_listings()->count(character_guid(h)) == 0, "the second seller's listing landed and the stuck one did not");
    CHECK(peer.s->market_pending() == 0, "the second seller's save was answered while the first one's is stuck");
    CHECK(sink.stuck_tries >= 2, "the stuck save was asked for again");
    CHECK(h.s->market_pending() == 1, "the stuck save is still in flight: its outcome is not guessed");
    const svc::InventoryItem* rest = h.s->inventory().at(Bag, 1);
    CHECK(rest && rest->quantity == 3, "the two listed units are still out of the bag");
    CHECK(h.s->wallet().copper() < purse, "and the fee stays taken");
    CHECK(slowest_ms < 250, "no tick was held up by the stuck save");
}

/// The service is down: the save is asked for at a growing pace that stops at a cap, the bags are not saved meanwhile, nothing is given back, and the tick is never held up;
/// when the service is back the listing is made once.
static void test_marketplace_service_down_fails_closed() {
    std::printf("the Marketplace: with the service down a listing waits, bounded, with nothing given back\n");
    MarketSaveSink sink;
    sink.answer = svc::CallStatus::Unavailable;
    sink.reason = "the service is down";
    sink.retry_ms = 40;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const int64_t purse = h.s->wallet().copper();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    long slowest_ms = 0;
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(400)) {
        const auto started = std::chrono::steady_clock::now();
        h.step();
        slowest_ms = std::max<long>(slowest_ms, static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    size_t tries = 0;
    {
        std::lock_guard<std::mutex> lock(sink.mu);
        tries = sink.market_saves.size();
        bool bag_saved = false;
        for (const std::string& what : sink.order) bag_saved = bag_saved || what == "bag";
        CHECK(!bag_saved, "no bag save was written while the listing's outcome is unknown");
    }
    CHECK(tries >= 2 && tries <= 7, "the save was asked for again at a growing pace, not in a loop");
    CHECK(h.s->market_pending() == 1 && h.fx.world->market_listings()->size() == 0, "the listing is neither made nor refused");
    const svc::InventoryItem* rest = h.s->inventory().at(Bag, 1);
    CHECK(rest && rest->quantity == 3 && h.s->wallet().copper() < purse, "the units and the fee are neither given back nor taken twice");
    CHECK(slowest_ms < 250, "no tick was held up");
    sink.answer = svc::CallStatus::Ok;
    sink.reason.clear();
    CHECK(step_session_until(h, [&] { return h.fx.world->market_listings()->size() == 1; }, 3000), "when the service is back the listing is made");
    CHECK(h.fx.world->market_listings()->size() == 1, "once");
}

/// A refused listing leaves the client with the server's bags and its own listings.
static void check_refusal_resynced(MarketHarness& h, size_t from, const char* what) {
    const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(0) != 0, what);
    CHECK(count_calls(test_mailbox_pawn_calls_since(h, from), "ClientReceiveStoragePayload") >= 2, "the client is sent its bags again");
    const repl::RpcCall* data = call_named(calls, "ClientReceiveMarketplaceData");
    CHECK(data != nullptr, "the client is sent its own listings again");
    if (data) {
        const repl::GuidValue named{word_of(*data, "PlayerGuid.A").value_or(0), word_of(*data, "PlayerGuid.B").value_or(0), word_of(*data, "PlayerGuid.C").value_or(0),
                                    word_of(*data, "PlayerGuid.D").value_or(0)};
        CHECK(named == character_guid(h), "and the listings are the player's own");
    }
}

static void test_marketplace_refusal_resyncs_the_client() {
    std::printf("the Marketplace: a refused listing resyncs the client's bags and window\n");
    {
        MarketHarness h;
        h.play();
        h.run(milliseconds(1500));
        h.open_market(h.manager);
        const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
        const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
        if (!held) return;
        const size_t from = h.got.size();
        h.client_request(sell_request(market, items::to_instance(*held), 2, 0), h.pc_ch);   // no price: refused before anything is taken
        check_refusal_resynced(h, from, "a listing with no price is refused");
    }
    {
        MarketSaveSink sink;
        sink.answer = svc::CallStatus::Refused;
        sink.reason = "the store refused it";
        MarketHarness h(cyb_record(), &sink);
        h.play();
        h.run(milliseconds(1500));
        h.open_market(h.manager);
        const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
        const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
        if (!held) return;
        const size_t from = h.got.size();
        h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
        check_refusal_resynced(h, from, "a listing the store refuses is refused");
        const svc::InventoryItem* back = h.s->inventory().at(Bag, 1);
        CHECK(back && back->quantity == 5, "and the units are back in the bag");
    }
}

/// Only the backpack and the resource containers are listed from.
static void test_marketplace_sells_from_listed_containers_only() {
    std::printf("the Marketplace: only the containers the Sell tab lists from are listed from\n");
    using svc::ItemContainer;
    CHECK(::aoc::worldd::market::sellable_container(ItemContainer::Backpack), "the backpack is listed from");
    CHECK(::aoc::worldd::market::sellable_container(ItemContainer::Resource0) && ::aoc::worldd::market::sellable_container(ItemContainer::Resource1) &&
              ::aoc::worldd::market::sellable_container(ItemContainer::Resource2),
          "the resource containers are listed from");
    CHECK(!::aoc::worldd::market::sellable_container(ItemContainer::Equipment) && !::aoc::worldd::market::sellable_container(ItemContainer::Quest),
          "worn gear and the quest storage are not");
    CHECK(!::aoc::worldd::market::sellable_container(static_cast<ItemContainer>(6)) && !::aoc::worldd::market::sellable_container(static_cast<ItemContainer>(200)),
          "a container no list names is not");
}

// ---- the log-in's wait for a seller's unanswered writes -------------------------------------------------------------------------------------------------------------
//
//  The world answers ONE predicate to a log-in (PlayerWorld::character_writes_pending): a listing's save without its answer, a mail claim that is not closed, or a write the
//  saver still holds, and the log-in has ONE bound (LoginService::Barrier::bound, 20 s) for all of them.  A log-in marks its character when its ticket is redeemed
//  (PlayerWorld::login_began), and while the mark stands the character's session starts no new listing and no new claim.

/// The log-in tests' gateway: every ticket is its character's own, and the redeems of each ticket are counted.
struct MarketLoginGateway : svc::GatewayApi {
    std::mutex                 mu;
    std::map<std::string, int> redeems;   // by ticket
    svc::RedeemResult redeem_ticket(const std::string& ticket, const std::string& character_id, uint32_t) override {
        {
            std::lock_guard<std::mutex> lock(mu);
            ++redeems[ticket];
        }
        svc::RedeemResult result;
        result.status = svc::CallStatus::Ok;
        result.account_id = 3;
        result.character_id = character_id;
        return result;
    }
    int redeemed(const std::string& ticket) {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = redeems.find(ticket);
        return it == redeems.end() ? 0 : it->second;
    }
};

/// The log-in tests' character service: the record it answers carries the bags the sink's service holds at that moment (for the one character `stored_for`), and every read is counted.
struct MarketLoginCharacters : svc::CharacterApi {
    MarketSaveSink&            sink;
    svc::Character             record;
    std::string                stored_for;
    std::mutex                 mu;
    std::map<std::string, int> reads;   // by character
    MarketLoginCharacters(MarketSaveSink& s, svc::Character base, std::string who) : sink(s), record(std::move(base)), stored_for(std::move(who)) {}
    svc::CharacterResult get_character(const std::string& id) override {
        {
            std::lock_guard<std::mutex> lock(mu);
            ++reads[id];
        }
        svc::CharacterResult result;
        result.status = svc::CallStatus::Ok;
        result.character = record;
        result.character.id = id;
        result.character.account_id = 3;
        if (id == stored_for) result.character.items = sink.stored_bags(record.items);
        return result;
    }
    svc::CallStatus save_position(const std::string&, const svc::Position&) override { return svc::CallStatus::Ok; }
    int loaded(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = reads.find(id);
        return it == reads.end() ? 0 : it->second;
    }
};

/// The world's barrier on a log-in service: the redeemed ticket marks the character, and the wait asks the world, as worldd's main does.
static void set_world_barrier(LoginService& service, PlayerWorld& world, std::chrono::milliseconds bound) {
    LoginService::Barrier barrier;
    PlayerWorld* w = &world;
    barrier.redeemed = [w](uint64_t session, const std::string& who) { w->login_began(who, session); };
    barrier.writes_pending = [w](const std::string& who) { return w->character_writes_pending(who); };
    barrier.bound = bound;
    barrier.poll = std::chrono::milliseconds(2);
    service.set_barrier(std::move(barrier));
}

/// A log-in of one character on its own thread, as the log-in worker runs it, with the world's barrier.
struct MarketLogin {
    static constexpr const char* kTicket = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    MarketLoginGateway       gateway;
    MarketLoginCharacters    characters;
    LoginService             service;
    std::string              id;
    uint64_t                 session;
    std::future<LoginResult> result;
    MarketLogin(PlayerWorld& world, MarketSaveSink& sink, svc::Character record, uint64_t session_id, std::chrono::milliseconds bound = std::chrono::seconds(10))
        : characters(sink, record, record.id), service(gateway, characters), id(record.id), session(session_id) {
        set_world_barrier(service, world, bound);
    }
    void start() {
        result = std::async(std::launch::async, [this] { return service.login(LoginRequest{id, kTicket}, 7, session); });
    }
    bool done(std::chrono::milliseconds wait) { return result.wait_for(wait) == std::future_status::ready; }
};

/// Lets every hold of the sink go when a test ends, whatever it did: a log-in thread of a failed test must not wait out its bound.  Declare it after the log-ins.
struct SinkReleaser {
    MarketSaveSink& sink;
    ~SinkReleaser() {
        sink.release();
        sink.release_claim();
        sink.release_bags();
    }
};

static bool wait_until_true(const std::function<bool()>& ok, int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!ok()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// The potion units a log-in loaded.
static int64_t loaded_units(const LoginResult& r) {
    int64_t n = 0;
    for (const svc::InventoryItem& it : r.character.items) n += it.item_record_id == fi::kPotion ? it.quantity : 0;
    return n;
}

/// The seller lists two of his five potions with the save held in `sink`, and a mail claim of the same character is held in `sink` too, on the mail worker.  The session never
/// starts both (a claim waits for a listing and a listing for a claim), so the claim stands for one that an earlier session of the character left: it is queued here directly, before
/// any log-in marks the character (the worker refuses a claim after the mark).  False when the test could not be set up.
static bool start_a_listing_and_a_claim(MarketHarness& h, MarketSaveSink& sink) {
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return false;
    sink.reset_started();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(::aoc::worldd::market::marketplace_guid(kNode), items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save is in flight");
    MailJobRequest claim;
    claim.kind = MailJobKind::Claim;
    claim.character_id = cyb_record().id;
    claim.claims.push_back(svc::MailClaim{guid_n(9301), guid_n(9302)});
    const uint64_t ticket = h.fx.world->submit_mail_job(std::move(claim));
    CHECK(ticket != 0, "the claim is queued");
    if (ticket == 0) return false;
    h.fx.world->forget_mail_job(ticket);   // nobody takes its answer: the worker closes the claim when it is answered
    CHECK(sink.wait_claim_started(3000), "the claim's call is in flight");
    CHECK(h.fx.world->character_writes_pending(cyb_record().id), "the world says the character has writes without an answer");
    return true;
}

/// An unanswered listing save holds the character's log-in until the answer is taken, and the log-in then reads the bags the save wrote.
static void test_marketplace_login_waits_for_an_unanswered_listing_save() {
    std::printf("the Marketplace: a log-in of the seller waits for the listing's save and reads the bags the save wrote\n");
    MarketSaveSink sink;
    sink.hold = true;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const std::string id = cyb_record().id;
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) {
        sink.release();
        return;
    }
    MarketLogin login(*h.fx.world, sink, cyb_record(), 71);
    SinkReleaser unhold{sink};
    sink.reset_started();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(::aoc::worldd::market::marketplace_guid(kNode), items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save is in flight");
    login.start();
    CHECK(wait_until_true([&] { return h.fx.world->login_pending(id); }, 3000), "the log-in redeemed its ticket and marked the character");
    CHECK(!login.done(milliseconds(150)), "the log-in is held while the save has no answer");
    CHECK(login.characters.loaded(id) == 0, "and nothing of the character is read");
    CHECK(login.gateway.redeemed(MarketLogin::kTicket) == 1, "the ticket was redeemed once");
    sink.release();
    CHECK(step_session_until(h, [&] { return login.done(milliseconds(0)); }, 5000), "the log-in goes on once the save is answered and the session has taken the answer");
    if (!login.result.valid()) return;
    const LoginResult r = login.result.get();
    h.fx.world->login_ended(71);
    CHECK(r.accepted && login.characters.loaded(id) == 1, "it is accepted and the character is read once");
    CHECK(loaded_units(r) == 3, "from the bags the listing's save wrote: three of the five units");
    CHECK(h.fx.world->market_listings()->count(character_guid(h)) == 1, "and the listing is in the world");
}

/// A listing's save and a mail claim of the same character without an answer, at once: the log-in opens only after BOTH are answered, in either order.
static void check_login_waits_for_both(bool listing_first) {
    MarketSaveSink sink;
    sink.hold = true;
    sink.hold_claim = true;
    MarketHarness h(cyb_record(), &sink);
    const std::string id = cyb_record().id;
    MarketLogin login(*h.fx.world, sink, cyb_record(), 72);
    SinkReleaser unhold{sink};
    if (!start_a_listing_and_a_claim(h, sink)) return;
    ::aoc::worldd::market::MarketStore* store = h.fx.world->market_listings();
    login.start();
    CHECK(wait_until_true([&] { return h.fx.world->login_pending(id); }, 3000), "the log-in marked the character");
    CHECK(!login.done(milliseconds(150)) && login.characters.loaded(id) == 0, "the log-in waits for the listing and the claim");
    if (listing_first) {
        sink.release();
        CHECK(step_session_until(h, [&] { return store->count(character_guid(h)) == 1 && !store->saving(id); }, 3000), "the listing is answered and the session has taken its answer");
        CHECK(!login.done(milliseconds(200)) && login.characters.loaded(id) == 0, "the log-in still waits: the claim has no answer");
        sink.release_claim();
    } else {
        sink.release_claim();
        CHECK(wait_until_true([&] {
                  std::lock_guard<std::mutex> lock(sink.mu);
                  return std::count(sink.order.begin(), sink.order.end(), std::string("bag")) >= 1;   // the claim stored its bags: its call is answered
              }, 3000),
              "the claim is answered");
        CHECK(store->saving(id), "and the listing still holds the character");
        CHECK(!login.done(milliseconds(200)) && login.characters.loaded(id) == 0, "the log-in still waits: the listing has no answer");
        sink.release();
    }
    CHECK(step_session_until(h, [&] { return login.done(milliseconds(0)); }, 5000), "the log-in opens once both are answered");
    if (!login.result.valid()) return;
    const LoginResult r = login.result.get();
    h.fx.world->login_ended(72);
    CHECK(r.accepted && login.characters.loaded(id) == 1, "and it is accepted and reads the character once");
    CHECK(sink.claim_saves.load() == 1, "the claim was asked for once");
    std::lock_guard<std::mutex> lock(sink.mu);
    CHECK(sink.market_saves.size() == 1, "the listing was saved once");
}

static void test_marketplace_login_waits_for_a_listing_and_a_claim_listing_answered_first() {
    std::printf("the Marketplace: a log-in waits for a listing's save and a mail claim at once (the listing is answered first)\n");
    check_login_waits_for_both(true);
}

static void test_marketplace_login_waits_for_a_listing_and_a_claim_claim_answered_first() {
    std::printf("the Marketplace: a log-in waits for a listing's save and a mail claim at once (the claim is answered first)\n");
    check_login_waits_for_both(false);
}

/// ONE bound for every wait: with the listing answered at 40% of it and the claim never, the log-in is refused at the bound counted from its first wait (about 1.0 bounds), not
/// after a second bound that starts when the first wait ends (about 1.4 bounds).  Nothing is read.
static void test_marketplace_login_has_one_bound_for_both_waits() {
    std::printf("the Marketplace: the log-in's bound is one bound for the listing's save and the claim together\n");
    MarketSaveSink sink;
    sink.hold = true;
    sink.hold_claim = true;
    MarketHarness h(cyb_record(), &sink);
    const std::string id = cyb_record().id;
    const auto bound = std::chrono::milliseconds(1000);
    MarketLogin login(*h.fx.world, sink, cyb_record(), 73, bound);
    SinkReleaser unhold{sink};
    if (!start_a_listing_and_a_claim(h, sink)) return;
    ::aoc::worldd::market::MarketStore* store = h.fx.world->market_listings();
    const auto began = std::chrono::steady_clock::now();
    login.start();
    CHECK(wait_until_true([&] { return h.fx.world->login_pending(id); }, 3000), "the log-in marked the character");
    std::this_thread::sleep_for(bound * 4 / 10 - (std::chrono::steady_clock::now() - began));
    sink.release();
    CHECK(step_session_until(h, [&] { return store->count(character_guid(h)) == 1 && !store->saving(id); }, 3000), "the listing is answered and its answer taken, at about 40% of the bound");
    CHECK(login.done(std::chrono::seconds(5)), "the log-in is answered");
    const auto took = std::chrono::steady_clock::now() - began;
    if (!login.result.valid()) return;
    const LoginResult r = login.result.get();
    h.fx.world->login_ended(73);
    CHECK(!r.accepted && r.error == login_text::kServiceUnavailable, "the log-in is refused with the try-again text");
    CHECK(login.characters.loaded(id) == 0, "and nothing of the character is read");
    CHECK(took >= std::chrono::milliseconds(900), "after about the bound");
    CHECK(took < std::chrono::milliseconds(1250), "and not after a second bound that starts when the listing's wait ends");
    CHECK(login.gateway.redeemed(MarketLogin::kTicket) == 1, "the ticket was redeemed once");
}

/// The KI-1 sequence: the session ends with a listing's save in flight, the store refuses it, and the world gives the units and the fee back into the bags it writes.  The market
/// releases its count only after those bags are queued in the saver, and the saver says the character is busy until it has written them, so a log-in that looks at any moment in
/// between is held, and the one that goes on reads the bags with the units back.
static void test_marketplace_login_never_sees_a_gap_after_a_refused_save_of_an_ended_session() {
    std::printf("the Marketplace: a log-in never sees a gap between a refused save's count and the give-back bags in the saver\n");
    MarketSaveSink sink;
    sink.hold = true;
    sink.answer = svc::CallStatus::Refused;
    sink.reason = "the store refused it";
    sink.hold_bags = true;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const std::string id = cyb_record().id;
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) {
        sink.release();
        return;
    }
    MarketLogin login(*h.fx.world, sink, cyb_record(), 74);
    SinkReleaser unhold{sink};
    ::aoc::worldd::market::MarketStore* store = h.fx.world->market_listings();
    sink.reset_started();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(::aoc::worldd::market::marketplace_guid(kNode), items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save is in flight");
    const repl::GuidValue seller = character_guid(h);
    h.s->finish("the client left");
    h.s.reset();
    CHECK(store->saving(id) && !sink.bags_busy.load(), "the session ended: the market counts the save and the saver holds no write yet");
    std::atomic<bool> counted_while_queueing{false};
    sink.on_bag_write = [&] { counted_while_queueing = store->saving(id); };
    login.start();
    CHECK(wait_until_true([&] { return h.fx.world->login_pending(id); }, 3000), "the log-in marked the character");
    CHECK(!login.done(milliseconds(150)) && login.characters.loaded(id) == 0, "the log-in waits for the save");
    sink.release();   // the store refuses the save
    CHECK(step_world_until(h, [&] { return sink.bags_busy.load(); }, 3000), "the refused save's give-back bags are queued in the saver");
    CHECK(counted_while_queueing.load(), "the market still counted the save while its bags were being queued");
    CHECK(!store->saving(id), "the market has released its count after queueing them");
    CHECK(h.fx.world->character_writes_pending(id), "the world still says the character is pending: the saver holds the queued bags, so there is no gap");
    CHECK(!login.done(milliseconds(200)) && login.characters.loaded(id) == 0, "the log-in still waits and reads nothing");
    sink.release_bags();   // the saver has written them
    CHECK(login.done(std::chrono::seconds(5)), "the log-in goes on once the saver has written the bags");
    if (!login.result.valid()) return;
    const LoginResult r = login.result.get();
    h.fx.world->login_ended(74);
    CHECK(r.accepted && login.characters.loaded(id) == 1, "it is accepted and reads the character once");
    CHECK(loaded_units(r) == 5, "from the bags with the two listed units given back: five units");
    CHECK(store->count(seller) == 0, "and the world holds no listing");
}

/// Another character's log-in is served while one waits: the log-in worker answers B at once and A when its listing's save is answered.
static void test_marketplace_login_of_another_character_is_served_while_one_waits() {
    std::printf("the Marketplace: another character's log-in is served while a seller's log-in waits for its save\n");
    MarketSaveSink sink;
    sink.hold = true;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const std::string id = cyb_record().id;
    const std::string other = "0000000000000000000000000000c0b0";
    const std::string ticket_a = "11111111111111111111111111111111", ticket_b = "22222222222222222222222222222222";
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) {
        sink.release();
        return;
    }
    MarketLoginGateway gateway;
    MarketLoginCharacters characters(sink, cyb_record(), id);
    LoginService service(gateway, characters);
    set_world_barrier(service, *h.fx.world, std::chrono::seconds(5));
    std::mutex results_mu;
    std::map<uint64_t, LoginResult> results;
    const auto has = [&](uint64_t session) {
        std::lock_guard<std::mutex> lock(results_mu);
        return results.count(session) != 0;
    };
    const auto result_of = [&](uint64_t session) {
        std::lock_guard<std::mutex> lock(results_mu);
        return results[session];
    };
    LoginWorker worker(service, 7, [&](uint64_t session, LoginResult r) {
        std::lock_guard<std::mutex> lock(results_mu);
        results[session] = std::move(r);
    });
    SinkReleaser unhold{sink};
    sink.reset_started();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(::aoc::worldd::market::marketplace_guid(kNode), items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save is in flight");
    worker.submit(1, LoginRequest{id, ticket_a});
    worker.submit(2, LoginRequest{other, ticket_b});
    CHECK(wait_until_true([&] { return has(2); }, 3000), "the other character's log-in is answered while the seller's waits");
    CHECK(has(2) && result_of(2).accepted && characters.loaded(other) == 1, "it is accepted and the character is read once");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(!has(1) && characters.loaded(id) == 0, "the seller's log-in has no answer and nothing of it is read");
    CHECK(gateway.redeemed(ticket_a) == 1, "its ticket was redeemed once, however often it was looked at");
    sink.release();
    CHECK(step_session_until(h, [&] { return has(1); }, 5000), "the seller's log-in is answered once its save is");
    CHECK(has(1) && result_of(1).accepted && characters.loaded(id) == 1 && gateway.redeemed(ticket_a) == 1, "it is accepted, the character read once, the ticket redeemed once");
    CHECK(loaded_units(result_of(1)) == 3, "with the bags the listing's save wrote");
    h.fx.world->login_ended(1);
    h.fx.world->login_ended(2);
}

/// A listing request of a character whose log-in has marked it is refused as in progress with nothing taken and nothing asked of the store, the client gets its bags and its
/// listings again, and when the mark ends the same request is made.
static void test_marketplace_listing_of_a_character_whose_login_began_is_refused() {
    std::printf("the Marketplace: a listing of a character whose log-in has begun is refused, with the bags and listings sent again\n");
    MarketSaveSink sink;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const std::string id = cyb_record().id;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const int64_t purse = h.s->wallet().copper();
    h.fx.world->login_began(id, 901);   // the log-in's thread does this when its ticket is redeemed
    CHECK(h.fx.world->login_pending(id), "the character is marked");
    const size_t from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
    const repl::RpcCall* answer = registration_answer(pc_calls_since(h, from));
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 3, "the listing is refused with REQUEST_ALREADY_IN_PROGRESS (3)");
    check_refusal_resynced(h, from, "the refusal carries its response");
    const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
    CHECK(still && still->quantity == 5 && h.s->wallet().copper() == purse, "it took no unit and no fee");
    {
        std::lock_guard<std::mutex> lock(sink.mu);
        CHECK(sink.market_saves.empty(), "and the store was asked for nothing");
    }
    CHECK(h.s->market_pending() == 0 && !h.fx.world->market_listings()->saving(id), "nothing is in flight and the world counts no save of the character");
    h.fx.world->login_ended(901);
    const size_t again = h.got.size();
    const svc::InventoryItem* after = h.s->inventory().at(Bag, 1);
    if (!after) return;
    h.client_request(sell_request(market, items::to_instance(*after), 2, kPrice), h.pc_ch);
    const repl::RpcCall* made = registration_answer(pc_calls_since(h, again));
    CHECK(made && enum_of(*made, "Response").value_or(1) == 0, "when the mark ends the same listing is made");
    std::lock_guard<std::mutex> lock(sink.mu);
    CHECK(sink.market_saves.size() == 1, "and saved once");
}

/// The mark comes between the session's look and the store's: the session found no mark, took the units and the fee and asked the store, which counted the save and then saw the
/// mark.  Nothing is saved, the units and the fee go back, the client is sent its bags and listings again, and the count is released at once: a log-in does not wait for a save
/// that never began.
static void test_marketplace_a_login_marking_between_the_looks_refuses_the_listing() {
    std::printf("the Marketplace: a log-in that marks the character between the session's look and the store's refuses the listing and gives everything back\n");
    MarketSaveSink sink;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const std::string id = cyb_record().id;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const int64_t purse = h.s->wallet().copper();
    auto* store = dynamic_cast<::aoc::worldd::market::ServiceMarketStore*>(h.fx.world->market_listings());
    CHECK(store != nullptr, "the world's store is the service store");
    if (!store) return;
    bool marked = false;
    bool saw_count = false;
    store->set_login_check([&](const std::string& who) {
        saw_count = store->saving(who);   // the store counts the save BEFORE it looks at the mark
        if (!marked) {
            marked = true;
            h.fx.world->login_began(who, 902);   // the log-in's thread marks the character after the session's own look
        }
        return h.fx.world->login_pending(who);
    });
    const size_t from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
    CHECK(marked && saw_count, "the mark came after the session's look and the store had counted the save when it looked");
    const repl::RpcCall* answer = registration_answer(pc_calls_since(h, from));
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 1, "the listing is refused with ERROR_GENERIC (1)");
    check_refusal_resynced(h, from, "the refusal carries its response");
    const svc::InventoryItem* back = h.s->inventory().at(Bag, 1);
    CHECK(back && back->quantity == 5 && potion_units(h.s->inventory()) == 5 && h.s->wallet().copper() == purse, "the units and the fee are back");
    {
        std::lock_guard<std::mutex> lock(sink.mu);
        CHECK(sink.market_saves.empty(), "the store was asked for nothing");
    }
    CHECK(h.s->market_pending() == 0 && !store->saving(id), "the store released its count: nothing is in flight");
    CHECK(!h.fx.world->character_writes_pending(id), "so the world has no write of the character a log-in would wait for");
    store->set_login_check(nullptr);
    h.fx.world->login_ended(902);
}

AOC_REGISTER_TEST(test_marketplace_login_waits_for_an_unanswered_listing_save);
AOC_REGISTER_TEST(test_marketplace_login_waits_for_a_listing_and_a_claim_listing_answered_first);
AOC_REGISTER_TEST(test_marketplace_login_waits_for_a_listing_and_a_claim_claim_answered_first);
AOC_REGISTER_TEST(test_marketplace_login_has_one_bound_for_both_waits);
AOC_REGISTER_TEST(test_marketplace_login_never_sees_a_gap_after_a_refused_save_of_an_ended_session);
AOC_REGISTER_TEST(test_marketplace_login_of_another_character_is_served_while_one_waits);
AOC_REGISTER_TEST(test_marketplace_listing_of_a_character_whose_login_began_is_refused);
AOC_REGISTER_TEST(test_marketplace_a_login_marking_between_the_looks_refuses_the_listing);
AOC_REGISTER_TEST(test_marketplace_one_save_in_flight);
AOC_REGISTER_TEST(test_marketplace_log_in_barrier_follows_the_saves);
AOC_REGISTER_TEST(test_marketplace_log_in_barrier_opens_at_the_answer);
AOC_REGISTER_TEST(test_marketplace_stuck_save_does_not_hold_others);
AOC_REGISTER_TEST(test_marketplace_service_down_fails_closed);
AOC_REGISTER_TEST(test_marketplace_refusal_resyncs_the_client);
AOC_REGISTER_TEST(test_marketplace_sells_from_listed_containers_only);

/// Whether all four words of a guid are zero.
static bool no_guid_words(const repl::GuidValue& g) { return (g.a | g.b | g.c | g.d) == 0; }

/// The data request the game's own client sent when it opened the Listings tab, as one recorded bunch of the PlayerController's channel.
static const char* kRecordedDataRequest = "e610785c40b0dc2cf87e790884e6e2e885111d697177529561d1b2d2a67b2ff64fa1f3f05001";

/// The recorded data request reads to its last bit and carries all eight words of its two guids; the request the tests build from the same two guids holds the same bits, so
/// the tests ask the way the client asks.
static void test_marketplace_recorded_data_request() {
    std::printf("the Marketplace: the recorded data request reads, and the tests' request is the recorded one\n");
    const std::vector<uint8_t> bytes = recorded_bytes(kRecordedDataRequest);
    BunchWriter recorded;
    recorded.write_bit_range(bytes.data(), 0, 302);
    const auto blocks = repl::read_content_blocks(recorded.data(), recorded.bit_pos(), 0, /*server_form=*/false);
    CHECK(blocks && !blocks->empty(), "the recorded bunch reads as content blocks");
    if (!blocks || blocks->empty()) return;
    std::optional<repl::RpcCall> call;
    for (const auto& b : *blocks) {
        if (!b.header.is_actor || b.header.has_rep_layout) continue;
        size_t p = b.payload_pos;
        auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), recorded.data(), b.payload_pos + b.payload_bits, p);
        if (calls && !calls->empty()) call = std::move(calls->front());
    }
    CHECK(call && call->rpc && std::string(call->rpc->name) == "ServerRequestMarketplaceData", "it is the data request");
    CHECK(call && call->decoded && call->param_bits == 258, "its 258 parameter bits read to the last bit");
    if (!call || !call->rpc || !call->decoded) return;
    const std::optional<repl::GuidValue> market = repl::market_rpc_detail::read_guid(*call->rpc, call->params, "MarketplaceActorGuid");
    const std::optional<repl::GuidValue> player = repl::market_rpc_detail::read_guid(*call->rpc, call->params, "PlayerGuid");
    CHECK(market && player, "both guids read, four words each");
    if (!market || !player) return;
    CHECK(!no_guid_words(*market) && !no_guid_words(*player) && !(*market == *player), "neither is empty, and they differ");
    const BunchWriter mine = data_request(*market, *player, true);
    bool same = mine.bit_pos() == recorded.bit_pos();
    for (size_t i = 0; same && i < mine.bit_pos(); ++i) same = ((mine.data()[i / 8] >> (i % 8)) & 1) == ((recorded.data()[i / 8] >> (i % 8)) & 1);
    CHECK(same, "the request the tests build from the same two guids is the recorded one, bit for bit");
}

AOC_REGISTER_TEST(test_marketplace_recorded_data_request);

AOC_REGISTER_TEST(test_marketplace_open_pushes_the_storefront_pages);
AOC_REGISTER_TEST(test_marketplace_open_sends_one_empty_page);
AOC_REGISTER_TEST(test_marketplace_listings_tab_story);

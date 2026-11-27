// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_market_sell_tests.inc: listing an item at the Marketplace through a real session
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc)
//
//    the sale          a stack of five is split by a listing of two at one gold each: two leave the bag, one stack storage event says so,
//                      the rest of the stack stays; the whole rest listed leaves the slot empty; two listings in a row come from one stack
//    the answer        one registration answer: Response 0 and the listing (the seller, the market's node, a new listing guid, the item with
//                      the listed quantity, the price in copper, issue now, expiry three days later, LISTED); a recorded answer reads to its last bit
//    the refusals      a bad quantity, a zero price, an item the seller does not have and a foreign market take nothing; worn gear is ITEM_IN_USE,
//                      a quest item is ITEM_UNTRADABLE, and neither changes the bags, the worn gear, the quest storage or the purse
//    the Buy tab       the storefront request answers one header per seller at that market, bComplete on the last page; a market with no
//                      listing answers with no header; a recorded storefront row and the page tail read to their last bits
//    the Sell tab      the data request with the seller's own guid carries his own listing at his price, and the answer reads back through
//                      the reader with every field of the recorded listing's element
//    two sellers       two players list at one market and each storefront is in the headers
//    another market    a listing at another node's market is ignored and takes nothing
//    the cap           ten listings pass; the eleventh answers ERROR_GENERIC and takes nothing
//    the fee           the fee function's recorded rows, its rounding, its ceiling and the record's RarityMin/RarityMax bound; a
//                      listing takes the fee from the seller's money and shows it as one money popup, a seller who cannot pay is
//                      refused with CURRENCY_NOT_FOUND and keeps the stack and the money, and a removal request refunds nothing
// ============================================================================
#include "bootstrap/constants.h"
#include "items/inventory.h"
#include "market/fee.h"
#include "market/market.h"
#include "protocol/repl/market_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/storage_rpc.h"

#include <spdlog/sinks/ostream_sink.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace market_sell_test {

/// One gold a unit (100 copper a silver, 100 silver a gold).
inline constexpr int64_t kPrice = 10000;
/// Three days, the retail listing's life.
inline constexpr double kLifeSeconds = 259200.0;
/// The Money tier's record type (CurrencyTierDefRecordBase).
inline constexpr int64_t kMoneyTierType = 456318420933392641ll;
/// Another node's market: the same town data has no second Manager in this world, and the store keys by node.
inline constexpr int64_t kOtherNode = kNode + 1;

}  // namespace market_sell_test

using namespace market_sell_test;

static bool put_guid(const schema::RpcSchema& rpc, repl::RpcParams& params, const char* prefix, const repl::GuidValue& g) {
    const std::string p(prefix);
    return repl::set_rpc_param(rpc, params, p + ".A", repl::Value::i64(static_cast<int32_t>(g.a))) &&
           repl::set_rpc_param(rpc, params, p + ".B", repl::Value::i64(static_cast<int32_t>(g.b))) &&
           repl::set_rpc_param(rpc, params, p + ".C", repl::Value::i64(static_cast<int32_t>(g.c))) &&
           repl::set_rpc_param(rpc, params, p + ".D", repl::Value::i64(static_cast<int32_t>(g.d)));
}

static repl::GuidValue character_guid(const Harness& h) {
    const auto& g = h.s->bootstrap().setup().character_guid;
    return repl::GuidValue{g[0], g[1], g[2], g[3]};
}

/// ServerRequestMarketplaceItemRegistration as the Sell tab sends it: the market, the stack, the number of units and the unit price in copper.
static BunchWriter sell_request(const repl::GuidValue& market, const repl::ItemInstance& item, int32_t quantity, int64_t copper) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestMarketplaceItemRegistration");
    if (!rpc) return BunchWriter();
    repl::RpcParams params;
    repl::CurrencyContainer price;
    price.entries.push_back({items::kMoneyTier, kMoneyTierType, copper});
    const bool ok = put_guid(*rpc, params, "MarketplaceActorGuid", market) && repl::set_rpc_param(*rpc, params, "Item", repl::Value(item)) &&
                    repl::set_rpc_param(*rpc, params, "Quantity", repl::Value::i64(quantity)) && repl::set_rpc_param(*rpc, params, "PricePerUnit", repl::Value(price));
    return ok ? actor_rpc(pc, "ServerRequestMarketplaceItemRegistration", params) : BunchWriter();
}

/// ServerRequestMarketplaceData: the market and the seller the window asks about; a zero guid names no seller and is not answered.
static BunchWriter data_request(const repl::GuidValue& market, const repl::GuidValue& seller, bool with_seller) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestMarketplaceData");
    if (!rpc) return BunchWriter();
    repl::RpcParams params;
    const bool ok = put_guid(*rpc, params, "MarketplaceActorGuid", market) && (!with_seller || put_guid(*rpc, params, "PlayerGuid", seller));
    return ok ? actor_rpc(pc, "ServerRequestMarketplaceData", params) : BunchWriter();
}

static const repl::RpcCall* call_named(const std::vector<repl::RpcCall>& calls, const char* name) {
    for (const repl::RpcCall& c : calls) {
        if (c.rpc && std::string(c.rpc->name) == name) return &c;
    }
    return nullptr;
}

static std::optional<int64_t> number_of(const repl::RpcCall& c, const char* path) {
    return repl::rpcf::int_or(repl::rpcf::value_of(*c.rpc, c.params, path), 0);
}

/// One of a guid's four words (the schema's int32 commands read as signed, the guid's words as unsigned).
static std::optional<uint32_t> word_of(const repl::RpcCall& c, const char* path) {
    const std::optional<int64_t> n = number_of(c, path);
    if (!n) return std::nullopt;
    return static_cast<uint32_t>(*n);
}

static std::optional<uint64_t> enum_of(const repl::RpcCall& c, const char* path) {
    return repl::rpcf::uint_or(repl::rpcf::value_of(*c.rpc, c.params, path), 0);
}

static std::optional<double> double_of(const repl::RpcCall& c, const char* path) {
    const auto* d = repl::rpcf::as<double>(repl::rpcf::value_of(*c.rpc, c.params, path));
    return d ? std::optional<double>(*d) : std::nullopt;
}

static std::string text_of(const repl::RpcCall& c, const char* path) {
    const auto* s = repl::rpcf::as<std::string>(repl::rpcf::value_of(*c.rpc, c.params, path));
    return s ? *s : std::string();
}

static const repl::CurrencyContainer* price_of(const repl::RpcCall& c, const char* path) {
    return repl::rpcf::as<repl::CurrencyContainer>(repl::rpcf::value_of(*c.rpc, c.params, path));
}static const repl::ArrayValue* array_of(const repl::RpcCall& c, const char* path) {
    return repl::rpcf::as<repl::ArrayValue>(repl::rpcf::value_of(*c.rpc, c.params, path));
}

/// A member of an array element by its schema path (the element commands' local handles).
static const repl::Value* member_of(const repl::RpcCall& c, const char* array, const repl::ElementState& e, const char* path) {
    const schema::RepCommand* cmd = repl::rpc_command(*c.rpc, array);
    if (!cmd) return nullptr;
    const auto elems = repl::rpcf::elements_of(*c.rpc, *cmd);
    return repl::rpcf::element_value(e, repl::rpcf::local(elems, path));
}

static std::optional<int64_t> member_number(const repl::RpcCall& c, const char* array, const repl::ElementState& e, const char* path) {
    return repl::rpcf::int_or(member_of(c, array, e, path), 0);
}

static std::optional<uint32_t> member_word(const repl::RpcCall& c, const char* array, const repl::ElementState& e, const char* path) {
    const std::optional<int64_t> n = member_number(c, array, e, path);
    if (!n) return std::nullopt;
    return static_cast<uint32_t>(*n);
}

/// An enum member of an array element (Enum values travel as unsigned).
static std::optional<uint64_t> member_enum(const repl::RpcCall& c, const char* array, const repl::ElementState& e, const char* path) {
    return repl::rpcf::uint_or(member_of(c, array, e, path), 0);
}

static std::string member_text(const repl::RpcCall& c, const char* array, const repl::ElementState& e, const char* path) {
    const auto* s = repl::rpcf::as<std::string>(member_of(c, array, e, path));
    return s ? *s : std::string();
}

/// The six hex digits a byte holds, LSB first as the wire does (the recorded bit runs are stored that way).
static std::vector<uint8_t> recorded_bytes(const char* hex) {
    std::vector<uint8_t> out;
    for (const char* p = hex; p[0] && p[1]; p += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(p, 2), nullptr, 16)));
    return out;
}

/// The registration answer of `calls` (nullptr: none).
static const repl::RpcCall* registration_answer(const std::vector<repl::RpcCall>& calls) {
    return call_named(calls, "ClientMarketplaceItemRegistrationResponse");
}

/// The one ClientShowCurrencyCallout among `calls` (nullptr: none).
static const repl::RpcCall* currency_callout(const std::vector<repl::RpcCall>& calls) {
    return call_named(calls, "ClientShowCurrencyCallout");
}

/// The listing guid a registration answer carries.
static repl::GuidValue listing_guid(const repl::RpcCall& c) {
    return {word_of(c, "Item.MarketplaceItemGuid.A").value_or(0), word_of(c, "Item.MarketplaceItemGuid.B").value_or(0),
            word_of(c, "Item.MarketplaceItemGuid.C").value_or(0), word_of(c, "Item.MarketplaceItemGuid.D").value_or(0)};
}

/// ServerRequestMarketplaceItemRemoval: the market and the listing the window takes off.
static BunchWriter removal_request(const repl::GuidValue& market, const repl::GuidValue& listing) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestMarketplaceItemRemoval");
    if (!rpc) return BunchWriter();
    repl::RpcParams params;
    const bool ok = put_guid(*rpc, params, "MarketplaceActorGuid", market) && put_guid(*rpc, params, "MarketplaceItemGuid", listing);
    return ok ? actor_rpc(pc, "ServerRequestMarketplaceItemRemoval", params) : BunchWriter();
}

/// The answer's item: the market's node, the seller, the listing and the listed stack.
static void check_listing(const repl::RpcCall& c, const repl::GuidValue& seller, const std::string& seller_name, int64_t record, int32_t quantity, int64_t copper) {
    CHECK(c.decoded, "the registration answer reads with the client's schema");
    CHECK(number_of(c, "Item.NodeGuid").value_or(0) == kNode, "the listing names the market's node");
    CHECK(word_of(c, "Item.PlayerGuid.A") == seller.a && word_of(c, "Item.PlayerGuid.B") == seller.b && word_of(c, "Item.PlayerGuid.C") == seller.c &&
              word_of(c, "Item.PlayerGuid.D") == seller.d,
          "the listing names the seller");
    CHECK(text_of(c, "Item.PlayerName") == seller_name, "the listing carries the seller's name");
    const uint32_t ga = word_of(c, "Item.MarketplaceItemGuid.A").value_or(0), gb = word_of(c, "Item.MarketplaceItemGuid.B").value_or(0);
    const uint32_t gc = word_of(c, "Item.MarketplaceItemGuid.C").value_or(0), gd = word_of(c, "Item.MarketplaceItemGuid.D").value_or(0);
    CHECK((ga | gb | gc | gd) != 0, "the listing has a guid of its own");
    const auto* item = repl::rpcf::as<repl::ItemInstance>(repl::rpcf::value_of(*c.rpc, c.params, "Item.Item"));
    CHECK(item && item->has_item && item->record_id == static_cast<uint64_t>(record) && item->quantity == static_cast<uint32_t>(quantity), "the listing's item is the stack as listed");
    CHECK(text_of(c, "Item.ItemName") == "Test_Potion", "the listing carries the item's name");
    const repl::CurrencyContainer* price = price_of(c, "Item.PricePerUnit");
    CHECK(price && price->entries.size() == 1 && price->entries[0].tier == items::kMoneyTier && price->entries[0].type_id == kMoneyTierType && price->entries[0].amount == copper,
          "the listing's price is that many copper, keyed by the Money tier");
    CHECK(enum_of(c, "Item.Status").value_or(99) == 0, "the listing is LISTED (0)");
}

/// The one Remove event in `calls` that took `count` units of the stack `stack` from the bag's slot `slot`.
static bool check_remove(const std::vector<repl::RpcCall>& calls, const repl::ItemInstance& stack, int32_t slot, int32_t count) {
    for (const repl::RpcCall& c : calls) {
        if (!c.rpc || std::string(c.rpc->name) != "ClientReceiveStorageEvent") continue;
        const auto event = repl::parse_storage_event(c);
        if (!event || event->type != static_cast<uint8_t>(repl::StorageEventType::Remove)) continue;
        if (event->targets.size() != 1 || event->targets[0].slots.size() != 1) continue;
        const repl::StorageEventSlot& s = event->targets[0].slots[0];
        if (s.slot == slot && s.count == count && event->item.guid == stack.guid && event->item.quantity == stack.quantity) return true;
    }
    return false;
}

static void test_marketplace_sell_takes_the_units() {
    std::printf("the Marketplace: a listing takes its units from the stack\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->item_record_id == fi::kPotion && held->quantity == 5, "the bag holds a stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);

    size_t from = h.got.size();
    h.client_request(sell_request(market, stack, 2, kPrice), h.pc_ch);
    auto calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientMarketplaceItemRegistrationResponse") == 1, "the listing is answered once");
    const svc::InventoryItem* after = h.s->inventory().at(Bag, 1);
    CHECK(after && after->quantity == 3, "two of the five leave the bag, three stay");
    CHECK(check_remove(calls, stack, 1, 2), "the client is told of the two units leaving by one Remove event of the stack");

    // the same stack again: a second listing comes from it and leaves one
    from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*after), 2, kPrice), h.pc_ch);
    const svc::InventoryItem* rest = h.s->inventory().at(Bag, 1);
    CHECK(count_calls(pc_calls_since(h, from), "ClientMarketplaceItemRegistrationResponse") == 1 && rest && rest->quantity == 1, "a second listing from the same stack leaves one");

    // the whole rest: the slot is empty
    const repl::ItemInstance last = items::to_instance(*rest);
    from = h.got.size();
    h.client_request(sell_request(market, last, 1, kPrice), h.pc_ch);
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientMarketplaceItemRegistrationResponse") == 1, "the whole rest is answered once");
    CHECK(h.s->inventory().at(Bag, 1) == nullptr && h.s->inventory().find_guid(items::guid_hex(stack.guid)) == nullptr, "a whole stack listed leaves its slot empty");
    CHECK(check_remove(calls, last, 1, 1), "the last unit leaves by one Remove event of the stack as it was");
}

static void test_marketplace_listing_fee() {
    std::printf("the Marketplace: the listing fee\n");
    const items::ItemRarityFacts bands = items::default_rarity_facts();
    // every recorded listing: the quality is the stack's, the level the item record's
    CHECK(market::listing_fee(bands, 6, 1, 1, 100) == 10, "Ruby (quality 6, Uncommon, level 1), 1 x 100 copper: 10");
    CHECK(market::listing_fee(bands, 1, 1, 1, 50) == 5, "Rough Ruby (quality 1, Common, level 1), 1 x 50 copper: 5");
    CHECK(market::listing_fee(bands, 16, 1, 1, 105) == 15, "Rough Ruby (quality 16, Rare, level 1), 1 x 105 copper: 15");
    CHECK(market::listing_fee(bands, 1, 10, 1, 200) == 50, "Rough Halcyonite (quality 1, Common, level 10), 1 x 200 copper: 50");
    CHECK(market::listing_fee(bands, 8, 1, 1, 1) == 10, "Enchanted River Rune (quality 8, Uncommon, level 1), 1 x 1 copper: the 10 item minimum");
    CHECK(market::listing_fee(bands, 8, 1, 1, 10000) == 500, "the same rune, 1 x 10,000 copper: 500");
    CHECK(market::listing_fee(bands, 8, 1, 1, 100) == 10, "the same rune, 1 x 100 copper: the 10 item minimum");
    CHECK(market::listing_fee(bands, 8, 1, 2, 1) == 10, "the same rune, 2 x 1 copper: the 10 item minimum");
    CHECK(market::listing_fee(bands, 51, 13, 1, 2000) == 325, "Dustbound Ring (quality 51, Epic, level 13), 1 x 2,000 copper: 325");
    // the rounding of the price part; a level of 0 takes the item minimum out, so the fee is the price part alone
    CHECK(market::listing_fee(bands, 1, 0, 1, 100) == 5, "5% of 100 copper, rounded up: 5");
    CHECK(market::listing_fee(bands, 1, 0, 1, 105) == 6, "5% of 105 copper, rounded up: 6");
    CHECK(market::listing_fee(bands, 1, 0, 1, 50) == 3, "5% of 50 copper, rounded up: 3");
    CHECK(market::listing_fee(bands, 1, 0, 1, 200) == 10, "5% of 200 copper, rounded up: 10");
    CHECK(market::listing_fee(bands, 1, 0, 1, 10000) == 500, "5% of 10,000 copper, rounded up: 500");
    CHECK(market::listing_fee(bands, 1, 0, 1, 1) == 1, "5% of 1 copper, rounded up: 1");
    CHECK(market::listing_fee(bands, 1, 0, 2, 1) == 1, "5% of 2 copper, rounded up: 1");
    CHECK(market::listing_fee(bands, 1, 0, 21, 1) == 2, "5% of 21 copper, rounded up: 2");
    CHECK(market::listing_fee(bands, 1, 0, 30, 1000000) == 1000000, "5% of 30 x 1,000,000 copper is held at 1,000,000");
    // RarityMin/RarityMax hold the rarity the record shows, not the quality's own band
    items::ItemRarityFacts at_least_epic = bands;
    at_least_epic.rarity_min = 4;
    CHECK(market::listing_fee(at_least_epic, 1, 1, 1, 100) == 25, "a record whose RarityMin is above the quality's band reads Epic: (4 + 1) x 1 x 5 = 25");
    items::ItemRarityFacts at_most_common = bands;
    at_most_common.rarity_max = 0;
    CHECK(market::listing_fee(at_most_common, 51, 1, 1, 100) == 5, "a record whose RarityMax is below the quality's band reads Common: 5, not the Epic 25");
}

static void test_marketplace_sell_answer() {
    std::printf("the Marketplace: the registration answer\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const double issue = h.s->sync_now(h.t);
    size_t from = h.got.size();
    h.client_request(sell_request(market, stack, 2, kPrice), h.pc_ch);
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(calls);
    CHECK(answer != nullptr, "one ClientMarketplaceItemRegistrationResponse is sent");
    if (!answer) return;
    CHECK(enum_of(*answer, "Response").value_or(99) == 0, "Response 0: the listing was made");
    check_listing(*answer, character_guid(h), "cyb", fi::kPotion, 2, kPrice);
    CHECK(std::fabs(double_of(*answer, "Item.IssueDate").value_or(-1) - issue) < 1e-9, "the issue date is now on the client's clock");
    const double expiry = double_of(*answer, "Item.ExpirationDate").value_or(-1);
    CHECK(std::fabs(expiry - (issue + kLifeSeconds)) < 1e-9, "the listing expires three days after it is listed");
    CHECK(!(word_of(*answer, "Item.MarketplaceItemGuid.A") == stack.guid.a && word_of(*answer, "Item.MarketplaceItemGuid.B") == stack.guid.b &&
            word_of(*answer, "Item.MarketplaceItemGuid.C") == stack.guid.c && word_of(*answer, "Item.MarketplaceItemGuid.D") == stack.guid.d),
          "the listing's guid is not the stack's instance guid");
}

static void test_marketplace_sell_charges_the_fee() {
    std::printf("the Marketplace: a listing takes its fee from the seller's money\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->item_record_id == fi::kPotion && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    // the potion is Common at quality 1 and level 1: its minimum is (0 + 1) x 1 x 5 = 5; 5% of 2 x 10,000 is 1,000
    const int64_t fee = 1000;
    const int64_t before = h.s->wallet().copper();
    CHECK(before == kStartPurse, "the seller starts with the 21 gold start purse");
    const size_t from = h.got.size();
    h.client_request(sell_request(market, stack, 2, kPrice), h.pc_ch);
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(1) == 0, "the listing is made");
    CHECK(h.s->wallet().copper() == before - fee, "the fee leaves the seller's money");
    const repl::RpcCall* callout = currency_callout(calls);
    CHECK(callout != nullptr, "one money popup is sent");
    if (callout) {
        CHECK(number_of(*callout, "CurrencyTierDefGuid").value_or(0) == items::kMoneyTier, "the popup is in the Money tier");
        CHECK(number_of(*callout, "Delta").value_or(0) == -fee, "the popup shows the fee as a loss");
    }
    const svc::InventoryItem* after = h.s->inventory().at(Bag, 1);
    CHECK(after && after->quantity == 3, "the listed two units leave the bag");
    CHECK(h.s->inventory_dirty() && h.s->inventory_changed(), "the bag is marked for saving");
}

static void test_marketplace_sell_refused_without_the_fee() {
    std::printf("the Marketplace: a seller who cannot pay the fee is refused\n");
    svc::Character poor = cyb_record();
    poor.wallet.push_back({items::kCopperCurrency, 0});
    MarketHarness h(poor);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    CHECK(h.s->wallet().copper() == 0, "the seller has no money");
    const size_t listings = h.fx.world->market_listings()->size();
    const size_t from = h.got.size();
    h.client_request(sell_request(market, stack, 2, kPrice), h.pc_ch);
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 13, "the listing is refused with CURRENCY_NOT_FOUND (13)");
    CHECK(h.s->wallet().copper() == 0, "the refusal takes no money");
    const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
    CHECK(still && still->quantity == 5 && still->instance_guid == items::guid_hex(stack.guid), "the refusal keeps the stack");
    CHECK(h.fx.world->market_listings()->size() == listings, "the refusal makes no listing");
    CHECK(currency_callout(calls) == nullptr, "the refusal sends no money popup");
    CHECK(!h.s->failed(), "the session lives");
}

static void test_marketplace_delist_refunds_nothing() {
    std::printf("the Marketplace: a removal refunds nothing\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t fee = 1000;   // the same potion rule as above
    const int64_t before = h.s->wallet().copper();
    const size_t from = h.got.size();
    h.client_request(sell_request(market, stack, 2, kPrice), h.pc_ch);
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(calls);
    CHECK(answer != nullptr, "the listing is answered");
    if (!answer) return;
    CHECK(h.s->wallet().copper() == before - fee, "the listing takes its fee");
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    CHECK(pc.rpc("ServerRequestMarketplaceItemRemoval") != nullptr, "the client's removal call is in the schema");
    const repl::GuidValue listing = listing_guid(*answer);
    const size_t removal_from = h.got.size();
    h.client_request(removal_request(market, listing), h.pc_ch);
    const auto removal_calls = pc_calls_since(h, removal_from);
    CHECK(count_calls(removal_calls, "ClientMarketplaceItemRemovalResponse") == 0, "the draft answers no removal");
    CHECK(count_calls(removal_calls, "ClientShowCurrencyCallout") == 0, "a removal sends no money popup");
    CHECK(h.s->wallet().copper() == before - fee, "a removal refunds nothing");
    CHECK(!h.s->failed(), "the removal keeps the session");
}

static void test_marketplace_recorded_registration() {
    std::printf("the Marketplace: a recorded registration answer reads to its last bit\n");
    static const char* kHex =
        "f3acec4aa0c5000092aa0c8b969536dd7bb17f0a9d87870a1c000000a2d2f2c2dcc2a6eae0e0dee4e80080eca0cbff9cf2234c277f247f3ca7da0300b028699ea75009983100000000000000c00601000000446ac554482611e6429939954e05fda302c880e9643579230000000000000000000000000000000088d48aa9904c22cc8532732a9d0afa47059001d3c96af246000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001e00000088eae6e8c4deeadcc840a4d2dcce00020000000000caba1ec853a8027267505f56aa0ca00f000000000000a0e267262e94d482a0e267264e91d58200";
    const std::vector<uint8_t> bits = recorded_bytes(kHex);
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ClientMarketplaceItemRegistrationResponse");
    CHECK(rpc != nullptr, "the client's registration answer is in the schema");
    if (!rpc) return;
    size_t pos = 0;
    const auto params = repl::read_rpc_params(*rpc, bits.data(), 2069, pos);
    CHECK(params && pos == 2069, "the recorded parameters read to their last bit (2069/2069)");
    if (!params) return;
    repl::RpcCall c;
    c.rpc = rpc;
    c.params = *params;
    c.decoded = true;
    c.param_bits = 2069;
    CHECK(number_of(c, "Item.NodeGuid").value_or(0) == 108646121231993ll, "the recorded listing names its node");
    CHECK(text_of(c, "Item.PlayerName") == "QiyanaSupport", "the recorded listing names its seller");
    const auto* item = repl::rpcf::as<repl::ItemInstance>(repl::rpcf::value_of(*c.rpc, c.params, "Item.Item"));
    CHECK(item && item->has_item && item->record_id == 6064633023086460928ull && item->quantity == 1 && item->quality == 51 && item->enchantment_level == 6,
          "the recorded item is the Dustbound Ring stack (quantity 1, quality 51, enchantment 6)");
    CHECK(text_of(c, "Item.ItemName") == "Dustbound Ring", "the recorded listing carries the item's name");
    const repl::CurrencyContainer* price = price_of(c, "Item.PricePerUnit");
    CHECK(price && price->entries.size() == 1 && price->entries[0].tier == items::kMoneyTier && price->entries[0].type_id == kMoneyTierType && price->entries[0].amount == 2000,
          "the recorded price is 2000 copper keyed by the Money tier");
    CHECK(std::fabs(double_of(c, "Item.IssueDate").value_or(-1) - 13783224.600090653) < 1e-6, "the recorded issue date is on the client's clock");
    CHECK(std::fabs(double_of(c, "Item.ExpirationDate").value_or(-1) - 14042424.600090653) < 1e-6, "the recorded listing expires three days later");
    CHECK(enum_of(c, "Item.Status").value_or(99) == 0, "the recorded listing is LISTED");
    CHECK(enum_of(c, "Response").value_or(0) == 0, "the recorded answer's Response is the default 0");
}

static void test_marketplace_sell_refusals() {
    std::printf("the Marketplace: a refused listing keeps the items\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    repl::ItemInstance ghost = stack;
    ghost.guid = repl::GuidValue{1, 2, 3, 4};
    repl::GuidValue foreign = market;
    foreign.c = 0;

    const auto refuse = [&](const BunchWriter& request, uint64_t expected, const char* what) {
        const size_t from = h.got.size();
        h.client_request(request, h.pc_ch);
        const auto calls = pc_calls_since(h, from);
        const repl::RpcCall* answer = registration_answer(calls);
        CHECK(answer != nullptr, what);
        if (answer) CHECK(enum_of(*answer, "Response").value_or(0) == expected, what);
        const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
        CHECK(still && still->quantity == 5 && still->instance_guid == items::guid_hex(stack.guid), what);
        CHECK(!h.s->failed(), what);
        bool closed = false;
        for (size_t i = from; i < h.got.size(); ++i) closed = closed || h.got[i].close;
        CHECK(!closed, what);
    };
    refuse(sell_request(market, stack, 0, kPrice), 8, "a quantity of zero is refused with INVALID_QUANTITY (8) and takes nothing");
    refuse(sell_request(market, stack, 6, kPrice), 8, "a quantity above the stack is refused with INVALID_QUANTITY (8) and takes nothing");
    refuse(sell_request(market, stack, 2, 0), 13, "a price of zero is refused with CURRENCY_NOT_FOUND (13) and takes nothing");
    refuse(sell_request(market, ghost, 2, kPrice), 5, "an item the seller does not have is refused with ITEM_NOT_FOUND (5) and takes nothing");
    // a guid that is not the opened Manager's market is ignored, as the game's access check does
    const size_t from = h.got.size();
    h.client_request(sell_request(foreign, stack, 2, kPrice), h.pc_ch);
    CHECK(registration_answer(pc_calls_since(h, from)) == nullptr, "a foreign market guid is ignored");
    const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
    CHECK(still && still->quantity == 5 && still->instance_guid == items::guid_hex(stack.guid), "the ignored foreign market takes nothing");
    CHECK(!h.s->failed(), "the ignored foreign market keeps the connection");
}

static void test_marketplace_buy_tab_headers() {
    std::printf("the Marketplace: the Buy tab's storefronts\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);

    // a market with no listing answers with no header, complete
    size_t from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    auto calls = pc_calls_since(h, from);
    const repl::RpcCall* headers = call_named(calls, "ClientReceiveMarketplaceHeaders");
    CHECK(headers != nullptr && array_of(*headers, "Headers") == nullptr, "a market with no listing answers with no storefront header");
    if (headers) {
        const auto* complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*headers->rpc, headers->params, "bComplete"));
        CHECK(complete && *complete, "the empty storefront list says it is complete");
    }

    const double issue = h.s->sync_now(h.t);
    h.client_request(sell_request(market, stack, 2, kPrice), h.pc_ch);

    from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    calls = pc_calls_since(h, from);
    headers = call_named(calls, "ClientReceiveMarketplaceHeaders");
    CHECK(headers != nullptr, "the window's storefront request is answered with ClientReceiveMarketplaceHeaders");
    if (headers) {
        CHECK(headers->decoded, "the headers packet reads with the client's schema");
        CHECK(number_of(*headers, "NodeId").value_or(0) == kNode, "the headers name the market's node");
        const repl::ArrayValue* list = array_of(*headers, "Headers");
        CHECK(list && list->elements.size() == 1, "one storefront header for the one seller with a listing");
        if (list && list->elements.size() == 1) {
            const repl::ElementState& e = list->elements[0];
            const repl::GuidValue seller = character_guid(h);
            CHECK(member_number(*headers, "Headers", e, "Headers.NodeGuid").value_or(0) == kNode, "the storefront is at the market's node");
            CHECK(member_word(*headers, "Headers", e, "Headers.PlayerGuid.A") == seller.a && member_word(*headers, "Headers", e, "Headers.PlayerGuid.B") == seller.b &&
                      member_word(*headers, "Headers", e, "Headers.PlayerGuid.C") == seller.c && member_word(*headers, "Headers", e, "Headers.PlayerGuid.D") == seller.d,
                  "the storefront is the seller's");
            CHECK(member_text(*headers, "Headers", e, "Headers.PlayerName") == "cyb", "the storefront carries his name");
            CHECK(member_text(*headers, "Headers", e, "Headers.Description") == "cyb's Storefront", "the storefront is described as his");
            const auto* date = repl::rpcf::as<double>(member_of(*headers, "Headers", e, "Headers.IssueDate"));
            CHECK(date && std::fabs(*date - issue) < 1e-9, "the storefront's issue date is its first listing's");
            const auto* city = repl::rpcf::as<bool>(member_of(*headers, "Headers", e, "Headers.bIsCityNodeHeader"));
            CHECK(city && !*city, "the storefront is no city node's header");
        }
        CHECK(enum_of(*headers, "HeaderVersion").value_or(0) == 1843, "the headers carry the storefront version the client knows (1843)");
        CHECK(repl::rpcf::value_of(*headers->rpc, headers->params, "SearchItemString") == nullptr, "no text search filtered the list");
        CHECK(enum_of(*headers, "ItemRarity").value_or(0) == 7, "the rarity filter is the client's own unfiltered value (7)");
        const auto* complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*headers->rpc, headers->params, "bComplete"));
        CHECK(complete && *complete, "the last page says it is complete");
    }
    CHECK(count_calls(calls, "ClientReceiveMarketplaceData") == 0, "the storefront request answers headers only");
}

static void test_marketplace_recorded_headers() {
    std::printf("the Marketplace: a recorded storefront row and the page tail read to their last bits\n");
    static const char* kHeader =
        "79567625d0620000263d7d64f64afaf462d7c7bb272e6af006000000616c7573690013000000616c75736927732053746f726566726f6e7400a2066dce0f376a4100";
    static const char* kTail = "670e000000000000bc";
    const std::vector<uint8_t> header = recorded_bytes(kHeader);
    const std::vector<uint8_t> tail = recorded_bytes(kTail);
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ClientReceiveMarketplaceHeaders");
    CHECK(rpc != nullptr, "the client's headers call is in the schema");
    if (!rpc) return;
    BunchWriter payload;
    payload.write_bit(0);          // NodeId left at its default
    payload.write_bit(1);          // the headers array is present
    payload.write_bits(1, 16);     // one header
    payload.write_bit_range(header.data(), 0, 521);
    payload.write_bit_range(tail.data(), 0, 72);
    size_t pos = 0;
    const auto params = repl::read_rpc_params(*rpc, payload.data(), payload.bit_pos(), pos);
    CHECK(params && pos == payload.bit_pos(), "the recorded row and tail read with the client's schema to the last bit");
    if (!params) return;
    const repl::ArrayValue* list = repl::rpcf::as<repl::ArrayValue>(repl::rpcf::value_of(*rpc, *params, "Headers"));
    CHECK(list && list->elements.size() == 1, "the recorded page holds its one header");
    if (list && list->elements.size() == 1) {
        repl::RpcCall c;
        c.rpc = rpc;
        c.params = *params;
        const repl::ElementState& e = list->elements[0];
        CHECK(member_number(c, "Headers", e, "Headers.NodeGuid").value_or(0) == 108646121231993ll, "the recorded header names its node");
        CHECK(member_text(c, "Headers", e, "Headers.PlayerName") == "alusi", "the recorded header names its seller");
        CHECK(member_text(c, "Headers", e, "Headers.Description") == "alusi's Storefront", "the recorded storefront is described as the seller's");
        const auto* date = repl::rpcf::as<double>(member_of(c, "Headers", e, "Headers.IssueDate"));
        CHECK(date && std::fabs(*date - 13744254.450808827) < 1e-6, "the recorded storefront's issue date reads");
        const auto* city = repl::rpcf::as<bool>(member_of(c, "Headers", e, "Headers.bIsCityNodeHeader"));
        CHECK(city && !*city, "the recorded storefront is no city node's header");
    }
    CHECK(enum_of((repl::RpcCall{rpc, *params, true, 0, payload.bit_pos()}), "HeaderVersion").value_or(0) == 1843, "the recorded page tail carries version 1843");
    CHECK(enum_of((repl::RpcCall{rpc, *params, true, 0, payload.bit_pos()}), "ItemRarity").value_or(0) == 7, "the recorded page tail carries the unfiltered rarity 7");
    const auto* complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*rpc, *params, "bComplete"));
    CHECK(complete && *complete, "the recorded page tail says it is complete");
}

static void test_marketplace_data_layout();

static void test_marketplace_sell_tab_data() {
    std::printf("the Marketplace: the seller's own storefront data\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const repl::GuidValue seller = character_guid(h);
    h.client_request(sell_request(market, stack, 2, kPrice), h.pc_ch);

    const size_t from = h.got.size();
    h.client_request(data_request(market, seller, true), h.pc_ch);
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* data = call_named(calls, "ClientReceiveMarketplaceData");
    CHECK(data != nullptr, "the data request with his own guid is answered with ClientReceiveMarketplaceData");
    if (!data) return;
    CHECK(data->decoded, "the data packet reads with the client's schema");
    CHECK(word_of(*data, "PlayerGuid.A") == seller.a && word_of(*data, "PlayerGuid.B") == seller.b && word_of(*data, "PlayerGuid.C") == seller.c && word_of(*data, "PlayerGuid.D") == seller.d,
          "the data is the seller's");
    const repl::ArrayValue* list = array_of(*data, "MarketplaceItemData");
    CHECK(list && list->elements.size() == 1, "the seller has one listing");
    if (list && list->elements.size() == 1) {
        const repl::ElementState& e = list->elements[0];
        CHECK(member_number(*data, "MarketplaceItemData", e, "MarketplaceItemData.NodeGuid").value_or(0) == kNode, "his listing is at this market");
        const auto* item = repl::rpcf::as<repl::ItemInstance>(member_of(*data, "MarketplaceItemData", e, "MarketplaceItemData.Item"));
        CHECK(item && item->record_id == static_cast<uint64_t>(fi::kPotion) && item->quantity == 2, "his listing carries his two units");
        const auto* price = repl::rpcf::as<repl::CurrencyContainer>(member_of(*data, "MarketplaceItemData", e, "MarketplaceItemData.PricePerUnit"));
        CHECK(price && price->entries.size() == 1 && price->entries[0].amount == kPrice, "his listing is at his price");
    }
    test_marketplace_data_layout();
}

static void test_marketplace_two_sellers() {
    std::printf("the Marketplace: two players list at one market\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the first seller's bag holds the stack of five");
    if (!held) return;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_request(sell_request(market, items::to_instance(*held), 1, kPrice), h.pc_ch);

    svc::Character second = with_start_purse(cyb_record());
    second.id = "0000000000000000000000000000c0b0";
    second.name = "bob";
    MarketPeer peer(h, second);
    peer.play();
    peer.request(interact(market_component(h.fx.world->registry(), h.manager)), peer.pawn_ch);
    const svc::InventoryItem* peer_held = peer.s->inventory().at(Bag, 1);
    CHECK(peer_held && peer_held->quantity == 5, "the second seller's bag holds its stack of five");
    if (peer_held) peer.request(sell_request(market, items::to_instance(*peer_held), 1, kPrice + 1), peer.pc_ch);

    size_t from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* headers = call_named(calls, "ClientReceiveMarketplaceHeaders");
    CHECK(headers != nullptr, "the storefront list is answered");
    if (headers) {
        const repl::ArrayValue* list = array_of(*headers, "Headers");
        CHECK(list && list->elements.size() == 2, "one storefront header for each of the two sellers");
        if (list && list->elements.size() == 2) {
            CHECK(member_text(*headers, "Headers", list->elements[0], "Headers.PlayerName") == "cyb" &&
                      member_text(*headers, "Headers", list->elements[1], "Headers.PlayerName") == "bob",
                  "both storefronts carry their sellers' names");
        }
    }

    from = peer.got.size();
    peer.request(data_request(market, character_guid(h), true), peer.pc_ch);
    const auto peer_calls = market_calls_since(peer.got, peer.pc_ch, from);
    const repl::RpcCall* data = call_named(peer_calls, "ClientReceiveMarketplaceData");
    CHECK(data != nullptr, "the second seller sees the first seller's storefront data");
    if (data) {
        const repl::ArrayValue* list = array_of(*data, "MarketplaceItemData");
        CHECK(list && list->elements.size() == 1 && member_word(*data, "MarketplaceItemData", list->elements[0], "MarketplaceItemData.PlayerGuid.B") == character_guid(h).b,
              "the first seller's listing is in it");
    }
}

static void test_marketplace_other_market() {
    std::printf("the Marketplace: a listing at another node's market\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue here = ::aoc::worldd::market::marketplace_guid(kNode);
    const repl::GuidValue elsewhere = ::aoc::worldd::market::marketplace_guid(kOtherNode);
    h.client_request(sell_request(here, stack, 1, kPrice), h.pc_ch);
    const size_t from = h.got.size();
    h.client_request(sell_request(elsewhere, stack, 1, kPrice + 1), h.pc_ch);
    const auto calls = pc_calls_since(h, from);
    CHECK(registration_answer(calls) == nullptr, "a listing at another node's market is ignored: the window is not open there");
    const svc::InventoryItem* after = h.s->inventory().at(Bag, 1);
    CHECK(after && after->quantity == 4, "the refused listing at another market takes nothing");

    const size_t at = h.got.size();
    h.client_request(market_headers_request(elsewhere, 1843), h.pc_ch);
    CHECK(count_calls(pc_calls_since(h, at), "ClientReceiveMarketplaceHeaders") == 0, "the other market's storefront is not answered either");
}

/// A kit with one quest item (the storage the client shows in its quest tab).
static svc::Character quest_record() {
    std::vector<svc::InventoryItem> items = kit();
    items.push_back(make_item(svc::ItemContainer::Quest, 0, fi::kPotion, 777, 1));
    return cyb_record(std::move(items));
}

/// A kit whose potion stack holds eleven units: ten listings fit, the eleventh must be refused.
static svc::Character eleven_record() {
    std::vector<svc::InventoryItem> items = kit();
    for (svc::InventoryItem& it : items) {
        if (it.container == Bag && it.slot == 1) it.quantity = 11;
    }
    return cyb_record(std::move(items));
}

/// A listing's answer as the emulator's reader reads it back: every FMarketplaceItemData member is present and reads.
static void test_marketplace_data_layout() {
    std::printf("the Marketplace: the data answer's layout\n");
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ClientReceiveMarketplaceData");
    CHECK(rpc != nullptr, "the client's data call is in the schema");
    if (!rpc) return;
    repl::MarketListing l;
    l.node = 108646121231993ll;
    l.seller = repl::GuidValue{0u, 0x11223344u, 0x55667788u, 0x99aabbccu};
    l.seller_name = "QiyanaSupport";
    l.guid = repl::GuidValue{0x0a000001u, 0x0b000002u, 0x0c000003u, 0x0d000004u};
    l.item.has_item = true;
    l.item.record_id = 6064633023086460928ull;
    l.item.quantity = 1;
    l.item.quality = 51;
    l.item.enchantment_level = 6;
    l.item_name = "Dustbound Ring";
    l.price.entries.push_back({items::kMoneyTier, kMoneyTierType, 2000});
    l.issue_date = 13783224.600090653;
    l.expiration_date = 14042424.600090653;
    l.status = 0;
    const repl::MarketListing one[1] = {l};
    const std::optional<repl::RpcParams> params = repl::market_data_params(*rpc, l.seller, one);
    CHECK(params.has_value(), "the data answer for a recorded listing builds");
    if (!params) return;
    BunchWriter w;
    bool failed = false;
    repl::write_rpc_params(*rpc, *params, w, {}, &failed);
    CHECK(!failed, "the data answer writes");
    size_t pos = 0;
    const std::optional<repl::RpcParams> back = repl::read_rpc_params(*rpc, w.data(), w.bit_pos(), pos);
    CHECK(back && pos == w.bit_pos(), "the emulator's own data answer reads back to its last bit");
    if (!back) return;
    const repl::RpcCall c{rpc, *back, true, 0, w.bit_pos()};
    CHECK(word_of(c, "PlayerGuid.A") == l.seller.a && word_of(c, "PlayerGuid.B") == l.seller.b && word_of(c, "PlayerGuid.C") == l.seller.c && word_of(c, "PlayerGuid.D") == l.seller.d,
          "the answer names the seller the window asked about");
    const repl::ArrayValue* list = array_of(c, "MarketplaceItemData");
    CHECK(list && list->elements.size() == 1, "the answer holds the one listing");
    if (!list || list->elements.size() != 1) return;
    const repl::ElementState& e = list->elements[0];
    CHECK(member_number(c, "MarketplaceItemData", e, "MarketplaceItemData.NodeGuid").value_or(0) == l.node, "the listing names its node");
    CHECK(member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.PlayerGuid.A") == l.seller.a && member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.PlayerGuid.B") == l.seller.b &&
              member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.PlayerGuid.C") == l.seller.c && member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.PlayerGuid.D") == l.seller.d,
          "the listing names its seller");
    CHECK(member_text(c, "MarketplaceItemData", e, "MarketplaceItemData.PlayerName") == l.seller_name, "the listing carries the seller's name");
    CHECK(member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.MarketplaceItemGuid.A") == l.guid.a &&
              member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.MarketplaceItemGuid.B") == l.guid.b &&
              member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.MarketplaceItemGuid.C") == l.guid.c &&
              member_word(c, "MarketplaceItemData", e, "MarketplaceItemData.MarketplaceItemGuid.D") == l.guid.d,
          "the listing has its own guid");
    const auto* item = repl::rpcf::as<repl::ItemInstance>(member_of(c, "MarketplaceItemData", e, "MarketplaceItemData.Item"));
    CHECK(item && item->has_item && item->record_id == l.item.record_id && item->quantity == 1 && item->quality == 51 && item->enchantment_level == 6, "the listing's item is the recorded one");
    CHECK(member_text(c, "MarketplaceItemData", e, "MarketplaceItemData.ItemName") == l.item_name, "the listing carries the item's name");
    const auto* price = repl::rpcf::as<repl::CurrencyContainer>(member_of(c, "MarketplaceItemData", e, "MarketplaceItemData.PricePerUnit"));
    CHECK(price && price->entries.size() == 1 && price->entries[0].tier == items::kMoneyTier && price->entries[0].amount == 2000, "the listing's price is 2000 copper in the Money tier");
    const auto* issue = repl::rpcf::as<double>(member_of(c, "MarketplaceItemData", e, "MarketplaceItemData.IssueDate"));
    const auto* expiry = repl::rpcf::as<double>(member_of(c, "MarketplaceItemData", e, "MarketplaceItemData.ExpirationDate"));
    CHECK(issue && std::fabs(*issue - l.issue_date) < 1e-9 && expiry && std::fabs(*expiry - l.expiration_date) < 1e-9, "the listing keeps its issue and expiration dates");
    CHECK(member_enum(c, "MarketplaceItemData", e, "MarketplaceItemData.Status").value_or(99) == 0, "the listing is LISTED");
}

/// Worn gear and quest items are refused before anything is taken, each with a code of its own (7 for worn gear, 6 for a quest item).
static void test_marketplace_worn_and_quest() {
    std::printf("the Marketplace: a worn item and a quest item are not listed\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* worn = h.s->inventory().at(Worn, 1);
    CHECK(worn && worn->item_record_id == fi::kHelmet, "the character wears the helmet");
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    if (worn) {
        const size_t bag_before = h.s->inventory().count(Bag), worn_before = h.s->inventory().count(Worn);
        const int64_t purse = h.s->wallet().copper();
        const size_t from = h.got.size();
        h.client_request(sell_request(market, items::to_instance(*worn), 1, kPrice), h.pc_ch);
        const repl::RpcCall* answer = registration_answer(pc_calls_since(h, from));
        CHECK(answer && enum_of(*answer, "Response").value_or(0) == 7, "a worn item is refused with ITEM_IN_USE (7)");
        CHECK(h.s->inventory().count(Bag) == bag_before && h.s->inventory().count(Worn) == worn_before && h.s->wallet().copper() == purse,
              "the worn refusal changes neither the bags nor the worn gear nor the purse");
    }

    MarketHarness q(quest_record());
    q.play();
    q.run(milliseconds(1500));
    q.open_market(q.manager);
    const svc::InventoryItem* quest = q.s->inventory().find_guid(guid_n(777));
    CHECK(quest && quest->container == svc::ItemContainer::Quest, "the character holds the quest item");
    if (quest) {
        const size_t bag_before = q.s->inventory().count(Bag), quest_before = q.s->inventory().count(svc::ItemContainer::Quest);
        const int64_t purse = q.s->wallet().copper();
        const size_t from = q.got.size();
        q.client_request(sell_request(market, items::to_instance(*quest), 1, kPrice), q.pc_ch);
        const repl::RpcCall* answer = registration_answer(market_calls_since(q.got, q.pc_ch, from));
        CHECK(answer && enum_of(*answer, "Response").value_or(0) == 6, "a quest item is refused with ITEM_UNTRADABLE (6)");
        CHECK(q.s->inventory().count(Bag) == bag_before && q.s->inventory().count(svc::ItemContainer::Quest) == quest_before && q.s->wallet().copper() == purse,
              "the quest refusal changes neither the bags nor the quest storage nor the purse");
    }
}

/// Ten active listings pass; the eleventh is refused with ERROR_GENERIC (1) and nothing is taken.
static void test_marketplace_listing_cap() {
    std::printf("the Marketplace: ten listings pass, the eleventh is refused\n");
    MarketHarness h(eleven_record());
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    for (int i = 0; i < 10; ++i) {
        const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
        CHECK(held && held->quantity == 11 - i, "the stack holds its units before the next listing");
        if (!held) return;
        const size_t from = h.got.size();
        h.client_request(sell_request(market, items::to_instance(*held), 1, kPrice), h.pc_ch);
        const repl::RpcCall* answer = registration_answer(pc_calls_since(h, from));
        CHECK(answer && enum_of(*answer, "Response").value_or(99) == 0, "a listing within the cap is made");
    }
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 1, "ten listings took ten units, one stays");
    if (!held) return;
    const size_t bags = h.s->inventory().count(Bag);
    const int64_t purse = h.s->wallet().copper();
    const size_t from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*held), 1, kPrice), h.pc_ch);
    const repl::RpcCall* answer = registration_answer(pc_calls_since(h, from));
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 1, "the eleventh listing is refused with ERROR_GENERIC (1)");
    const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
    CHECK(still && still->quantity == 1 && h.s->inventory().count(Bag) == bags && h.s->wallet().copper() == purse,
          "the eleventh listing takes nothing from the bag and charges nothing");
}

/// A sink whose listing save can be held, so a test sees the state while it is in flight, and which answers what the test set.
struct MarketSaveSink : RecordingSink {
    mutable std::mutex      mu;
    std::condition_variable cv;
    bool                    hold = false;          // a save waits here until release()
    size_t                  hold_attempt = 0;      // 0: every save is held; n: only the n-th save asked for is held, the ones before it answer at once
    bool                    started = false;       // a save is in the worker
    svc::CallStatus         answer = svc::CallStatus::Ok;
    std::string             reason;
    std::deque<svc::CallStatus> answers;           // the answers of the first saves, in order; once they are used up `answer` is the answer
    svc::MarketListingsResult saved_listings{svc::CallStatus::Ok, std::string(), {}};   // what a world that restarted reads
    std::deque<svc::MarketListingsResult> reads;   // the answers of the first reads of the listings, in order; once they are used up `saved_listings` is the answer
    std::atomic<int>        read_calls{0};
    std::vector<svc::MarketListing> market_saves;
    std::vector<std::vector<svc::InventoryItem>> save_bags;
    std::vector<std::vector<svc::WalletEntry>> save_wallets;
    std::vector<std::string> order;                // "listing" for a listing's save and "bag" for an ordinary bag save, in the order the sink was asked
    std::vector<std::vector<svc::WalletEntry>> bag_wallets;   // the purse of each ordinary bag save
    std::vector<svc::Mail>  mails;                 // the mails the character waits for
    bool                    hold_claim = false;    // a mail claim's bag save waits here until release_claim()
    bool                    claim_started = false;
    std::atomic<int>        claim_saves{0};
    // what the log-in tests need of the service and the saver
    std::optional<std::vector<svc::InventoryItem>> stored;   // the bags the service holds: the last of an Ok listing save, an ordinary bag save or a claim (none: nothing was written)
    bool                    hold_bags = false;     // an ordinary bag save counts as queued in the saver (character_busy) from its call until release_bags(), as a write the saver still holds
    std::atomic<bool>       bags_busy{false};
    std::function<void()>   on_bag_write;          // called when an ordinary bag save is asked for, before the saver counts it as queued

    /// Whether a write is still queued in the saver (see hold_bags): the log-in's barrier reads this, from its own thread.
    bool character_busy(const std::string&) const override { return bags_busy.load(); }
    void release_bags() { bags_busy = false; }
    /// The bags the service holds now; `fallback` when no save wrote any.
    std::vector<svc::InventoryItem> stored_bags(const std::vector<svc::InventoryItem>& fallback) const {
        std::lock_guard<std::mutex> lock(mu);
        return stored ? *stored : fallback;
    }

    /// The pace of a repeated call: a test does not wait the seconds a live saver does.
    std::chrono::milliseconds retry_delay() const override { return std::chrono::milliseconds(retry_ms); }
    int                     retry_ms = 1;              // the delay a test sets to watch the pace of the repeated calls
    std::string             stuck_character;           // the character whose listing saves the service fails for good (the others are answered as usual)
    std::atomic<int>        stuck_tries{0};

    svc::MarketListingsResult list_market_listings() override {
        std::lock_guard<std::mutex> lock(mu);
        ++read_calls;
        if (!reads.empty()) {
            svc::MarketListingsResult next = reads.front();
            reads.pop_front();
            return next;
        }
        return saved_listings;
    }
    svc::MarketSaveResult save_inventory_and_listing_now(const std::string& id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet,
                                                         std::vector<svc::ProcessingJob>, std::vector<int64_t>, std::vector<svc::WarehouseItem>,
                                                         const svc::MarketListing& listing) override {
        if (!stuck_character.empty() && id == stuck_character) {
            ++stuck_tries;
            return svc::MarketSaveResult{svc::CallStatus::Failed, "the service fails this character's save"};
        }
        std::unique_lock<std::mutex> lock(mu);
        market_saves.push_back(listing);
        save_bags.push_back(items);
        save_wallets.push_back(wallet);
        order.push_back("listing");
        started = true;
        cv.notify_all();
        if (hold_attempt == 0 || market_saves.size() == hold_attempt) cv.wait(lock, [this] { return !hold; });
        svc::MarketSaveResult out{answer, reason};
        if (!answers.empty()) {
            const svc::CallStatus next = answers.front();
            answers.pop_front();
            out = svc::MarketSaveResult{next, next == svc::CallStatus::Ok ? std::string() : reason};
        }
        if (out.status == svc::CallStatus::Ok) stored = items;   // one transaction: the listing and the bags together
        return out;
    }
    /// An ordinary bag save (the periodic one, the logout's, or the claim's), and the purse the six-part one carries.
    void save_inventory(const std::string& id, std::vector<svc::InventoryItem> items) override {
        {
            std::lock_guard<std::mutex> lock(mu);
            order.push_back("bag");
            stored = items;
        }
        if (on_bag_write) on_bag_write();
        if (hold_bags) bags_busy = true;
        RecordingSink::save_inventory(id, std::move(items));
    }
    void save_inventory(const std::string& id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                        std::vector<int64_t> known, std::vector<svc::WarehouseItem> warehouse) override {
        {
            std::lock_guard<std::mutex> lock(mu);
            bag_wallets.push_back(wallet);
        }
        PositionSink::save_inventory(id, std::move(items), std::move(wallet), std::move(jobs), std::move(known), std::move(warehouse));
    }
    svc::MailsResult load_mails(const std::string&) override {
        svc::MailsResult result;
        result.status = svc::CallStatus::Ok;
        result.mails = mails;
        return result;
    }
    svc::MailSaveResult save_mail(const std::string&, const svc::Mail&) override {
        svc::MailSaveResult result;
        result.status = svc::CallStatus::Ok;
        return result;
    }
    /// A mail claim: ONE call that stores the bags and marks the attachments claimed.  It waits at hold_claim, so a test sees the state while it is in flight, and stores the
    /// bags as a bag save of this sink does (a "bag" in `order`).
    svc::MailClaimResult claim_mail_items(const std::string& id, const std::vector<svc::MailClaim>& claims, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry>,
                                          std::vector<svc::ProcessingJob>, std::vector<int64_t>, std::vector<svc::WarehouseItem>) override {
        {
            std::unique_lock<std::mutex> lock(mu);
            ++claim_saves;
            claim_started = true;
            cv.notify_all();
            cv.wait(lock, [this] { return !hold_claim; });
        }
        save_inventory(id, std::move(items));
        svc::MailClaimResult result;
        result.status = svc::CallStatus::Ok;
        result.claimed = static_cast<int32_t>(claims.size());
        return result;
    }
    bool wait_started(int milliseconds) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return started; });
    }
    /// Waits until the listing's save was asked for `n` times.
    bool wait_attempts(size_t n, int milliseconds) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::milliseconds(milliseconds), [this, n] { return market_saves.size() >= n; });
    }
    bool wait_claim_started(int milliseconds) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return claim_started; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mu);
        hold = false;
        cv.notify_all();
    }
    void release_claim() {
        std::lock_guard<std::mutex> lock(mu);
        hold_claim = false;
        cv.notify_all();
    }
    void reset_started() {
        std::lock_guard<std::mutex> lock(mu);
        started = false;
    }
};

/// The Marketplace: a listing's save carries the bag after the units left it, and the client hears of the listing only when the store answered.
static void test_marketplace_listing_reaches_the_store() {
    std::printf("the Marketplace: a listing reaches the store with the bag after it\n");
    MarketSaveSink sink;
    sink.hold = true;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t fee = 1000;
    const int64_t purse = h.s->wallet().copper();
    const size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, stack, 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save reached the sink");
    for (int i = 0; i < 3; ++i) h.step();
    CHECK(registration_answer(pc_calls_since(h, from)) == nullptr, "the client is told nothing before the store answers");
    CHECK(currency_callout(pc_calls_since(h, from)) == nullptr, "no money popup is sent before the store answers");
    {
        std::lock_guard<std::mutex> lock(sink.mu);
        CHECK(sink.market_saves.size() == 1 && sink.save_bags.size() == 1, "the store was asked once");
        if (!sink.market_saves.empty() && !sink.save_bags.empty()) {
            const svc::MarketListing& listing = sink.market_saves.back();
            CHECK(listing.item.quantity == 2, "the listing carries two units");
            bool listedInBag = false;
            for (const svc::InventoryItem& it : sink.save_bags.back()) {
                listedInBag = listedInBag || it.instance_guid == listing.item.instance_guid;
            }
            CHECK(!listedInBag, "the saved bag does not hold the listed stack");
        }
    }
    // the purse the worker was given already lost the fee
    CHECK(!sink.save_wallets.empty() && sink.save_wallets.back().size() >= 1 && sink.save_wallets.back()[0].amount == purse - fee,
          "the saved purse has paid the listing fee");
    sink.release();
    h.settle();
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(1) == 0, "the listing is answered once the store has it");
    CHECK(currency_callout(calls) != nullptr, "the money popup follows the saved listing");
    CHECK(h.s->wallet().copper() == purse - fee, "the fee left the purse");
}

/// The Marketplace: a refused save gives the stack and the fee back and answers ERROR_GENERIC.
static void test_marketplace_refused_save_gives_back() {
    std::printf("the Marketplace: a refused listing save gives the stack and the fee back\n");
    MarketSaveSink sink;
    sink.answer = svc::CallStatus::Refused;
    sink.reason = "the store refused it";
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::ItemInstance stack = items::to_instance(*held);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    const size_t listings = h.fx.world->market_listings()->size();
    const size_t from = h.got.size();
    h.client_request(sell_request(market, stack, 2, kPrice), h.pc_ch);
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 1, "a refused save answers ERROR_GENERIC (1)");
    const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
    CHECK(still && still->quantity == 5 && still->instance_guid == items::guid_hex(stack.guid), "the stack is back in the bag");
    CHECK(h.s->wallet().copper() == purse, "the fee is back in the purse");
    CHECK(h.fx.world->market_listings()->size() == listings, "the refusal makes no listing");
    CHECK(currency_callout(calls) == nullptr, "a refused save sends no money popup");
}

/// The Marketplace: a periodic bag save waits while a listing's save is in flight, then goes after it.
static void test_marketplace_bag_save_waits() {
    std::printf("the Marketplace: a bag save is held while a listing's save is in flight\n");
    MarketSaveSink sink;
    sink.hold = true;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    if (!held) return;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save reached the sink");
    const size_t saved_before = sink.inventories.size();
    h.run(milliseconds(61'000));   // the periodic save's interval, held by the market's
    CHECK(sink.inventories.size() == saved_before, "no bag save was written while the listing's save is in flight");
    CHECK(h.s->inventory_dirty(), "the bag stays marked for saving");
    sink.release();
    h.settle();
    CHECK(sink.inventories.size() > saved_before, "the held bag save goes after the listing's answer");
}

/// The Marketplace: a part of a stack leaves as a new instance; a whole stack keeps the stack's guid.
static void test_marketplace_partial_stack_new_guid() {
    std::printf("the Marketplace: a part of a stack gets a new instance guid\n");
    MarketSaveSink sink;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    if (!held) return;
    const repl::GuidValue stack = items::to_instance(*held).guid;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
    CHECK(sink.market_saves.size() == 1, "the partial listing was saved");
    if (!sink.market_saves.empty()) {
        const std::optional<repl::GuidValue> listed = items::guid_of(sink.market_saves.back().item.instance_guid);
        CHECK(listed && !(listed->a == stack.a && listed->b == stack.b && listed->c == stack.c && listed->d == stack.d), "the listed part has a guid of its own");
    }
    const svc::InventoryItem* rest = h.s->inventory().at(Bag, 1);
    CHECK(rest && rest->quantity == 3, "three units stay in the bag");
    if (!rest) return;
    h.client_request(sell_request(market, items::to_instance(*rest), 3, kPrice), h.pc_ch);
    CHECK(sink.market_saves.size() == 2, "the whole rest was saved");
    if (sink.market_saves.size() == 2) {
        const std::optional<repl::GuidValue> listed = items::guid_of(sink.market_saves.back().item.instance_guid);
        CHECK(listed && listed->a == stack.a && listed->b == stack.b && listed->c == stack.c && listed->d == stack.d, "a whole stack keeps its own instance guid");
    }
}

/// The Marketplace: the cap counts the listing whose save is in flight.
static void test_marketplace_cap_counts_in_flight() {
    std::printf("the Marketplace: a listing while another one's save is in flight waits, and the cap counts the one that landed\n");
    MarketSaveSink sink;
    MarketHarness h(eleven_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    for (int i = 0; i < 9; ++i) {
        const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
        if (!held) return;
        h.client_request(sell_request(market, items::to_instance(*held), 1, kPrice), h.pc_ch);
    }
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 2, "nine listings took nine of the eleven units");
    if (!held) return;
    sink.hold = true;
    sink.reset_started();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 1, kPrice), true)});
    CHECK(sink.wait_started(3000), "the tenth listing's save is in flight");
    const svc::InventoryItem* left = h.s->inventory().at(Bag, 1);
    CHECK(left && left->quantity == 1, "the tenth listing took one unit");
    if (!left) { sink.release(); return; }
    // the eleventh listing is another stack (the sword at the bag's first slot): the same stack is refused as held
    const svc::InventoryItem* other = h.s->inventory().at(Bag, 0);
    CHECK(other && other->instance_guid != left->instance_guid, "the bag holds a second stack to list");
    if (!other) { sink.release(); return; }
    const size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*other), 1, kPrice), true)});
    for (int i = 0; i < 3; ++i) h.step();
    const repl::RpcCall* answer = registration_answer(pc_calls_since(h, from));
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 3, "a listing is refused with REQUEST_ALREADY_IN_PROGRESS (3) while the tenth is in flight");
    const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
    CHECK(still && still->quantity == 1, "the refused listing takes nothing");
    sink.release();
    h.settle();
    // the tenth landed: the cap now holds the ten
    const svc::InventoryItem* again = h.s->inventory().at(Bag, 0);
    CHECK(again != nullptr, "the second stack is still in the bag");
    if (!again) return;
    const size_t after = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*again), 1, kPrice), h.pc_ch);
    const repl::RpcCall* capped = registration_answer(pc_calls_since(h, after));
    CHECK(capped && enum_of(*capped, "Response").value_or(0) == 1, "the eleventh listing is refused with ERROR_GENERIC (1) by the cap");
}

/// The Marketplace: a world that restarts reads the saved listings and shows their storefronts, prices and dates, and the cap counts them.
static void test_marketplace_restart_shows_saved_listings() {
    std::printf("the Marketplace: a restarted world shows the saved listings\n");
    MarketSaveSink sink;
    std::vector<svc::InventoryItem> many = kit();
    for (svc::InventoryItem& it : many) {
        if (it.container == Bag && it.slot == 1) it.quantity = 15;
    }
    const svc::Character who = cyb_record(std::move(many));
    const std::optional<repl::GuidValue> seller = items::guid_of(who.id);
    CHECK(seller.has_value(), "the seller's id reads as a guid");
    if (!seller) return;
    const int64_t unix_before = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    svc::MarketListing row;
    row.id = guid_n(0x9001);
    row.seller_id = who.id;
    row.seller_name = "cyb";
    row.node = kNode;
    row.item = make_item(Bag, 1, fi::kPotion, 920, 5);
    row.item.quantity = 2;
    row.price_per_unit = kPrice;
    row.status = 0;
    row.issue_date_ms = unix_before - 3'600'000;   // listed an hour before the restart
    row.expiry_date_ms = row.issue_date_ms + 259'200'000;
    sink.saved_listings.status = svc::CallStatus::Ok;
    sink.saved_listings.listings.push_back(row);
    MarketHarness h(who, &sink);
    const double expected_issue = bootstrap::effect_clock(0.0) + (static_cast<double>(row.issue_date_ms) - static_cast<double>(unix_before)) / 1000.0;
    h.play();
    h.run(milliseconds(1500));
    CHECK(h.fx.world->market_listings()->size() == 1 && h.fx.world->market_listings()->count(*seller) == 1, "the world read the one saved listing");
    CHECK(h.fx.world->market_listings()->storefronts(kNode).size() == 1, "the saved listing is at the market's node");
    CHECK(h.fx.world->market_listings()->listings(kNode, *seller).size() == 1, "the saved listing is the seller's at the market's node");
    const world::WorldActor* manager = h.fx.world->registry().find(h.manager);
    CHECK(manager && ::aoc::worldd::market::node_of(manager->actor) == kNode, "the test Manager carries the market's node");
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    CHECK(h.fx.world->market_listings()->storefronts(kNode).size() == 1, "the storefront is in the cache before the request");
    size_t from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    // one header built from the loaded storefront reads back on its own, so the wire below is checked against a known-good encoding
    {
        const std::vector<::aoc::worldd::market::Storefront> fronts = h.fx.world->market_listings()->storefronts(kNode);
        const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
        const schema::RpcSchema* rpc = pc.rpc("ClientReceiveMarketplaceHeaders");
        CHECK(fronts.size() == 1 && rpc != nullptr, "the storefront reads for its own encoding");
        if (fronts.size() == 1 && rpc) {
            repl::MarketHeader one;
            one.node = kNode;
            one.seller = fronts[0].seller;
            one.seller_name = fronts[0].seller_name;
            one.description = fronts[0].seller_name + "'s Storefront";
            one.issue_date = fronts[0].issue_date;
            const std::optional<repl::RpcParams> params = repl::market_headers_params(*rpc, kNode, std::span<const repl::MarketHeader>(&one, 1), true);
            BunchWriter w;
            bool failed = false;
            if (params) repl::write_rpc_params(*rpc, *params, w, {}, &failed);
            size_t pos = 0;
            const std::optional<repl::RpcParams> back = params && !failed ? repl::read_rpc_params(*rpc, w.data(), w.bit_pos(), pos) : std::nullopt;
            CHECK(back && pos == w.bit_pos(), "a loaded storefront's header writes and reads back");
            if (back) {
                const repl::RpcCall c{rpc, *back, true, 0, w.bit_pos()};
                const repl::ArrayValue* list = array_of(c, "Headers");
                CHECK(list && list->elements.size() == 1, "the same header reads as one array element");
            }
        }
    }
    const std::vector<repl::RpcCall> header_calls = pc_calls_since(h, from);
    const repl::RpcCall* headers = call_named(header_calls, "ClientReceiveMarketplaceHeaders");
    CHECK(headers != nullptr, "the storefront page is answered after the restart");
    CHECK(h.fx.world->market_listings()->storefronts(kNode).size() == 1, "the storefront is still in the cache after the request");
    if (headers) {
        CHECK(headers->decoded, "the headers packet reads with the client's schema");
        CHECK(number_of(*headers, "NodeId").value_or(-1) == kNode, "the headers name the market's node");
        CHECK(enum_of(*headers, "HeaderVersion").value_or(0) == 1843, "the headers carry their version");
        const repl::ArrayValue* list = array_of(*headers, "Headers");
        CHECK(list && list->elements.size() == 1, "one storefront header for the saved listing");
        if (list && list->elements.size() == 1) {
            CHECK(member_text(*headers, "Headers", list->elements[0], "Headers.PlayerName") == "cyb", "the saved listing's storefront names its seller");
            const auto* date = repl::rpcf::as<double>(member_of(*headers, "Headers", list->elements[0], "Headers.IssueDate"));
            CHECK(date && std::fabs(*date - expected_issue) < 0.2, "the storefront's issue date is the saved one on the client's clock");
        }
    }
    from = h.got.size();
    h.client_request(data_request(market, *seller, true), h.pc_ch);
    const std::vector<repl::RpcCall> data_calls = pc_calls_since(h, from);
    const repl::RpcCall* data = call_named(data_calls, "ClientReceiveMarketplaceData");
    CHECK(data != nullptr, "the Sell tab is answered after the restart");
    if (data) {
        CHECK(word_of(*data, "PlayerGuid.A") == seller->a && word_of(*data, "PlayerGuid.B") == seller->b, "the data echoes the seller's guid");
        const repl::ArrayValue* list = array_of(*data, "MarketplaceItemData");
        CHECK(list && list->elements.size() == 1, "the seller's saved listing is in the Sell tab");
        if (list && list->elements.size() == 1) {
            CHECK(word_of(*data, "PlayerGuid.A") == seller->a && word_of(*data, "PlayerGuid.B") == seller->b, "the data names the seller the window asked about");
            const auto* item = repl::rpcf::as<repl::ItemInstance>(member_of(*data, "MarketplaceItemData", list->elements[0], "MarketplaceItemData.Item"));
            const auto* price = repl::rpcf::as<repl::CurrencyContainer>(member_of(*data, "MarketplaceItemData", list->elements[0], "MarketplaceItemData.PricePerUnit"));
            const auto* issue = repl::rpcf::as<double>(member_of(*data, "MarketplaceItemData", list->elements[0], "MarketplaceItemData.IssueDate"));
            const auto* expiry = repl::rpcf::as<double>(member_of(*data, "MarketplaceItemData", list->elements[0], "MarketplaceItemData.ExpirationDate"));
            CHECK(item && item->quantity == 2 && price && price->entries.size() == 1 && price->entries[0].amount == kPrice, "the saved listing keeps its stack and its price");
            CHECK(issue && expiry && std::fabs(*issue - expected_issue) < 0.2 && std::fabs((*expiry - *issue) - kLifeSeconds) < 0.01, "the saved listing keeps its dates");
        }
    }
    // the cap counts the saved listing: nine new ones fit, the tenth does not.
    for (int i = 0; i < 9 && h.s->inventory().at(Bag, 1); ++i) {
        const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
        h.client_request(sell_request(market, items::to_instance(*held), 1, kPrice), h.pc_ch);
    }
    CHECK(h.fx.world->market_listings()->count(*seller) == 10, "the saved listing and the nine new ones fill the cap");
}

/// The Marketplace: until the saved listings are read, a registration is refused and no storefront answers.
static void test_marketplace_not_loaded_refuses() {
    std::printf("the Marketplace: a world whose listings did not load refuses registrations\n");
    MarketSaveSink sink;
    sink.saved_listings.status = svc::CallStatus::Failed;
    sink.saved_listings.reason = "the character service is down";
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    CHECK(!h.fx.world->market_listings()->loaded(), "the world knows the listings were not read");
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    const size_t from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
    const repl::RpcCall* answer = registration_answer(pc_calls_since(h, from));
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 1, "a registration before the read is refused with ERROR_GENERIC (1)");
    const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
    CHECK(still && still->quantity == 5 && h.s->wallet().copper() == purse, "the refusal takes nothing");
    const size_t at = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    const std::vector<repl::RpcCall> calls = pc_calls_since(h, at);
    const repl::RpcCall* headers = call_named(calls, "ClientReceiveMarketplaceHeaders");
    CHECK(headers == nullptr || array_of(*headers, "Headers") == nullptr, "a world whose listings did not load answers no storefront");
}

// ---- a save that does not finish as it started: the refunds, the unclear answers, the logout, the first read, the mail ------------------------------------------------

/// ServerRequestStorageEvent as the client sends it for the bag: a Move of `count` units of the stack `guid` from slot `from` to the empty slot `to`, or the destroy of `count`
/// units of it in slot `from` (the Remove event of the stack, `to` unused).
static BunchWriter bag_event(const Harness& h, repl::StorageEventType type, int64_t record, const std::string& guid, int from, int count, int to) {
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    repl::ItemStorageEvent ev;
    ev.type = static_cast<uint8_t>(type);
    const auto gv = items::guid_of(guid);
    const repl::GuidValue named = gv ? *gv : repl::GuidValue{};
    ev.item.has_item = true;
    ev.item.record_id = static_cast<uint64_t>(record);
    ev.item.quantity = static_cast<uint32_t>(count);
    ev.item.quality = 1;
    ev.item.guid = named;
    ev.item.creation_time = items::ticks_of_ms(1'771'299'474'000ll);
    repl::StorageEventContainer target;
    target.object = g.backpack;
    if (type == repl::StorageEventType::Move) {
        ev.move_source.object = g.backpack;
        ev.move_source.slots.push_back({from, count, named});
        target.slots.push_back({to, 1, repl::GuidValue{}});
    } else {
        target.slots.push_back({from, count, named});
    }
    ev.targets.push_back(target);
    const auto& cg = h.s->bootstrap().setup().character_guid;
    ev.instigator = repl::GuidValue{cg[0], cg[1], cg[2], cg[3]};
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestStorageEvent");
    const auto params = rpc ? repl::storage_event_params(*rpc, ev) : std::nullopt;
    return params ? actor_rpc(pc, "ServerRequestStorageEvent", *params) : BunchWriter();
}

/// How many units of the potion the inventory holds, and how many of its items carry `guid` (a guid held twice shows as two).
static int64_t potion_units(const items::Inventory& inventory) { return inventory.count_of(fi::kPotion); }
static size_t items_with_guid(const items::Inventory& inventory, const std::string& guid) {
    size_t n = 0;
    for (const svc::InventoryItem& it : inventory.items()) n += it.instance_guid == guid ? 1 : 0;
    return n;
}

/// The world's own step, as the endpoint's tick makes it, until `done` or `milliseconds` of real time have passed.
static bool step_world_until(Harness& h, const std::function<bool()>& done, int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        h.t += std::chrono::milliseconds(20);
        h.fx.world->tick_world(world_time_at(h.t));
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/// The item of a saved bag that holds `guid` (nullptr: none does).
static const svc::InventoryItem* saved_item(const std::vector<svc::InventoryItem>& bag, const std::string& guid) {
    for (const svc::InventoryItem& it : bag) {
        if (it.instance_guid == guid) return &it;
    }
    return nullptr;
}

/// A refused save gives back exactly the listed units and the fee, to the rest of the stack wherever it is by then; no other item of the bag changes.
static void test_marketplace_refund_follows_the_rest_that_moved() {
    std::printf("the Marketplace: a refused save gives the units back to the rest of the stack where it went\n");
    MarketSaveSink sink;
    sink.hold = true;
    sink.answer = svc::CallStatus::Refused;
    sink.reason = "the store refused it";
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const std::string guid = held->instance_guid;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save reached the sink");
    // the three units that stayed go to another slot while the save is in flight
    h.client_sends({h.client.actor(h.pc_ch, bag_event(h, repl::StorageEventType::Move, fi::kPotion, guid, 1, 3, 9), true)});
    for (int i = 0; i < 3; ++i) h.step();
    const svc::InventoryItem* moved = h.s->inventory().at(Bag, 9);
    CHECK(moved && moved->instance_guid == guid && moved->quantity == 3 && !h.s->inventory().at(Bag, 1), "the three units that stayed are in slot 9 now");
    sink.release();
    h.settle();
    const svc::InventoryItem* back = h.s->inventory().at(Bag, 9);
    CHECK(back && back->instance_guid == guid && back->quantity == 5, "the two listed units joined the rest where it is now: five in slot 9");
    CHECK(!h.s->inventory().at(Bag, 1), "the stack's first slot stays empty");
    CHECK(potion_units(h.s->inventory()) == 5 && items_with_guid(h.s->inventory(), guid) == 1, "five units in all, and the stack's guid is held once");
    CHECK(h.s->wallet().copper() == purse, "the fee is back in the purse");
    const svc::InventoryItem* sword = h.s->inventory().at(Bag, 0);
    CHECK(sword && sword->item_record_id == fi::kSword2H && sword->instance_guid == guid_n(200), "no other item of the bag changed");
}

/// A unit of the rest destroyed during the flight stays destroyed: the refund adds the listed units to what the rest holds now.
static void test_marketplace_refund_keeps_a_unit_the_player_destroyed() {
    std::printf("the Marketplace: a refused save does not give back a unit the player destroyed meanwhile\n");
    MarketSaveSink sink;
    sink.hold = true;
    sink.answer = svc::CallStatus::Refused;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const std::string guid = held->instance_guid;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save reached the sink");
    h.client_sends({h.client.actor(h.pc_ch, bag_event(h, repl::StorageEventType::Remove, fi::kPotion, guid, 1, 1, 0), true)});
    for (int i = 0; i < 3; ++i) h.step();
    const svc::InventoryItem* rest = h.s->inventory().at(Bag, 1);
    CHECK(rest && rest->quantity == 2, "one of the three units that stayed was destroyed: two are left");
    sink.release();
    h.settle();
    const svc::InventoryItem* back = h.s->inventory().at(Bag, 1);
    CHECK(back && back->instance_guid == guid && back->quantity == 4, "the two listed units joined the two that are left: four, not five");
    CHECK(potion_units(h.s->inventory()) == 4 && items_with_guid(h.s->inventory(), guid) == 1, "four units in all, and the stack's guid is held once");
    CHECK(h.s->wallet().copper() == purse, "the fee is back in the purse");
}

/// The whole rest destroyed during the flight: the listed units come back under the stack's own guid at its slot.
static void test_marketplace_refund_when_the_rest_is_gone() {
    std::printf("the Marketplace: a refused save puts the units back at the stack's slot when the rest is gone\n");
    MarketSaveSink sink;
    sink.hold = true;
    sink.answer = svc::CallStatus::Refused;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const std::string guid = held->instance_guid;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save reached the sink");
    h.client_sends({h.client.actor(h.pc_ch, bag_event(h, repl::StorageEventType::Remove, fi::kPotion, guid, 1, 3, 0), true)});
    for (int i = 0; i < 3; ++i) h.step();
    CHECK(!h.s->inventory().at(Bag, 1) && potion_units(h.s->inventory()) == 0, "the three units that stayed were destroyed");
    sink.release();
    h.settle();
    const svc::InventoryItem* back = h.s->inventory().at(Bag, 1);
    CHECK(back && back->instance_guid == guid && back->quantity == 2, "the two listed units are back under the stack's guid in its slot");
    CHECK(potion_units(h.s->inventory()) == 2 && items_with_guid(h.s->inventory(), guid) == 1, "two units in all, and the guid is held once");
    CHECK(h.s->wallet().copper() == purse, "the fee is back in the purse");
}

/// A whole stack listed: when its slot was taken during the flight the stack comes back to the lowest free slot, and the item that took the slot stays.
static void test_marketplace_refund_of_a_whole_stack_to_a_free_slot() {
    std::printf("the Marketplace: a refused save puts a whole stack back at a free slot when its own was taken\n");
    MarketSaveSink sink;
    sink.hold = true;
    sink.answer = svc::CallStatus::Refused;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const std::string guid = held->instance_guid;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 5, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save reached the sink");
    CHECK(!h.s->inventory().at(Bag, 1), "the whole stack left its slot");
    // the sword moves into the slot the stack left
    h.client_sends({h.client.actor(h.pc_ch, bag_event(h, repl::StorageEventType::Move, fi::kSword2H, guid_n(200), 0, 1, 1), true)});
    for (int i = 0; i < 3; ++i) h.step();
    const svc::InventoryItem* sword = h.s->inventory().at(Bag, 1);
    CHECK(sword && sword->instance_guid == guid_n(200) && !h.s->inventory().at(Bag, 0), "the sword took the stack's slot");
    sink.release();
    h.settle();
    const svc::InventoryItem* back = h.s->inventory().at(Bag, 0);
    CHECK(back && back->instance_guid == guid && back->quantity == 5, "the stack is back under its guid in the lowest free slot");
    const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
    CHECK(still && still->instance_guid == guid_n(200), "the item that took the slot stays in it");
    CHECK(potion_units(h.s->inventory()) == 5 && items_with_guid(h.s->inventory(), guid) == 1, "five units in all, and the guid is held once");
    CHECK(h.s->wallet().copper() == purse, "the fee is back in the purse");
}

/// A save the store answers with Unavailable or Failed may have been written: the same save is asked for again until the answer is clear, and the seller's bags stay held and
/// the client unanswered meanwhile.
static void test_marketplace_unclear_save_is_tried_again() {
    std::printf("the Marketplace: a save that is not answered clearly is tried again, the same one, until it is\n");
    MarketSaveSink sink;
    sink.answers = {svc::CallStatus::Unavailable, svc::CallStatus::Failed};
    sink.hold = true;
    sink.hold_attempt = 3;   // the third try waits, so the test sees the seller while the save is undecided
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const repl::GuidValue seller = character_guid(h);
    const int64_t fee = 1000;
    const int64_t purse = h.s->wallet().copper();
    const size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_attempts(3, 2000), "the save was asked for three times: the two unclear answers were tried again");
    for (int i = 0; i < 3; ++i) h.step();
    CHECK(count_calls(pc_calls_since(h, from), "ClientMarketplaceItemRegistrationResponse") == 0, "the client is told nothing while the save is undecided");
    CHECK(h.fx.world->market_listings()->count(seller) == 0, "no listing is in the world while the save is undecided");
    const size_t bags_before = sink.inventories.size();
    h.run(milliseconds(61'000));   // the periodic bag save's interval
    CHECK(sink.inventories.size() == bags_before && h.s->inventory_dirty(), "no bag save is written while the save is undecided");
    {
        std::lock_guard<std::mutex> lock(sink.mu);
        bool same = sink.market_saves.size() == 3;
        for (size_t i = 1; same && i < sink.market_saves.size(); ++i) {
            same = sink.market_saves[i] == sink.market_saves[0] && sink.save_bags[i] == sink.save_bags[0] && sink.save_wallets[i] == sink.save_wallets[0];
        }
        CHECK(same, "every try carries the same listing, the same bags and the same purse");
    }
    sink.release();
    h.settle();
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(calls);
    CHECK(count_calls(calls, "ClientMarketplaceItemRegistrationResponse") == 1 && answer && enum_of(*answer, "Response").value_or(1) == 0, "the client is answered once, with the listing");
    CHECK(h.fx.world->market_listings()->count(seller) == 1, "the world holds one listing");
    const svc::InventoryItem* rest = h.s->inventory().at(Bag, 1);
    CHECK(rest && rest->quantity == 3 && h.s->wallet().copper() == purse - fee, "three units stay in the bag and the fee was spent once");
    CHECK(sink.market_saves.size() == 3, "no fourth try was made after the clear answer");
    CHECK(sink.inventories.size() > bags_before, "the held bag save goes after the answer");
}

/// A clear refusal after an unclear answer gives back exactly what the listing took.
static void test_marketplace_unclear_then_refused_gives_back() {
    std::printf("the Marketplace: an unclear answer and then a refusal give the stack and the fee back\n");
    MarketSaveSink sink;
    sink.answers = {svc::CallStatus::Unavailable};
    sink.answer = svc::CallStatus::Refused;
    sink.reason = "the store refused it";
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const std::string guid = held->instance_guid;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const repl::GuidValue seller = character_guid(h);
    const int64_t purse = h.s->wallet().copper();
    const size_t from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
    const auto calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(calls);
    CHECK(count_calls(calls, "ClientMarketplaceItemRegistrationResponse") == 1 && answer && enum_of(*answer, "Response").value_or(0) == 1, "the client is answered once, with ERROR_GENERIC (1)");
    const svc::InventoryItem* back = h.s->inventory().at(Bag, 1);
    CHECK(back && back->instance_guid == guid && back->quantity == 5 && potion_units(h.s->inventory()) == 5, "the stack is back in the bag");
    CHECK(h.s->wallet().copper() == purse, "the fee is back in the purse");
    CHECK(h.fx.world->market_listings()->count(seller) == 0, "the world holds no listing");
    CHECK(currency_callout(calls) == nullptr, "no money popup is sent");
    CHECK(sink.market_saves.size() == 2, "the save was asked for twice");
}

/// A world that stops while a save is still undecided says so once, with what the character service may hold.
static void test_marketplace_undecided_save_is_logged_at_stop() {
    std::printf("the Marketplace: a world that stops with an undecided save logs it once\n");
    MarketSaveSink sink;
    sink.answer = svc::CallStatus::Unavailable;
    sink.reason = "the character service is down";
    std::ostringstream log;
    auto logger = std::make_shared<spdlog::logger>("market_stop_test", std::make_shared<spdlog::sinks::ostream_sink_mt>(log, true));
    logger->set_level(spdlog::level::err);
    const std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
    spdlog::set_default_logger(logger);
    std::string listing_id, stack_guid;
    {
        MarketHarness h(cyb_record(), &sink);
        h.play();
        h.run(milliseconds(1500));
        h.open_market(h.manager);
        const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
        CHECK(held && held->quantity == 5, "the bag holds the stack of five");
        if (held) {
            const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
            h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
            CHECK(sink.wait_attempts(3, 2000), "the save keeps being tried while the store does not answer clearly");
            std::lock_guard<std::mutex> lock(sink.mu);
            if (!sink.market_saves.empty()) {
                listing_id = sink.market_saves.back().id;
                stack_guid = sink.market_saves.back().item.instance_guid;
            }
        }
    }   // the session ends and the world stops with the save undecided
    spdlog::set_default_logger(previous);
    spdlog::drop("market_stop_test");
    std::istringstream lines(log.str());
    std::string line, found;
    size_t undecided = 0;
    while (std::getline(lines, line)) {
        if (line.find("undecided") == std::string::npos) continue;
        ++undecided;
        found = line;
    }
    CHECK(undecided == 1, "one line says a listing was left undecided");
    CHECK(!listing_id.empty() && found.find(listing_id) != std::string::npos, "the line names the listing's guid");
    CHECK(found.find(kCyb) != std::string::npos, "the line names the seller");
    CHECK(!stack_guid.empty() && found.find(stack_guid) != std::string::npos && found.find("x2") != std::string::npos, "the line names the stack and its units");
    CHECK(found.find("fee 1000 copper") != std::string::npos, "the line names the fee");
}

/// The seller lists two of the five potions with the save held in `sink`, destroys one of the three that stayed and logs out, the save being in flight.  False when the
/// test could not be set up.
static bool list_destroy_and_log_out(MarketHarness& h, MarketSaveSink& sink, size_t attempts, std::string& guid, int64_t& purse, repl::GuidValue& seller) {
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return false;
    guid = held->instance_guid;
    purse = h.s->wallet().copper();
    seller = character_guid(h);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_attempts(attempts, 3000), "the listing's save reached the sink");
    h.client_sends({h.client.actor(h.pc_ch, bag_event(h, repl::StorageEventType::Remove, fi::kPotion, guid, 1, 1, 0), true)});
    for (int i = 0; i < 3; ++i) h.step();
    const svc::InventoryItem* rest = h.s->inventory().at(Bag, 1);
    CHECK(rest && rest->quantity == 2, "the player destroyed one of the three units that stayed: two are left");
    h.s->finish("the client left");
    h.s.reset();
    return true;
}

/// After the logout the storefront and the Sell tab of a new session of the seller show the listing.
static void check_listing_shown_to_a_new_session(MarketHarness& h, const repl::GuidValue& seller) {
    MarketPeer peer(h, cyb_record());
    peer.play();
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    peer.request(interact(market_component(h.fx.world->registry(), h.manager)), peer.pawn_ch);
    size_t from = peer.got.size();
    peer.request(market_headers_request(market, 1843), peer.pc_ch);
    const std::vector<repl::RpcCall> header_calls = market_calls_since(peer.got, peer.pc_ch, from);
    const repl::RpcCall* headers = call_named(header_calls, "ClientReceiveMarketplaceHeaders");
    const repl::ArrayValue* fronts = headers ? array_of(*headers, "Headers") : nullptr;
    CHECK(fronts && fronts->elements.size() == 1, "the storefront of a new session shows the seller");
    from = peer.got.size();
    peer.request(data_request(market, seller, true), peer.pc_ch);
    const std::vector<repl::RpcCall> data_calls = market_calls_since(peer.got, peer.pc_ch, from);
    const repl::RpcCall* data = call_named(data_calls, "ClientReceiveMarketplaceData");
    const repl::ArrayValue* sold = data ? array_of(*data, "MarketplaceItemData") : nullptr;
    CHECK(sold && sold->elements.size() == 1, "the Sell tab of a new session shows the listing");
}

/// A logout while the listing's save is in flight and the store answers Ok: the world takes the answer, the listing is on the storefront and the Sell tab, the cap counts it, and the
/// bag the player left with is written after the listing.
static void test_marketplace_logout_during_a_save_keeps_the_listing() {
    std::printf("the Marketplace: a seller who logs out while the save is in flight still has the listing\n");
    MarketSaveSink sink;
    sink.hold = true;
    MarketHarness h(cyb_record(), &sink);
    std::string guid;
    int64_t purse = 0;
    repl::GuidValue seller;
    if (!list_destroy_and_log_out(h, sink, 1, guid, purse, seller)) {
        sink.release();
        return;
    }
    const size_t bags_before = sink.inventories.size();
    CHECK(h.fx.world->market_listings()->count(seller) == 0, "the listing is not in the world before the store answered");
    sink.release();
    CHECK(step_world_until(h, [&] { return h.fx.world->market_listings()->count(seller) == 1; }, 3000), "the world takes the answer of the seller who left: the listing is in the market");
    CHECK(step_world_until(h, [&] { return sink.inventories.size() > bags_before; }, 3000), "the bag the seller left with is written once the listing is saved");
    {
        std::lock_guard<std::mutex> lock(sink.mu);
        const size_t n = sink.order.size();
        CHECK(n >= 2 && sink.order[n - 2] == "listing" && sink.order[n - 1] == "bag", "the final bag is saved after the listing");
    }
    if (!sink.inventories.empty()) {
        const std::vector<svc::InventoryItem>& last = sink.inventories.back();
        const svc::InventoryItem* rest = saved_item(last, guid);
        CHECK(rest && rest->quantity == 2, "the final bag keeps the unit the player destroyed after the listing");
        std::string listed;
        {
            std::lock_guard<std::mutex> lock(sink.mu);
            if (!sink.market_saves.empty()) listed = sink.market_saves.back().item.instance_guid;
        }
        CHECK(!listed.empty() && saved_item(last, listed) == nullptr, "the final bag does not hold the listed units");
    }
    CHECK(!sink.bag_wallets.empty() && !sink.bag_wallets.back().empty() && sink.bag_wallets.back()[0].amount == purse - 1000, "the final purse has paid the fee once");
    check_listing_shown_to_a_new_session(h, seller);
    CHECK(h.fx.world->market_listings()->count(seller) == 1, "the cap counts the listing");
}

/// A logout while the save is in flight and the store refuses it: the final bag gets the units and the fee back.
static void test_marketplace_logout_during_a_refused_save_gives_back() {
    std::printf("the Marketplace: a seller who logs out while a save is in flight gets the units back when it is refused\n");
    MarketSaveSink sink;
    sink.hold = true;
    sink.answer = svc::CallStatus::Refused;
    sink.reason = "the store refused it";
    MarketHarness h(cyb_record(), &sink);
    std::string guid;
    int64_t purse = 0;
    repl::GuidValue seller;
    if (!list_destroy_and_log_out(h, sink, 1, guid, purse, seller)) {
        sink.release();
        return;
    }
    const size_t bags_before = sink.inventories.size();
    sink.release();
    CHECK(step_world_until(h, [&] { return sink.inventories.size() > bags_before; }, 3000), "the bag the seller left with is written once the save is refused");
    if (!sink.inventories.empty()) {
        const std::vector<svc::InventoryItem>& last = sink.inventories.back();
        const svc::InventoryItem* rest = saved_item(last, guid);
        CHECK(rest && rest->quantity == 4, "the final bag has the two listed units back with the two that were left");
        int64_t units = 0;
        size_t with_guid = 0;
        for (const svc::InventoryItem& it : last) {
            if (it.item_record_id == fi::kPotion) units += it.quantity;
            with_guid += it.instance_guid == guid ? 1 : 0;
        }
        CHECK(units == 4 && with_guid == 1, "four units in all, and the guid is held once");
    }
    CHECK(!sink.bag_wallets.empty() && !sink.bag_wallets.back().empty() && sink.bag_wallets.back()[0].amount == purse, "the final purse has the fee back");
    CHECK(h.fx.world->market_listings()->count(seller) == 0, "the world holds no listing");
}

/// A logout while the save is being tried again: the world finishes it, and the listing is made.
static void test_marketplace_logout_during_an_unclear_save_finishes_it() {
    std::printf("the Marketplace: a seller who logs out while an unclear save is tried again still has the listing\n");
    MarketSaveSink sink;
    sink.answers = {svc::CallStatus::Unavailable};
    sink.hold = true;
    sink.hold_attempt = 2;   // the second try waits: the logout comes while the save is undecided
    MarketHarness h(cyb_record(), &sink);
    std::string guid;
    int64_t purse = 0;
    repl::GuidValue seller;
    if (!list_destroy_and_log_out(h, sink, 2, guid, purse, seller)) {
        sink.release();
        return;
    }
    const size_t bags_before = sink.inventories.size();
    CHECK(h.fx.world->market_listings()->count(seller) == 0, "the listing is not in the world while the save is undecided");
    sink.release();
    CHECK(step_world_until(h, [&] { return h.fx.world->market_listings()->count(seller) == 1; }, 3000), "the world takes the answer: the listing is in the market");
    CHECK(step_world_until(h, [&] { return sink.inventories.size() > bags_before; }, 3000), "the final bag is written after the listing");
    CHECK(sink.market_saves.size() == 2, "the save was asked for twice, no more");
    CHECK(!sink.bag_wallets.empty() && !sink.bag_wallets.back().empty() && sink.bag_wallets.back()[0].amount == purse - 1000, "the final purse has paid the fee once");
    check_listing_shown_to_a_new_session(h, seller);
}

/// The listings are read again, off the network thread, when the first read failed: until they are read a registration is refused and no storefront answers.
static void test_marketplace_failed_read_is_tried_again() {
    std::printf("the Marketplace: a world whose first read of the listings failed reads them again\n");
    MarketSaveSink sink;
    const svc::Character who = cyb_record();
    const std::optional<repl::GuidValue> seller = items::guid_of(who.id);
    CHECK(seller.has_value(), "the seller's id reads as a guid");
    if (!seller) return;
    const int64_t unix_now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    svc::MarketListing row;
    row.id = guid_n(0x9101);
    row.seller_id = who.id;
    row.seller_name = "cyb";
    row.node = kNode;
    row.item = make_item(Bag, 1, fi::kPotion, 921, 5);
    row.item.quantity = 2;
    row.price_per_unit = kPrice;
    row.status = 0;
    row.issue_date_ms = unix_now - 3'600'000;
    row.expiry_date_ms = row.issue_date_ms + 259'200'000;
    sink.saved_listings.listings.push_back(row);
    svc::MarketListingsResult down;
    down.status = svc::CallStatus::Unavailable;
    down.reason = "the character service is not up yet";
    sink.reads.push_back(down);
    MarketHarness h(who, &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    CHECK(!h.fx.world->market_listings()->loaded(), "the listings are not loaded: the first read failed");
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    size_t from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
    const std::vector<repl::RpcCall> refused_calls = pc_calls_since(h, from);
    const repl::RpcCall* refused = registration_answer(refused_calls);
    CHECK(refused && enum_of(*refused, "Response").value_or(0) == 1 && h.s->wallet().copper() == purse, "a registration before the listings are read is refused with ERROR_GENERIC (1) and takes nothing");
    from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    const std::vector<repl::RpcCall> none_calls = pc_calls_since(h, from);
    const repl::RpcCall* none = call_named(none_calls, "ClientReceiveMarketplaceHeaders");
    CHECK(none == nullptr || array_of(*none, "Headers") == nullptr, "no storefront answers before the listings are read");
    h.run(milliseconds(30'000));   // the world runs half a minute before the world's step takes the rows the second read got
    double loaded_at = 0;          // the world's time at the step that took the rows, and Unix time then
    int64_t unix_at_load = 0;
    CHECK(step_world_until(h, [&] {
              if (!h.fx.world->market_listings()->loaded()) return false;
              loaded_at = world_time_at(h.t);
              unix_at_load = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
              return true;
          }, 3000),
          "the listings are loaded after the read was tried again");
    CHECK(loaded_at > 30.0, "the world's time had passed half a minute when the rows were taken");
    CHECK(sink.read_calls.load() == 2, "the listings were read twice");
    CHECK(h.fx.world->market_listings()->size() == 1 && h.fx.world->market_listings()->count(*seller) == 1, "the saved listing is in the world");
    from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    const std::vector<repl::RpcCall> header_calls = pc_calls_since(h, from);
    const repl::RpcCall* headers = call_named(header_calls, "ClientReceiveMarketplaceHeaders");
    const repl::ArrayValue* fronts = headers ? array_of(*headers, "Headers") : nullptr;
    CHECK(fronts && fronts->elements.size() == 1, "the storefront shows the saved listing");
    // the dates are the saved Unix times on the client's clock as it reads at the step that took the rows, not as it read when the world started
    const double expected_issue = bootstrap::effect_clock(loaded_at) + (static_cast<double>(row.issue_date_ms) - static_cast<double>(unix_at_load)) / 1000.0;
    const double expected_expiry = bootstrap::effect_clock(loaded_at) + (static_cast<double>(row.expiry_date_ms) - static_cast<double>(unix_at_load)) / 1000.0;
    if (fronts && fronts->elements.size() == 1) {
        const auto* date = repl::rpcf::as<double>(member_of(*headers, "Headers", fronts->elements[0], "Headers.IssueDate"));
        CHECK(date && std::fabs(*date - expected_issue) < 0.2, "the storefront's issue date is the saved one on the client's clock");
    }
    from = h.got.size();
    h.client_request(data_request(market, *seller, true), h.pc_ch);
    const std::vector<repl::RpcCall> data_calls = pc_calls_since(h, from);
    const repl::RpcCall* data = call_named(data_calls, "ClientReceiveMarketplaceData");
    const repl::ArrayValue* sold = data ? array_of(*data, "MarketplaceItemData") : nullptr;
    CHECK(sold && sold->elements.size() == 1, "the Sell tab shows the saved listing");
    if (sold && sold->elements.size() == 1) {
        const auto* issue = repl::rpcf::as<double>(member_of(*data, "MarketplaceItemData", sold->elements[0], "MarketplaceItemData.IssueDate"));
        const auto* expiry = repl::rpcf::as<double>(member_of(*data, "MarketplaceItemData", sold->elements[0], "MarketplaceItemData.ExpirationDate"));
        CHECK(issue && std::fabs(*issue - expected_issue) < 0.2, "the Sell tab's issue date is the saved one on the client's clock");
        CHECK(expiry && std::fabs(*expiry - expected_expiry) < 0.2, "the Sell tab's expiry date is the saved one on the client's clock");
    }
    from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*h.s->inventory().at(Bag, 1)), 2, kPrice), h.pc_ch);
    const std::vector<repl::RpcCall> made_calls = pc_calls_since(h, from);
    const repl::RpcCall* made = registration_answer(made_calls);
    CHECK(made && enum_of(*made, "Response").value_or(1) == 0, "a registration is made once the listings are read");
}

/// A call on the PlayerController's social component (the mail's claims), as the client sends it.
static BunchWriter social_rpc(const Harness& h, const char* name, const repl::RpcParams& params) {
    BunchWriter out;
    const schema::ClassSchema& cls = class_named("UIntrepidSocialComponent");
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) return out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.object = h.s->bootstrap().setup().guids.social;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// ServerClaimMailItem for the first item of the mail `mail_id`.
static BunchWriter claim_request(const Harness& h, const std::string& mail_id) {
    const schema::RpcSchema* rpc = class_named("UIntrepidSocialComponent").rpc("ServerClaimMailItem");
    const auto mail = items::guid_of(mail_id);
    repl::RpcParams params;
    if (!rpc || !mail || !put_guid(*rpc, params, "MailboxGuid", repl::GuidValue{5100u, 0u, 0u, 0u}) || !put_guid(*rpc, params, "MailGuid", *mail)) return BunchWriter();
    return social_rpc(h, "ServerClaimMailItem", params);
}

/// A mail from the post with one potion.
static svc::Mail potion_mail(const std::string& id, const std::string& item_guid) {
    svc::Mail mail;
    mail.id = id;
    mail.sender = "Lionhold Post";
    mail.subject = "A potion";
    mail.body = "The post left this for you.";
    mail.status = 0;
    mail.issued_at_ms = 1'700'000'000'000ll;
    mail.expires_at_ms = 1'700'604'800'000ll;
    svc::MailItem attachment;
    attachment.item = make_item(Bag, 0, fi::kPotion, 0, 1);
    attachment.item.instance_guid = item_guid;
    attachment.item.created_at_ms = 1'700'000'000'000ll;
    attachment.claimed = false;
    mail.items.push_back(attachment);
    return mail;
}

/// A mail claim writes the seller's bags on the mail worker: a listing is refused while one is in flight, and made, with the claimed item in its bags, after it.
static void test_marketplace_listing_waits_for_a_mail_claim() {
    std::printf("the Marketplace: a listing is refused while a mail claim writes the bags\n");
    MarketSaveSink sink;
    const std::string mail_id = guid_n(9001), attachment = guid_n(9101);
    sink.mails.push_back(potion_mail(mail_id, attachment));
    sink.hold_claim = true;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    h.client_request(actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerFetchMail", {}), h.pc_ch);
    CHECK(h.s->mail_loaded(), "the mails are loaded");
    h.client_sends({h.client.actor(h.pc_ch, claim_request(h, mail_id), true)});
    CHECK(sink.wait_claim_started(3000), "the claim's bag save is in flight");
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    const svc::InventoryItem* claimed = h.s->inventory().at(Bag, 2);
    CHECK(held && held->quantity == 5 && claimed && claimed->instance_guid == attachment && potion_units(h.s->inventory()) == 6, "the claimed potion is in the next slot: six units in all");
    if (!held) {
        sink.release_claim();
        return;
    }
    const std::string guid = held->instance_guid;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    const size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    for (int i = 0; i < 5; ++i) h.step();
    const std::vector<repl::RpcCall> refused_calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(refused_calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 1, "the listing is refused with ERROR_GENERIC (1) while the claim is in flight");
    const svc::InventoryItem* still = h.s->inventory().at(Bag, 1);
    CHECK(still && still->quantity == 5 && potion_units(h.s->inventory()) == 6 && h.s->wallet().copper() == purse, "the refusal takes neither units nor money");
    {
        std::lock_guard<std::mutex> lock(sink.mu);
        CHECK(sink.market_saves.empty(), "no listing save was asked for");
    }
    sink.release_claim();
    h.settle();
    const svc::InventoryItem* after_claim = h.s->inventory().at(Bag, 1);
    CHECK(after_claim && after_claim->instance_guid == guid, "the claim is done and the stack is where it was");
    if (!after_claim) return;
    h.client_request(sell_request(market, items::to_instance(*after_claim), 2, kPrice), h.pc_ch);
    std::lock_guard<std::mutex> lock(sink.mu);
    CHECK(sink.market_saves.size() == 1, "the listing is saved once the claim is done");
    if (sink.market_saves.size() == 1) {
        int64_t units = 0;
        for (const svc::InventoryItem& it : sink.save_bags[0]) units += it.item_record_id == fi::kPotion ? it.quantity : 0;
        CHECK(units == 4, "its bags hold the claimed potion: three of the stack and the claimed one");
    }
}

/// A mail claim is turned down while a listing's save writes the bags, and goes through after it.
static void test_marketplace_mail_claim_waits_for_a_listing() {
    std::printf("the Marketplace: a mail claim is turned down while a listing's save writes the bags\n");
    MarketSaveSink sink;
    sink.hold = true;
    const std::string mail_id = guid_n(9002), attachment = guid_n(9102);
    sink.mails.push_back(potion_mail(mail_id, attachment));
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    h.client_request(actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerFetchMail", {}), h.pc_ch);
    CHECK(h.s->mail_loaded(), "the mails are loaded");
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) {
        sink.release();
        return;
    }
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_sends({h.client.actor(h.pc_ch, sell_request(market, items::to_instance(*held), 2, kPrice), true)});
    CHECK(sink.wait_started(3000), "the listing's save is in flight");
    h.client_sends({h.client.actor(h.pc_ch, claim_request(h, mail_id), true)});
    for (int i = 0; i < 5; ++i) h.step();
    CHECK(sink.claim_saves.load() == 0, "no mail claim wrote the bags while the listing's save is in flight");
    const svc::InventoryItem* rest = h.s->inventory().at(Bag, 1);
    CHECK(rest && rest->quantity == 3 && potion_units(h.s->inventory()) == 3 && !h.s->inventory().at(Bag, 2), "the claim placed nothing in the bag");
    sink.release();
    h.settle();
    h.client_request(claim_request(h, mail_id), h.pc_ch);
    CHECK(sink.claim_saves.load() == 1, "the claim goes through once the listing is saved");
    const svc::InventoryItem* after = h.s->inventory().at(Bag, 2);
    CHECK(after && after->instance_guid == attachment && potion_units(h.s->inventory()) == 4, "the claimed potion is in the bag");
}

// ---- the reach to the Manager, the answers of a second login, and the answers against the recorded ones --------------------------------------------------------------

/// A client move on the ground: the time stamp, the place and whether the player holds the walk key.
static repl::NetworkMove walk_move(float ts, double x, double y, double z, bool moving) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, moving ? 100 : 0, 0, 0);
    m.location = repl::quantize_vector(100, x, y, z);
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = moving ? 1 : 0;
    return m;
}

static BunchWriter walk_bunch(const repl::NetworkMove& mv) {
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    BunchWriter out;
    repl::MovePacked p;
    p.new_move = mv;
    BunchWriter params;
    BunchWriter field;
    if (!f || !repl::write_move_packed(params, p) || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// The client walks along x at `cm_per_s`: a move every 100 ms, each answered by the server's tick.
struct Walker {
    MarketHarness& h;
    float          ts = 10.0f;
    double         x, y, z;
    explicit Walker(MarketHarness& harness) : h(harness), x(harness.s->movement().good().x), y(harness.s->movement().good().y), z(harness.s->movement().good().z) {}
    void move(bool moving) {
        ts += 0.1f;
        h.client_sends({h.client.actor(h.pawn_ch, walk_bunch(walk_move(ts, x, y, z, moving)), /*reliable=*/false)});
        h.step(milliseconds(100));
    }
    void to(double target, double cm_per_s) {
        while (std::fabs(target - x) > 1e-6) {
            const double stride = cm_per_s * 0.1;
            x += std::fabs(target - x) <= stride ? target - x : (target > x ? stride : -stride);
            move(true);
        }
        move(false);
    }
};

/// The reach to the Manager (1000 units) is measured from where the player stands now, not from the place the last periodic save recorded: a player whose last save is far away
/// walks to the Manager, and his calls are answered.
static void test_marketplace_range_follows_the_live_position() {
    std::printf("the Marketplace: the reach to the Manager is measured from where the player stands\n");
    MarketSaveSink sink;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    Walker w(h);
    const double home = w.x;
    w.to(home - 2500, 300);   // 33 m from the Manager, who stands 8 m from the login spot
    CHECK(std::fabs(h.s->movement().good().x - (home - 2500)) < 1.0, "the player walked 25 m away");
    const size_t saves = h.s->positions_handed_over();
    for (int i = 0; i < 700 && h.s->positions_handed_over() == saves; ++i) w.move(false);
    CHECK(h.s->positions_handed_over() > saves, "the periodic save recorded the far place");
    const size_t saves_at_the_far_place = h.s->positions_handed_over();
    w.to(home, 300);   // back at the Manager's 8 m, before the next save
    CHECK(std::fabs(h.s->movement().good().x - home) < 1.0, "the player walked back to the Manager");
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const repl::GuidValue seller = character_guid(h);
    size_t from = h.got.size();
    h.open_market(h.manager);
    CHECK(count_calls(pc_calls_since(h, from), "ClientOpenMarketplaceUI") == 1, "the Manager opens his window");
    from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    CHECK(count_calls(pc_calls_since(h, from), "ClientReceiveMarketplaceHeaders") == 1, "the storefront request is answered");
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    from = h.got.size();
    h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
    const std::vector<repl::RpcCall> listed = pc_calls_since(h, from);
    const repl::RpcCall* answer = registration_answer(listed);
    CHECK(answer && enum_of(*answer, "Response").value_or(1) == 0, "the listing is made");
    from = h.got.size();
    h.client_request(data_request(market, seller, true), h.pc_ch);
    const std::vector<repl::RpcCall> data_calls = pc_calls_since(h, from);
    const repl::RpcCall* data = call_named(data_calls, "ClientReceiveMarketplaceData");
    const repl::ArrayValue* sold = data ? array_of(*data, "MarketplaceItemData") : nullptr;
    CHECK(sold && sold->elements.size() == 1, "the Sell tab request is answered with the listing");
    CHECK(h.s->positions_handed_over() == saves_at_the_far_place, "no periodic save came between the walk and these calls: the last save still records the far place");
}

/// The same character logs out and in again on the same world: the storefront shows the seller, and the Sell tab's data, asked with the header's guid and with the
/// character's own guid, carries the listing.
static void test_marketplace_relog_shows_the_listing() {
    std::printf("the Marketplace: a seller who logs out and in again sees the listing in the storefront and the data\n");
    MarketSaveSink sink;
    MarketHarness h(cyb_record(), &sink);
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
    CHECK(held && held->quantity == 5, "the bag holds the stack of five");
    if (!held) return;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
    CHECK(h.fx.world->market_listings()->size() == 1, "the world holds the listing");
    h.s->finish("the client left");
    h.s.reset();

    MarketPeer peer(h, cyb_record());
    peer.play();
    size_t from = peer.got.size();
    peer.request(interact(market_component(h.fx.world->registry(), h.manager)), peer.pawn_ch);
    CHECK(count_calls(market_calls_since(peer.got, peer.pc_ch, from), "ClientOpenMarketplaceUI") == 1, "the second session opens the Manager's window");
    from = peer.got.size();
    peer.request(market_headers_request(market, 1843), peer.pc_ch);
    const std::vector<repl::RpcCall> header_calls = market_calls_since(peer.got, peer.pc_ch, from);
    const repl::RpcCall* headers = call_named(header_calls, "ClientReceiveMarketplaceHeaders");
    const repl::ArrayValue* fronts = headers ? array_of(*headers, "Headers") : nullptr;
    CHECK(fronts && fronts->elements.size() == 1, "the storefront shows one seller");
    if (!fronts || fronts->elements.size() != 1) return;
    const repl::ElementState& front = fronts->elements[0];
    const repl::GuidValue header_guid{member_word(*headers, "Headers", front, "Headers.PlayerGuid.A").value_or(0), member_word(*headers, "Headers", front, "Headers.PlayerGuid.B").value_or(0),
                                      member_word(*headers, "Headers", front, "Headers.PlayerGuid.C").value_or(0), member_word(*headers, "Headers", front, "Headers.PlayerGuid.D").value_or(0)};
    const auto& own = peer.s->bootstrap().setup().character_guid;
    const repl::GuidValue own_guid{own[0], own[1], own[2], own[3]};
    CHECK(header_guid == own_guid, "the storefront names the seller by the guid the character has in the second session");
    for (const repl::GuidValue& asked : {header_guid, own_guid}) {
        from = peer.got.size();
        peer.request(data_request(market, asked, true), peer.pc_ch);
        const std::vector<repl::RpcCall> data_calls = market_calls_since(peer.got, peer.pc_ch, from);
        const repl::RpcCall* data = call_named(data_calls, "ClientReceiveMarketplaceData");
        const repl::ArrayValue* sold = data ? array_of(*data, "MarketplaceItemData") : nullptr;
        CHECK(data && word_of(*data, "PlayerGuid.A") == asked.a && word_of(*data, "PlayerGuid.D") == asked.d, "the data answer names the seller asked about");
        CHECK(sold && sold->elements.size() == 1, "the data answer carries the listing");
        if (sold && sold->elements.size() == 1) {
            const auto* item = repl::rpcf::as<repl::ItemInstance>(member_of(*data, "MarketplaceItemData", sold->elements[0], "MarketplaceItemData.Item"));
            CHECK(item && item->quantity == 2 && item->record_id == static_cast<uint64_t>(fi::kPotion), "the listing holds the two potions");
        }
    }
}

/// The calls of the window leave a line each: the answers say what they carried, and a call that is ignored says why.
static void test_marketplace_answers_are_logged() {
    std::printf("the Marketplace: each storefront and data answer, and each ignored call, leaves a line\n");
    MarketSaveSink sink;
    std::ostringstream log;
    auto logger = std::make_shared<spdlog::logger>("market_log_test", std::make_shared<spdlog::sinks::ostream_sink_mt>(log, true));
    logger->set_level(spdlog::level::info);
    const std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
    spdlog::set_default_logger(logger);
    repl::GuidValue seller;
    {
        MarketHarness h(cyb_record(), &sink);
        h.play();
        h.run(milliseconds(1500));
        h.open_market(h.manager);
        const svc::InventoryItem* held = h.s->inventory().at(Bag, 1);
        const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
        seller = character_guid(h);
        h.client_request(market_headers_request(market, 1843), h.pc_ch);
        if (held) h.client_request(sell_request(market, items::to_instance(*held), 2, kPrice), h.pc_ch);
        h.client_request(market_headers_request(market, 1843), h.pc_ch);
        h.client_request(data_request(market, seller, true), h.pc_ch);
        h.s->gm_teleport(h.conn, kCx - 4000, kCy, bootstrap::kDefaultSpawn.z, h.t, "the test");   // 48 m from the Manager
        h.client_request(market_headers_request(market, 1843), h.pc_ch);
        h.client_request(data_request(market, seller, true), h.pc_ch);
    }
    spdlog::set_default_logger(previous);
    spdlog::drop("market_log_test");
    std::istringstream lines(log.str());
    std::string line;
    std::vector<std::string> pages, answers, ignored;
    while (std::getline(lines, line)) {
        if (line.find("[market]") == std::string::npos) continue;
        if (line.find("storefront page") != std::string::npos) pages.push_back(line);
        if (line.find("the data of seller") != std::string::npos) answers.push_back(line);
        if (line.find("is ignored") != std::string::npos) ignored.push_back(line);
    }
    CHECK(pages.size() == 2, "one line for each storefront page that was sent");
    if (pages.size() == 2) {
        CHECK(pages[0].find(std::to_string(kNode)) != std::string::npos && pages[0].find("0 storefront(s)") != std::string::npos, "the first page names the node and holds no storefront");
        CHECK(pages[1].find("1 storefront(s)") != std::string::npos, "the second page holds the seller's storefront");
    }
    CHECK(answers.size() == 1, "one line for the data that was sent");
    if (answers.size() == 1) {
        char guid[64];
        std::snprintf(guid, sizeof guid, "%u/%u/%u/%u", seller.a, seller.b, seller.c, seller.d);
        CHECK(answers[0].find(guid) != std::string::npos && answers[0].find(std::to_string(kNode)) != std::string::npos && answers[0].find("1 listing(s)") != std::string::npos,
              "the line names the node, the guid that was asked and the listing sent");
    }
    CHECK(ignored.size() == 2, "one line for each call that was ignored");
    for (const std::string& each : ignored) CHECK(each.find(" m away") != std::string::npos, "the line says how far the Manager is");
}

/// The retail answers the emulator reads in its tests, as one bunch of the PlayerController's channel each: a seller's data with one listing, and a storefront page of two
/// sellers (the answer to a search for "ruby").
static const char* kRecordedDataBunch = "be8888b30f1251c3169163296358a7076415fcbda66b00409e955d09b4180000a2862d22c752c6b04e0fc82af87b4d57020000001d5a99599a5b181b00320b8b664d93dc16f77c142354d826d17f0074d59df2142a01010000000000000000b02000000080fb1351b87d2573820b6012defb28400c403ad70249306f0400000000000000000000000000000000f727a270fb4ae60417c024bcf75180188074ae059260de08000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000c003000040539a5cd9dd9b5d991b88549adb194000000000004059d703790a5540ee0ceacb4a9501fa00000000000000d3aa9eae7b725a10d3aa9eae1f925a1000";
static const char* kRecordedHeadersBunch = "56538c134a31cfcaae045a0c00a000409e955d09b41800c0e3ac3187c5d372c1404114ee57c052840200008051195bd8599d1b5918c00500008051195bd8599d1b59d8c91cc814dd9b5c9999dc9b1b1d00ffba3475f53a5a903c2bbb1268310080e802227170a0bd08d050d6d1cd65bd0c06000000a43436b6bb3436b6b4b036800c000000a43436b6bb3436b6b4b0b693399029ba37b93233b937373a008759a1a1fe19b52016000000c8d589e50108";

/// The first call of the recorded bunch (nullopt when it does not read).
static std::optional<repl::RpcCall> recorded_pc_call(const char* hex, size_t bits) {
    const std::vector<uint8_t> bytes = recorded_bytes(hex);
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, bits);
    const auto blocks = repl::read_content_blocks(w.data(), w.bit_pos(), 0);
    if (!blocks) return std::nullopt;
    for (const auto& b : *blocks) {
        if (!b.header.is_actor || b.header.has_rep_layout) continue;
        size_t p = b.payload_pos;
        auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), w.data(), b.payload_pos + b.payload_bits, p);
        if (calls && !calls->empty()) return std::move(calls->front());
    }
    return std::nullopt;
}

/// The data answer the emulator builds from a recorded listing is the recorded answer: every field of the element and the seller's guid, the same values at the same places.
static void test_marketplace_data_answer_is_the_recorded_one() {
    std::printf("the Marketplace: the data answer built from a recorded listing is the recorded answer\n");
    const std::optional<repl::RpcCall> recorded = recorded_pc_call(kRecordedDataBunch, 2217);
    CHECK(recorded && recorded->decoded && recorded->rpc && std::string(recorded->rpc->name) == "ClientReceiveMarketplaceData", "the recorded data answer reads");
    if (!recorded || !recorded->decoded || !recorded->rpc) return;
    const repl::ArrayValue* list = array_of(*recorded, "MarketplaceItemData");
    CHECK(list && list->elements.size() == 1, "it holds one listing");
    if (!list || list->elements.size() != 1) return;
    const repl::ElementState& e = list->elements[0];
    const char* array = "MarketplaceItemData";
    repl::MarketListing l;
    l.node = member_number(*recorded, array, e, "MarketplaceItemData.NodeGuid").value_or(0);
    l.seller = repl::GuidValue{member_word(*recorded, array, e, "MarketplaceItemData.PlayerGuid.A").value_or(0), member_word(*recorded, array, e, "MarketplaceItemData.PlayerGuid.B").value_or(0),
                               member_word(*recorded, array, e, "MarketplaceItemData.PlayerGuid.C").value_or(0), member_word(*recorded, array, e, "MarketplaceItemData.PlayerGuid.D").value_or(0)};
    l.seller_name = member_text(*recorded, array, e, "MarketplaceItemData.PlayerName");
    l.guid = repl::GuidValue{member_word(*recorded, array, e, "MarketplaceItemData.MarketplaceItemGuid.A").value_or(0), member_word(*recorded, array, e, "MarketplaceItemData.MarketplaceItemGuid.B").value_or(0),
                             member_word(*recorded, array, e, "MarketplaceItemData.MarketplaceItemGuid.C").value_or(0), member_word(*recorded, array, e, "MarketplaceItemData.MarketplaceItemGuid.D").value_or(0)};
    if (const auto* item = repl::rpcf::as<repl::ItemInstance>(member_of(*recorded, array, e, "MarketplaceItemData.Item"))) l.item = *item;
    l.item_name = member_text(*recorded, array, e, "MarketplaceItemData.ItemName");
    if (const auto* price = repl::rpcf::as<repl::CurrencyContainer>(member_of(*recorded, array, e, "MarketplaceItemData.PricePerUnit"))) l.price = *price;
    if (const auto* issue = repl::rpcf::as<double>(member_of(*recorded, array, e, "MarketplaceItemData.IssueDate"))) l.issue_date = *issue;
    if (const auto* expiry = repl::rpcf::as<double>(member_of(*recorded, array, e, "MarketplaceItemData.ExpirationDate"))) l.expiration_date = *expiry;
    l.status = static_cast<uint8_t>(member_enum(*recorded, array, e, "MarketplaceItemData.Status").value_or(99));
    const repl::GuidValue asked{word_of(*recorded, "PlayerGuid.A").value_or(0), word_of(*recorded, "PlayerGuid.B").value_or(0), word_of(*recorded, "PlayerGuid.C").value_or(0),
                                word_of(*recorded, "PlayerGuid.D").value_or(0)};
    CHECK(asked == l.seller, "the recorded answer names the seller whose listing it carries");
    const repl::MarketListing one[1] = {l};
    const std::optional<repl::RpcParams> built = repl::market_data_params(*recorded->rpc, asked, one);
    CHECK(built.has_value(), "the answer builds from the recorded listing");
    if (!built) return;
    CHECK(*built == recorded->params, "the built answer holds the recorded values at the recorded places: nothing more, nothing less");
    BunchWriter mine, theirs;
    bool failed = false;
    repl::write_rpc_params(*recorded->rpc, *built, mine, {}, &failed);
    repl::write_rpc_params(*recorded->rpc, recorded->params, theirs, {}, &failed);
    bool same = !failed && mine.bit_pos() == theirs.bit_pos();
    for (size_t i = 0; same && i < mine.bit_pos(); ++i) same = ((mine.data()[i / 8] >> (i % 8)) & 1) == ((theirs.data()[i / 8] >> (i % 8)) & 1);
    CHECK(same, "the built answer is the recorded one bit for bit");
}

/// The storefront page the emulator builds from the recorded storefronts holds the recorded storefronts, the recorded node and the recorded last-page flag.
static void test_marketplace_headers_answer_is_the_recorded_one() {
    std::printf("the Marketplace: the storefront page built from recorded storefronts is the recorded page\n");
    const std::optional<repl::RpcCall> recorded = recorded_pc_call(kRecordedHeadersBunch, 1404);
    CHECK(recorded && recorded->decoded && recorded->rpc && std::string(recorded->rpc->name) == "ClientReceiveMarketplaceHeaders", "the recorded storefront page reads");
    if (!recorded || !recorded->decoded || !recorded->rpc) return;
    const repl::ArrayValue* list = array_of(*recorded, "Headers");
    CHECK(list && list->elements.size() == 2, "it holds two storefronts");
    if (!list || list->elements.size() != 2) return;
    std::vector<repl::MarketHeader> headers;
    for (const repl::ElementState& e : list->elements) {
        repl::MarketHeader h;
        h.node = member_number(*recorded, "Headers", e, "Headers.NodeGuid").value_or(0);
        h.seller = repl::GuidValue{member_word(*recorded, "Headers", e, "Headers.PlayerGuid.A").value_or(0), member_word(*recorded, "Headers", e, "Headers.PlayerGuid.B").value_or(0),
                                   member_word(*recorded, "Headers", e, "Headers.PlayerGuid.C").value_or(0), member_word(*recorded, "Headers", e, "Headers.PlayerGuid.D").value_or(0)};
        h.seller_name = member_text(*recorded, "Headers", e, "Headers.PlayerName");
        h.description = member_text(*recorded, "Headers", e, "Headers.Description");
        if (const auto* issue = repl::rpcf::as<double>(member_of(*recorded, "Headers", e, "Headers.IssueDate"))) h.issue_date = *issue;
        if (const auto* city = repl::rpcf::as<bool>(member_of(*recorded, "Headers", e, "Headers.bIsCityNodeHeader"))) h.city_node = *city;
        headers.push_back(h);
    }
    const int64_t node = number_of(*recorded, "NodeId").value_or(0);
    const auto* complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*recorded->rpc, recorded->params, "bComplete"));
    const std::optional<repl::RpcParams> built = repl::market_headers_params(*recorded->rpc, node, headers, complete && *complete);
    CHECK(built.has_value(), "the page builds from the recorded storefronts");
    if (!built) return;
    const repl::Value* mine = repl::rpcf::value_of(*recorded->rpc, *built, "Headers");
    const repl::Value* theirs = repl::rpcf::value_of(*recorded->rpc, recorded->params, "Headers");
    CHECK(mine && theirs && *mine == *theirs, "the built storefronts are the recorded ones, field for field");
    CHECK(number_of(*recorded, "NodeId") == node && repl::rpcf::int_or(repl::rpcf::value_of(*recorded->rpc, *built, "NodeId"), 0) == node, "the page names the recorded node");
    const auto* built_complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*recorded->rpc, *built, "bComplete"));
    CHECK(complete && built_complete && *complete == *built_complete, "the page carries the recorded last-page flag");
}

AOC_REGISTER_TEST(test_marketplace_not_loaded_refuses);
AOC_REGISTER_TEST(test_marketplace_listing_reaches_the_store);
AOC_REGISTER_TEST(test_marketplace_refused_save_gives_back);
AOC_REGISTER_TEST(test_marketplace_bag_save_waits);
AOC_REGISTER_TEST(test_marketplace_partial_stack_new_guid);
AOC_REGISTER_TEST(test_marketplace_cap_counts_in_flight);
AOC_REGISTER_TEST(test_marketplace_restart_shows_saved_listings);
AOC_REGISTER_TEST(test_marketplace_sell_takes_the_units);
AOC_REGISTER_TEST(test_marketplace_listing_fee);
AOC_REGISTER_TEST(test_marketplace_sell_answer);
AOC_REGISTER_TEST(test_marketplace_sell_charges_the_fee);
AOC_REGISTER_TEST(test_marketplace_sell_refused_without_the_fee);
AOC_REGISTER_TEST(test_marketplace_delist_refunds_nothing);
AOC_REGISTER_TEST(test_marketplace_recorded_registration);
AOC_REGISTER_TEST(test_marketplace_sell_refusals);
AOC_REGISTER_TEST(test_marketplace_buy_tab_headers);
AOC_REGISTER_TEST(test_marketplace_recorded_headers);
AOC_REGISTER_TEST(test_marketplace_sell_tab_data);
AOC_REGISTER_TEST(test_marketplace_two_sellers);
AOC_REGISTER_TEST(test_marketplace_other_market);
AOC_REGISTER_TEST(test_marketplace_worn_and_quest);
AOC_REGISTER_TEST(test_marketplace_listing_cap);
AOC_REGISTER_TEST(test_marketplace_refund_follows_the_rest_that_moved);
AOC_REGISTER_TEST(test_marketplace_refund_keeps_a_unit_the_player_destroyed);
AOC_REGISTER_TEST(test_marketplace_refund_when_the_rest_is_gone);
AOC_REGISTER_TEST(test_marketplace_refund_of_a_whole_stack_to_a_free_slot);
AOC_REGISTER_TEST(test_marketplace_unclear_save_is_tried_again);
AOC_REGISTER_TEST(test_marketplace_unclear_then_refused_gives_back);
AOC_REGISTER_TEST(test_marketplace_undecided_save_is_logged_at_stop);
AOC_REGISTER_TEST(test_marketplace_logout_during_a_save_keeps_the_listing);
AOC_REGISTER_TEST(test_marketplace_logout_during_a_refused_save_gives_back);
AOC_REGISTER_TEST(test_marketplace_logout_during_an_unclear_save_finishes_it);
AOC_REGISTER_TEST(test_marketplace_failed_read_is_tried_again);
AOC_REGISTER_TEST(test_marketplace_listing_waits_for_a_mail_claim);
AOC_REGISTER_TEST(test_marketplace_mail_claim_waits_for_a_listing);
AOC_REGISTER_TEST(test_marketplace_range_follows_the_live_position);
AOC_REGISTER_TEST(test_marketplace_relog_shows_the_listing);
AOC_REGISTER_TEST(test_marketplace_answers_are_logged);
AOC_REGISTER_TEST(test_marketplace_data_answer_is_the_recorded_one);
AOC_REGISTER_TEST(test_marketplace_headers_answer_is_the_recorded_one);

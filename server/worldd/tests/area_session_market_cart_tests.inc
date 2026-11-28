// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_market_cart_tests.inc: a cart of several listings, and a money claim that is refused for a changed amount, through real sessions
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc, after the sales tests, whose
//  helpers it uses)
//
//    the cart     the Buy panel sends one purchase for each line of its cart in one frame and waits for an answer to every line: each line is bought, or answered with its real
//                 reason, one step of the session in flight at a time, in the order of the lines; the lines past the bound are answered 3; a session that ends drops the lines
//                 that never started
//    the money    a claim that credited the amount the world read is refused by the service when a sale was added to the mail meanwhile: the credit goes back, nothing is
//                 marked, the mail is read again and sent to the client, and the next claim pays everything once
// ============================================================================
#include "market/market.h"

#include <chrono>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace market_cart_test {

/// One line of a cart: the listing and the units.
struct Line {
    std::string id;
    int32_t     quantity = 1;
};

}  // namespace market_cart_test

using namespace market_cart_test;

/// The Buy panel's confirm: one ServerRequestMarketplaceItemPurchase for each line of the cart, all in one frame (the client's loop sends them without waiting for an answer).
static std::vector<fake::Bunch> cart_frame(MarketHarness& h, const std::vector<Line>& lines) {
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    std::vector<fake::Bunch> frame;
    for (const Line& line : lines) frame.push_back(h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(line.id), line.quantity), true));
    return frame;
}

/// The purchase answers among `calls`, in the order they were sent: the listing and the response (0 when the answer carries none).
static std::vector<std::pair<repl::GuidValue, uint64_t>> purchase_answers(const std::vector<repl::RpcCall>& calls) {
    std::vector<std::pair<repl::GuidValue, uint64_t>> out;
    for (const repl::RpcCall& c : calls) {
        if (c.rpc && std::string(c.rpc->name) == "ClientMarketplaceItemPurchaseResponse") out.push_back({answer_listing(c), enum_of(c, "Response").value_or(0)});
    }
    return out;
}

/// The calls of the PlayerController's social component (the mail's answers and details) among the chains `got[from..]`.
static std::vector<repl::RpcCall> social_calls_since(const Harness& h, size_t from) {
    std::vector<repl::RpcCall> out;
    const auto social = h.s->bootstrap().setup().guids.social;
    for (size_t i = from; i < h.got.size(); ++i) {
        const auto blocks = repl::read_content_blocks(h.got[i].content.data(), h.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& block : *blocks) {
            if (block.header.is_actor || block.header.has_rep_layout || !(block.header.object == social)) continue;
            size_t pos = block.payload_pos;
            auto calls = repl::read_rpc_fields(class_named("UIntrepidSocialComponent"), h.got[i].content.data(), block.payload_pos + block.payload_bits, pos);
            if (!calls) continue;
            for (repl::RpcCall& call : *calls) out.push_back(std::move(call));
        }
    }
    return out;
}

/// The money the last ClientGetMailData of the mail `id` among `calls` shows (-1: none was sent); `claimed` says whether it shows the money taken.
static int64_t detail_money(const std::vector<repl::RpcCall>& calls, const std::string& id, bool* claimed = nullptr) {
    int64_t found = -1;
    const auto guid = items::guid_of(id);
    for (const repl::RpcCall& c : calls) {
        if (!c.rpc || !c.decoded || std::string(c.rpc->name) != "ClientGetMailData") continue;
        const auto* data = repl::rpcf::as<repl::MailPersistenceData>(repl::rpcf::value_of(*c.rpc, c.params, "MailData"));
        if (!data || !guid || !(data->guid == *guid)) continue;
        int64_t sum = 0;
        for (const auto& entry : data->currency.entries) sum += entry.amount;
        found = sum;
        if (claimed) *claimed = data->currency_claimed;
    }
    return found;
}

// ---- the cart ---------------------------------------------------------------------------------------------------------------------------

/// A cart of two listings, then a cart of three: every line is bought and answered 0 in the order of the lines, the purse pays each line's price and tax, the bags hold every line,
/// and the session never has more than one step in flight.
static void test_market_a_cart_of_several_listings_buys_every_line() {
    std::printf("the Marketplace: a cart of several listings: every line is bought and answered\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string l1 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice), l2 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice),
                      l3 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    db.hold_buy = true;
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const int64_t purse = h.s->wallet().copper();
    const int64_t potions = potion_units(h.s->inventory());
    // the 2-line cart: line 1 is in flight (held in the database), line 2 waits in the session, and nothing else reaches the database
    size_t from = h.got.size();
    h.client_sends(cart_frame(h, {{l1, 2}, {l2, 1}}));
    CHECK(db.wait_started(3000), "the first line reached the database");
    for (int i = 0; i < 4; ++i) h.step();
    {
        std::lock_guard<std::mutex> lock(db.mu);
        CHECK(db.buy_calls == 1, "the database was asked about the first line only: one step of the session in flight at a time");
    }
    CHECK(h.s->market_pending() == 1, "the session has one step in flight");
    CHECK(purchase_answers(pc_calls_since(h, from)).empty(), "no line is answered before the database answers");
    db.release_all();
    h.settle();
    std::vector<std::pair<repl::GuidValue, uint64_t>> answers = purchase_answers(pc_calls_since(h, from));
    CHECK(answers.size() == 2 && answers[0].first == guid_of_hex(l1) && answers[0].second == 0 && answers[1].first == guid_of_hex(l2) && answers[1].second == 0,
          "both lines are answered 0, in the order of the lines");
    CHECK(h.s->wallet().copper() == purse - 300 - 150 && db.copper_of(record.id) == purse - 450, "the purse paid each line's price and tax: 200 and 100, then 100 and 50");
    CHECK(potion_units(h.s->inventory()) == potions + 3 && db.units_of(record.id, fi::kPotion) == potions + 3, "the bags hold the 3 units of the two lines, here and in the database");
    CHECK(db.buy_calls == 2 && db.buys.size() == 2 && db.buys[0].listing_id == l1 && db.buys[1].listing_id == l2, "the database was asked twice, in the order of the lines");
    CHECK(db.buys.size() == 2 && db.buys[1].bags.wallet != db.buys[0].bags.wallet, "the second step stores the purse after the first one's payment: a step is made from the bags the one before it left");
    CHECK(cached_quantity(h, l1) == 8 && cached_quantity(h, l2) == 9, "the window shows what is left of each listing");
    // the 3-line cart
    const int64_t purse2 = h.s->wallet().copper();
    const int64_t potions2 = potion_units(h.s->inventory());
    from = h.got.size();
    h.client_sends(cart_frame(h, {{l1, 1}, {l2, 2}, {l3, 3}}));
    h.settle();
    answers = purchase_answers(pc_calls_since(h, from));
    CHECK(answers.size() == 3 && answers[0].first == guid_of_hex(l1) && answers[1].first == guid_of_hex(l2) && answers[2].first == guid_of_hex(l3) &&
              answers[0].second == 0 && answers[1].second == 0 && answers[2].second == 0,
          "all three lines are answered 0, in the order of the lines");
    CHECK(h.s->wallet().copper() == purse2 - 150 - 300 - 450 && db.copper_of(record.id) == purse2 - 900, "the purse paid 150, 300 and 450: each line's price plus the tax");
    CHECK(potion_units(h.s->inventory()) == potions2 + 6 && db.units_of(record.id, fi::kPotion) == potions2 + 6, "the bags hold the 6 units of the three lines");
    CHECK(cached_quantity(h, l1) == 7 && cached_quantity(h, l2) == 7 && cached_quantity(h, l3) == 7, "the window shows 7 units of each listing");
    const std::vector<svc::Mail> sales = db.sale_mails(kSeller);
    CHECK(sales.size() == 3 && sales[0].money + sales[1].money + sales[2].money == 900,
          "the seller has one mail for each listing, and the mails hold the price of every unit sold: 9 units at 100 copper");
    CHECK(db.collected_of(kNode) == 150 + 300, "the node's total is the tax of all five sales: 100 + 50, then 50 + 100 + 150");
    CHECK(!h.s->failed(), "the session is fine");
}

/// A line whose listing is gone by the time it starts is answered with the real reason (5), and nothing is paid for it; a line naming a listing the world never held is answered 5 too.
static void test_market_a_cart_line_that_is_gone_when_it_starts() {
    std::printf("the Marketplace: a cart line whose listing is gone by the time it starts is answered 5\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string l1 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice), l2 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice),
                      l3 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    db.hold_buy = true;
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const int64_t purse = h.s->wallet().copper();
    const int64_t potions = potion_units(h.s->inventory());
    const size_t from = h.got.size();
    // line 1 is in flight; the seller takes line 2 off meanwhile (in another world); line 3 is bought; line 4 is a listing nobody holds
    h.client_sends(cart_frame(h, {{l1, 1}, {l2, 1}, {l3, 1}, {std::string(32, 'e'), 1}}));
    CHECK(db.wait_started(3000), "the first line reached the database");
    {
        std::lock_guard<std::mutex> lock(db.mu);
        db.find_row(l2)->status = market_db::kRemoved;
    }
    db.release_all();
    h.settle();
    const std::vector<std::pair<repl::GuidValue, uint64_t>> answers = purchase_answers(pc_calls_since(h, from));
    CHECK(answers.size() == 4, "every line is answered");
    if (answers.size() == 4) {
        CHECK(answers[0].first == guid_of_hex(l1) && answers[0].second == 0, "line 1 is answered 0");
        CHECK(answers[1].first == guid_of_hex(l2) && answers[1].second == 5, "line 2, gone when it started, is answered 5: no longer available");
        CHECK(answers[2].first == guid_of_hex(l3) && answers[2].second == 0, "line 3 is answered 0: a line that was refused does not stop the ones behind it");
        CHECK(answers[3].second == 5, "line 4, a listing nobody holds, is answered 5");
    }
    CHECK(h.s->wallet().copper() == purse - 150 - 150 && db.copper_of(record.id) == purse - 300, "the purse paid for lines 1 and 3 only: nothing was paid for line 2");
    CHECK(potion_units(h.s->inventory()) == potions + 2 && db.units_of(record.id, fi::kPotion) == potions + 2, "the bags hold the units of lines 1 and 3 only");
    CHECK(db.quantity_of(l2) == 10 && db.sale_mails(kSeller).size() == 2, "line 2's listing was not sold and the seller is mailed twice");
    CHECK(db.buy_calls == 3, "the database was asked for lines 1, 2 and 3; line 4 never reached it");
}

/// A cart longer than the session's queue: the lines past the bound are answered 3 at once, the others are bought.
static void test_market_a_cart_over_the_bound_answers_the_extra_lines_3() {
    std::printf("the Marketplace: the lines of a cart past the bound are answered 3\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const size_t lines = ::aoc::worldd::market::kMaxQueuedSteps + 2;   // one in flight, the queue full, and one more
    std::vector<Line> cart;
    for (size_t i = 0; i < lines; ++i) cart.push_back({db.seed_listing(i % 2 == 0 ? kSeller : kSecondId, i % 2 == 0 ? "Brannoc" : "Cathal", kNode, fi::kPotion, 10, kUnitPrice), 1});
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const int64_t purse = h.s->wallet().copper();
    const int64_t potions = potion_units(h.s->inventory());
    const size_t from = h.got.size();
    h.client_sends(cart_frame(h, cart));
    h.settle();
    const std::vector<std::pair<repl::GuidValue, uint64_t>> answers = purchase_answers(pc_calls_since(h, from));
    CHECK(answers.size() == lines, "every line is answered: none is dropped without an answer");
    size_t done = 0, busy = 0;
    for (const auto& a : answers) {
        if (a.second == 0) ++done;
        if (a.second == 3) ++busy;
    }
    CHECK(done == lines - 1 && busy == 1, "all the lines but the last one are bought and the line past the bound is answered 3");
    bool last_is_busy = false;
    for (const auto& a : answers) {
        if (a.first == guid_of_hex(cart.back().id)) last_is_busy = a.second == 3;
    }
    CHECK(last_is_busy, "the line past the bound is the last one of the cart");
    CHECK(h.s->wallet().copper() == purse - 150 * static_cast<int64_t>(lines - 1) && potion_units(h.s->inventory()) == potions + static_cast<int64_t>(lines - 1),
          "the purse paid for the lines that were bought and the bags hold their units");
    CHECK(db.buy_calls == static_cast<int>(lines - 1) && db.quantity_of(cart.back().id) == 10, "the line past the bound never reached the database");
}

/// A session that ends with lines still queued drops them: nothing is paid for them, and a log-in right after runs none of them (the one in flight is finished as before).
static void test_market_a_session_that_ends_drops_the_queued_lines() {
    std::printf("the Marketplace: the lines of a cart that never started are dropped when the session ends\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string l1 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice), l2 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice),
                      l3 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    db.hold_buy = true;
    MarketHarness h(record, &db);
    ready_to_trade(h);
    h.client_sends(cart_frame(h, {{l1, 1}, {l2, 1}, {l3, 1}}));
    CHECK(db.wait_started(3000), "the first line reached the database");
    h.s->finish("the client left");
    h.s.reset();
    CHECK(h.fx.world->character_writes_pending(record.id), "a log-in of the buyer waits: the first line has no answer yet");
    db.release_all();
    CHECK(tick_world_until(h, [&] { return !h.fx.world->character_writes_pending(record.id); }, 5000), "the world finished the line that was in flight and wrote the bags");
    CHECK(db.units_of(record.id, fi::kPotion) == 5 + 1 && db.copper_of(record.id) == kStartPurse - 150, "the bags hold the one unit and the purse paid its 100 and 50 tax: once");
    CHECK(db.quantity_of(l1) == 9 && db.quantity_of(l2) == 10 && db.quantity_of(l3) == 10, "the lines that never started were not bought");
    // the log-in right after: it runs none of the dropped lines, and none runs twice
    MarketPeer again(h, db.stored_character(record));
    again.play();
    for (int i = 0; i < 200; ++i) again.step();
    CHECK(potion_units(again.s->inventory()) == 5 + 1 && again.s->wallet().copper() == kStartPurse - 150, "the relog reads the bags the one line made");
    CHECK(db.buy_calls == 1 && db.quantity_of(l1) == 9 && db.quantity_of(l2) == 10 && db.quantity_of(l3) == 10 && db.sale_mails(kSeller).size() == 1 && db.collected_of(kNode) == 50,
          "no line ran twice and none that was dropped ran: one purchase, one mail, one tax");
    again.s->finish("the client left");
    again.s.reset();
}

/// A purchase of a listing that this session already has in flight or waiting is answered 3 at once (a double submit), and a removal of a listing the session has waiting is too.
static void test_market_a_listing_already_in_the_session_is_answered_3() {
    std::printf("the Marketplace: the same listing twice in the session is answered 3\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string l1 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice), l2 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    db.hold_buy = true;
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const int64_t purse = h.s->wallet().copper();
    const size_t from = h.got.size();
    h.client_sends(cart_frame(h, {{l1, 2}, {l2, 1}, {l1, 2}, {l2, 1}}));   // the lines of l1 and l2 again: double clicks of the confirm button
    CHECK(db.wait_started(3000), "the first line reached the database");
    for (int i = 0; i < 4; ++i) h.step();
    size_t threes = 0;
    for (const auto& a : purchase_answers(pc_calls_since(h, from))) threes += a.second == 3 ? 1 : 0;
    CHECK(threes == 2, "the two lines that name a listing the session has in flight or waiting are answered 3 at once");
    db.release_all();
    h.settle();
    const std::vector<std::pair<repl::GuidValue, uint64_t>> answers = purchase_answers(pc_calls_since(h, from));
    size_t zeros = 0;
    for (const auto& a : answers) zeros += a.second == 0 ? 1 : 0;
    CHECK(answers.size() == 4 && zeros == 2, "four answers: two 3 and two 0");
    CHECK(h.s->wallet().copper() == purse - 300 - 150 && db.buy_calls == 2, "each listing was bought once: 300 and 150 paid, two steps");
}

// ---- the seller's money claim -------------------------------------------------------------------------------------------------------------

/// A money claim for the amount the world read, when a second sale of the same listing was added to the mail after that: the service refuses it, the world takes the credit back and
/// keeps nothing marked, reads the mail again and sends the client the mail as it is now; the next claim pays the whole amount, once.
static void test_market_money_claim_for_a_changed_amount_is_read_again() {
    std::printf("the Marketplace: a money claim for an amount the mail no longer holds is refused, read again and then pays everything\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string mail = add_sale_mail(db, record.id, 700);
    MarketHarness h(record, &db);
    ready_to_trade(h);
    h.client_request(actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerFetchMail", {}), h.pc_ch);
    const svc::Mail* m = mail_in_cache(h, mail);
    CHECK(m && m->money == 700 && !m->money_claimed, "the world read the sale mail with 700 copper");
    const int64_t purse = h.s->wallet().copper();
    {
        std::lock_guard<std::mutex> lock(db.mu);
        db.mails[0].mail.money += 300;   // a second sale of the same listing was added to the mail after the world read it
    }
    size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, claim_currency_request(h, mail), true)});
    h.settle();
    CHECK(db.claim_money_asks == 1 && db.claim_changed_refusals == 1, "the service was asked once and refused: the mail held 1000 copper and the claim had credited 700");
    CHECK(h.s->wallet().copper() == purse && db.copper_of(record.id) == purse, "the 700 copper went back out of the purse: it is what it was, here and in the database");
    CHECK(db.sale_mails(record.id).size() == 1 && !db.sale_mails(record.id)[0].money_claimed && db.sale_mails(record.id)[0].money == 1000, "nothing is marked claimed: the mail holds 1000 copper");
    std::vector<repl::RpcCall> social = social_calls_since(h, from);
    CHECK(currency_callout(pc_calls_since(h, from)) == nullptr && count_calls(social, "ClientClaimMailCurrency") == 0, "the client is told of no payment");
    m = mail_in_cache(h, mail);
    CHECK(m && m->money == 1000 && !m->money_claimed, "the world read the mail again: 1000 copper, not claimed");
    bool shown_claimed = true;
    CHECK(detail_money(social, mail, &shown_claimed) == 1000 && !shown_claimed, "the client was sent the mail as it is now: 1000 copper, not claimed");
    // the player claims again: the whole amount is paid, once
    from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, claim_currency_request(h, mail), true)});
    h.settle();
    CHECK(h.s->wallet().copper() == purse + 1000 && db.copper_of(record.id) == purse + 1000, "the second claim paid the 1000 copper of both sales, here and in the database");
    CHECK(db.sale_mails(record.id)[0].money_claimed && db.claim_money_asks == 2 && db.claim_changed_refusals == 1, "the mail is claimed and the service was asked twice");
    social = social_calls_since(h, from);
    CHECK(currency_callout(pc_calls_since(h, from)) != nullptr && count_calls(social, "ClientClaimMailCurrency") == 1, "the client is told of the payment once");
    const int64_t asks = db.claim_money_asks;
    from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, claim_currency_request(h, mail), true)});
    h.settle();
    CHECK(h.s->wallet().copper() == purse + 1000 && db.claim_money_asks == asks, "a third claim of the same money changes nothing and is not asked");
}

/// The claim-all path: a claim of an item and of the money of a mail that grew is refused whole (the item goes back too), the mail is read again and sent, and the next claim-all takes
/// the item and all the money.
static void test_market_claim_all_for_a_changed_amount_is_read_again() {
    std::printf("the Marketplace: a claim-all whose mail grew is refused whole and then pays everything\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string sale = add_sale_mail(db, record.id, 700);
    const std::string post = db.fresh(), attachment = db.fresh();   // (words under 2^31: the claim-all request writes each as the wire's signed int)
    {
        market_db::MailRow row;
        row.owner = record.id;
        row.mail = potion_mail(post, attachment);
        db.mails.push_back(row);
    }
    MarketHarness h(record, &db);
    ready_to_trade(h);
    h.client_request(actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerFetchMail", {}), h.pc_ch);
    CHECK(h.s->mails().size() == 2, "both mails are loaded");
    const int64_t purse = h.s->wallet().copper();
    const int64_t potions = potion_units(h.s->inventory());
    {
        std::lock_guard<std::mutex> lock(db.mu);
        db.mails[0].mail.money += 300;   // the sale mail grew after the world read it
    }
    size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, claim_all_request(h, {sale, post}), true)});
    h.settle();
    CHECK(db.claim_changed_refusals == 1, "the service refused the claim-all: the sale mail held 1000 copper, the claim had credited 700");
    CHECK(h.s->wallet().copper() == purse && db.copper_of(record.id) == purse, "the money went back out of the purse");
    CHECK(potion_units(h.s->inventory()) == potions && h.s->inventory().find_guid(attachment) == nullptr, "the mailed potion went back too: the bags are what they were");
    CHECK(db.mails_of(record.id).size() == 2 && !db.sale_mails(record.id)[0].money_claimed && !db.mails_of(record.id)[1].items[0].claimed, "nothing is marked claimed in the database");
    const svc::Mail* m = mail_in_cache(h, sale);
    CHECK(m && m->money == 1000 && !m->money_claimed, "the world read the sale mail again: 1000 copper");
    CHECK(detail_money(social_calls_since(h, from), sale) == 1000, "the client was sent the sale mail as it is now");
    from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, claim_all_request(h, {sale, post}), true)});
    h.settle();
    CHECK(h.s->wallet().copper() == purse + 1000 && db.copper_of(record.id) == purse + 1000, "the second claim-all paid the 1000 copper of both sales");
    CHECK(potion_units(h.s->inventory()) == potions + 1 && h.s->inventory().find_guid(attachment) != nullptr && db.units_of(record.id, fi::kPotion) == potions + 1, "... and brought the mailed potion");
    CHECK(db.sale_mails(record.id)[0].money_claimed && db.mails_of(record.id)[1].items[0].claimed, "both are marked claimed");
}

AOC_REGISTER_TEST(test_market_a_cart_of_several_listings_buys_every_line);
AOC_REGISTER_TEST(test_market_a_cart_line_that_is_gone_when_it_starts);
AOC_REGISTER_TEST(test_market_a_cart_over_the_bound_answers_the_extra_lines_3);
AOC_REGISTER_TEST(test_market_a_session_that_ends_drops_the_queued_lines);
AOC_REGISTER_TEST(test_market_a_listing_already_in_the_session_is_answered_3);
AOC_REGISTER_TEST(test_market_money_claim_for_a_changed_amount_is_read_again);
AOC_REGISTER_TEST(test_market_claim_all_for_a_changed_amount_is_read_again);

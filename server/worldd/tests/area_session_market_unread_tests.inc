// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_market_unread_tests.inc: the seller who was away is told of the sale mail at the log-in, through real sessions
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc, after the sales tests, whose
//  helpers it uses)
//
//    the log-in    the client's map header shows the mail badge from the SocialComponent's NumUnreadMail: it reads the count when it is built and again whenever the count changes
//                  on the client.  A seller whose listing sold while he was away logs in with unread mail: one update of the count follows the join, behind the phase-in; the
//                  join's own chain is the same bits whatever the count, and a character with no unread mail is sent nothing
//    the reading   marking the mail read at a mailbox drops the count as before
// ============================================================================
#include "market/market.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

/// One value of the SocialComponent's NumUnreadMail the client was given, and the chain that carried it.
struct UnreadUpdate {
    size_t  chain = 0;
    int64_t count = -1;
};

/// The SocialComponent of a session's PlayerController, or nullptr before the join has put it there.
static const repl::Subobject* unread_social_of(const PlayerSession& s) {
    for (const repl::Subobject& sub : s.bootstrap().player_controller().subobjects) {
        if (sub.name == "SocialComponent") return &sub;
    }
    return nullptr;
}

/// Every value of NumUnreadMail the SocialComponent's property blocks of the chains [from, ...) carry, in order (a block that does not carry the property says nothing about it).
static std::vector<UnreadUpdate> unread_updates_since(const Harness& h, size_t from) {
    std::vector<UnreadUpdate> out;
    const repl::Subobject* social = unread_social_of(*h.s);
    if (!social || !social->obj.schema) return out;
    for (size_t i = from; i < h.got.size(); ++i) {
        const fake::Delivered& d = h.got[i];
        if (d.close || d.content.bit_pos() == 0) continue;
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& b : *blocks) {
            if (b.header.is_actor || !(b.header.object == social->guid) || !b.header.has_rep_layout) continue;
            size_t pos = b.payload_pos;
            const std::optional<repl::RepState> state = repl::read_rep_block(*social->obj.schema, d.content.data(), b.payload_pos + b.payload_bits, pos, social->obj.opts);
            if (!state) continue;
            const repl::Value* value = state->get(*social->obj.schema, "NumUnreadMail");
            const int64_t* number = value ? value->get_if<int64_t>() : nullptr;
            if (number) out.push_back(UnreadUpdate{i, *number});
        }
    }
    return out;
}

/// The chain of the join that carries the SocialComponent's first block (an export with no property of its own: the join sends the component at its defaults), and that whole chain's
/// bits as hex (empty: none).
static std::pair<size_t, std::string> unread_join_chain(const Harness& h) {
    const repl::Subobject* social = unread_social_of(*h.s);
    if (!social) return {0, std::string()};
    for (size_t i = 0; i < h.got.size(); ++i) {
        const fake::Delivered& d = h.got[i];
        if (d.close || d.content.bit_pos() == 0) continue;
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const repl::BlockView& b : *blocks) {
            if (b.header.is_actor || !(b.header.object == social->guid)) continue;
            std::string hex;
            for (size_t at = 0; at < d.content.bit_pos(); at += 8) {
                uint32_t byte = 0;
                for (size_t k = 0; k < 8 && at + k < d.content.bit_pos(); ++k) byte |= static_cast<uint32_t>((d.content.data()[(at + k) >> 3] >> ((at + k) & 7)) & 1u) << k;
                char text[3];
                std::snprintf(text, sizeof text, "%02x", byte);
                hex += text;
            }
            return {i, hex};
        }
    }
    return {0, std::string()};
}

/// The seller of the sale mails of a database as a log-in reads him: the character service counts his unread mails.
static svc::Character seller_logging_in(MarketDb& db, bool with_his_mails) {
    svc::Character seller = with_start_purse(cyb_record());
    seller.id = kSeller;
    seller.name = "Brannoc";
    seller.unread_mail_count = with_his_mails ? static_cast<int32_t>(db.sale_mails(kSeller).size()) : 0;
    return seller;
}

/// A buyer buys one unit of a listing of a seller who is away: his sale mail waits in the database.  The buyer's session ends.
static std::string sell_to_a_buyer_while_the_seller_is_away(MarketDb& db) {
    const svc::Character buyer = buyer_record();
    db.store_character(buyer);
    const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 2, kUnitPrice);
    MarketHarness h(buyer, &db);
    ready_to_trade(h);
    h.client_request(purchase_request(::aoc::worldd::market::marketplace_guid(kNode), guid_of_hex(listing), 1), h.pc_ch);
    h.s->finish("the client left");
    h.s.reset();
    return listing;
}

/// A seller whose listing sold while he was away logs in: the count of his unread mail follows the join, once.
static void test_a_seller_who_was_away_is_told_of_his_unread_sale_mail_at_the_log_in() {
    std::printf("the Marketplace: the seller who was away is told of the unread sale mail at the log-in\n");
    MarketDb db;
    sell_to_a_buyer_while_the_seller_is_away(db);
    CHECK(db.sale_mails(kSeller).size() == 1, "the sale left one mail with the money for the seller");
    MarketHarness h(seller_logging_in(db, true), &db);
    h.play();
    CHECK(h.s->unread_mail_count() == 1, "the log-in reads one unread mail");
    const std::vector<UnreadUpdate> updates = unread_updates_since(h, 0);
    CHECK(updates.size() == 1 && updates[0].count == 1, "the client was given the count 1, once");
    const std::pair<size_t, std::string> join = unread_join_chain(h);
    CHECK(!join.second.empty() && !updates.empty() && updates[0].chain > join.first, "and after the join's own chain of the SocialComponent");
    int32_t held = -1;
    CHECK(test_mailbox_unread_state(h, held) && held == 1, "the session holds the count 1 on the component");
}

/// A character with no unread mail is sent nothing, and the join's chain of the SocialComponent is the same bits whatever the count.
static void test_a_character_with_no_unread_mail_is_sent_no_count_and_the_join_is_unchanged() {
    std::printf("the Marketplace: no unread mail, no count, and the join chain is the same\n");
    MarketDb db;
    sell_to_a_buyer_while_the_seller_is_away(db);
    MarketHarness none(seller_logging_in(db, false), &db);
    none.play();
    CHECK(none.s->unread_mail_count() == 0 && unread_updates_since(none, 0).empty(), "no unread mail: the client is given no count");
    MarketHarness some(seller_logging_in(db, true), &db);
    some.play();
    const std::string without = unread_join_chain(none).second, with = unread_join_chain(some).second;
    size_t first_difference = 0;
    while (first_difference < without.size() && first_difference < with.size() && without[first_difference] == with[first_difference]) ++first_difference;
    CHECK(!without.empty() && without == with, ("the join's chain of the SocialComponent is the same bits with 0 and with 1 unread mails (" + std::to_string(without.size()) + " and " + std::to_string(with.size()) +
                                                " hex digits, the first difference at " + std::to_string(first_difference) + ")").c_str());
}

/// The seller reads the mail: the count drops to 0 as it does for a seller who is in the world when the sale lands.
static void test_reading_the_sale_mail_drops_the_count_as_before() {
    std::printf("the Marketplace: reading the sale mail drops the count\n");
    MarketDb db;
    sell_to_a_buyer_while_the_seller_is_away(db);
    const std::vector<svc::Mail> mails = db.sale_mails(kSeller);
    CHECK(mails.size() == 1, "one sale mail waits");
    if (mails.size() != 1) return;
    MarketHarness h(seller_logging_in(db, true), &db);
    h.play();
    const schema::RpcSchema* rpc = class_named("UIntrepidSocialComponent").rpc("ServerUpdateMailStatus");
    const auto mail = items::guid_of(mails[0].id);
    repl::RpcParams params;
    CHECK(rpc && mail && put_guid(*rpc, params, "MailboxGuid", repl::GuidValue{5100u, 0u, 0u, 0u}) && put_guid(*rpc, params, "MailGuid", *mail) &&
              repl::set_rpc_param(*rpc, params, "Status", repl::Value::u64(static_cast<uint64_t>(svc::kMailStatusRead))),
          "the status call builds");
    if (!rpc || !mail) return;
    h.client_sends({h.client.actor(h.pc_ch, social_rpc(h, "ServerUpdateMailStatus", params), true)});
    for (int i = 0; i < 600 && h.s->unread_mail_count() != 0; ++i) {
        h.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int i = 0; i < 20; ++i) h.step();
    const std::vector<UnreadUpdate> updates = unread_updates_since(h, 0);
    CHECK(h.s->unread_mail_count() == 0 && updates.size() == 2 && updates[0].count == 1 && updates[1].count == 0, "the client was given 1 at the log-in and 0 when the mail was read");
}

AOC_REGISTER_TEST(test_a_seller_who_was_away_is_told_of_his_unread_sale_mail_at_the_log_in);
AOC_REGISTER_TEST(test_a_character_with_no_unread_mail_is_sent_no_count_and_the_join_is_unchanged);
AOC_REGISTER_TEST(test_reading_the_sale_mail_drops_the_count_as_before);

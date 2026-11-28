// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/market_storefronts.inc (see market.h)
//
//  The window's search and its storefront descriptions: ServerSearchMarketplaceHeaders answers the storefronts that list an item the text names, and
//  ServerRequestMarketplaceHeaderUpdate stores the text a seller gives his storefront at this market.
//
//    the search     1 to 50 characters (UTF-16 units, as the game counts them) or ClientMarketplaceHeaderUpdateResponse(text, 16); then the storefronts of this
//                   market that list an item whose name holds the text, whatever the case, at exactly the rarity picked (7: any), never the searcher's own, 100 headers a page with
//                   bComplete on the last, each page echoing the text and the rarity and carrying no version (retail's did not).  One search a second: the game lets one
//                   through when a second has passed since the last and answers nothing to the others.  The cache is read; no database is asked and nothing is written.
//    the edit       0 to 50 characters and none of the control characters the client's own edit box takes out (14 too long, 15 for a text no client sends or that holds a word of
//                   the client's bad-word list), and a listing of the seller's at this market (1 otherwise: the Edit button is off in the client without one).  The text is stored
//                   as sent, untrimmed, per (market, seller), in ONE step of the character service that also raises the market's page version; an empty text clears it and the
//                   storefront shows "<name>'s Storefront" again.  The answer is 0 with the text the storefront shows now.  Edits wait in line one behind the other (at most
//                   kMaxQueuedDescribes; the last in the line is the text that stays), and an edit of a session that ends is finished by the world.
// ============================================================================
#include "market/market.h"
#include "market/storefront_text.h"

#include "bootstrap/bootstrap.h"
#include "protocol/repl/market_rpc.h"
#include "session/player_session.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

constexpr uint8_t kInvalidDescriptionLength = 14;   // EMarketplaceResponse INVALID_DESCRIPTION_LEN: "Invalid Description Length"
constexpr uint8_t kInvalidDescription = 15;         // INVALID_DESCRIPTION: "Description contained invalid content"
constexpr uint8_t kInvalidSearchLength = 16;        // INVALID_SEARCH_LEN: the client's own answer to an empty search (as recorded) and to one past 50

}  // namespace

void PlayerSession::market_answer_header_update(Connection& conn, const std::string& text, uint8_t response) {
    queue("ClientMarketplaceHeaderUpdateResponse", [text, response](bootstrap::PlayerBootstrap& pb) { return pb.market_header_update_answer(text, response); }, conn, /*must_build=*/true);
}

void PlayerSession::on_market_search(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_ || !world_) return;
    const std::optional<repl::MarketSearchRequest> request = repl::parse_market_search(rpc.call);
    if (!request) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerSearchMarketplaceHeaders does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(market_budget_, "ServerSearchMarketplaceHeaders", now)) return;
    const GoodPosition at = checker_.good();   // where the player stands now, not where the last periodic save put him
    std::string why_not;
    const std::optional<int64_t> node = opened_market_node(pb_.market_window, &world_->registry(), world_->city_nodes(), request->market, at.x, at.y, at.z, why_not);
    if (!node) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerSearchMarketplaceHeaders is ignored: {}", name(), why_not);
        return;
    }
    if (!room_for(1)) return;

    // the game's own check: the text is 1 to 50 characters, else it answers 16 (with the text that was sent) and goes no further
    const std::optional<size_t> length = market::utf16_length(request->text);
    if (!length || *length < 1 || *length > market::kTextMax) {
        spdlog::debug("[market] {}: a search of {} characters is refused (16)", name(), length ? std::to_string(*length) : std::string("ill-formed"));
        market_answer_header_update(conn, length ? request->text : std::string(), kInvalidSearchLength);
        return;
    }
    if (request->rarity > static_cast<uint64_t>(market::kAnyRarity)) {   // the dropdown sends 0 to 6 and 7: another value is no request of the client's
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a search with the rarity {} is ignored", name(), request->rarity);
        return;
    }
    // one search a second (the game goes on only when a second has passed since the last one, and answers nothing to the others)
    if (!market_search_budget_.take(now)) {
        spdlog::log(level_for(market_search_dropped_), "[market] {}: a search within a second of the one before is not answered", name());
        return;
    }
    market::MarketStore* store = world_->market_listings();
    if (!store->loaded()) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a search is ignored: the saved listings are not loaded yet", name());
        return;
    }
    const repl::GuidValue self = character_guid(pb_.setup());
    std::vector<repl::MarketHeader> found;
    for (const market::Storefront& sf : store->search(*node, request->text, static_cast<int>(request->rarity), self)) found.push_back(header_of(*node, sf));
    send_storefront_pages(conn, *node, std::move(found), repl::MarketPageEcho{0, request->text, request->rarity}, "a search");
}

void PlayerSession::send_storefront_pages(Connection& conn, int64_t node, std::vector<repl::MarketHeader> headers, const repl::MarketPageEcho& echo, const char* what) {
    const size_t total = headers.size();
    if (headers.size() > market::kMaxPushedStorefronts) headers.resize(market::kMaxPushedStorefronts);   // the reply queue holds this many pages
    const size_t pages = std::max<size_t>(1, (headers.size() + market::kStorefrontPageSize - 1) / market::kStorefrontPageSize);
    if (!room_for(pages)) return;
    for (size_t p = 0; p < pages; ++p) {
        const size_t first = p * market::kStorefrontPageSize;
        const size_t last = std::min(headers.size(), first + market::kStorefrontPageSize);
        std::vector<repl::MarketHeader> page(headers.begin() + static_cast<std::ptrdiff_t>(first), headers.begin() + static_cast<std::ptrdiff_t>(last));
        const bool complete = p + 1 == pages;
        queue("ClientReceiveMarketplaceHeaders", [node, page = std::move(page), complete, echo](bootstrap::PlayerBootstrap& pb) { return pb.market_headers(node, page, complete, echo); }, conn,
              /*must_build=*/true);
    }
    spdlog::log(level_for(market_answered_logged_, 20), "[market] {}: {} of node {} is answered with {} page(s): {} of {} storefront(s)", name(), what, node, pages, headers.size(), total);
}

void PlayerSession::on_market_header_update(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_ || !world_) return;
    const std::optional<repl::MarketHeaderUpdateRequest> request = repl::parse_market_header_update(rpc.call);
    if (!request) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceHeaderUpdate does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(market_budget_, "ServerRequestMarketplaceHeaderUpdate", now)) return;
    const GoodPosition at = checker_.good();   // where the player stands now, not where the last periodic save put him
    std::string why_not;
    const std::optional<int64_t> node = opened_market_node(pb_.market_window, &world_->registry(), world_->city_nodes(), request->market, at.x, at.y, at.z, why_not);
    if (!node) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceHeaderUpdate is ignored: {}", name(), why_not);
        return;
    }
    if (!room_for(1)) return;
    const std::string& text = request->description;
    const auto refuse = [&](uint8_t response, const char* why) {
        spdlog::debug("[market] {}: a storefront description is refused ({}): {}", name(), static_cast<int>(response), why);
        market_answer_header_update(conn, text, response);
    };

    // the text: 0 to 50 UTF-16 units (14), well-formed and with no control character (15: the client's edit box never sends one), and no word of the client's bad-word list (15)
    const std::optional<size_t> length = market::utf16_length(text);
    if (!length) return refuse(kInvalidDescription, "the text is not well-formed UTF-8");
    if (*length > market::kTextMax) return refuse(kInvalidDescriptionLength, "the text is longer than 50 characters");
    if (market::has_control_character(text)) return refuse(kInvalidDescription, "the text holds a control character");
    const market::BadWords* bad_words = world_->market_bad_words();
    if (!bad_words) return refuse(kErrorGeneric, "the bad-word list could not be read");
    if (bad_words->contains(text)) return refuse(kInvalidDescription, "the text holds a word of the client's bad-word list");

    // a seller with no listing at this market has no storefront there (the client turns the button off; the server does not trust it)
    market::MarketStore* store = world_->market_listings();
    if (!store->loaded()) return refuse(kErrorGeneric, "the saved listings are not loaded yet");
    const repl::GuidValue seller = character_guid(pb_.setup());
    if (store->listings(*node, seller).empty()) return refuse(kErrorGeneric, "the seller has no listing at this market");
    if (market_describes_.size() >= market::kMaxQueuedDescribes) return refuse(kRequestInProgress, "too many edits of this session are waiting");

    market::DescribeRequest edit;
    edit.node = *node;
    edit.seller = seller;
    edit.seller_name = name();
    edit.description = text;
    const uint64_t ticket = store->submit_describe(std::move(edit));
    if (ticket == 0) return refuse(kErrorGeneric, "the description could not be saved");
    market_describes_.push_back(PendingDescribe{ticket, text, *node});
    spdlog::debug("[market] {}: a storefront description of {} character(s) at node {} is being saved", name(), *length, *node);
}

void PlayerSession::market_run_describes(Connection& conn, Clock::time_point now) {
    (void)now;
    if (!world_) return;
    market::MarketStore* store = world_->market_listings();
    while (!market_describes_.empty()) {
        market::ListingSaveAnswer answer;
        if (!store->take_describe(market_describes_.front().ticket, answer)) break;   // one worker answers in order
        PendingDescribe job = std::move(market_describes_.front());
        market_describes_.pop_front();
        if (answer.stored()) {
            // the cache has the text and the market's new version now (the store joined them when it gave this answer); the seller's own panel shows what the storefront shows
            const std::string shown = answer.description.empty() ? market::default_description(name()) : answer.description;
            market_answer_header_update(conn, shown, 0);
            spdlog::log(level_for(market_logged_, 20), "[market] {}: the storefront description at node {} is now {} (page version count {})", name(), job.node,
                        answer.description.empty() ? std::string("the default") : "\"" + answer.description + "\"", answer.header_raises);
        } else {
            // the seller had no listing by the time the service looked, or the service refused the request: nothing was written
            market_answer_header_update(conn, job.text, kErrorGeneric);
            spdlog::log(level_for(market_refused_logged_), "[market] {}: the storefront description was refused ({}: {}): nothing is changed", name(), svc::call_status_name(answer.status),
                        answer.status == svc::CallStatus::Ok ? "the seller has no listing at the market" : answer.reason);
        }
    }
}

}  // namespace aoc::worldd

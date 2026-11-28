// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/market_sales.inc (see market.h)
//
//  The window's buying and removing: ServerRequestMarketplaceItemPurchase takes the price and the node's sales tax from the buyer's purse and puts the units in his bags;
//  ServerRequestMarketplaceItemRemoval puts a seller's stack back in his bags.  Each is ONE step of the world's market worker and ONE transaction of the character service
//  (the buyer's or seller's bags and purse, the listing, the seller's mail and the node's tax together), and the session answers when the store has answered.
//
//    the line      the Buy panel sends one purchase for each line of its cart in one frame: a step that arrives while the session has one in flight waits in a line of at most
//                  market::kMaxQueuedSteps, runs all its checks when the step before it has landed and gets its own answer; a listing the session already has a step for, and a
//                  step past the bound, are answered 3 at once; a session that ends drops the steps that never started
//    the answers   0 done; 3 a step of this character or of the listing is in flight; 5 no such listing, not the seller's own (a removal), or past its time (and swept at once);
//                  8 more units than the listing holds (or none); 9 the buyer's own listing; 12 the bags have no room; 13 the sum or the buyer's money is no sale.
//    the order     a done purchase sends the money popup (minus the price and the tax), the purse, the bag event and the answer; a done removal the bag event and the answer.  The bag
//                  event is the retail one: one ClientRecieveBatchedStorageEvents call whose Adds carry the "Acquired" pop-up.  The answer carries the listing and the units the
//                  request named (a refusal too).  The client asks for the storefront again by itself.
//    the refusals  nothing the client saw has changed: a refusal before the step answers and nothing more; one after it gives back what the step put in the bags and purse.
// ============================================================================
#include "market/market.h"
#include "market/market_tax.h"
#include "market/undo.h"

#include "bootstrap/bootstrap.h"
#include "items/inventory.h"
#include "items/wallet.h"
#include "protocol/repl/market_rpc.h"
#include "session/player_session.h"

#include <spdlog/spdlog.h>

#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;

uint8_t PlayerSession::market_step_blocked(const repl::GuidValue& listing) const {
    if (!market_pending_.empty()) return kRequestInProgress;                       // a step of this session is still being saved: both write the same bags
    if (!mail_claim_in_flight_.empty()) return kRequestInProgress;                 // a mail claim writes the same bags on the mail worker
    if (trade_commit_pending()) return kRequestInProgress;                         // a player trade commit writes the same bags and purse
    if (world_->login_pending(character_id_)) return kRequestInProgress;           // a log-in of this character waits for the saves the world holds
    market::MarketStore* store = world_->market_listings();
    if (store->busy(character_guid(pb_.setup()))) return kRequestInProgress;       // a step of an earlier session of this character is still being finished
    if (store->listing_busy(listing)) return kRequestInProgress;                   // a step of the listing is in flight: one at a time
    return 0;
}

void PlayerSession::market_answer_purchase(Connection& conn, const repl::GuidValue& listing, int32_t quantity, uint8_t response) {
    queue("ClientMarketplaceItemPurchaseResponse", [listing, quantity, response](bootstrap::PlayerBootstrap& pb) { return pb.market_purchase_answer(listing, quantity, response); }, conn,
          /*must_build=*/true);
}

void PlayerSession::market_answer_removal(Connection& conn, const repl::GuidValue& listing, uint8_t response) {
    queue("ClientMarketplaceItemRemovalResponse", [listing, response](bootstrap::PlayerBootstrap& pb) { return pb.market_removal_answer(listing, response); }, conn,
          /*must_build=*/true);
}

void PlayerSession::market_undo(const PendingMarket& job) {
    // exactly what the placement added to the bags (each Add event, where it is now: market/undo.h) and the money the purse paid; nothing else the player did meanwhile changes
    market::undo_added(inventory_, job.added.events);
    if (job.paid > 0) wallet_.give(job.paid);
    inventory_dirty_ = true;
    inventory_changed_ = true;
}

uint8_t PlayerSession::market_refusal_code(const market::ListingSaveAnswer& answer) const {
    if (answer.status != svc::CallStatus::Ok) return kErrorGeneric;   // the request itself was refused, or the service keeps no sales
    switch (answer.outcome) {
    case svc::MarketOutcome::ListingGone:
    case svc::MarketOutcome::ListingExpired:
    case svc::MarketOutcome::PriceChanged: return kItemNotFound;   // "no longer available, please refresh"
    case svc::MarketOutcome::Quantity: return kInvalidQuantity;
    case svc::MarketOutcome::OwnListing: return kInvalidPlayer;
    case svc::MarketOutcome::RateChanged: return kCurrencyNotFound;
    default: break;
    }
    return kErrorGeneric;
}

/// A step that arrives while this session has one in flight or waiting keeps its place in line behind it: the Buy panel sends one purchase for each line of its cart in one frame and
/// waits for an answer to every line, and a session runs one step at a time (both write the same bags and purse).  True when the step was taken in (queued, or answered 3: a
/// listing that already has a step of this session in flight or waiting is a double submit, and a step past the bound of the line is refused); false when nothing of the session is in
/// its way and the step starts at once.  A step waiting here has changed nothing and holds nothing: it runs all its checks when it starts, so its answer is the real one.
bool PlayerSession::market_wait_turn(Connection& conn, QueuedMarket step) {
    if (market_pending_.empty() && market_queue_.empty()) return false;
    const bool named = std::any_of(market_pending_.begin(), market_pending_.end(), [&](const PendingMarket& job) { return job.listing.guid == step.listing; }) ||
                       std::any_of(market_queue_.begin(), market_queue_.end(), [&](const QueuedMarket& waiting) { return waiting.listing == step.listing; });
    if (named || market_queue_.size() >= market::kMaxQueuedSteps) {
        spdlog::debug("[market] {}: a step is refused while the session has {} in flight and {} waiting ({}): {}", name(), market_pending_.size(), market_queue_.size(),
                      named ? "the listing is the session's own step already" : "the line is full", static_cast<int>(kRequestInProgress));
        if (!room_for(1)) return true;
        if (step.kind == market::StepKind::Buy) market_answer_purchase(conn, step.listing, step.quantity, kRequestInProgress);
        else market_answer_removal(conn, step.listing, kRequestInProgress);
        return true;
    }
    market_queue_.push_back(std::move(step));
    spdlog::debug("[market] {}: a step waits behind the {} in flight and {} waiting", name(), market_pending_.size(), market_queue_.size() - 1);
    return true;
}

/// Starts the steps that waited, one at a time, from the bags and purse the step before them left: a step that is refused or ignored at its start leaves the line to the next one.
void PlayerSession::market_run_queue(Connection& conn, Clock::time_point now) {
    while (market_pending_.empty() && !market_queue_.empty()) {
        const QueuedMarket step = std::move(market_queue_.front());
        market_queue_.pop_front();
        if (step.kind == market::StepKind::Buy) market_begin_purchase(conn, step.window, step.listing, step.quantity, now);
        else market_begin_removal(conn, step.window, step.listing, now);
    }
}

void PlayerSession::on_market_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_ || !world_) return;
    const std::optional<repl::MarketPurchase> request = repl::parse_market_purchase(rpc.call);
    if (!request) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceItemPurchase does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(market_budget_, "ServerRequestMarketplaceItemPurchase", now)) return;
    if (market_wait_turn(conn, QueuedMarket{market::StepKind::Buy, request->market, request->listing, request->quantity})) return;
    market_begin_purchase(conn, request->market, request->listing, request->quantity, now);
}

/// The purchase's own steps, with all its checks: at its arrival when nothing of this session is in its way, or when the step before it has landed (market_run_queue).
void PlayerSession::market_begin_purchase(Connection& conn, const repl::GuidValue& market_guid, const repl::GuidValue& listing_guid, int32_t asked_quantity, Clock::time_point now) {
    if (dead_ || !world_ || !room_for(6)) return;
    const repl::MarketPurchase request{market_guid, listing_guid, asked_quantity};
    const auto refuse = [&](uint8_t response, const char* why) {
        spdlog::debug("[market] {}: a purchase of {} unit(s) is refused ({}): {}", name(), request.quantity, why, static_cast<int>(response));
        market_answer_purchase(conn, request.listing, request.quantity, response);
    };
    const GoodPosition at = checker_.good();   // where the player stands now, not where the last periodic save put him
    std::string why_not;
    const std::optional<int64_t> node = opened_market_node(pb_.market_window, &world_->registry(), world_->city_nodes(), request.market, at.x, at.y, at.z, why_not);
    if (!node) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a purchase is ignored: {}", name(), why_not);
        return;
    }
    market::MarketStore* store = world_->market_listings();
    if (!store->loaded()) return refuse(kErrorGeneric, "the saved listings are not loaded yet");
    const repl::GuidValue buyer = character_guid(pb_.setup());
    const market::Listing* cached = store->find(request.listing);
    if (!cached || cached->node != *node || cached->status != market::kListed) return refuse(kItemNotFound, "no such listing at this market");
    const market::Listing listing = *cached;   // a copy: the cache changes while the step is in flight
    if (sync_now(now) >= listing.expiration_date) {
        store->sweep_now(listing.guid);   // the listing goes to its seller's mail at once
        return refuse(kItemNotFound, "the listing's time has come");
    }
    if (const uint8_t blocked = market_step_blocked(listing.guid)) return refuse(blocked, "a step is in flight");
    if (listing.seller == buyer) return refuse(kInvalidPlayer, "the buyer's own listing");
    if (request.quantity < 1 || request.quantity > listing.item.quantity) return refuse(kInvalidQuantity, "the listing does not hold that many units");

    // what the buyer pays: the price and the tax of the very table the open sends, at the node's rate
    const repl::NodeTaxDetails table = market::market_tax_table(*node, store->node_rate(*node));
    const market::TaxRow row = market::sale_row(table);
    const std::optional<market::SaleCharge> charge = market::charge_for(listing.price_per_unit, request.quantity, row);
    if (!charge) return refuse(kCurrencyNotFound, "the sum is no sale: it is 2^31 or more");
    if (!wallet_.can_pay(charge->total)) return refuse(kCurrencyNotFound, "the purse does not hold the price and the tax");

    // the units: the whole stack keeps the listing's guid; a part of it is a stack of its own
    const bool whole = request.quantity == listing.item.quantity;
    svc::InventoryItem units = listing.item;
    units.quantity = request.quantity;
    if (!whole) {
        const std::optional<repl::GuidValue> instance = items::guid_of(inventory_.fresh_guid());
        if (!instance) return refuse(kErrorGeneric, "no item guid could be made");
        units.instance_guid = items::guid_hex(*instance);
    } else if (inventory_.find_guid(units.instance_guid) != nullptr) {
        return refuse(kErrorGeneric, "the bags hold the listing's guid");
    }
    const std::optional<repl::GuidValue> purchase = items::guid_of(inventory_.fresh_guid());
    if (!purchase) return refuse(kErrorGeneric, "no purchase id could be made");
    items::Outcome placed = inventory_.grant({units}, item_catalog());
    if (!placed.ok) return refuse(kInventoryFull, placed.refusal.c_str());
    wallet_.pay(charge->total);   // checked above: it cannot be short
    inventory_dirty_ = true;
    inventory_changed_ = true;

    PendingMarket job;
    job.kind = market::StepKind::Buy;
    job.listing = listing;
    job.quantity = request.quantity;
    job.asked = request.quantity;
    job.paid = charge->total;
    job.events = item_events_for(placed);
    job.added = std::move(placed);
    svc::BuyRequest buy;
    buy.purchase_id = items::guid_hex(*purchase);
    buy.character_id = character_id_;
    buy.listing_id = items::guid_hex(listing.guid);
    buy.quantity = request.quantity;
    buy.price_per_unit = listing.price_per_unit;
    buy.tax_rate = row.percent;
    buy.tax = charge->tax;
    buy.item_name = listing.item_name;
    buy.bags.items = inventory_.items();
    buy.bags.wallet = wallet_.entries();
    buy.bags.jobs = jobs_;
    buy.bags.known_recipes = taught_;
    buy.bags.warehouse = wh_storage_.rows();
    job.ticket = store->submit_buy(std::move(buy), listing);
    if (job.ticket == 0) {
        market_undo(job);
        return refuse(kRequestInProgress, "the purchase could not be queued");
    }
    market_save_in_flight_ = true;
    market_pending_.push_back(std::move(job));
    spdlog::debug("[market] {}: buying {} x{} of listing {}/{}/{}/{} for {} copper and {} tax is being saved", name(), request.quantity, listing.item_name, listing.guid.a, listing.guid.b,
                  listing.guid.c, listing.guid.d, charge->sum, charge->tax);
}

void PlayerSession::on_market_removal(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_ || !world_) return;
    const std::optional<repl::MarketRemoval> request = repl::parse_market_removal(rpc.call);
    if (!request) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceItemRemoval does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(market_budget_, "ServerRequestMarketplaceItemRemoval", now)) return;
    if (market_wait_turn(conn, QueuedMarket{market::StepKind::Remove, request->market, request->listing, 0})) return;
    market_begin_removal(conn, request->market, request->listing, now);
}

void PlayerSession::market_begin_removal(Connection& conn, const repl::GuidValue& market_guid, const repl::GuidValue& listing_guid, Clock::time_point now) {
    if (dead_ || !world_ || !room_for(4)) return;
    const repl::MarketRemoval request{market_guid, listing_guid};
    const auto refuse = [&](uint8_t response, const char* why) {
        spdlog::debug("[market] {}: a removal is refused ({}): {}", name(), why, static_cast<int>(response));
        market_answer_removal(conn, request.listing, response);
    };
    const GoodPosition at = checker_.good();
    std::string why_not;
    const std::optional<int64_t> node = opened_market_node(pb_.market_window, &world_->registry(), world_->city_nodes(), request.market, at.x, at.y, at.z, why_not);
    if (!node) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a removal is ignored: {}", name(), why_not);
        return;
    }
    market::MarketStore* store = world_->market_listings();
    if (!store->loaded()) return refuse(kErrorGeneric, "the saved listings are not loaded yet");
    const repl::GuidValue seller = character_guid(pb_.setup());
    const market::Listing* cached = store->find(request.listing);
    // another player's listing is answered like a listing that is not there
    if (!cached || cached->node != *node || cached->status != market::kListed || !(cached->seller == seller)) return refuse(kItemNotFound, "no such listing of this seller at this market");
    const market::Listing listing = *cached;
    if (sync_now(now) >= listing.expiration_date) {
        store->sweep_now(listing.guid);   // an expired listing comes back by mail, not through Cancel Listing
        return refuse(kItemNotFound, "the listing's time has come");
    }
    if (const uint8_t blocked = market_step_blocked(listing.guid)) return refuse(blocked, "a step is in flight");

    // the stack comes back under the listing's own guid
    if (inventory_.find_guid(listing.item.instance_guid) != nullptr) return refuse(kErrorGeneric, "the bags hold the listing's guid");
    svc::InventoryItem stack = listing.item;
    items::Outcome placed = inventory_.grant({stack}, item_catalog());
    if (!placed.ok) return refuse(kInventoryFull, placed.refusal.c_str());
    inventory_dirty_ = true;
    inventory_changed_ = true;

    PendingMarket job;
    job.kind = market::StepKind::Remove;
    job.listing = listing;
    job.quantity = listing.item.quantity;
    job.events = item_events_for(placed);
    job.added = std::move(placed);
    svc::RemoveRequest remove;
    remove.character_id = character_id_;
    remove.listing_id = items::guid_hex(listing.guid);
    remove.expected_quantity = listing.item.quantity;
    remove.bags.items = inventory_.items();
    remove.bags.wallet = wallet_.entries();
    remove.bags.jobs = jobs_;
    remove.bags.known_recipes = taught_;
    remove.bags.warehouse = wh_storage_.rows();
    job.ticket = store->submit_remove(std::move(remove), listing);
    if (job.ticket == 0) {
        market_undo(job);
        return refuse(kRequestInProgress, "the removal could not be queued");
    }
    market_save_in_flight_ = true;
    market_pending_.push_back(std::move(job));
    spdlog::debug("[market] {}: taking back {} x{} of listing {}/{}/{}/{} is being saved", name(), listing.item.quantity, listing.item_name, listing.guid.a, listing.guid.b, listing.guid.c,
                  listing.guid.d);
}

void PlayerSession::market_purchase_landed(Connection& conn, PendingMarket job, const market::ListingSaveAnswer& answer) {
    const repl::GuidValue guid = job.listing.guid;
    if (answer.stored()) {
        // retail's order: the money popup and the purse, then the bag event, then the answer
        pb_.set_currency(items::kCopperCurrency, wallet_.copper());
        queue("ClientShowCurrencyCallout", [paid = job.paid](bootstrap::PlayerBootstrap& pb) { return pb.currency_callout(items::kMoneyTier, -paid); }, conn, /*must_build=*/true);
        queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
        // the retail bag event: ONE ClientRecieveBatchedStorageEvents call of the Adds, each with the "Acquired" pop-up (the client shows the pop-up only from the event's own callout; a top-up of a
        // held stack is one Add of the units that joined it, under the held stack's guid, and its pop-up names those units)
        std::vector<bootstrap::ItemEvent> events = std::move(job.events);
        add_item_toasts(events, /*removals=*/false);
        queue("ClientRecieveBatchedStorageEvents", [events = std::move(events)](bootstrap::PlayerBootstrap& pb) { return pb.craft_batched_events(events); }, conn, /*must_build=*/true);
        market_answer_purchase(conn, guid, job.asked, kMarketOk);
        spdlog::log(level_for(market_logged_, 20), "[market] {}: bought {} x{} of item {} from {} for {} copper (listing {}/{}/{}/{})", name(), job.quantity, job.listing.item_name,
                    job.listing.item.item_record_id, job.listing.seller_name, job.paid, guid.a, guid.b, guid.c, guid.d);
        return;
    }
    market_undo(job);
    const uint8_t code = market_refusal_code(answer);
    market_answer_purchase(conn, guid, job.asked, code);
    if (answer.status == svc::CallStatus::Ok && answer.outcome == svc::MarketOutcome::ListingExpired && world_) world_->market_listings()->sweep_now(guid);
    spdlog::log(level_for(market_refused_logged_), "[market] {}: the purchase was refused ({}: {}, outcome {}): the bags and the purse stay as they were; answered {}", name(),
                svc::call_status_name(answer.status), answer.reason, static_cast<int>(answer.outcome), static_cast<int>(code));
}

void PlayerSession::market_removal_landed(Connection& conn, PendingMarket job, const market::ListingSaveAnswer& answer) {
    const repl::GuidValue guid = job.listing.guid;
    if (answer.stored()) {
        // the retail bag event: ONE ClientRecieveBatchedStorageEvents call of the Adds, each with the "Acquired" pop-up (the client shows the pop-up only from the event's own callout; a top-up of a
        // held stack is one Add of the units that joined it, under the held stack's guid, and its pop-up names those units)
        std::vector<bootstrap::ItemEvent> events = std::move(job.events);
        add_item_toasts(events, /*removals=*/false);
        queue("ClientRecieveBatchedStorageEvents", [events = std::move(events)](bootstrap::PlayerBootstrap& pb) { return pb.craft_batched_events(events); }, conn, /*must_build=*/true);
        market_answer_removal(conn, guid, kMarketOk);
        spdlog::log(level_for(market_logged_, 20), "[market] {}: took back {} x{} of item {} (listing {}/{}/{}/{}); the fee stays paid", name(), job.quantity, job.listing.item_name,
                    job.listing.item.item_record_id, guid.a, guid.b, guid.c, guid.d);
        return;
    }
    market_undo(job);
    const uint8_t code = market_refusal_code(answer);
    market_answer_removal(conn, guid, code);
    if (answer.status == svc::CallStatus::Ok && answer.outcome == svc::MarketOutcome::ListingExpired && world_) world_->market_listings()->sweep_now(guid);
    spdlog::log(level_for(market_refused_logged_), "[market] {}: the removal was refused ({}: {}, outcome {}): the bags stay as they were; answered {}", name(), svc::call_status_name(answer.status),
                answer.reason, static_cast<int>(answer.outcome), static_cast<int>(code));
}

}  // namespace aoc::worldd

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/market_requests.inc (see market.h)
//
//  The window's calls: ServerRequestMarketplaceItemRegistration takes the listed units from the seller's stack and answers
//  with the listing; ServerRequestMarketplaceHeaders answers the storefront page; ServerRequestMarketplaceData answers one
//  seller's listings.  The listings are the world's (market/listings.h), and the answers' bits are market_rpc.h's.
// ============================================================================
#include "market/fee.h"
#include "market/give_back.h"
#include "market/listings.h"
#include "market/market.h"
#include "market/undo.h"

#include "bootstrap/bootstrap.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "items/wallet.h"
#include "protocol/repl/market_rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "session/player_session.h"
#include "world/city_nodes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// EMarketplaceResponse: the refusals it answers with, and why.
constexpr uint8_t kErrorGeneric = 1;        // the stack left the bag while the listing was made, or the seller holds 10 listings
constexpr uint8_t kRequestInProgress = 3;   // a listing of the seller's is still being saved
constexpr uint8_t kItemNotFound = 5;        // the seller does not hold the stack the call names
constexpr uint8_t kItemUntradable = 6;      // a quest item is not listed
constexpr uint8_t kItemInUse = 7;           // a worn item is not listed
constexpr uint8_t kInvalidQuantity = 8;     // zero units, or more than the stack holds
constexpr uint8_t kCurrencyNotFound = 13;   // the price names no money, or the purse does not hold the listing fee, or the price and the tax of a purchase
constexpr uint8_t kMarketOk = 0;            // a step that is stored
constexpr uint8_t kInvalidPlayer = 9;       // a purchase of the buyer's own listing
constexpr uint8_t kInventoryFull = 12;      // the bags have no room for the units a purchase or a removal brings

/// The node of the Manager whose window the player opened, when `guid` names that market and the player, at (x, y, z), is within retail's 1000 units of him.  nullopt for
/// no window, another market or a player who walked away; `why` then says which.
std::optional<int64_t> opened_market_node(const market::MarketWindow& window, const world::ActorRegistry* registry, const world::CityNodes* nodes,
                                          const repl::GuidValue& guid, double x, double y, double z, std::string& why) {
    if (window.manager == 0 || !registry) {
        why = "no market window is open";
        return std::nullopt;
    }
    const world::WorldActor* manager = registry->find(window.manager);
    if (!manager) {
        why = "the Manager of the open window is gone";
        return std::nullopt;
    }
    int64_t node = market::node_of(manager->actor);
    if (node == 0 && nodes) {
        const world::NodeSite site = nodes->node_at(manager->x, manager->y);
        if (const world::CityNodeInfo* info = nodes->by_rank(site.node_rank)) node = info->guid;
    }
    if (node == 0 || node != window.node) {
        why = "the Manager stands in no node, or another one than the window's";
        return std::nullopt;
    }
    if (market::marketplace_guid(node) != guid) {
        why = "the call names another market than the open window's";
        return std::nullopt;
    }
    const double dx = manager->x - x, dy = manager->y - y, dz = manager->z - z;
    const double distance_sq = dx * dx + dy * dy + dz * dz;
    if (distance_sq > market::kWindowRangeCm * market::kWindowRangeCm) {
        why = "the Manager is " + std::to_string(static_cast<int>(std::sqrt(distance_sq) / 100.0)) + " m away";
        return std::nullopt;
    }
    return node;
}

repl::GuidValue character_guid(const bootstrap::PlayerSetup& setup) {
    return repl::GuidValue{setup.character_guid[0], setup.character_guid[1], setup.character_guid[2], setup.character_guid[3]};
}

bool no_guid(const repl::GuidValue& g) { return (g.a | g.b | g.c | g.d) == 0; }

/// The name the client shows for an item record (the record's own when the data has none).
std::string item_name_of(const content::ItemRecord& record) {
    return record.display_name.empty() ? record.name : record.display_name;
}

/// The copper a price names, 0 when it names no Money amount (a zero or another currency).
int64_t copper_of(const repl::CurrencyContainer& price) {
    for (const repl::CurrencyEntry& e : price.entries) {
        if (e.tier == items::kMoneyTier) return e.amount > 0 ? e.amount : 0;
    }
    return 0;
}

/// A stored listing as the wire carries it.
repl::MarketListing wire_listing(const market::Listing& l) {
    repl::MarketListing out;
    out.node = l.node;
    out.seller = l.seller;
    out.seller_name = l.seller_name;
    out.guid = l.guid;
    out.item = items::to_instance(l.item);
    out.item_name = l.item_name;
    if (l.price_per_unit > 0) out.price.entries.push_back({items::kMoneyTier, repl::kMoneyTierType, l.price_per_unit});
    out.issue_date = l.issue_date;
    out.expiration_date = l.expiration_date;
    out.status = l.status;
    return out;
}

/// A storefront as its header on a page.
repl::MarketHeader header_of(int64_t node, const market::Storefront& sf) {
    repl::MarketHeader h;
    h.node = node;
    h.seller = sf.seller;
    h.seller_name = sf.seller_name;
    h.description = sf.seller_name + "'s Storefront";
    h.issue_date = sf.issue_date;
    return h;
}

}  // namespace

void PlayerSession::on_market_registration(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_ || !world_) return;
    const std::optional<repl::MarketRegistration> request = repl::parse_market_registration(rpc.call);
    if (!request) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceItemRegistration does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    if (!within_budget(item_budget_, "ServerRequestMarketplaceItemRegistration", now) || !room_for(4)) return;
    const auto ignore = [&](const std::string& why) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a listing of {} x{} is ignored: {}", name(), request->item.record_id, request->quantity, why);
    };
    const auto refuse = [&](uint8_t response, const char* why) {
        spdlog::debug("[market] {}: a listing of {} x{} is refused ({}): {}", name(), request->item.record_id, request->quantity, why, static_cast<int>(response));
        queue("ClientMarketplaceItemRegistrationResponse", [listing = wire_listing(market::Listing{}), response](bootstrap::PlayerBootstrap& pb) {
            return pb.market_registration_answer(listing, response);
        }, conn, /*must_build=*/true);
        market_resync(conn, now);   // the client's bags and window show what the server holds, whatever it showed when it asked
    };

    // where the player stands now (the last good position), not where the last periodic save put him
    const GoodPosition at = checker_.good();
    std::string why_not;
    const std::optional<int64_t> node = opened_market_node(pb_.market_window, &world_->registry(), world_->city_nodes(), request->market, at.x, at.y, at.z, why_not);
    if (!node) return ignore(why_not);
    market::MarketStore* store = world_->market_listings();
    if (!store->loaded()) return refuse(kErrorGeneric, "the saved listings are not loaded yet");
    const repl::GuidValue seller = character_guid(pb_.setup());
    // one listing save in flight for a character: a second listing waits for the first one's answer, because both write the same bags and purse
    if (!market_pending_.empty()) return refuse(kRequestInProgress, "a listing of this session is still being saved");
    // a mail claim writes the same bags on the mail worker, and a listing the seller left in flight when a session ended is still being finished: the later of two writes
    // of the bags would win from an older copy
    if (!mail_claim_in_flight_.empty()) return refuse(kErrorGeneric, "a mail claim is writing the bags");
    // a log-in of this character, redeemed and not applied yet, waits for the saves the world holds and then reads the bags: a listing made now would be saved under them, as a mail
    // claim would (mail_claims_held).  The store looks again when it takes the save (submit_listing), so a log-in that marks the character in between is seen too.
    if (world_->login_pending(character_id_)) return refuse(kRequestInProgress, "a log-in of this character is waiting");
    if (store->busy(seller)) return refuse(kRequestInProgress, "a listing of the seller's earlier session is still being saved");
    const svc::InventoryItem* held = inventory_.find_guid(items::guid_hex(request->item.guid));
    if (!held || held->item_record_id != static_cast<int64_t>(request->item.record_id)) return refuse(kItemNotFound, "the seller does not hold that stack");
    const std::string stack_guid = held->instance_guid;
    if (market_stacks_in_flight_.count(stack_guid) != 0) return refuse(kItemNotFound, "the stack is already being listed");
    if (!market::sellable_container(held->container)) {
        if (held->container == svc::ItemContainer::Quest) return refuse(kItemUntradable, "a quest item is not listed");
        if (held->container == svc::ItemContainer::Equipment) return refuse(kItemInUse, "a worn item is not listed");
        return refuse(kItemNotFound, "the stack is in a container the Sell tab does not list from");
    }
    if (request->quantity < 1 || request->quantity > held->quantity) return refuse(kInvalidQuantity, "the stack does not hold that many units");
    const int64_t copper = copper_of(request->price);
    if (copper <= 0) return refuse(kCurrencyNotFound, "the price names no money");
    if (store->count(seller) >= market::kMaxListingsPerSeller) return refuse(kErrorGeneric, "the seller holds 10 active listings");
    // the fee uses the server's own stack and record: its rarity at the stack's quality, the record's Level and the listed quantity and unit price
    const content::ContentDb* content = world_->content();
    std::string error;
    const std::optional<content::ItemRecord> record = content ? content->item(held->item_record_id, error) : std::nullopt;
    if (!record) return refuse(kItemNotFound, error.empty() ? "the stack's record is not in content.db" : error.c_str());
    const int64_t fee = market::listing_fee(*record, held->quality, record->level, request->quantity, copper);
    if (!wallet_.can_pay(fee)) return refuse(kCurrencyNotFound, "the purse does not hold the listing fee");

    market::Listing listing;
    listing.node = *node;
    listing.seller = seller;
    listing.seller_name = name();
    const std::optional<repl::GuidValue> guid = items::guid_of(inventory_.fresh_guid());
    if (!guid) return refuse(kErrorGeneric, "no listing guid could be made");
    listing.guid = *guid;
    listing.item = *held;
    listing.item.quantity = request->quantity;
    if (request->quantity < held->quantity) {
        // a part of a stack leaves as its own instance; a whole stack keeps the stack's own guid
        const std::optional<repl::GuidValue> instance = items::guid_of(inventory_.fresh_guid());
        if (!instance) return refuse(kErrorGeneric, "no item guid could be made");
        listing.item.instance_guid = items::guid_hex(*instance);
    }
    listing.item_name = item_name_of(*record);
    listing.price_per_unit = copper;
    listing.issue_date = sync_now(now);
    listing.expiration_date = listing.issue_date + market::kListingSeconds;
    listing.status = market::kListed;
    const int64_t issue_ms = wall_ms();
    const int64_t expiry_ms = issue_ms + static_cast<int64_t>(market::kListingSeconds * 1000.0);
    const int64_t purse_before = wallet_.copper();
    const svc::InventoryItem taken = *held;   // the stack as it is before the units leave it (the pointer does not survive the consume)

    const items::Outcome outcome = inventory_.consume(held->container, held->slot, held->instance_guid, request->quantity);
    if (!outcome.ok) {
        spdlog::warn("[market] {}: the stack could not be taken: {}", name(), outcome.refusal);
        return refuse(kErrorGeneric, "the stack could not be taken");
    }
    wallet_.pay(fee);   // checked above: it cannot be short
    inventory_dirty_ = true;
    inventory_changed_ = true;

    PendingMarket job;
    job.listing = listing;
    job.taken = taken;
    job.quantity = request->quantity;
    job.stack_guid = stack_guid;
    job.fee = fee;
    job.events = item_events_for(outcome);
    market::ListingSave save;
    save.listing = listing;
    save.issue_date_ms = issue_ms;
    save.expiry_date_ms = expiry_ms;
    save.fee = fee;
    save.items = inventory_.items();
    save.wallet = wallet_.entries();
    save.jobs = jobs_;
    save.recipes = taught_;
    save.warehouse = wh_storage_.rows();
    job.ticket = store->submit_listing(std::move(save));
    if (job.ticket == 0) {
        market_refund(job);
        return refuse(kErrorGeneric, "the listing could not be saved");
    }
    market_stacks_in_flight_.insert(stack_guid);
    market_save_in_flight_ = true;
    market_pending_.push_back(std::move(job));
    spdlog::debug("[market] {}: listing {} x{} of item {} for a {} copper fee is being saved", name(), request->quantity, listing.item_name,
                  listing.item.item_record_id, fee);
}

/// After a refusal the client is told what the server holds: its bags, and with a window open its own listings at that market.
void PlayerSession::market_resync(Connection& conn, Clock::time_point now) {
    resync_storage(conn, now);
    if (pb_.market_window.manager == 0 || !world_ || !room_for(1)) return;
    const repl::GuidValue seller = character_guid(pb_.setup());
    std::vector<repl::MarketListing> listings;
    for (const market::Listing& l : world_->market_listings()->listings(pb_.market_window.node, seller)) listings.push_back(wire_listing(l));
    queue("ClientReceiveMarketplaceData", [seller, listings](bootstrap::PlayerBootstrap& pb) { return pb.market_data(seller, listings); }, conn, /*must_build=*/true);
}

bool PlayerSession::market_hold_bag_save(const char* why) {
    if (!market_save_in_flight_) return false;
    // a listing's save writes these bags and the purse in one transaction: a second bag save must not be written before it or after it out of order
    market_bag_held_ = true;
    inventory_dirty_ = true;
    spdlog::debug("[session] {}: a bag save is held while a listing's save is in flight ({})", name(), why);
    return true;
}

void PlayerSession::run_market(Connection& conn, Clock::time_point now) {
    if (!world_) return;
    market::MarketStore* store = world_->market_listings();
    while (!market_pending_.empty()) {
        market::ListingSaveAnswer answer;
        if (!store->take_listing(market_pending_.front().ticket, answer)) break;   // one worker answers in order
        PendingMarket job = std::move(market_pending_.front());
        market_pending_.pop_front();
        market_landed(conn, now, std::move(job), answer);
    }
}

void PlayerSession::market_refund(const PendingMarket& job) {
    // exactly the units the listing took and its fee, to the rest of the stack wherever the seller has put it since (market/give_back.h)
    if (!market::give_back(inventory_, wallet_, item_catalog(), job)) {
        spdlog::error("[market] {}: the stack {} x{} could not be given back", name(), job.taken.instance_guid, job.quantity);
    }
    inventory_dirty_ = true;
    inventory_changed_ = true;
}

void PlayerSession::market_end(const char* why) {
    // the steps of a cart that never started hold nothing and have changed nothing: they are dropped, and no answer is owed to a client that is gone
    if (!market_queue_.empty()) {
        spdlog::info("[market] {}: the session ends with {} queued step(s) that never started ({}): dropped, nothing paid", name(), market_queue_.size(), why);
        market_queue_.clear();
    }
    if (market_pending_.empty()) return;
    // the saves still in flight, and the bags the session ends with (the held bag save was never written), go to the world: it takes each answer, makes the listing or gives
    // back what it took, and writes the bags after them
    market::HandOver over;
    over.character_id = character_id_;
    over.seller = character_guid(pb_.setup());
    for (PendingMarket& job : market_pending_) {
        if (job.kind != market::StepKind::Register) {
            // a purchase or a removal that is refused after this session ended leaves the bags as they would be without it: what it placed taken out, the price and the tax back
            items::Inventory without = inventory_;
            market::undo_added(without, job.added.events);
            items::Wallet purse(wallet_.entries());
            if (job.paid > 0) purse.give(job.paid);
            job.without.items = without.items();
            job.without.wallet = purse.entries();
            job.without.jobs = jobs_;
            job.without.recipes = taught_;
            job.without.warehouse = wh_storage_.rows();
        }
        over.listings.push_back(job);
    }
    over.bags.items = inventory_.items();
    over.bags.wallet = wallet_.entries();
    over.bags.jobs = jobs_;
    over.bags.recipes = taught_;
    over.bags.warehouse = wh_storage_.rows();
    spdlog::info("[market] {}: the session ends with {} listing save(s) in flight ({}): the world takes their answers and writes the bags", name(), market_pending_.size(), why);
    market_pending_.clear();
    market_stacks_in_flight_.clear();
    market_save_in_flight_ = false;
    market_bag_held_ = false;
    if (world_) world_->market_listings()->hand_over(std::move(over));
}

void PlayerSession::market_send_listed(Connection& conn, const PendingMarket& job, Clock::time_point now) {
    (void)now;
    queue("ClientReceiveStorageEvent", [events = job.events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn, /*must_build=*/true);
    queue("ClientMarketplaceItemRegistrationResponse", [listed = wire_listing(job.listing)](bootstrap::PlayerBootstrap& pb) {
        return pb.market_registration_answer(listed, 0);
    }, conn, /*must_build=*/true);
    // the money path of a vendor buy: the popup of minus the fee, then the new balance in the pawn's Currencies
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    queue("ClientShowCurrencyCallout", [fee = job.fee](bootstrap::PlayerBootstrap& pb) { return pb.currency_callout(items::kMoneyTier, -fee); }, conn, /*must_build=*/true);
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
}

void PlayerSession::market_landed(Connection& conn, Clock::time_point now, PendingMarket job, const market::ListingSaveAnswer& answer) {
    market_stacks_in_flight_.erase(job.stack_guid);
    if (job.kind == market::StepKind::Buy) {
        market_purchase_landed(conn, std::move(job), answer);
    } else if (job.kind == market::StepKind::Remove) {
        market_removal_landed(conn, std::move(job), answer);
    } else if (answer.stored()) {
        // (the store joined the listing to the world's cache when it gave this answer)
        market_send_listed(conn, job, now);
        spdlog::log(level_for(market_logged_, 20), "[market] {}: listed {} x{} of item {} at {} copper each for a {} copper fee (listing {}/{}/{}/{} at node {})", name(),
                    job.listing.item.quantity, job.listing.item_name, job.listing.item.item_record_id, job.listing.price_per_unit, job.fee, job.listing.guid.a,
                    job.listing.guid.b, job.listing.guid.c, job.listing.guid.d, job.listing.node);
    } else {
        // a clear refusal (the store asks again until the answer is clear): the stack and the fee go back to the seller
        market_refund(job);
        queue("ClientMarketplaceItemRegistrationResponse", [listing = wire_listing(market::Listing{})](bootstrap::PlayerBootstrap& pb) {
            return pb.market_registration_answer(listing, kErrorGeneric);
        }, conn, /*must_build=*/true);
        market_resync(conn, now);   // the stack may sit in another slot than where the client saw it leave
        spdlog::log(level_for(market_refused_logged_), "[market] {}: the listing was refused ({}: {}): the stack and the fee stay the seller's", name(),
                    svc::call_status_name(answer.status), answer.reason);
    }
    market_run_queue(conn, now);   // the next line of a cart starts from the bags and purse this step left, before a held bag save is let go
    market_save_in_flight_ = !market_pending_.empty();
    if (!market_save_in_flight_ && market_bag_held_) {
        market_bag_held_ = false;
        hand_over_inventory("a listing's save landed", false);
    }
}

/// The storefront page of the market the player has open: one header per seller, and the version the client knows.
void PlayerSession::on_market_headers(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    if (dead_ || !world_) return;
    const std::optional<repl::MarketHeadersRequest> request = repl::parse_market_headers(rpc.call);
    if (!request) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceHeaders does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        return;
    }
    const GoodPosition at = checker_.good();   // where the player stands now, not where the last periodic save put him
    std::string why_not;
    const std::optional<int64_t> node = opened_market_node(pb_.market_window, &world_->registry(), world_->city_nodes(), request->market, at.x, at.y, at.z, why_not);
    if (!node) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceHeaders is ignored: {}", name(), why_not);
        return;
    }
    if (!room_for(1)) return;

    std::vector<repl::MarketHeader> headers;
    for (const market::Storefront& sf : world_->market_listings()->storefronts(*node)) headers.push_back(header_of(*node, sf));
    spdlog::log(level_for(market_answered_logged_, 20), "[market] {}: the storefront page of node {} is sent: {} storefront(s), the client holds version {}", name(), *node, headers.size(),
                request->version);
    queue("ClientReceiveMarketplaceHeaders", [node = *node, headers](bootstrap::PlayerBootstrap& pb) { return pb.market_headers(node, headers, true); }, conn, /*must_build=*/true);
}

/// The storefront pages the window's open is followed by, as the game's own server sends them without being asked: the other sellers' storefronts, 100 a page, bComplete on the
/// last page, which is empty when nobody else sells at the market.  The window's tabs learn their market only from the last page (a click on the Listings tab before it asks
/// with no market), so one is always sent.  The player's own storefront is not among them: the Listings tab shows the player's own listings.
void PlayerSession::push_storefront_pages(Connection& conn, int64_t node) {
    if (!world_) return;
    const repl::GuidValue self = character_guid(pb_.setup());
    std::vector<repl::MarketHeader> others;
    for (const market::Storefront& sf : world_->market_listings()->storefronts(node)) {
        if (!(sf.seller == self)) others.push_back(header_of(node, sf));
    }
    const size_t total = others.size();
    if (others.size() > market::kMaxPushedStorefronts) others.resize(market::kMaxPushedStorefronts);   // the Refresh button and the search reach the rest
    const size_t pages = std::max<size_t>(1, (others.size() + market::kStorefrontPageSize - 1) / market::kStorefrontPageSize);
    if (!room_for(pages)) return;
    for (size_t p = 0; p < pages; ++p) {
        const size_t first = p * market::kStorefrontPageSize;
        const size_t last = std::min(others.size(), first + market::kStorefrontPageSize);
        std::vector<repl::MarketHeader> page(others.begin() + static_cast<std::ptrdiff_t>(first), others.begin() + static_cast<std::ptrdiff_t>(last));
        const bool complete = p + 1 == pages;
        queue("ClientReceiveMarketplaceHeaders", [node, page = std::move(page), complete](bootstrap::PlayerBootstrap& pb) { return pb.market_headers(node, page, complete); }, conn,
              /*must_build=*/true);
    }
    spdlog::log(level_for(market_answered_logged_, 20), "[market] {}: the open of node {} pushes {} page(s) of storefronts, {} of the {} other storefront(s)", name(), node, pages,
                others.size(), total);
}

void PlayerSession::on_market_data(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)now;
    if (dead_ || !world_) return;
    const repl::RpcCall& call = rpc.call;
    if (!call.decoded || !call.rpc) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceData does not read ({} parameter bits): ignored", name(), call.param_bits);
        return;
    }
    const std::optional<repl::GuidValue> market_guid = repl::market_rpc_detail::read_guid(*call.rpc, call.params, "MarketplaceActorGuid");
    const GoodPosition at = checker_.good();   // where the player stands now, not where the last periodic save put him
    std::string why_not = "the call names no market";
    const std::optional<int64_t> node = market_guid ? opened_market_node(pb_.market_window, &world_->registry(), world_->city_nodes(), *market_guid, at.x, at.y, at.z, why_not) : std::nullopt;
    if (!node) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceData is ignored: {}", name(), why_not);
        return;
    }

    const std::optional<repl::GuidValue> seller = repl::market_rpc_detail::read_guid(*call.rpc, call.params, "PlayerGuid");
    if (!seller || no_guid(*seller)) {
        spdlog::log(level_for(market_ignored_logged_), "[market] {}: a ServerRequestMarketplaceData is ignored: the call names no seller", name());   // there is nothing to send
        return;
    }
    if (!room_for(1)) return;
    std::vector<repl::MarketListing> listings;
    for (const market::Listing& l : world_->market_listings()->listings(*node, *seller)) listings.push_back(wire_listing(l));
    spdlog::log(level_for(market_answered_logged_, 20), "[market] {}: the data of seller {}/{}/{}/{} at node {} is sent: {} listing(s)", name(), seller->a, seller->b, seller->c, seller->d, *node,
                listings.size());
    queue("ClientReceiveMarketplaceData", [seller = *seller, listings](bootstrap::PlayerBootstrap& pb) { return pb.market_data(seller, listings); }, conn, /*must_build=*/true);
}

/// The client calls of the marketplace (session/rpc_table.h).
void PlayerSession::register_market_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceItemRegistration", [](PlayerSession& s, const RpcContext& c) { s.on_market_registration(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceHeaders", [](PlayerSession& s, const RpcContext& c) { s.on_market_headers(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceData", [](PlayerSession& s, const RpcContext& c) { s.on_market_data(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceItemPurchase", [](PlayerSession& s, const RpcContext& c) { s.on_market_purchase(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestMarketplaceItemRemoval", [](PlayerSession& s, const RpcContext& c) { s.on_market_removal(c.conn, c.rpc, c.now); });
}

}  // namespace aoc::worldd

namespace aoc::worldd::bootstrap {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

std::optional<Send> PlayerBootstrap::market_registration_answer(const repl::MarketListing& listing, uint8_t response) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientMarketplaceItemRegistrationResponse");
    if (!rpc) return std::nullopt;
    const std::optional<repl::RpcParams> params = repl::market_registration_params(*rpc, listing, response);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientMarketplaceItemRegistrationResponse";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientMarketplaceItemRegistrationResponse", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::market_purchase_answer(const repl::GuidValue& listing, int32_t quantity, uint8_t response) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientMarketplaceItemPurchaseResponse");
    if (!rpc) return std::nullopt;
    const std::optional<repl::RpcParams> params = repl::market_purchase_params(*rpc, listing, quantity, response);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientMarketplaceItemPurchaseResponse";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientMarketplaceItemPurchaseResponse", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::market_removal_answer(const repl::GuidValue& listing, uint8_t response) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientMarketplaceItemRemovalResponse");
    if (!rpc) return std::nullopt;
    const std::optional<repl::RpcParams> params = repl::market_removal_params(*rpc, listing, response);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientMarketplaceItemRemovalResponse";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientMarketplaceItemRemovalResponse", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::market_headers(int64_t node, std::span<const repl::MarketHeader> headers, bool complete) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveMarketplaceHeaders");
    if (!rpc) return std::nullopt;
    const std::optional<repl::RpcParams> params = repl::market_headers_params(*rpc, node, headers, complete);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientReceiveMarketplaceHeaders";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientReceiveMarketplaceHeaders", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::market_data(const repl::GuidValue& seller, std::span<const repl::MarketListing> listings) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveMarketplaceData");
    if (!rpc) return std::nullopt;
    const std::optional<repl::RpcParams> params = repl::market_data_params(*rpc, seller, listings);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientReceiveMarketplaceData";
    call.params = *params;
    return rpcs(Role::PlayerController, "ClientReceiveMarketplaceData", std::span<const repl::RpcRequest>(&call, 1));
}

}  // namespace aoc::worldd::bootstrap

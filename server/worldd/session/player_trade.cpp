// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_trade.cpp
//
//  PlayerSession side of player-to-player trading: request, response, window
//  open, live validated offer updates, lock/unlock/confirm, atomic two-character
//  swap settlement, and logout/relog persistence barriers.
// ============================================================================
#include "session/player_session.h"

#include "items/inventory.h"
#include "items/wallet.h"
#include "protocol/repl/trade_rpc.h"
#include "session/item_cast.h"
#include "session/player_session_util.h"
#include "session/player_world.h"
#include "session/rpc_table.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

bool allowed_trade_container(svc::ItemContainer c) {
    return c == svc::ItemContainer::Backpack || items::is_resource(c);
}

std::vector<bootstrap::ItemEvent> trade_wire_events(const std::vector<items::Event>& events) {
    std::vector<bootstrap::ItemEvent> out;
    out.reserve(events.size());
    for (const items::Event& e : events) {
        bootstrap::ItemEvent ev;
        ev.type = e.type;
        ev.storage = bootstrap::item_storage_of(e.container);
        ev.slot = e.slot;
        ev.item = items::to_instance(e.item);
        ev.instigated = e.instigated;
        ev.count = e.count;
        out.push_back(std::move(ev));
    }
    return out;
}

}  // namespace

class PlayerSession::TradePeerAdapter final : public trade::TradePeer {
public:
    explicit TradePeerAdapter(PlayerSession& session) : session_(session) {}

    std::string character_id() const override { return session_.character_id_; }
    bool        can_commit_trade() const override { return session_.can_commit_trade(); }
    bool        collect_trade_offer(const trade::OfferSide& mine, std::vector<svc::InventoryItem>& out_offered) const override {
        return session_.collect_trade_offer(mine, out_offered);
    }
    trade::PlanError plan_trade_side(const trade::OfferSide& mine, const std::vector<svc::InventoryItem>& incoming_items,
                                     int64_t incoming_copper, trade::TradeSidePlan& out_plan) const override {
        return session_.plan_trade_side(mine, incoming_items, incoming_copper, out_plan);
    }
    trade::TradeBeforeSnapshot capture_before_trade_snapshot() const override {
        return session_.capture_before_trade_snapshot();
    }
    void apply_committed_trade(const trade::TradeSidePlan& plan) override {
        session_.apply_committed_trade(plan);
    }
    void on_trade_commit_aborted() override {
        session_.on_trade_commit_aborted();
    }
    void on_trade_commit_undecided() override {
        session_.on_trade_commit_undecided();
    }

private:
    PlayerSession& session_;
};

void PlayerSession::register_trade_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerRequestTrade",
          [](PlayerSession& s, const RpcContext& c) { s.on_trade_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerRequestTradeResponse",
          [](PlayerSession& s, const RpcContext& c) { s.on_trade_request_response(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerUpdateTradeItems",
          [](PlayerSession& s, const RpcContext& c) { s.on_trade_update_items(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerUpdateTradeTransaction",
          [](PlayerSession& s, const RpcContext& c) { s.on_trade_transaction(c.conn, c.rpc, c.now); });
}

bool PlayerSession::trade_commit_pending() const {
    if (trade_commit_left_in_flight_ || trade_undecided_) return true;
    return trade_book_ && trade_book_->is_committing(pb_.setup().character_guid, pb_.setup().guids.pawn.ObjectId);
}

bool PlayerSession::trade_defers_hand_over(const char* why, bool periodic) {
    (void)why;
    (void)periodic;
    if (trade_undecided_) return true;
    if (trade_commit_pending()) {
        trade_bag_held_ = true;
        inventory_dirty_ = true;
        return true;
    }
    return false;
}

void PlayerSession::connect_trade(PlayerWorld& world) {
    trade_book_ = world.trade_book();
    if (!trade_book_) return;
    if (!trade_peer_) trade_peer_ = std::make_unique<TradePeerAdapter>(*this);
    trade_book_->connect(pb_.setup().character_guid, name(), pb_.setup().guids.pawn.ObjectId, trade_peer_.get());
    const GoodPosition g = checker_.good();
    trade_book_->update_position(pb_.setup().character_guid, pb_.setup().guids.pawn.ObjectId, g.x, g.y, g.z);
}

void PlayerSession::leave_trade(Clock::time_point now) {
    if (trade_book_) {
        const uint64_t pawn = pb_.setup().guids.pawn.ObjectId;
        if (trade_book_->is_committing(pb_.setup().character_guid, pawn)) {
            trade_commit_left_in_flight_ = true;
        }
        const double world_now = pb_.setup().world_time + session_seconds(now);
        trade_book_->disconnect(pb_.setup().character_guid, world_now, pawn);
    }
    trade_book_.reset();
    trade_peer_.reset();
    trade_in_progress_ = false;
}

void PlayerSession::sync_trade_status(Clock::time_point now) {
    if (!trade_book_) return;
    const uint64_t pawn = pb_.setup().guids.pawn.ObjectId;
    const GoodPosition g = checker_.good();
    trade_book_->update_position(pb_.setup().character_guid, pawn, g.x, g.y, g.z);
    trade::PlayerStatus st;
    st.phased_in = phase_in_done_;
    st.dead = dead_;
    st.in_combat = combat_flag_ || pvp_combat_;
    st.corrupted = corruption_ > 0.0;
    const double world_now = pb_.setup().world_time + session_seconds(now);
    trade_book_->update_status(pb_.setup().character_guid, pawn, st, world_now);
}

void PlayerSession::trade_interrupted(Clock::time_point now, const char* why) {
    if (!trade_book_) return;
    sync_trade_status(now);
    trade_book_->interrupt(pb_.setup().character_guid, why, pb_.setup().guids.pawn.ObjectId);
}

void PlayerSession::reserve_item_for_test(int32_t cast_id, const std::string& instance_guid) {
    auto cast = std::make_shared<ItemCast>();
    cast->instance_guid = instance_guid;
    item_reserved_[cast_id] = std::move(cast);
}

bool PlayerSession::trade_item_reserved(const std::string& instance_guid) const {
    if (market_stacks_in_flight_.count(instance_guid) != 0) return true;
    for (const auto& [id, cast] : item_reserved_) {
        if (cast && cast->instance_guid == instance_guid) return true;
    }
    return false;
}

bool PlayerSession::validate_trade_offer(const repl::TradeOffer& incoming,
                                         std::vector<repl::ItemInstance>& out_items,
                                         std::vector<trade::OfferedItemSnapshot>& out_snapshots,
                                         int64_t& out_copper,
                                         std::string& refusal) const {
    out_items.clear();
    out_snapshots.clear();
    out_copper = 0;

    if (incoming.items.size() > trade::kMaxTradeTotalStacks) {
        refusal = "exceeds total trade stack limit";
        return false;
    }

    // Validate currency container: empty (0 copper) or a single Money-tier entry within wallet and cap.
    if (!incoming.money.entries.empty()) {
        if (incoming.money.entries.size() != 1) {
            refusal = "multiple currency entries are not allowed";
            return false;
        }
        const repl::CurrencyEntry& e = incoming.money.entries[0];
        if (e.tier != items::kMoneyTier || e.type_id != repl::kTradeMoneyTierType) {
            refusal = "non-money currency tier is not tradeable";
            return false;
        }
        if (e.amount < 0 || e.amount > items::kMoneyCap || e.amount > wallet_.copper()) {
            refusal = "offered copper is negative, over cap, or exceeds wallet balance";
            return false;
        }
        out_copper = e.amount;
    }

    // Validate every offered stack against live authoritative inventory and catalog rules.
    const items::ItemCatalog& cat = item_catalog();
    std::set<std::string> seen_guids;
    size_t backpack_stacks = 0;
    out_items.reserve(incoming.items.size());
    out_snapshots.reserve(incoming.items.size());

    for (const repl::ItemInstance& req_item : incoming.items) {
        if (!req_item.has_item) {
            refusal = "empty item instance in offer";
            return false;
        }
        const std::string hex = items::guid_hex(req_item.guid);
        if (!items::guid_of(hex)) {
            refusal = "invalid item instance guid";
            return false;
        }
        if (!seen_guids.insert(hex).second) {
            refusal = "duplicate item instance guid in offer";
            return false;
        }
        const svc::InventoryItem* held = inventory_.find_guid(hex);
        if (!held) {
            refusal = "item instance not found in inventory";
            return false;
        }
        if (!allowed_trade_container(held->container)) {
            refusal = "equipped or quest-storage items cannot be traded";
            return false;
        }
        if (held->container == svc::ItemContainer::Backpack) {
            if (++backpack_stacks > trade::kMaxTradeBackpackStacks) {
                refusal = "exceeds 12 backpack trade slots";
                return false;
            }
        }
        if (trade_item_reserved(hex)) {
            refusal = "reserved or in-flight item cannot be traded";
            return false;
        }
        const auto info = cat.info(held->item_record_id);
        if (!info || !info->tradeable || info->quest || info->cargo_crate()) {
            refusal = "item is untradeable, quest-bound, or a cargo crate";
            return false;
        }
        if (req_item.record_id != held->item_record_id ||
            req_item.quantity != held->quantity ||
            req_item.quality != held->quality) {
            refusal = "offered item fields do not match authoritative inventory";
            return false;
        }
        out_items.push_back(items::to_instance(*held));
        out_snapshots.push_back(trade::OfferedItemSnapshot{
            held->instance_guid,
            held->container,
            held->slot,
            held->item_record_id,
            held->quantity,
            held->quality,
        });
    }
    return true;
}

bool PlayerSession::verify_active_trade_offer(const trade::OfferSide& side) const {
    if (side.copper < 0 || side.copper > items::kMoneyCap || wallet_.copper() < side.copper) return false;
    const items::ItemCatalog& cat = item_catalog();
    for (const trade::OfferedItemSnapshot& snap : side.snapshots) {
        const svc::InventoryItem* held = inventory_.find_guid(snap.instance_guid);
        if (!held) return false;
        if (!allowed_trade_container(held->container)) return false;
        if (held->container != snap.container || held->slot != snap.slot) return false;
        if (held->item_record_id != snap.record_id ||
            held->quantity != snap.quantity ||
            held->quality != snap.quality) {
            return false;
        }
        if (trade_item_reserved(snap.instance_guid)) return false;
        const auto info = cat.info(held->item_record_id);
        if (!info || !info->tradeable || info->quest || info->cargo_crate()) return false;
    }
    return true;
}

bool PlayerSession::can_commit_trade() const {
    if (finished_ || failed_ || !phase_in_done_ || dead_ || combat_flag_ || pvp_combat_ || corruption_ > 0.0) return false;
    if (trade_undecided_ || trade_commit_pending()) return false;
    if (!mail_claim_in_flight_.empty()) return false;
    if (!market_pending_.empty() || !market_queue_.empty()) return false;
    if (!world_) return false;
    if (world_->mail_claim_open(character_id_)) return false;
    if (world_->login_pending(character_id_)) return false;
    if (world_->market_saving(character_id_)) return false;
    if (world_->trade_saving(character_id_)) return false;
    return true;
}

bool PlayerSession::collect_trade_offer(const trade::OfferSide& mine, std::vector<svc::InventoryItem>& out_offered) const {
    out_offered.clear();
    if (!verify_active_trade_offer(mine)) return false;
    out_offered.reserve(mine.snapshots.size());
    for (const trade::OfferedItemSnapshot& snap : mine.snapshots) {
        const svc::InventoryItem* held = inventory_.find_guid(snap.instance_guid);
        if (!held) return false;
        out_offered.push_back(*held);
    }
    return true;
}

trade::PlanError PlayerSession::plan_trade_side(const trade::OfferSide& mine,
                                                const std::vector<svc::InventoryItem>& incoming_items,
                                                int64_t incoming_copper,
                                                trade::TradeSidePlan& out_plan) const {
    out_plan = {};
    if (mine.copper < 0 || incoming_copper < 0 || mine.copper > items::kMoneyCap || incoming_copper > items::kMoneyCap) {
        return trade::PlanError::Drifted;
    }

    items::Wallet after_wallet = wallet_;
    if (!after_wallet.pay(mine.copper)) return trade::PlanError::Drifted;
    if (after_wallet.copper() + incoming_copper > items::kMoneyCap) return trade::PlanError::MaxCurrency;
    if (incoming_copper > 0) after_wallet.give(incoming_copper);

    items::Inventory after_inv = inventory_;
    for (const trade::OfferedItemSnapshot& snap : mine.snapshots) {
        const items::Outcome o = after_inv.consume(snap.container, snap.slot, snap.instance_guid, snap.quantity);
        if (!o.ok) return trade::PlanError::Drifted;
        out_plan.removed_events.insert(out_plan.removed_events.end(), o.events.begin(), o.events.end());
    }

    if (!incoming_items.empty()) {
        const items::Outcome g = after_inv.grant(incoming_items, item_catalog());
        if (!g.ok) return trade::PlanError::InventorySpace;
        out_plan.added_events.insert(out_plan.added_events.end(), g.events.begin(), g.events.end());
    }

    out_plan.after_inventory = std::move(after_inv);
    out_plan.after_wallet = std::move(after_wallet);
    out_plan.copper_before = wallet_.copper();
    out_plan.copper_after = out_plan.after_wallet.copper();
    out_plan.net_copper_delta = incoming_copper - mine.copper;

    out_plan.persist.character_id = character_id_;
    out_plan.persist.items = out_plan.after_inventory.items();
    out_plan.persist.wallet = out_plan.after_wallet.entries();
    out_plan.persist.jobs = jobs_;
    out_plan.persist.known_recipes = taught_;
    out_plan.persist.warehouse = wh_storage_.rows();
    return trade::PlanError::None;
}

trade::TradeBeforeSnapshot PlayerSession::capture_before_trade_snapshot() const {
    trade::TradeBeforeSnapshot snap;
    snap.character_id = character_id_;
    snap.inventory_changed = inventory_changed_ || inventory_dirty_ || trade_bag_held_;
    snap.items = inventory_.items();
    snap.wallet = wallet_.entries();
    snap.jobs = jobs_;
    snap.known_recipes = taught_;
    snap.warehouse = wh_storage_.rows();
    return snap;
}

void PlayerSession::apply_committed_trade(const trade::TradeSidePlan& plan) {
    inventory_ = plan.after_inventory;
    wallet_ = plan.after_wallet;
    inventory_dirty_ = false;
    inventory_changed_ = false;
    trade_bag_held_ = false;
    ++items_applied_;
}

void PlayerSession::on_trade_commit_aborted() {
    const bool need_save = trade_bag_held_ || inventory_dirty_ || inventory_changed_;
    trade_bag_held_ = false;
    if (need_save) {
        hand_over_inventory("player trade commit aborted", /*periodic=*/false);
    }
}

void PlayerSession::on_trade_commit_undecided() {
    trade_undecided_ = true;
    trade_bag_held_ = false;
    fail("player trade commit status is undecided");
}

void PlayerSession::on_trade_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!trade_book_) return;
    if (!within_budget(trade_budget_, "ServerRequestTrade", now)) return;
    ++trade_calls_;
    const auto parsed = repl::parse_trade_request(rpc.call);
    if (!parsed) {
        spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: unreadable ServerRequestTrade dropped", name());
        return;
    }
    sync_trade_status(now);
    const double world_now = pb_.setup().world_time + session_seconds(now);
    const uint8_t res = trade_book_->request(pb_.setup().character_guid, trade::from_guid_value(parsed->character_guid), world_now);
    if (res != 0) {
        spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: ServerRequestTrade rejected (code={})", name(), res);
    } else {
        spdlog::log(level_for(trade_logged_, 20), "[trade] {}: trade request sent", name());
    }
}

void PlayerSession::on_trade_request_response(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!trade_book_) return;
    if (!within_budget(trade_budget_, "ServerRequestTradeResponse", now)) return;
    ++trade_calls_;
    const auto parsed = repl::parse_trade_request_response(rpc.call);
    if (!parsed) {
        spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: unreadable ServerRequestTradeResponse dropped", name());
        return;
    }
    sync_trade_status(now);
    const double world_now = pb_.setup().world_time + session_seconds(now);
    const bool matched = trade_book_->respond(pb_.setup().character_guid, trade::from_guid_value(parsed->target_character_guid),
                                              parsed->response, world_now);
    if (!matched) {
        spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: stale ServerRequestTradeResponse dropped", name());
    }
}

void PlayerSession::on_trade_update_items(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!trade_book_) return;
    if (!within_budget(trade_budget_, "ServerUpdateTradeItems", now)) return;
    ++trade_calls_;
    const auto parsed = repl::parse_trade_items_update(rpc.call);
    if (!parsed) {
        spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: unreadable ServerUpdateTradeItems dropped", name());
        return;
    }
    const trade::TradeGuid tg = trade::from_guid_value(parsed->trade_guid);
    const trade::ActiveTrade* active = trade_book_->active_trade(pb_.setup().character_guid);
    if (!active || active->guid != tg) {
        spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: ServerUpdateTradeItems for inactive/wrong trade dropped", name());
        return;
    }
    if (active->committing) return;

    const trade::OfferSide& side = active->side_of(pb_.setup().character_guid);
    if (parsed->offer.version <= side.last_version ||
        parsed->offer.version >= std::numeric_limits<int32_t>::max()) {
        spdlog::log(level_for(trade_refused_logged_, 20),
                    "[trade] {}: out-of-range/stale ServerUpdateTradeItems version {} (last={}) dropped",
                    name(), parsed->offer.version, side.last_version);
        return;
    }

    if (side.state != trade::SideState::Opened) {
        spdlog::log(level_for(trade_refused_logged_, 20),
                    "[trade] {}: ServerUpdateTradeItems rejected while locked; reverting UI at version {}",
                    name(), parsed->offer.version + 1);
        trade_book_->apply_offer(pb_.setup().character_guid, tg, parsed->offer.version, /*valid=*/false, {}, {}, 0);
        return;
    }

    std::vector<repl::ItemInstance> auth_items;
    std::vector<trade::OfferedItemSnapshot> snapshots;
    int64_t copper = 0;
    std::string refusal;
    const bool valid = validate_trade_offer(parsed->offer, auth_items, snapshots, copper, refusal);
    if (!valid) {
        spdlog::log(level_for(trade_refused_logged_, 20),
                    "[trade] {}: ServerUpdateTradeItems rejected ({}); reverting UI at version {}",
                    name(), refusal, parsed->offer.version + 1);
    }
    trade_book_->apply_offer(pb_.setup().character_guid, tg, parsed->offer.version, valid,
                             std::move(auth_items), std::move(snapshots), copper);
}

void PlayerSession::on_trade_transaction(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    (void)conn;
    if (!trade_book_) return;
    if (!within_budget(trade_budget_, "ServerUpdateTradeTransaction", now)) return;
    ++trade_calls_;
    const auto parsed = repl::parse_trade_transaction(rpc.call);
    if (!parsed) {
        spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: unreadable ServerUpdateTradeTransaction dropped", name());
        return;
    }
    const trade::TradeGuid tg = trade::from_guid_value(parsed->trade_guid);
    const double world_now = pb_.setup().world_time + session_seconds(now);
    switch (static_cast<repl::TradeTransactionRequest>(parsed->request)) {
        case repl::TradeTransactionRequest::OpenTrade:
            break;
        case repl::TradeTransactionRequest::CancelTrade: {
            const bool closed = trade_book_->cancel(pb_.setup().character_guid, tg, /*allow_partner_fallback=*/true, "cancelled by player");
            if (!closed) {
                spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: CANCEL_TRADE matched no active trade", name());
            }
            break;
        }
        case repl::TradeTransactionRequest::LockTrade: {
            sync_trade_status(now);
            if (!trade_book_->lock_trade(pb_.setup().character_guid, tg, world_now)) {
                spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: LOCK_TRADE ignored for inactive/locked trade", name());
            }
            break;
        }
        case repl::TradeTransactionRequest::UnlockTrade: {
            sync_trade_status(now);
            if (!trade_book_->unlock_trade(pb_.setup().character_guid, tg, world_now)) {
                spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: UNLOCK_TRADE ignored for inactive/unlocked trade", name());
            }
            break;
        }
        case repl::TradeTransactionRequest::FinalizeTrade: {
            sync_trade_status(now);
            if (!trade_book_->confirm_trade(pb_.setup().character_guid, tg, world_now)) {
                spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: FINALIZE_TRADE ignored for inactive/unready trade", name());
            }
            break;
        }
        default:
            spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: unknown trade transaction request {} dropped",
                        name(), parsed->request);
            break;
    }
}

void PlayerSession::run_trade(Connection& conn, Clock::time_point now) {
    if (!trade_book_ || finished_ || failed_) return;
    sync_trade_status(now);

    if (const trade::ActiveTrade* active = trade_book_->active_trade(pb_.setup().character_guid)) {
        if (!active->committing && !verify_active_trade_offer(active->side_of(pb_.setup().character_guid))) {
            spdlog::log(level_for(trade_refused_logged_, 20), "[trade] {}: offered item or copper drifted mid-trade; auto-cancelling", name());
            trade_book_->interrupt(pb_.setup().character_guid, "offered inventory or currency changed", pb_.setup().guids.pawn.ObjectId);
        }
    }

    for (const trade::TradeNotice& n : trade_book_->take_notices(pb_.setup().character_guid)) {
        switch (n.kind) {
            case trade::TradeNotice::Kind::RequestReceived: {
                const repl::GuidValue from = trade::to_guid_value(n.other_guid);
                queue("ClientRequestTrade", [from](bootstrap::PlayerBootstrap& pb) { return pb.trade_request(from); }, conn, /*must_build=*/true);
                break;
            }
            case trade::TradeNotice::Kind::RequestResponse: {
                const repl::GuidValue target = trade::to_guid_value(n.other_guid);
                const uint8_t code = n.response;
                queue("ClientProcessTradeRequestResponse",
                      [target, code](bootstrap::PlayerBootstrap& pb) { return pb.trade_request_response(target, code); },
                      conn, /*must_build=*/true);
                break;
            }
            case trade::TradeNotice::Kind::BeginTrade: {
                const repl::GuidValue tg = trade::to_guid_value(n.trade_guid);
                const repl::GuidValue partner = trade::to_guid_value(n.other_guid);
                queue("ClientBeginTrade",
                      [tg, partner](bootstrap::PlayerBootstrap& pb) { return pb.trade_begin(tg, partner); },
                      conn, /*must_build=*/true);
                break;
            }
            case trade::TradeNotice::Kind::OfferUpdated: {
                const repl::GuidValue owner = trade::to_guid_value(n.other_guid);
                const repl::TradeOffer offer = n.offer;
                queue("ClientUpdateTrade",
                      [owner, offer](bootstrap::PlayerBootstrap& pb) { return pb.trade_update(owner, offer); },
                      conn, /*must_build=*/true);
                break;
            }
            case trade::TradeNotice::Kind::TradeLocked: {
                const repl::GuidValue who = trade::to_guid_value(n.other_guid);
                queue("ClientLockTrade", [who](bootstrap::PlayerBootstrap& pb) { return pb.trade_lock(who); }, conn, /*must_build=*/true);
                break;
            }
            case trade::TradeNotice::Kind::TradeUnlocked: {
                const repl::GuidValue who = trade::to_guid_value(n.other_guid);
                queue("ClientUnlockTrade", [who](bootstrap::PlayerBootstrap& pb) { return pb.trade_unlock(who); }, conn, /*must_build=*/true);
                break;
            }
            case trade::TradeNotice::Kind::TradeConfirmed: {
                const repl::GuidValue who = trade::to_guid_value(n.other_guid);
                queue("ClientTradeConfirmed", [who](bootstrap::PlayerBootstrap& pb) { return pb.trade_confirmed(who); }, conn, /*must_build=*/true);
                break;
            }
            case trade::TradeNotice::Kind::CommitSucceeded: {
                // Exact completion order:
                //   1. ClientReceiveStorageEvent (only if items moved for this player; Add toasts + actionbar link updates)
                //   2. OwnerInfo.Currencies update (if copper changed)
                //   3. ClientShowCurrencyCallout (if net copper delta != 0)
                //   4. ClientCloseTrade(1 = Success) LAST
                if (!n.commit_wire.removed_events.empty() || !n.commit_wire.added_events.empty()) {
                    std::vector<bootstrap::ItemEvent> removed = trade_wire_events(n.commit_wire.removed_events);
                    std::vector<bootstrap::ItemEvent> added = trade_wire_events(n.commit_wire.added_events);
                    add_item_toasts(added, /*removals=*/false);
                    std::vector<bootstrap::ItemEvent> all;
                    all.reserve(removed.size() + added.size());
                    all.insert(all.end(), std::make_move_iterator(removed.begin()), std::make_move_iterator(removed.end()));
                    all.insert(all.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
                    for (size_t offset = 0; offset < all.size(); offset += bootstrap::kMaxEventsPerChain) {
                        const size_t end = std::min(all.size(), offset + bootstrap::kMaxEventsPerChain);
                        std::vector<bootstrap::ItemEvent> part(all.begin() + static_cast<std::ptrdiff_t>(offset),
                                                               all.begin() + static_cast<std::ptrdiff_t>(end));
                        queue("ClientReceiveStorageEvent",
                              [part = std::move(part)](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); },
                              conn, /*must_build=*/true);
                    }
                    follow_linked_stack(conn, all);
                }
                if (n.commit_wire.copper_changed) {
                    pb_.set_currency(items::kCopperCurrency, n.commit_wire.copper_after);
                    queue("OwnerInfo", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
                }
                if (n.commit_wire.net_copper_delta != 0) {
                    const int64_t delta = n.commit_wire.net_copper_delta;
                    queue("ClientShowCurrencyCallout",
                          [delta](bootstrap::PlayerBootstrap& pb) { return pb.currency_callout(items::kMoneyTier, delta); },
                          conn, /*must_build=*/true);
                }
                queue("ClientCloseTrade",
                      [](bootstrap::PlayerBootstrap& pb) { return pb.trade_close(static_cast<uint8_t>(repl::TradeStatus::Success)); },
                      conn, /*must_build=*/true);
                break;
            }
            case trade::TradeNotice::Kind::TradeClosed: {
                const uint8_t status = n.status;
                queue("ClientCloseTrade", [status](bootstrap::PlayerBootstrap& pb) { return pb.trade_close(status); }, conn, /*must_build=*/true);
                break;
            }
            case trade::TradeNotice::Kind::Callout: {
                say(conn, n.message.c_str(), now);
                break;
            }
        }
    }

    const bool want_in_trade = trade_book_->in_trade(pb_.setup().character_guid);
    if (want_in_trade != trade_in_progress_) {
        if (pb_.set_trade_in_progress(want_in_trade)) {
            trade_in_progress_ = want_in_trade;
            queue("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("trade state"); }, conn, /*must_build=*/false);
        }
    }
}

}}  // namespace aoc::worldd

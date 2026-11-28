// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  trade/trade_book.cpp
// ============================================================================
#include "trade/trade_book.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <utility>

namespace aoc::worldd::trade {

namespace repl = ::aoc::protocol::repl;

namespace {

bool is_empty_trade(const ActiveTrade& t) {
    return t.offer_a.snapshots.empty() && t.offer_a.copper == 0 &&
           t.offer_b.snapshots.empty() && t.offer_b.copper == 0;
}

bool plans_disjoint_guids(const TradeSidePlan& a, const TradeSidePlan& b) {
    std::set<std::string> guids_a;
    for (const svc::InventoryItem& it : a.persist.items) {
        if (!guids_a.insert(it.instance_guid).second) return false;
    }
    for (const svc::WarehouseItem& w : a.persist.warehouse) {
        if (!guids_a.insert(w.item.instance_guid).second) return false;
    }
    std::set<std::string> guids_b;
    for (const svc::InventoryItem& it : b.persist.items) {
        if (guids_a.count(it.instance_guid) != 0) return false;
        if (!guids_b.insert(it.instance_guid).second) return false;
    }
    for (const svc::WarehouseItem& w : b.persist.warehouse) {
        if (guids_a.count(w.item.instance_guid) != 0) return false;
        if (!guids_b.insert(w.item.instance_guid).second) return false;
    }
    return true;
}

int64_t wall_clock_ms() {
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return ms > 0 ? ms : 1;
}

}  // namespace

TradeBook::TradeBook(const world::ActorRegistry* registry) : registry_(registry) {}

void TradeBook::attach_jobs(TradeJobs* jobs, PositionSink* sink) {
    jobs_ = jobs;
    sink_ = sink;
}

void TradeBook::detach_jobs() {
    jobs_ = nullptr;
    sink_ = nullptr;
}

void TradeBook::connect(const PlayerGuid& guid, std::string name, uint64_t pawn, TradePeer* peer) {
    if (is_zero_guid(guid)) return;
    if (auto it = players_.find(guid); it != players_.end()) {
        if (it->second.pawn != pawn) {
            const uint64_t old_pawn = it->second.pawn;
            for (ActiveTrade& t : active_) {
                if (!t.committing) continue;
                if (t.a == guid && t.pawn_a == old_pawn) t.disconnected_a = true;
                if (t.b == guid && t.pawn_b == old_pawn) t.disconnected_b = true;
            }
            interrupt(guid, "replaced by a new session", old_pawn);
            cancel_pending_for(guid, true);
        }
        it->second.name = std::move(name);
        it->second.pawn = pawn;
        it->second.peer = peer;
        it->second.status = PlayerStatus{};
        it->second.has_pos = false;
        it->second.notices.clear();
        return;
    }
    Player p;
    p.name = std::move(name);
    p.pawn = pawn;
    p.peer = peer;
    players_.emplace(guid, std::move(p));
}

void TradeBook::disconnect(const PlayerGuid& guid, double now, uint64_t pawn) {
    (void)now;
    auto it = players_.find(guid);
    if (it == players_.end()) return;
    // Relog-safe guard: an old session's late finish/destructor must not evict or cancel a newer session's state.
    if (pawn != 0 && it->second.pawn != pawn) return;
    const uint64_t actual_pawn = it->second.pawn;
    // If this player is in a Committing trade, detach their peer without cancelling the atomic DB swap!
    for (ActiveTrade& t : active_) {
        if (!t.committing) continue;
        if (t.a == guid && (pawn == 0 || t.pawn_a == actual_pawn)) t.disconnected_a = true;
        if (t.b == guid && (pawn == 0 || t.pawn_b == actual_pawn)) t.disconnected_b = true;
    }
    interrupt(guid, "player disconnected", actual_pawn);
    cancel_pending_for(guid, true);
    players_.erase(it);
}

void TradeBook::update_status(const PlayerGuid& guid, uint64_t pawn, const PlayerStatus& status, double now) {
    (void)now;
    auto it = players_.find(guid);
    if (it == players_.end()) return;
    if (pawn != 0 && it->second.pawn != pawn) return;
    it->second.status = status;
    if (status.dead || status.in_combat || status.corrupted) {
        const char* why = status.dead ? "player died" : status.corrupted ? "player became corrupted" : "player entered combat";
        interrupt(guid, why, pawn);
    }
}

void TradeBook::update_position(const PlayerGuid& guid, uint64_t pawn, double x, double y, double z) {
    auto it = players_.find(guid);
    if (it == players_.end()) return;
    if (pawn != 0 && it->second.pawn != pawn) return;
    it->second.has_pos = true;
    it->second.x = x;
    it->second.y = y;
    it->second.z = z;
}

bool TradeBook::where(const PlayerGuid& guid, double& x, double& y, double& z) const {
    const auto it = players_.find(guid);
    if (it == players_.end()) return false;
    if (it->second.has_pos) {
        x = it->second.x;
        y = it->second.y;
        z = it->second.z;
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
    if (!registry_ || it->second.pawn == 0) return false;
    const world::WorldActor* actor = registry_->find(it->second.pawn);
    if (!actor) return false;
    x = actor->x;
    y = actor->y;
    z = actor->z;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool TradeBook::within_range(const PlayerGuid& a, const PlayerGuid& b) const {
    double ax = 0, ay = 0, az = 0, bx = 0, by = 0, bz = 0;
    if (!where(a, ax, ay, az) || !where(b, bx, by, bz)) return false;
    const double dx = ax - bx;
    const double dy = ay - by;
    const double dz = az - bz;
    const double dist_sq = dx * dx + dy * dy + dz * dz;
    return std::isfinite(dist_sq) && dist_sq <= kMaxTradeRangeSqCm;
}

bool TradeBook::is_busy(const PlayerGuid& guid) const {
    return in_trade(guid) || has_pending(guid);
}

bool TradeBook::participants_valid(const ActiveTrade& t) const {
    const auto ita = players_.find(t.a);
    const auto itb = players_.find(t.b);
    if (ita == players_.end() || itb == players_.end()) return false;
    if (!ita->second.status.phased_in || !itb->second.status.phased_in ||
        ita->second.status.dead || itb->second.status.dead ||
        ita->second.status.in_combat || itb->second.status.in_combat ||
        ita->second.status.corrupted || itb->second.status.corrupted) {
        return false;
    }
    return within_range(t.a, t.b);
}

TradeGuid TradeBook::mint_trade_guid() {
    std::random_device rd;
    for (;;) {
        TradeGuid g{
            static_cast<uint32_t>(rd()),
            static_cast<uint32_t>(rd()),
            static_cast<uint32_t>(rd()),
            static_cast<uint32_t>(rd()),
        };
        if (is_zero_guid(g)) continue;
        const bool collides = std::any_of(active_.begin(), active_.end(), [&](const ActiveTrade& t) {
            return t.guid == g || t.a == g || t.b == g;
        });
        if (!collides) return g;
    }
}

void TradeBook::notify(const PlayerGuid& to, TradeNotice notice) {
    auto it = players_.find(to);
    if (it == players_.end()) return;
    it->second.notices.push_back(std::move(notice));
}

void TradeBook::notify_callout(const PlayerGuid& to, const TradeGuid& tg, std::string text) {
    TradeNotice n;
    n.kind = TradeNotice::Kind::Callout;
    n.trade_guid = tg;
    n.message = std::move(text);
    notify(to, std::move(n));
}

void TradeBook::close_trade_at(size_t index, uint8_t status, const char* why) {
    if (index >= active_.size()) return;
    const ActiveTrade closed = std::move(active_[index]);
    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(index));
    TradeNotice na;
    na.kind = TradeNotice::Kind::TradeClosed;
    na.other_guid = closed.b;
    na.trade_guid = closed.guid;
    na.status = status;
    notify(closed.a, std::move(na));

    TradeNotice nb;
    nb.kind = TradeNotice::Kind::TradeClosed;
    nb.other_guid = closed.a;
    nb.trade_guid = closed.guid;
    nb.status = status;
    notify(closed.b, std::move(nb));

    spdlog::debug("[trade] closed active trade (status={}, reason={})", status, why ? why : "");
}

void TradeBook::cancel_pending_for(const PlayerGuid& who, bool disconnected) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->requester == who || it->target == who) {
            if (it->target == who && disconnected) {
                TradeNotice n;
                n.kind = TradeNotice::Kind::RequestResponse;
                n.other_guid = who;
                n.response = static_cast<uint8_t>(repl::TradeRequestResponse::Decline);
                notify(it->requester, std::move(n));
            }
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

uint8_t TradeBook::request(const PlayerGuid& from, const PlayerGuid& target, double now) {
    const auto reject = [&](repl::TradeRequestResponse code) -> uint8_t {
        const uint8_t c = static_cast<uint8_t>(code);
        TradeNotice n;
        n.kind = TradeNotice::Kind::RequestResponse;
        n.other_guid = target;
        n.response = c;
        notify(from, std::move(n));
        return c;
    };

    const auto from_it = players_.find(from);
    if (from_it == players_.end()) return static_cast<uint8_t>(repl::TradeRequestResponse::Reject);
    if (!from_it->second.status.phased_in) return reject(repl::TradeRequestResponse::Reject);
    if (from == target || is_zero_guid(target)) return reject(repl::TradeRequestResponse::Reject);

    const auto target_it = players_.find(target);
    if (target_it == players_.end()) return reject(repl::TradeRequestResponse::PlayerNotFound);
    if (!target_it->second.status.phased_in) return reject(repl::TradeRequestResponse::Reject);
    if (is_busy(from) || is_busy(target)) return reject(repl::TradeRequestResponse::Busy);
    if (from_it->second.status.corrupted || target_it->second.status.corrupted) {
        return reject(repl::TradeRequestResponse::RejectCorrupted);
    }
    if (from_it->second.status.dead || from_it->second.status.in_combat ||
        target_it->second.status.dead || target_it->second.status.in_combat) {
        return reject(repl::TradeRequestResponse::Reject);
    }
    if (!within_range(from, target)) return reject(repl::TradeRequestResponse::RejectDistance);

    pending_.push_back(PendingRequest{from, target, now});
    TradeNotice ask;
    ask.kind = TradeNotice::Kind::RequestReceived;
    ask.other_guid = from;
    notify(target, std::move(ask));
    return 0;
}

bool TradeBook::respond(const PlayerGuid& by, const PlayerGuid& requester, uint8_t response, double now) {
    auto pit = std::find_if(pending_.begin(), pending_.end(), [&](const PendingRequest& p) {
        return p.target == by && p.requester == requester;
    });
    if (pit == pending_.end()) return false;
    const double asked_at = pit->asked_at;
    pending_.erase(pit);

    // Direct 60 s expiry check before accepting so late responses arriving prior to tick() expire cleanly.
    if (now - asked_at >= kTradeRequestTimeoutSec) {
        TradeNotice nr;
        nr.kind = TradeNotice::Kind::RequestResponse;
        nr.other_guid = by;
        nr.response = static_cast<uint8_t>(repl::TradeRequestResponse::Timeout);
        notify(requester, std::move(nr));
        return true;
    }

    if (response != static_cast<uint8_t>(repl::TradeRequestResponse::Accept)) {
        const uint8_t code = (response == static_cast<uint8_t>(repl::TradeRequestResponse::Timeout))
                                 ? static_cast<uint8_t>(repl::TradeRequestResponse::Timeout)
                                 : static_cast<uint8_t>(repl::TradeRequestResponse::Decline);
        TradeNotice n;
        n.kind = TradeNotice::Kind::RequestResponse;
        n.other_guid = by;
        n.response = code;
        notify(requester, std::move(n));
        return true;
    }

    const auto req_it = players_.find(requester);
    const auto by_it = players_.find(by);
    const auto fail_both = [&](repl::TradeRequestResponse code) {
        const uint8_t c = static_cast<uint8_t>(code);
        if (req_it != players_.end()) {
            TradeNotice nr;
            nr.kind = TradeNotice::Kind::RequestResponse;
            nr.other_guid = by;
            nr.response = c;
            notify(requester, std::move(nr));
        }
        if (by_it != players_.end()) {
            TradeNotice nb;
            nb.kind = TradeNotice::Kind::RequestResponse;
            nb.other_guid = requester;
            nb.response = c;
            notify(by, std::move(nb));
        }
    };

    if (req_it == players_.end() || by_it == players_.end()) {
        fail_both(repl::TradeRequestResponse::PlayerNotFound);
        return true;
    }
    if (!req_it->second.status.phased_in || !by_it->second.status.phased_in) {
        fail_both(repl::TradeRequestResponse::Reject);
        return true;
    }
    if (is_busy(requester) || is_busy(by)) {
        fail_both(repl::TradeRequestResponse::Busy);
        return true;
    }
    if (req_it->second.status.corrupted || by_it->second.status.corrupted) {
        fail_both(repl::TradeRequestResponse::RejectCorrupted);
        return true;
    }
    if (req_it->second.status.dead || req_it->second.status.in_combat ||
        by_it->second.status.dead || by_it->second.status.in_combat) {
        fail_both(repl::TradeRequestResponse::Reject);
        return true;
    }
    if (!within_range(requester, by)) {
        fail_both(repl::TradeRequestResponse::RejectDistance);
        return true;
    }

    ActiveTrade tr;
    tr.guid = mint_trade_guid();
    tr.a = requester;
    tr.b = by;
    tr.started_at = now;
    tr.pawn_a = req_it->second.pawn;
    tr.pawn_b = by_it->second.pawn;
    const TradeGuid tg = tr.guid;
    active_.push_back(std::move(tr));

    TradeNotice ba;
    ba.kind = TradeNotice::Kind::BeginTrade;
    ba.trade_guid = tg;
    ba.other_guid = by;
    notify(requester, std::move(ba));

    TradeNotice bb;
    bb.kind = TradeNotice::Kind::BeginTrade;
    bb.trade_guid = tg;
    bb.other_guid = requester;
    notify(by, std::move(bb));
    return true;
}

bool TradeBook::apply_offer(const PlayerGuid& by, const TradeGuid& trade_guid, int32_t version, bool valid,
                            std::vector<repl::ItemInstance> items, std::vector<OfferedItemSnapshot> snapshots,
                            int64_t copper) {
    auto it = std::find_if(active_.begin(), active_.end(), [&](const ActiveTrade& t) {
        return (t.a == by || t.b == by) && t.guid == trade_guid;
    });
    if (it == active_.end()) return false;
    if (it->committing) return false;

    OfferSide& side = it->side_of(by);
    if (version <= side.last_version || version >= std::numeric_limits<int32_t>::max()) return false;
    side.last_version = version;

    // Locked or confirmed sender cannot edit their offer: reject and revert UI at version + 1.
    const bool accepted = valid && side.state == SideState::Opened;
    if (accepted) {
        side.items = std::move(items);
        side.snapshots = std::move(snapshots);
        side.copper = copper;

        // Anti-bait-and-switch: valid edit while partner is Locked/Confirmed unlocks partner first,
        // so ClientUnlockTrade arrives before the updated offer on both clients.
        const PlayerGuid partner = it->partner_of(by);
        OfferSide& partner_side = it->side_of(partner);
        if (partner_side.state != SideState::Opened) {
            partner_side.state = SideState::Opened;
            it->both_locked_at = 0.0;

            TradeNotice ua;
            ua.kind = TradeNotice::Kind::TradeUnlocked;
            ua.other_guid = partner;
            ua.trade_guid = it->guid;
            notify(it->a, std::move(ua));

            TradeNotice ub;
            ub.kind = TradeNotice::Kind::TradeUnlocked;
            ub.other_guid = partner;
            ub.trade_guid = it->guid;
            notify(it->b, std::move(ub));
        }

        repl::TradeOffer wire;
        wire.items = side.items;
        wire.money = repl::trade_money_container(side.copper);
        wire.version = version;

        TradeNotice na;
        na.kind = TradeNotice::Kind::OfferUpdated;
        na.other_guid = by;
        na.trade_guid = it->guid;
        na.offer = wire;
        notify(it->a, std::move(na));

        TradeNotice nb;
        nb.kind = TradeNotice::Kind::OfferUpdated;
        nb.other_guid = by;
        nb.trade_guid = it->guid;
        nb.offer = std::move(wire);
        notify(it->b, std::move(nb));
    } else {
        repl::TradeOffer revert;
        revert.items = side.items;
        revert.money = repl::trade_money_container(side.copper);
        revert.version = version + 1;

        TradeNotice nr;
        nr.kind = TradeNotice::Kind::OfferUpdated;
        nr.other_guid = by;
        nr.trade_guid = it->guid;
        nr.offer = std::move(revert);
        notify(by, std::move(nr));
    }
    return true;
}

bool TradeBook::lock_trade(const PlayerGuid& by, const TradeGuid& trade_guid, double now) {
    size_t idx = active_.size();
    for (size_t i = 0; i < active_.size(); ++i) {
        if ((active_[i].a == by || active_[i].b == by) && active_[i].guid == trade_guid) {
            idx = i;
            break;
        }
    }
    if (idx == active_.size()) return false;
    ActiveTrade& t = active_[idx];
    if (t.committing) return false;
    OfferSide& side = t.side_of(by);
    if (side.state != SideState::Opened) return false;

    if (now - side.last_state_change < kTradeServerCooldownSec) {
        notify_callout(by, t.guid, "Trade action is on cooldown.");
        return true;
    }
    if (is_empty_trade(t)) {
        notify_callout(by, t.guid, "Cannot lock an empty trade.");
        return true;
    }
    if (!participants_valid(t)) {
        close_trade_at(idx, static_cast<uint8_t>(repl::TradeStatus::Failure), "participant state invalid at lock");
        return true;
    }
    const auto ita = players_.find(t.a);
    const auto itb = players_.find(t.b);
    if (ita == players_.end() || itb == players_.end() || !ita->second.peer || !itb->second.peer) {
        close_trade_at(idx, static_cast<uint8_t>(repl::TradeStatus::Failure), "participant unavailable at lock");
        return true;
    }

    std::vector<svc::InventoryItem> offered_a, offered_b;
    if (!ita->second.peer->collect_trade_offer(t.offer_a, offered_a) ||
        !itb->second.peer->collect_trade_offer(t.offer_b, offered_b)) {
        close_trade_at(idx, static_cast<uint8_t>(repl::TradeStatus::Failure), "offered inventory or currency changed");
        return true;
    }
    TradeSidePlan plan_a, plan_b;
    const PlanError err_a = ita->second.peer->plan_trade_side(t.offer_a, offered_b, t.offer_b.copper, plan_a);
    const PlanError err_b = itb->second.peer->plan_trade_side(t.offer_b, offered_a, t.offer_a.copper, plan_b);
    if (err_a == PlanError::Drifted || err_b == PlanError::Drifted) {
        close_trade_at(idx, static_cast<uint8_t>(repl::TradeStatus::Failure), "offered inventory or currency changed");
        return true;
    }
    if (err_a == PlanError::MaxCurrency || err_b == PlanError::MaxCurrency) {
        notify_callout(by, t.guid, "Unable to Lock Trade. Max Currency Reached.");
        return true;
    }
    if (err_a == PlanError::InventorySpace || err_b == PlanError::InventorySpace) {
        notify_callout(by, t.guid, "Unable to Lock Trade. Check Inventory Space.");
        return true;
    }

    side.state = SideState::Locked;
    side.last_state_change = now;
    if (t.offer_a.state == SideState::Locked && t.offer_b.state == SideState::Locked) {
        t.both_locked_at = now;
    }

    TradeNotice la;
    la.kind = TradeNotice::Kind::TradeLocked;
    la.other_guid = by;
    la.trade_guid = t.guid;
    notify(t.a, std::move(la));

    TradeNotice lb;
    lb.kind = TradeNotice::Kind::TradeLocked;
    lb.other_guid = by;
    lb.trade_guid = t.guid;
    notify(t.b, std::move(lb));
    return true;
}

bool TradeBook::unlock_trade(const PlayerGuid& by, const TradeGuid& trade_guid, double now) {
    auto it = std::find_if(active_.begin(), active_.end(), [&](const ActiveTrade& t) {
        return (t.a == by || t.b == by) && t.guid == trade_guid;
    });
    if (it == active_.end() || it->committing) return false;
    OfferSide& sender_side = it->side_of(by);
    if (sender_side.state != SideState::Locked && sender_side.state != SideState::Confirmed) return false;

    // No cooldown check on UNLOCK_TRADE so a player can always back out immediately.
    // Dual-unlock: transition EVERY locked/confirmed side back to Opened and notify BOTH players.
    sender_side.last_state_change = now;
    it->both_locked_at = 0.0;

    const PlayerGuid partner = it->partner_of(by);
    const std::array<PlayerGuid, 2> order = {by, partner};
    for (const PlayerGuid& who : order) {
        OfferSide& s = it->side_of(who);
        if (s.state == SideState::Opened) continue;
        s.state = SideState::Opened;

        TradeNotice ua;
        ua.kind = TradeNotice::Kind::TradeUnlocked;
        ua.other_guid = who;
        ua.trade_guid = it->guid;
        notify(it->a, std::move(ua));

        TradeNotice ub;
        ub.kind = TradeNotice::Kind::TradeUnlocked;
        ub.other_guid = who;
        ub.trade_guid = it->guid;
        notify(it->b, std::move(ub));
    }
    return true;
}

bool TradeBook::confirm_trade(const PlayerGuid& by, const TradeGuid& trade_guid, double now) {
    size_t idx = active_.size();
    for (size_t i = 0; i < active_.size(); ++i) {
        if ((active_[i].a == by || active_[i].b == by) && active_[i].guid == trade_guid) {
            idx = i;
            break;
        }
    }
    if (idx == active_.size()) return false;
    ActiveTrade& t = active_[idx];
    if (t.committing) return false;

    const PlayerGuid partner = t.partner_of(by);
    OfferSide& side = t.side_of(by);
    OfferSide& other = t.side_of(partner);
    if (side.state != SideState::Locked) return false;
    if (other.state != SideState::Locked && other.state != SideState::Confirmed) return false;

    if (now - side.last_state_change < kTradeServerCooldownSec ||
        now - t.both_locked_at < kTradeConfirmGuardSec) {
        notify_callout(by, t.guid, "Trade action is on cooldown.");
        return true;
    }
    if (is_empty_trade(t)) {
        notify_callout(by, t.guid, "Cannot lock an empty trade.");
        return true;
    }
    if (!participants_valid(t)) {
        close_trade_at(idx, static_cast<uint8_t>(repl::TradeStatus::Failure), "participant state invalid at confirm");
        return true;
    }
    const auto ita = players_.find(t.a);
    const auto itb = players_.find(t.b);
    if (ita == players_.end() || itb == players_.end() || !ita->second.peer || !itb->second.peer) {
        close_trade_at(idx, static_cast<uint8_t>(repl::TradeStatus::Failure), "participant unavailable at confirm");
        return true;
    }
    TradePeer* peer_a = ita->second.peer;
    TradePeer* peer_b = itb->second.peer;
    if (!peer_a->can_commit_trade() || !peer_b->can_commit_trade()) {
        notify_callout(by, t.guid, "Trade cannot be finalized right now. Please try again.");
        return true;
    }

    std::vector<svc::InventoryItem> offered_a, offered_b;
    if (!peer_a->collect_trade_offer(t.offer_a, offered_a) ||
        !peer_b->collect_trade_offer(t.offer_b, offered_b)) {
        close_trade_at(idx, static_cast<uint8_t>(repl::TradeStatus::Failure), "offered inventory or currency changed");
        return true;
    }
    TradeSidePlan plan_a, plan_b;
    const PlanError err_a = peer_a->plan_trade_side(t.offer_a, offered_b, t.offer_b.copper, plan_a);
    const PlanError err_b = peer_b->plan_trade_side(t.offer_b, offered_a, t.offer_a.copper, plan_b);
    if (err_a == PlanError::Drifted || err_b == PlanError::Drifted) {
        close_trade_at(idx, static_cast<uint8_t>(repl::TradeStatus::Failure), "offered inventory or currency changed");
        return true;
    }
    if (err_a == PlanError::MaxCurrency || err_b == PlanError::MaxCurrency) {
        notify_callout(by, t.guid, "Unable to Lock Trade. Max Currency Reached.");
        return true;
    }
    if (err_a == PlanError::InventorySpace || err_b == PlanError::InventorySpace) {
        notify_callout(by, t.guid, "Unable to lock trade. Check Inventory space.");
        return true;
    }

    // If partner already confirmed, both sides are now ready to submit the atomic DB swap.
    if (other.state == SideState::Confirmed) {
        if (!plans_disjoint_guids(plan_a, plan_b)) {
            close_trade_at(idx, static_cast<uint8_t>(repl::TradeStatus::Failure), "duplicate item instance detected across trade sides");
            return true;
        }
        if (!jobs_ || !sink_) {
            notify_callout(by, t.guid, "Trade cannot be finalized right now. Please try again.");
            return true;
        }
        const std::string trade_id = items::guid_hex(to_guid_value(t.guid));
        svc::CommitTradeRequest req;
        req.trade_id = trade_id;
        req.side_a = plan_a.persist;
        req.side_b = plan_b.persist;
        req.copper_a_to_b = t.offer_a.copper;
        req.copper_b_to_a = t.offer_b.copper;
        req.items_a_to_b = static_cast<int32_t>(t.offer_a.snapshots.size());
        req.items_b_to_a = static_cast<int32_t>(t.offer_b.snapshots.size());
        req.committed_at_ms = wall_clock_ms();

        const uint64_t ticket = jobs_->submit(std::move(req), sink_);
        if (ticket == 0) {
            notify_callout(by, t.guid, "Trade cannot be finalized right now. Please try again.");
            return true;
        }

        side.state = SideState::Confirmed;
        side.last_state_change = now;
        t.committing = true;
        t.commit_ticket = ticket;
        t.trade_id = trade_id;
        t.pawn_a = ita->second.pawn;
        t.pawn_b = itb->second.pawn;
        t.disconnected_a = false;
        t.disconnected_b = false;
        t.plan_a = std::move(plan_a);
        t.plan_b = std::move(plan_b);
        t.before_a = peer_a->capture_before_trade_snapshot();
        t.before_b = peer_b->capture_before_trade_snapshot();

        TradeNotice ca;
        ca.kind = TradeNotice::Kind::TradeConfirmed;
        ca.other_guid = by;
        ca.trade_guid = t.guid;
        notify(t.a, std::move(ca));

        TradeNotice cb;
        cb.kind = TradeNotice::Kind::TradeConfirmed;
        cb.other_guid = by;
        cb.trade_guid = t.guid;
        notify(t.b, std::move(cb));
        return true;
    }

    side.state = SideState::Confirmed;
    side.last_state_change = now;

    TradeNotice ca;
    ca.kind = TradeNotice::Kind::TradeConfirmed;
    ca.other_guid = by;
    ca.trade_guid = t.guid;
    notify(t.a, std::move(ca));

    TradeNotice cb;
    cb.kind = TradeNotice::Kind::TradeConfirmed;
    cb.other_guid = by;
    cb.trade_guid = t.guid;
    notify(t.b, std::move(cb));
    return true;
}

bool TradeBook::cancel(const PlayerGuid& by, const TradeGuid& trade_guid, bool allow_partner_fallback, const char* why) {
    for (size_t i = 0; i < active_.size(); ++i) {
        const ActiveTrade& t = active_[i];
        if (t.committing) continue;
        if (t.a != by && t.b != by) continue;
        const bool match_trade = t.guid == trade_guid;
        const bool match_partner = allow_partner_fallback && t.partner_of(by) == trade_guid;
        if (!match_trade && !match_partner) continue;
        close_trade_at(i, static_cast<uint8_t>(repl::TradeStatus::Failure), why);
        return true;
    }
    return false;
}

bool TradeBook::interrupt(const PlayerGuid& player, const char* why, uint64_t pawn) {
    if (pawn != 0) {
        const auto pit = players_.find(player);
        if (pit != players_.end() && pit->second.pawn != pawn) return false;
    }
    for (size_t i = 0; i < active_.size(); ++i) {
        const ActiveTrade& t = active_[i];
        if (t.committing) continue;
        if (t.a == player || t.b == player) {
            if (pawn != 0) {
                const uint64_t side_pawn = (t.a == player) ? t.pawn_a : t.pawn_b;
                if (side_pawn != 0 && side_pawn != pawn) continue;
            }
            close_trade_at(i, static_cast<uint8_t>(repl::TradeStatus::Failure), why);
            return true;
        }
    }
    return false;
}

void TradeBook::settle_commits() {
    if (!jobs_) return;
    for (size_t i = active_.size(); i-- > 0;) {
        if (!active_[i].committing) continue;
        TradeCommitAnswer answer;
        if (!jobs_->take(active_[i].commit_ticket, answer)) continue;

        ActiveTrade tr = std::move(active_[i]);
        active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));

        const std::string char_a = tr.before_a.character_id;
        const std::string char_b = tr.before_b.character_id;

        if (answer.status == svc::CallStatus::Ok) {
            auto apply_side = [&](const PlayerGuid& who, uint64_t pawn, bool disconnected,
                                  const PlayerGuid& other, const TradeSidePlan& plan) {
                if (disconnected) return;
                const auto it = players_.find(who);
                if (it == players_.end() || it->second.pawn != pawn || !it->second.peer) return;
                it->second.peer->apply_committed_trade(plan);

                TradeNotice done;
                done.kind = TradeNotice::Kind::CommitSucceeded;
                done.other_guid = other;
                done.trade_guid = tr.guid;
                done.status = static_cast<uint8_t>(repl::TradeStatus::Success);
                done.commit_wire.removed_events = plan.removed_events;
                done.commit_wire.added_events = plan.added_events;
                done.commit_wire.copper_changed = (plan.copper_after != plan.copper_before);
                done.commit_wire.copper_after = plan.copper_after;
                done.commit_wire.net_copper_delta = plan.net_copper_delta;
                notify(who, std::move(done));
            };
            apply_side(tr.a, tr.pawn_a, tr.disconnected_a, tr.b, tr.plan_a);
            apply_side(tr.b, tr.pawn_b, tr.disconnected_b, tr.a, tr.plan_b);
            spdlog::info("[trade] commit {} ({} <-> {}) succeeded{}", tr.trade_id, char_a, char_b,
                         answer.replayed ? " (replayed)" : "");
        } else if (answer.undecided) {
            auto fail_closed = [&](const PlayerGuid& who, uint64_t pawn, bool disconnected) {
                if (disconnected) return;
                const auto it = players_.find(who);
                if (it == players_.end() || it->second.pawn != pawn || !it->second.peer) return;
                it->second.peer->on_trade_commit_undecided();
            };
            fail_closed(tr.a, tr.pawn_a, tr.disconnected_a);
            fail_closed(tr.b, tr.pawn_b, tr.disconnected_b);
        } else {
            auto abort_side = [&](const PlayerGuid& who, uint64_t pawn, bool disconnected,
                                  const PlayerGuid& other, const TradeBeforeSnapshot& before) {
                const auto it = players_.find(who);
                const bool online = !disconnected && it != players_.end() && it->second.pawn == pawn && it->second.peer != nullptr;
                if (online) {
                    TradeNotice closed;
                    closed.kind = TradeNotice::Kind::TradeClosed;
                    closed.other_guid = other;
                    closed.trade_guid = tr.guid;
                    closed.status = static_cast<uint8_t>(repl::TradeStatus::Failure);
                    notify(who, std::move(closed));
                    it->second.peer->on_trade_commit_aborted();
                } else if (before.inventory_changed && sink_) {
                    sink_->save_inventory(before.character_id, before.items, before.wallet,
                                          before.jobs, before.known_recipes, before.warehouse);
                }
            };
            abort_side(tr.a, tr.pawn_a, tr.disconnected_a, tr.b, tr.before_a);
            abort_side(tr.b, tr.pawn_b, tr.disconnected_b, tr.a, tr.before_b);
            spdlog::warn("[trade] commit {} ({} <-> {}) refused ({}: {}); both characters reverted",
                         tr.trade_id, char_a, char_b, svc::call_status_name(answer.status), answer.reason);
        }

        // Release open_commits_ LAST, after synchronous swap or orphan before-save handoff.
        jobs_->release(char_a, char_b);
    }
}

void TradeBook::tick(double now) {
    // 1. Settle any finished asynchronous CommitPlayerTrade jobs first.
    settle_commits();

    // 2. Expire pending requests past 60 s server TTL.
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->asked_at >= kTradeRequestTimeoutSec) {
            TradeNotice n;
            n.kind = TradeNotice::Kind::RequestResponse;
            n.other_guid = it->target;
            n.response = static_cast<uint8_t>(repl::TradeRequestResponse::Timeout);
            notify(it->requester, std::move(n));
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    // 3. Enforce distance and participant status invariants on non-committing active trades.
    for (size_t i = active_.size(); i-- > 0;) {
        const ActiveTrade& t = active_[i];
        if (t.committing) continue;
        const auto ita = players_.find(t.a);
        const auto itb = players_.find(t.b);
        const bool bad_player = ita == players_.end() || itb == players_.end() ||
                                !ita->second.status.phased_in || !itb->second.status.phased_in ||
                                ita->second.status.dead || itb->second.status.dead ||
                                ita->second.status.in_combat || itb->second.status.in_combat ||
                                ita->second.status.corrupted || itb->second.status.corrupted;
        if (bad_player || !within_range(t.a, t.b)) {
            close_trade_at(i, static_cast<uint8_t>(repl::TradeStatus::Failure), bad_player ? "participant state invalid" : "out of range");
        }
    }
}

bool TradeBook::connected(const PlayerGuid& guid) const {
    return players_.find(guid) != players_.end();
}

bool TradeBook::in_trade(const PlayerGuid& guid) const {
    return active_trade(guid) != nullptr;
}

bool TradeBook::is_committing(const PlayerGuid& guid, uint64_t pawn) const {
    for (const ActiveTrade& t : active_) {
        if (!t.committing) continue;
        if (t.a == guid && (pawn == 0 || t.pawn_a == pawn)) return true;
        if (t.b == guid && (pawn == 0 || t.pawn_b == pawn)) return true;
    }
    return false;
}

bool TradeBook::has_pending(const PlayerGuid& guid) const {
    return std::any_of(pending_.begin(), pending_.end(), [&](const PendingRequest& p) {
        return p.requester == guid || p.target == guid;
    });
}

const ActiveTrade* TradeBook::active_trade(const PlayerGuid& guid) const {
    for (const ActiveTrade& t : active_) {
        if (t.a == guid || t.b == guid) return &t;
    }
    return nullptr;
}

std::vector<TradeNotice> TradeBook::take_notices(const PlayerGuid& guid) {
    auto it = players_.find(guid);
    if (it == players_.end()) return {};
    std::vector<TradeNotice> out;
    out.swap(it->second.notices);
    return out;
}

}  // namespace aoc::worldd::trade

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  trade/trade_book.cpp
// ============================================================================
#include "trade/trade_book.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace aoc::worldd::trade {

namespace repl = ::aoc::protocol::repl;

namespace {

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

}  // namespace

TradeBook::TradeBook(const world::ActorRegistry* registry) : registry_(registry) {}

void TradeBook::connect(const PlayerGuid& guid, std::string name, uint64_t pawn) {
    if (is_zero_guid(guid)) return;
    if (auto it = players_.find(guid); it != players_.end()) {
        if (it->second.pawn != pawn) {
            interrupt(guid, "replaced by a new session");
            cancel_pending_for(guid, true);
        }
        it->second.name = std::move(name);
        it->second.pawn = pawn;
        it->second.status = PlayerStatus{};
        it->second.has_pos = false;
        it->second.notices.clear();
        return;
    }
    Player p;
    p.name = std::move(name);
    p.pawn = pawn;
    players_.emplace(guid, std::move(p));
}

void TradeBook::disconnect(const PlayerGuid& guid, double now, uint64_t pawn) {
    (void)now;
    auto it = players_.find(guid);
    if (it == players_.end()) return;
    // Relog-safe guard: an old session's late finish/destructor must not evict or cancel a newer session's state.
    if (pawn != 0 && it->second.pawn != pawn) return;
    interrupt(guid, "player disconnected");
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
        interrupt(guid, why);
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

TradeGuid TradeBook::mint_trade_guid() {
    for (;;) {
        const uint64_t hi = mix64(guid_seq_++);
        const uint64_t lo = mix64(guid_seq_++);
        TradeGuid g{
            static_cast<uint32_t>(hi >> 32),
            static_cast<uint32_t>(hi & 0xFFFFFFFFu),
            static_cast<uint32_t>(lo >> 32),
            static_cast<uint32_t>((lo & 0xFFFFFFFFu) | 1u),
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
    pending_.erase(pit);

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
    OfferSide& side = it->side_of(by);
    if (version <= side.last_version || version >= std::numeric_limits<int32_t>::max()) return false;
    side.last_version = version;

    if (valid) {
        side.items = std::move(items);
        side.snapshots = std::move(snapshots);
        side.copper = copper;
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

bool TradeBook::cancel(const PlayerGuid& by, const TradeGuid& trade_guid, bool allow_partner_fallback, const char* why) {
    for (size_t i = 0; i < active_.size(); ++i) {
        const ActiveTrade& t = active_[i];
        if (t.a != by && t.b != by) continue;
        const bool match_trade = t.guid == trade_guid;
        const bool match_partner = allow_partner_fallback && t.partner_of(by) == trade_guid;
        if (!match_trade && !match_partner) continue;
        close_trade_at(i, static_cast<uint8_t>(repl::TradeStatus::Failure), why);
        return true;
    }
    return false;
}

bool TradeBook::interrupt(const PlayerGuid& player, const char* why) {
    for (size_t i = 0; i < active_.size(); ++i) {
        if (active_[i].a == player || active_[i].b == player) {
            close_trade_at(i, static_cast<uint8_t>(repl::TradeStatus::Failure), why);
            return true;
        }
    }
    return false;
}

void TradeBook::tick(double now) {
    // 1. Expire pending requests past 60 s server TTL.
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

    // 2. Enforce distance and participant status invariants on active trades.
    for (size_t i = active_.size(); i-- > 0;) {
        const ActiveTrade& t = active_[i];
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

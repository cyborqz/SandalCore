// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/duels.cpp  (PvP, slice 1, step 3): the duels and their banners (world/duels.h)
// ============================================================================
#include "world/duels.h"

#include "bootstrap/constants.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/value.h"
#include "protocol/schema/generated/index.h"
#include "spawns/generic_npc.h"
#include "spawns/spawn_util.h"
#include "world/actor_registry.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <cmath>

namespace aoc::worldd::world {

namespace {
/// Two character names are the same one, whatever their case (the /duel command's argument as the player typed it).
bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}
constexpr int64_t kRoleInstigator = 0, kRoleRecipient = 1;   // EDuelParticipantRole

repl::Value guid_word(uint32_t w) { return repl::Value::i64(static_cast<int32_t>(w)); }

bool zero(const DuelGuid& g) { return g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0; }
}  // namespace

const char* duel_result_name(DuelResult r) {
    switch (r) {
        case DuelResult::WinLoss: return "a win";
        case DuelResult::Forfeit: return "a forfeit";
        case DuelResult::Flee: return "a flight";
        case DuelResult::TimeOut: return "the time running out";
        case DuelResult::System: return "an interruption";
    }
    return "an end";
}

uint64_t duel_key(const DuelGuid& g) {
    const uint64_t k = (static_cast<uint64_t>(g[0]) << 32 | g[1]) ^ (static_cast<uint64_t>(g[2]) << 32 | g[3]) * 0x9E3779B97F4A7C15ull;
    return k == 0 ? 1 : k;
}

DuelBook::DuelBook(ActorRegistry* registry, std::function<protocol::emit::FIntrepidNetworkGUID()> mint, DuelSettings settings)
    : registry_(registry), mint_(std::move(mint)), settings_(settings) {}

const protocol::schema::ClassSchema* DuelBook::banner_schema() { return protocol::schema::gen::find_class_schema("ADuelCenter"); }

const protocol::repl::ExportChain& DuelBook::banner_archetype() {
    // BP_DuelCenter_C's default object and its package, named so the client loads them (checksum 0: the client accepts it, spawns/generic_npc.h)
    static const repl::ExportChain chain = [] {
        const std::string path = kBannerArchetype;
        const std::string package = path.substr(0, path.rfind('.'));
        repl::ExportLink obj, pkg;
        obj.guid = protocol::emit::FIntrepidNetworkGUID::from_u64(spawns::static_object_id(path, {}));
        obj.name = path.substr(path.rfind('.') + 1);
        obj.has_checksum = true;
        pkg.guid = protocol::emit::FIntrepidNetworkGUID::from_u64(spawns::static_object_id(package + "#package", {}));
        pkg.name = package;
        pkg.has_checksum = true;
        return repl::ExportChain{obj, pkg};
    }();
    return chain;
}

void DuelBook::connect(const DuelGuid& player, const std::string& name, uint64_t pawn) {
    Player& p = players_[player];
    p.name = name;
    p.pawn = pawn;
}

void DuelBook::disconnect(const DuelGuid& player, double now) {
    (void)now;
    if (auto it = find_of(player); it != duels_.end()) end(it, DuelResult::System, nullptr, players_.count(player) ? players_[player].name + " left the world" : "a player left");
    players_.erase(player);
    notices_.erase(player);
}

bool DuelBook::where(const DuelGuid& player, double& x, double& y, double& z) const {
    const auto it = players_.find(player);
    if (it == players_.end() || !registry_) return false;
    const WorldActor* a = registry_->find(it->second.pawn);
    if (!a) return false;
    x = a->x;
    y = a->y;
    z = a->z;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

std::map<DuelGuid, DuelBook::Duel>::iterator DuelBook::find_of(const DuelGuid& player) {
    for (auto it = duels_.begin(); it != duels_.end(); ++it) {
        if (it->second.a == player || it->second.b == player) return it;
    }
    return duels_.end();
}

const DuelBook::Duel* DuelBook::duel_of(const DuelGuid& player) const {
    for (const auto& [id, d] : duels_) {
        if (d.a == player || d.b == player) return &d;
    }
    return nullptr;
}

uint64_t DuelBook::fighting_key(const DuelGuid& player) const {
    const Duel* d = duel_of(player);
    return d && d->phase == Phase::Fighting ? duel_key(d->id) : 0;
}

std::optional<DuelGuid> DuelBook::fighting(const DuelGuid& player) const {
    const Duel* d = duel_of(player);
    if (!d || d->phase != Phase::Fighting) return std::nullopt;
    return d->id;
}

uint64_t DuelBook::partner_pawn(const DuelGuid& player) const {
    const Duel* d = duel_of(player);
    if (!d || d->phase != Phase::Fighting) return 0;
    const auto it = players_.find(d->a == player ? d->b : d->a);
    return it == players_.end() ? 0 : it->second.pawn;
}

void DuelBook::tell(const DuelGuid& player, DuelNotice n) {
    auto& q = notices_[player];
    if (q.size() < 32) q.push_back(std::move(n));
}

std::vector<DuelNotice> DuelBook::take_notices(const DuelGuid& player) {
    const auto it = notices_.find(player);
    if (it == notices_.end()) return {};
    std::vector<DuelNotice> out = std::move(it->second);
    notices_.erase(it);
    return out;
}

std::string DuelBook::request(const DuelGuid& from, const std::string& name, double now) {
    const auto me = players_.find(from);
    if (me == players_.end()) return "You cannot duel right now.";
    DuelGuid to{};
    for (const auto& [g, p] : players_) {
        if (same_name(p.name, name)) to = g;
    }
    if (zero(to) || name.empty()) return "That player is not online.";
    if (to == from) return "You cannot duel yourself.";
    if (duel_of(from)) return "You are already in a duel.";
    if (duel_of(to)) return players_[to].name + " is already in a duel.";
    const Player& target = players_[to];
    if (const auto d = target.declined.find(from); d != target.declined.end() && now - d->second < settings_.request_cooldown) return "Wait a moment before asking " + target.name + " again.";
    double ax = 0, ay = 0, az = 0, bx = 0, by = 0, bz = 0;
    if (registry_ && (!where(from, ax, ay, az) || !where(to, bx, by, bz))) return "You cannot duel right now.";
    if (registry_ && std::hypot(ax - bx, ay - by) > settings_.start_distance_cm) return "Out of range to duel";
    Duel d;
    d.id = from;
    d.a = from;
    d.b = to;
    d.a_name = me->second.name;
    d.b_name = target.name;
    d.asked_at = now;
    d.cx = (ax + bx) / 2;
    d.cy = (ay + by) / 2;
    d.cz = (az + bz) / 2;
    auto [it, fresh] = duels_.emplace(d.id, std::move(d));
    if (!fresh) return "You are already in a duel.";
    spawn_banner(it->second);
    DuelNotice sent;
    sent.kind = DuelNotice::Kind::Sent;
    sent.duel = it->second.id;
    sent.other = it->second.b_name;
    sent.text = "You challenged " + it->second.b_name + " to a duel.";
    tell(from, sent);
    DuelNotice asked;
    asked.kind = DuelNotice::Kind::Asked;
    asked.duel = it->second.id;
    asked.other = it->second.a_name;
    tell(to, asked);
    spdlog::info("[duel] {} asks {} for a duel (banner {})", it->second.a_name, it->second.b_name, it->second.banner);
    return "";
}

bool DuelBook::accept(const DuelGuid& by, const DuelGuid& duel, double now) {
    const auto it = duels_.find(duel);
    if (it == duels_.end() || it->second.b != by || it->second.phase != Phase::Asked) return false;
    Duel& d = it->second;
    d.phase = Phase::Counting;
    d.begins_at = now + settings_.countdown_seconds;
    update_banner(d);
    for (const DuelGuid& p : {d.a, d.b}) {
        DuelNotice n;
        n.kind = DuelNotice::Kind::Counting;
        n.duel = d.id;
        n.other = p == d.a ? d.b_name : d.a_name;
        tell(p, n);
    }
    spdlog::info("[duel] {} accepts {}'s duel: it begins in {:.0f} s", d.b_name, d.a_name, settings_.countdown_seconds);
    return true;
}

bool DuelBook::decline(const DuelGuid& by, const DuelGuid& duel, double now) {
    const auto it = duels_.find(duel);
    if (it == duels_.end() || it->second.b != by || it->second.phase != Phase::Asked) return false;
    players_[by].declined[it->second.a] = now;
    DuelNotice n;
    n.kind = DuelNotice::Kind::Declined;
    n.duel = it->second.id;
    n.other = it->second.b_name;
    n.text = it->second.b_name + " declined your duel.";
    tell(it->second.a, n);
    spdlog::info("[duel] {} declines {}'s duel", it->second.b_name, it->second.a_name);
    if (registry_ && it->second.banner) registry_->remove(it->second.banner);
    duels_.erase(it);
    return true;
}

bool DuelBook::forfeit(const DuelGuid& by, double now) {
    (void)now;
    const auto it = find_of(by);
    if (it == duels_.end()) return false;
    if (it->second.phase == Phase::Fighting) {
        end(it, DuelResult::Forfeit, &by, "");
    } else {
        end(it, DuelResult::System, nullptr, (it->second.a == by ? it->second.a_name : it->second.b_name) + " called the duel off");
    }
    return true;
}

void DuelBook::defeated(const DuelGuid& loser, double now) {
    (void)now;
    const auto it = find_of(loser);
    if (it == duels_.end() || it->second.phase != Phase::Fighting) return;
    end(it, DuelResult::WinLoss, &loser, "");
}

void DuelBook::interrupted(const DuelGuid& player, const std::string& why, double now) {
    (void)now;
    const auto it = find_of(player);
    if (it == duels_.end()) return;
    end(it, DuelResult::System, nullptr, why);
}

void DuelBook::end(std::map<DuelGuid, Duel>::iterator it, DuelResult result, const DuelGuid* loser, const std::string& why) {
    const Duel d = it->second;
    duels_.erase(it);
    if (registry_ && d.banner) registry_->remove(d.banner);
    const bool fought = d.phase == Phase::Fighting;
    const std::string& winner_name = loser ? (*loser == d.a ? d.b_name : d.a_name) : std::string();
    const std::string& loser_name = loser ? (*loser == d.a ? d.a_name : d.b_name) : std::string();
    for (const DuelGuid& p : {d.a, d.b}) {
        DuelNotice n;
        n.kind = DuelNotice::Kind::Ended;
        n.duel = d.id;
        n.other = p == d.a ? d.b_name : d.a_name;
        n.result = result;
        n.fought = fought;
        n.won = loser && *loser != p;
        switch (result) {
            case DuelResult::WinLoss: n.text = winner_name + " has defeated " + loser_name + " in a duel."; break;
            case DuelResult::Forfeit: n.text = loser_name + " forfeited the duel to " + winner_name + "."; break;
            case DuelResult::Flee: n.text = loser_name + " fled the duel: " + winner_name + " wins."; break;
            case DuelResult::TimeOut: n.text = "The duel between " + d.a_name + " and " + d.b_name + " ran out of time: nobody wins."; break;
            case DuelResult::System: n.text = fought ? "The duel between " + d.a_name + " and " + d.b_name + " was interrupted" + (why.empty() ? "." : " (" + why + ").")
                                                     : (why.empty() ? "The duel request was withdrawn." : "The duel request is over: " + why + ".");
                break;
        }
        tell(p, n);
    }
    spdlog::info("[duel] {} vs {}: ended by {}{}{}{}", d.a_name, d.b_name, duel_result_name(result), loser ? ", " + winner_name + " won" : "", why.empty() ? "" : " (", why.empty() ? "" : why + ")");
}

void DuelBook::tick(double now) {
    for (auto it = duels_.begin(); it != duels_.end();) {
        Duel& d = it->second;
        auto next = std::next(it);
        if (d.phase == Phase::Asked && now - d.asked_at >= settings_.answer_seconds) {
            DuelNotice n;
            n.kind = DuelNotice::Kind::Declined;
            n.duel = d.id;
            n.other = d.b_name;
            n.text = d.b_name + " did not answer your duel.";
            tell(d.a, n);
            if (registry_ && d.banner) registry_->remove(d.banner);
            spdlog::info("[duel] {}'s request to {} lapsed", d.a_name, d.b_name);
            duels_.erase(it);
            it = next;
            continue;
        }
        if (d.phase == Phase::Counting && now >= d.begins_at) {
            d.phase = Phase::Fighting;
            d.began_at = now;
            update_banner(d);
            for (const DuelGuid& p : {d.a, d.b}) {
                DuelNotice n;
                n.kind = DuelNotice::Kind::Started;
                n.duel = d.id;
                n.other = p == d.a ? d.b_name : d.a_name;
                tell(p, n);
            }
            spdlog::info("[duel] {} vs {}: the fight begins", d.a_name, d.b_name);
        }
        if (d.phase == Phase::Fighting) {
            if (now - d.began_at >= settings_.max_seconds) {
                end(it, DuelResult::TimeOut, nullptr, "");
                it = next;
                continue;
            }
            // the radius: each participant may stray for flee_seconds; ExitRadiusTimestamp shows when he left (the synchronized clock), -1 while inside
            bool changed = false;
            const DuelGuid* fled = nullptr;
            for (const DuelGuid* p : {&d.a, &d.b}) {
                double x = 0, y = 0, z = 0;
                if (!where(*p, x, y, z)) continue;
                double& since = p == &d.a ? d.a_out_since : d.b_out_since;
                const bool outside = std::hypot(x - d.cx, y - d.cy) > settings_.radius_cm;
                if (outside && since < 0) {
                    since = now;
                    changed = true;
                } else if (!outside && since >= 0) {
                    since = -1;
                    changed = true;
                }
                if (outside && since >= 0 && now - since >= settings_.flee_seconds && !fled) fled = p;
            }
            if (fled) {
                const DuelGuid loser = *fled;
                end(it, DuelResult::Flee, &loser, "");
                it = next;
                continue;
            }
            if (changed) update_banner(d);
        }
        it = next;
    }
}

// ---- the banner -------------------------------------------------------------------------------------------------------------------------------------------------

namespace {
bool write_duel(repl::ObjectState& o, const DuelBook::Duel& d, std::string& err) {
    namespace util = spawns::util;
    const char* const words[] = {"Duel.InstanceGuid.A", "Duel.InstanceGuid.B", "Duel.InstanceGuid.C", "Duel.InstanceGuid.D"};
    bool ok = true;
    for (size_t i = 0; i < 4; ++i) ok = ok && util::set_path(o, words[i], guid_word(d.id[i]), err);
    repl::ArrayValue parts;
    for (const auto* p : {&d.a, &d.b}) {
        repl::ElementState e;
        e.values[1] = repl::Value::u64(static_cast<uint64_t>(p == &d.a ? kRoleInstigator : kRoleRecipient));
        for (size_t i = 0; i < 4; ++i) e.values[static_cast<uint16_t>(2 + i)] = guid_word((*p)[i]);
        const double since = p == &d.a ? d.a_out_since : d.b_out_since;
        e.values[6] = repl::Value::f64(since >= 0 ? bootstrap::effect_clock(since) : -1.0);
        parts.elements.push_back(std::move(e));
    }
    ok = ok && util::set_path(o, "Duel.Participants", repl::Value(std::move(parts)), err);
    // SyncBeginTime: -1 until the countdown; then when the fight begins, on the synchronized clock (the client counts down to it)
    ok = ok && util::set_path(o, "Duel.SyncBeginTime", repl::Value::f64(d.begins_at >= 0 ? bootstrap::effect_clock(d.begins_at) : -1.0), err);
    ok = ok && util::set_path(o, "Duel.bIsActive", repl::Value::boolean(d.phase == DuelBook::Phase::Fighting), err);
    return ok;
}
}  // namespace

void DuelBook::spawn_banner(Duel& d) {
    const protocol::schema::ClassSchema* schema = banner_schema();
    if (!registry_ || !mint_ || !schema) return;
    namespace util = spawns::util;
    WorldActor w;
    w.kind = ActorKind::DuelBanner;
    repl::Actor& a = w.actor;
    a.guid = mint_();
    w.id = a.guid.ObjectId;
    a.obj.schema = schema;
    std::string err;
    bool ok = util::set_path(a.obj, "AuthServerIDReplicated", repl::Value::u64(bootstrap::kAuthServerIdReplicated), err);
    {
        repl::RepMovement m;
        m.location = repl::quantize_location(a.obj.opts, d.cx, d.cy, d.cz);
        m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
        m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
        m.rotation = repl::Rot{0, 0, 0};
        ok = ok && util::set_path(a.obj, "ReplicatedMovement", repl::Value(m), err);
    }
    ok = ok && util::set_path(a.obj, "RemoteRole", repl::Value::u64(1), err);   // SimulatedProxy
    ok = ok && util::set_path(a.obj, "Role", repl::Value::u64(4), err);          // Authority
    ok = ok && write_duel(a.obj, d, err);
    if (!ok) {
        spdlog::error("[duel] the banner does not build: {}", err);
        return;
    }
    a.open.archetype = banner_archetype();
    a.open.level = bootstrap::persistent_level_export();
    a.open.spawn.location = repl::quantize_vector(10, d.cx, d.cy, d.cz);
    a.open.spawn.rotation = repl::Rot{0, 0, 0};
    a.open.spawn.velocity.reset();
    w.x = d.cx;
    w.y = d.cy;
    w.z = d.cz;
    w.never_dormant = true;   // (its state changes while the two stand by it: a dormant channel would miss the countdown)
    const uint64_t id = w.id;
    if (!registry_->add(std::move(w))) {
        spdlog::error("[duel] the registry refuses the banner");
        return;
    }
    d.banner = id;
}

void DuelBook::update_banner(const Duel& d) {
    if (!registry_ || !d.banner) return;
    std::string err;
    bool ok = true;
    registry_->modify(d.banner, Change::State, [&](WorldActor& w) { ok = write_duel(w.actor.obj, d, err); });
    if (!ok) spdlog::log(logged_++ < 5 ? spdlog::level::warn : spdlog::level::debug, "[duel] the banner of {} vs {} does not take the duel's state: {}", d.a_name, d.b_name, err);
}

}  // namespace aoc::worldd::world

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/actor_view.cpp
//
//  See actor_view.h.
// ============================================================================
#include "session/actor_view.h"

#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;
using world::ClientState;
using world::Ring;

namespace {
/// The update chains whose events the view waits for, over all actors, before it sweeps the routes of ones it no longer holds.
constexpr size_t kMaxRoutes = 4096;
}  // namespace

ActorView::ActorView(world::ActorRegistry& registry, Config config, std::function<bool(uint32_t)> reserved)
    : registry_(registry), config_(std::move(config)), reserved_(std::move(reserved)) {
    listener_ = registry_.on_remove([this](const world::WorldActor& a) { on_removed(a); });
}

ActorView::~ActorView() { finish(); }

void ActorView::finish() {
    if (finished_) return;
    finished_ = true;
    registry_.off_remove(listener_);
}
// C.O.A + C | Y | B

uint32_t ActorView::channel_of(uint64_t actor) const {
    const auto it = entries_.find(actor);
    if (it == entries_.end()) return 0;
    return it->second.state == ClientState::Awake || it->second.state == ClientState::Snapshot ? it->second.channel : 0;
}

ClientState ActorView::state_of(uint64_t actor) const {
    const auto it = entries_.find(actor);
    return it == entries_.end() ? ClientState::None : it->second.state;
}

size_t ActorView::count(ClientState state) const {
    size_t n = 0;
    for (const auto& [id, e] : entries_) n += e.state == state ? 1 : 0;
    return n;
}

std::string ActorView::describe() const {
    return std::to_string(count(ClientState::Awake)) + " awake, " + std::to_string(count(ClientState::Dormant)) + " dormant, " +
           std::to_string(count(ClientState::Snapshot)) + " snapshots closing; channels " + std::to_string(channel_owner_.size()) + " (peak " +
           std::to_string(stats_.peak_channels) + "); opens " + std::to_string(stats_.opens) + ", snapshots " + std::to_string(stats_.snapshots) + ", wakes " +
           std::to_string(stats_.wakes) + ", updates " + std::to_string(stats_.updates) + ", closes (dormancy " + std::to_string(stats_.closes_dormancy) +
           ", relevancy " + std::to_string(stats_.closes_relevancy) + ", destroyed " + std::to_string(stats_.closes_destroyed) + "); throttled passes " + std::to_string(stats_.throttled);
}

uint32_t ActorView::allocate_channel(const Connection& conn) const {
    for (uint32_t index = 3; index <= config_.max_channel; ++index) {
        if (conn.channel_open(index) || conn.channel_closing(index)) continue;
        if (reserved_ && reserved_(index)) continue;
        if (channel_owner_.count(index) != 0) continue;
        return index;
    }
    return 0;
}

void ActorView::release(Entry& e) {
    if (e.channel != 0) channel_owner_.erase(e.channel);
    e.channel = 0;
}

bool ActorView::send(Connection& conn, uint32_t channel, bool open, bool reliable, repl::Emission& em, ChainId* chain_id, size_t* chain_parts) {
    Chain chain;
    chain.channel = channel;
    chain.open = open;
    chain.reliable = reliable;
    chain.exports = std::move(em.exports);
    chain.payload = std::move(em.payload);
    if (!conn.can_send_chain(chain)) {
        em.exports = std::move(chain.exports);
        em.payload = std::move(chain.payload);
        return false;
    }
    size_t bits = chain.payload.bit_pos();
    for (const auto& x : chain.exports) bits += x.bit_pos();
    const size_t export_parts = chain.exports.size();
    const size_t parts = Connection::part_count(chain);
    const ChainId id = conn.send_chain(chain);
    if (id == 0) {
        em.exports = std::move(chain.exports);
        em.payload = std::move(chain.payload);
        return false;
    }
    stats_.bits_sent += bits;
    if (!em.exported_by_bunch.empty()) {
        ChainExports ce;
        ce.by_bunch = std::move(em.exported_by_bunch);
        ce.reliable = reliable;
        ce.outstanding = std::min(export_parts, ce.by_bunch.size());
        exports_[id] = std::move(ce);
    }
    if (trace_) trace_(channel, open, reliable, bits);
    if (chain_id) *chain_id = id;
    if (chain_parts) *chain_parts = parts;
    return true;
}

void ActorView::forget_chains(Entry& e) {
    for (const ChainId id : e.chains) chain_owner_.erase(id);
    e.chains.clear();
    e.ledger.clear();
}

const repl::Actor& ActorView::view_of(const world::WorldActor& a, repl::Actor& scratch) const {
    if (!config_.player_guid || !world::is_interactable(a.actor)) return a.actor;
    scratch = a.actor;
    world::set_player(scratch, *config_.player_guid, true);
    return scratch;
}

std::optional<repl::Emission> ActorView::build_open(const world::WorldActor& a, repl::ActorShadow& work) {
    bool failed = false;
    repl::Actor scratch;
    const repl::Actor& actor = view_of(a, scratch);
    if (a.open_exports.empty()) return repl::open_actor(actor, work, acks_, &failed);
    // the blocks by hand: the actor's block carries the exports its values name by path
    std::vector<repl::ContentBlock> blocks;
    repl::ShadowUndo undo;
    work.forget_sent();
    if (!repl::collect_open_blocks(actor, work, blocks, &undo) || blocks.empty() || blocks[0].sub) {
        undo.rollback(work);
        return std::nullopt;
    }
    for (const repl::ExportChain& x : a.open_exports) blocks[0].exports.push_back(x);
    auto em = repl::assemble_open(actor, blocks, acks_);
    if (!em) undo.rollback(work);
    return em;
}
// C-Y-B - C O A

bool ActorView::open(Connection& conn, world::WorldActor& a, Entry& e, bool snapshot, Clock::time_point now) {
    const uint32_t channel = allocate_channel(conn);
    if (channel == 0) {
        ++stats_.no_channel;
        ++stats_.refused_opens;
        return false;
    }
    repl::ActorShadow work;   // a new channel: the client has nothing of the actor (the shadow's masks stay: there are none)
    auto em = build_open(a, work);
    if (!em) {
        e.broken = true;
        ++stats_.broken;
        spdlog::warn("[actors] the open of actor {} ({}) does not build: it is never sent", a.id, a.npc.name.empty() ? a.npc.archetype : a.npc.name);
        return false;
    }
    ChainId open_chain = 0;
    size_t open_parts = 1;
    const size_t open_bits = em->payload.bit_pos();
    if (!send(conn, channel, /*open=*/true, /*reliable=*/true, *em, &open_chain, &open_parts)) {
        ++stats_.refused_opens;
        return false;
    }
    if (a.kind == world::ActorKind::NodeLayout) {   // Nodes 1, stage 3a: each open of a town's scenery, for the user's test
        spdlog::info("[scenery] open for player {} on channel {}: {} (node {}), level {}, {} items, PlotChangeVersion {}, {} payload bits, {} bits in the open", config_.player_id, channel,
                     a.scenery.name, a.scenery.node, a.scenery.level, a.scenery.items, a.scenery.plot_version, a.scenery.payload_bits, open_bits);
    }
    forget_chains(e);   // a new channel: the client has nothing, the open sends it all
    e.open_chain = open_chain;
    e.open_parts_left = open_parts;
    e.open_acked = false;
    e.chains.push_back(open_chain);
    chain_owner_[open_chain] = a.id;
    e.shadow = std::move(work);
    e.channel = channel;
    e.sent_version = a.version;
    e.sent_reliable = a.reliable_version;
    e.state = snapshot ? ClientState::Snapshot : ClientState::Awake;
    e.close_due = now + config_.relevancy.dormancy_delay;
    channel_owner_[channel] = a.id;
    stats_.peak_channels = std::max(stats_.peak_channels, channel_owner_.size());
    if (snapshot) pending_snapshots_.push_back(a.id);
    ++(snapshot ? stats_.snapshots : stats_.opens);
    return true;
}

bool ActorView::send_update(Connection& conn, world::WorldActor& a, Entry& e) {
    if (a.version == e.sent_version && a.reliable_version == e.sent_reliable) return true;
    repl::ActorShadow work = e.shadow;
    bool failed = false;
    repl::SentRecord record;
    repl::Actor scratch;
    auto em = repl::update_actor(view_of(a, scratch), work, acks_, &failed, &record);
    if (failed) {
        e.broken = true;
        ++stats_.broken;
        spdlog::warn("[actors] an update of actor {} does not build: it is not updated again", a.id);
        return false;
    }
    // reliably when the change must not be lost, when the client may not have the channel yet (open_acked), and while an earlier reliable chain
    // is in flight: its resend would otherwise land after this chain and overwrite it with older values (send_ledger.h)
    const bool reliable = a.reliable_version != e.sent_reliable || !e.open_acked || e.ledger.reliable_pending();
    if (!em) {   // the version moved but no handle changed (a value set to what it was)
        e.sent_version = a.version;
        e.sent_reliable = a.reliable_version;
        return true;
    }
    ChainId chain = 0;
    size_t parts = 1;
    if (!send(conn, e.channel, /*open=*/false, reliable, *em, &chain, &parts)) return false;
    e.shadow = std::move(work);
    e.sent_version = a.version;
    e.sent_reliable = a.reliable_version;
    e.ledger.sent(chain, reliable, parts, record, e.shadow);
    if (chain_trace_) chain_trace_(chain, a.id, reliable, parts, e.ledger.entries());
    if (e.ledger.tracks(chain)) {
        e.chains.push_back(chain);
        chain_owner_[chain] = a.id;
        if (chain_owner_.size() > kMaxRoutes) {   // routes of actors the view dropped while their events were in flight, or whose events never came
            for (auto it = chain_owner_.begin(); it != chain_owner_.end();) {
                const auto en = entries_.find(it->second);
                it = en == entries_.end() || !en->second.ledger.tracks(it->first) ? chain_owner_.erase(it) : std::next(it);
            }
        }
    }
    ++(reliable ? stats_.updates : stats_.updates_unreliable);
    if (a.kind == world::ActorKind::NodeLayout) {
        spdlog::info("[scenery] update for player {} on channel {}: {} (node {}), now level {}, {} items, PlotChangeVersion {}", config_.player_id, e.channel, a.scenery.name,
                     a.scenery.node, a.scenery.level, a.scenery.items, a.scenery.plot_version);
    }
    return true;
}

bool ActorView::close(Connection& conn, uint64_t id, Entry& e, uint32_t reason, ClientState next) {
    if (!conn.close_channel(e.channel, reason)) {
        ++stats_.refused_closes;
        return false;
    }
    if (reason != kCloseDormancy) {
        const world::WorldActor* a = registry_.find(id);
        if (a && a->kind == world::ActorKind::NodeLayout) {
            spdlog::info("[scenery] close for player {} on channel {}: {} (node {}), {}", config_.player_id, e.channel, a->scenery.name, a->scenery.node,
                         reason == kCloseRelevancy ? "out of range" : "destroyed");
        }
    }
    release(e);
    e.shadow = repl::ActorShadow{};
    // A Dormancy close leaves the client's actor as the last updates left it: an update still in flight that is lost makes that stale, so
    // the ledger and the routing of its events stay until they come (on_ack).  Any other close ends the entry.
    if (reason != kCloseDormancy) forget_chains(e);
    e.state = next;
    if (reason == kCloseDormancy) ++stats_.closes_dormancy;
    else if (reason == kCloseRelevancy) ++stats_.closes_relevancy;
    else ++stats_.closes_destroyed;
    return true;
}
// built by Cyb with C.O.A

void ActorView::on_removed(const world::WorldActor& a) {
    const auto it = entries_.find(a.id);
    if (it == entries_.end() || it->second.state == ClientState::None) return;
    Removed r;
    r.id = a.id;
    r.state = it->second.state;
    if (it->second.state == ClientState::Dormant) r.actor = a.actor;   // opened once more, to be destroyed
    removed_.push_back(std::move(r));
}

void ActorView::flush_removed(Connection& conn) {
    for (size_t i = 0; i < removed_.size();) {
        Removed& r = removed_[i];
        const auto it = entries_.find(r.id);
        if (it == entries_.end()) {
            removed_.erase(removed_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        Entry& e = it->second;
        bool done = false;
        if (e.state == ClientState::Awake || e.state == ClientState::Snapshot) {
            done = close(conn, r.id, e, kCloseDestroyed, ClientState::None);
        } else if (e.state == ClientState::Dormant) {
            // the client holds it dormant: a channel opened and closed with Destroyed makes it forget the actor
            const uint32_t channel = allocate_channel(conn);
            if (channel != 0) {
                repl::ActorShadow work;
                bool failed = false;
                auto em = repl::open_actor(r.actor, work, acks_, &failed);
                if (!em) {
                    done = true;   // cannot be written: the client keeps a ghost
                } else if (send(conn, channel, true, true, *em)) {
                    channel_owner_[channel] = r.id;
                    e.channel = channel;
                    e.state = ClientState::Awake;
                    done = close(conn, r.id, e, kCloseDestroyed, ClientState::None);
                }
            }
        } else {
            done = true;
        }
        if (done) {
            entries_.erase(r.id);
            removed_.erase(removed_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void ActorView::on_ack(const AckEvent& e) {
    const auto owner = chain_owner_.find(e.chain);
    if (owner != chain_owner_.end()) {
        const auto en = entries_.find(owner->second);
        if (en == entries_.end()) {
            chain_owner_.erase(owner);
        } else {
            Entry& entry = en->second;
            if (e.chain == entry.open_chain && e.delivered && entry.open_parts_left > 0 && --entry.open_parts_left == 0) entry.open_acked = true;
            const size_t naks = entry.ledger.naks(), before = entry.ledger.resent();
            entry.ledger.on_event(e.chain, e.delivered, entry.shadow);
            if (entry.ledger.naks() > naks) {
                ++stats_.naks;
                stats_.nak_entries += entry.ledger.resent() - before;
                // The shadow has been given back what was lost, but the view only builds an update for an actor whose version moved: it
                // moves it by forgetting what it sent (versions start at 1).  An awake actor is updated at the next pass; a dormant one's
                // snapshot is stale, and the next pass in the snapshot or the awake ring opens it again from the current state.
                // (`sent_reliable` stays: the resend of an awake actor's lost values is unreliable, as the chain was; a stale dormant snapshot
                // is opened again on `sent_version` alone)
                if (entry.sent_version != 0) {
                    entry.sent_version = 0;
                    if (entry.state == ClientState::Dormant) ++stats_.stale_snapshots;
                }
            }
            if (!entry.ledger.tracks(e.chain) && (e.chain != entry.open_chain || entry.open_acked)) {
                chain_owner_.erase(owner);
                entry.chains.erase(std::remove(entry.chains.begin(), entry.chains.end(), e.chain), entry.chains.end());
            }
        }
    }
    const auto it = exports_.find(e.chain);
    if (it == exports_.end() || !e.export_part) return;
    ChainExports& c = it->second;
    if (e.delivered) {
        if (e.part < c.by_bunch.size()) acks_.mark(c.by_bunch[e.part]);
    } else if (c.reliable) {
        return;   // the connection sends the part again and its event follows
    }
    if (c.outstanding > 0 && --c.outstanding == 0) exports_.erase(it);   // a lost unreliable part is final, too
}

bool ActorView::update_now(Connection& conn, uint64_t actor) {
    if (finished_) return false;
    const auto it = entries_.find(actor);
    if (it == entries_.end() || it->second.state != ClientState::Awake || it->second.broken) return false;
    world::WorldActor* a = registry_.find(actor);
    return a && send_update(conn, *a, it->second);
}

void ActorView::tick(Connection& conn, double x, double y, Clock::time_point now) {
    if (finished_) return;
    flush_removed(conn);
    // the Dormancy closes whose time has come: the snapshots in the order they were opened
    while (!pending_snapshots_.empty()) {
        const uint64_t id = pending_snapshots_.front();
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.state != ClientState::Snapshot) {   // woke up, was destroyed or forgotten meanwhile
            pending_snapshots_.pop_front();
            continue;
        }
        if (now < it->second.close_due) break;
        if (!close(conn, id, it->second, kCloseDormancy, ClientState::Dormant)) break;
        pending_snapshots_.pop_front();
    }
    if (passed_ && now - last_pass_ < config_.relevancy.tick) return;
    passed_ = true;
    last_pass_ = now;
    pass(conn, x, y, now);
}
// (C-Y-B) (C.O.A)

void ActorView::pass(Connection& conn, double x, double y, Clock::time_point now) {
    ++stats_.passes;
    if (conn.packets_in_flight() > config_.max_packets_in_flight || conn.queued_bunches() > config_.max_queued_bunches) {
        ++stats_.throttled;   // the player's own data goes first: nothing of the world is added to a busy connection
        return;
    }
    const world::RelevancyConfig& cfg = config_.relevancy;
    scratch_.clear();
    registry_.query(x, y, world::query_radius(cfg, largest_awake_), scratch_);
    registry_.query_wide(x, y, cfg.hysteresis_cm, scratch_);   // the towns' replicators (1.45 km): beyond the grid query's reach
    for (const auto& [id, e] : entries_) scratch_.push_back(id);   // what the view tracks, in range or not
    std::sort(scratch_.begin(), scratch_.end());
    scratch_.erase(std::unique(scratch_.begin(), scratch_.end()), scratch_.end());

    struct Cand {
        double   d;
        uint64_t id;
        Ring     ring;
    };
    std::vector<Cand> cands;
    cands.reserve(scratch_.size());
    for (const uint64_t id : scratch_) {
        const world::WorldActor* a = registry_.find(id);
        if (!a) continue;   // removed: flush_removed deals with it
        if (a->owner != 0 && a->owner != config_.player_id) continue;   // another player's (its lingering zones): not for this connection
        const auto it = entries_.find(id);
        const ClientState prior = it == entries_.end() ? ClientState::None : it->second.state;
        const double d = world::horizontal_distance(a->x, a->y, x, y);
        cands.push_back({d, id, world::classify(cfg, d, a->awake_radius_cm, a->never_dormant, prior)});
        // a wide actor (a town's replicator, 1.45 km) is found by query_wide: raising the grid query's radius to it would make every view visit a quarter of the world's cells for good
        if (a->awake_radius_cm > largest_awake_ && a->awake_radius_cm <= world::ActorRegistry::kWideCm) largest_awake_ = a->awake_radius_cm;
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& l, const Cand& r) { return l.d != r.d ? l.d < r.d : l.id < r.id; });

    size_t opens = 0;
    bool capped = false;
    for (const Cand& c : cands) {
        world::WorldActor* a = registry_.find(c.id);
        if (!a) continue;
        if (conn.queued_bunches() > config_.max_queued_bunches) {   // this pass's own sends filled the queue
            ++stats_.throttled;
            break;
        }
        auto it = entries_.find(c.id);
        if (it == entries_.end()) {
            if (c.ring == Ring::Out) continue;
            it = entries_.emplace(c.id, Entry{}).first;
        }
        Entry& e = it->second;
        if (e.broken) continue;
        switch (e.state) {
            case ClientState::None:
                if (c.ring == Ring::Out) {
                    entries_.erase(it);
                    break;
                }
                if (opens >= cfg.max_opens_per_tick) {
                    capped = true;
                    entries_.erase(it);
                    break;
                }
                if (open(conn, *a, e, c.ring == Ring::Snapshot, now)) ++opens;
                else if (!e.broken && e.state == ClientState::None) entries_.erase(it);
                break;
            case ClientState::Dormant:
                if (c.ring == Ring::Out) {
                    ++stats_.forgotten;
                    entries_.erase(it);   // the client keeps the actor it has
                    break;
                }
                // nothing that must not be lost changed since the snapshot: a walking NPC moves `version` every step, and a dormant actor's
                // position is sent whole when it wakes anyway (E1 review A2: re-opening on every move made each walker 160-170 m away, or near
                // another player, a whole snapshot about five times a second)
                if (c.ring == Ring::Snapshot && a->reliable_version == e.sent_reliable && e.sent_version != 0) break;
                if (opens >= cfg.max_opens_per_tick) {
                    capped = true;
                    break;
                }
                if (open(conn, *a, e, c.ring == Ring::Snapshot, now)) {
                    ++opens;
                    if (c.ring != Ring::Snapshot) ++stats_.wakes;   // (a new snapshot of a changed dormant actor is not a wake)
                }
                break;
            case ClientState::Snapshot:
                if (c.ring == Ring::Awake) {   // came close before the Dormancy close was due: it stays open
                    e.state = ClientState::Awake;
                    ++stats_.wakes;
                }
                break;   // its close is the pending list's
            case ClientState::Awake:
                if (c.ring == Ring::Awake) {
                    send_update(conn, *a, e);
                } else if (c.ring == Ring::Snapshot) {
                    if (send_update(conn, *a, e) || e.broken) {
                        const uint32_t version = a->version;
                        if (close(conn, c.id, e, kCloseDormancy, ClientState::Dormant)) e.sent_version = version;
                    }
                } else {
                    if (close(conn, c.id, e, kCloseRelevancy, ClientState::None)) entries_.erase(it);
                }
                break;
        }
    }
    if (capped) ++stats_.capped;
}

}  // namespace aoc::worldd

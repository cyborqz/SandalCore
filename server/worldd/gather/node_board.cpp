// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  gather/node_board.cpp (see node_board.h)
// ============================================================================
#include "gather/node_board.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace aoc::worldd::gather {

const char* state_name(NodeState s) {
    switch (s) {
        case NodeState::Available: return "available";
        case NodeState::Channeling: return "channeling";
        case NodeState::Gathered: return "gathered";
    }
    return "?";
}

NodeState NodeBoard::state_of(uint64_t node) const {
    const auto it = nodes_.find(node);
    return it == nodes_.end() ? NodeState::Available : it->second.state;
}

const Channel* NodeBoard::channel_of(uint64_t node) const {
    const auto it = nodes_.find(node);
    return it != nodes_.end() && it->second.state == NodeState::Channeling ? &it->second.channel : nullptr;
}

std::optional<Clock::time_point> NodeBoard::gathered_until(uint64_t node) const {
    const auto it = nodes_.find(node);
    if (it == nodes_.end() || it->second.state != NodeState::Gathered) return std::nullopt;
    return it->second.until;
}
// [C | O | A - Cyb]

size_t NodeBoard::channeling() const {
    size_t n = 0;
    for (const auto& [id, node] : nodes_) n += node.state == NodeState::Channeling ? 1 : 0;
    return n;
}

size_t NodeBoard::gathered() const {
    size_t n = 0;
    for (const auto& [id, node] : nodes_) n += node.state == NodeState::Gathered ? 1 : 0;
    return n;
}

std::vector<uint64_t> NodeBoard::channels_of(uint64_t player_id) const {
    std::vector<uint64_t> out;
    for (const auto& [id, node] : nodes_) {
        if (node.state == NodeState::Channeling && node.channel.player_id == player_id) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::chrono::duration<double> NodeBoard::respawn_of(double override_respawn, double empty_duration) const {
    const double base = override_respawn > 0 ? override_respawn : empty_duration;
    const double mult = cfg_.respawn_mult > 0 ? cfg_.respawn_mult : 1.0;
    return std::chrono::duration<double>(std::max(1.0, base * mult));
}

bool NodeBoard::begin(uint64_t node, const Channeler& by, Clock::time_point now, std::chrono::duration<double> length, std::string& why) {
    if (!registry_.find(node)) {
        why = "the node is not in the world";
        return false;
    }
    const NodeState state = state_of(node);
    if (state != NodeState::Available) {
        why = std::string("the node is ") + state_name(state);
        return false;
    }
    std::string error;
    bool wrote = false;
    bool corpse = false;
    registry_.modify(node, world::Change::State, [&](world::WorldActor& a) {
        corpse = a.kind == world::ActorKind::Npc;   // Gathering stage 3: a huntable creature's corpse
        wrote = corpse ? corpse_begin(a, by, error) : node_begin(a, by, error);
    });
    if (!wrote) {
        why = error;
        return false;
    }
    Node n;
    n.state = NodeState::Channeling;
    n.channel.player_id = by.player_id;
    n.channel.started = now;
    n.channel.completes = now + std::chrono::duration_cast<Clock::duration>(length);
    n.channel.next_beat = now + (corpse ? cfg_.corpse_beat : cfg_.beat);
    n.channel.step = 0;
    n.channel.corpse = corpse;
    n.channel.counter = 1;
    n.channel.last_beat = now;
    nodes_[node] = n;
    ++stats_.started;
    return true;
}

bool NodeBoard::beat(uint64_t node, Clock::time_point now) {
    const auto it = nodes_.find(node);
    if (it == nodes_.end() || it->second.state != NodeState::Channeling) return false;
    Channel& c = it->second.channel;
    if (now < c.next_beat || now >= c.completes) return false;   // none due; the completion is the session's
    std::string error;
    bool wrote = false;
    const uint32_t step = c.step + 1;
    const uint32_t counter = c.counter + kCorpseCounterStep;
    // the heartbeats are unreliable updates (position-only changes in the registry's terms): a lost one is replaced by the next
    registry_.modify(node, world::Change::Movement, [&](world::WorldActor& a) { wrote = c.corpse ? corpse_beat(a, counter, error) : node_beat(a, step, error); });
    if (!wrote) return false;
    c.step = step;
    if (c.corpse) c.counter = counter;
    c.last_beat = now;
    // one step however late: a stall does not make a burst of updates; the next is a beat after this one (or after now when far behind)
    const std::chrono::milliseconds period = c.corpse ? cfg_.corpse_beat : cfg_.beat;
    c.next_beat += period;
    if (c.next_beat < now) c.next_beat = now + period;
    ++stats_.beats;
    return true;
}

bool NodeBoard::complete(uint64_t node, Clock::time_point now, std::chrono::duration<double> respawn, std::string& why) {
    const auto it = nodes_.find(node);
    if (it == nodes_.end() || it->second.state != NodeState::Channeling) {
        why = std::string("the node is ") + state_name(state_of(node));
        return false;
    }
    std::string error;
    bool wrote = false;
    const Channel& c = it->second.channel;
    const uint32_t step = c.step + 1;
    // a corpse's done update: the counter one game tick (33 ms) on from the last update's when it follows within kCorpseDoneSplit, else two [R: 1456 1457 1458 1178 1179]
    const uint32_t done_counter = c.counter + (now - c.last_beat < kCorpseDoneSplit ? 1u : 2u);
    registry_.modify(node, world::Change::State, [&](world::WorldActor& a) { wrote = c.corpse ? corpse_done(a, done_counter, error) : node_done(a, step, error); });
    if (!wrote) {
        why = error.empty() ? "the node is not in the world" : error;
        if (!registry_.find(node)) {
            nodes_.erase(it);
            ++stats_.gone;
        }
        return false;
    }
    it->second.state = NodeState::Gathered;
    it->second.until = now + std::chrono::duration_cast<Clock::duration>(respawn);
    it->second.channel = Channel{};
    ++stats_.completed;
    return true;
}

bool NodeBoard::cancel(uint64_t node, const std::string& why) {
    const auto it = nodes_.find(node);
    if (it == nodes_.end() || it->second.state != NodeState::Channeling) return false;
    std::string error;
    bool wrote = false;
    registry_.modify(node, world::Change::State, [&](world::WorldActor& a) { wrote = a.kind == world::ActorKind::Npc ? corpse_reset(a, error) : node_reset(a, error); });
    if (!wrote && registry_.find(node)) {
        spdlog::warn("[gather] the node {} cannot be put back to available after a cancel ({}): {}", node, why, error);
    }
    nodes_.erase(it);
    ++stats_.cancelled;
    return true;
}
// -- C.Y.B . CoA --

size_t NodeBoard::cancel_player(uint64_t player_id, const std::string& why) {
    size_t n = 0;
    for (const uint64_t node : channels_of(player_id)) n += cancel(node, why) ? 1 : 0;
    return n;
}

size_t NodeBoard::advance(Clock::time_point now) {
    size_t changed = 0;
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        const uint64_t id = it->first;
        Node& n = it->second;
        if (!registry_.find(id)) {   // the node left the world
            it = nodes_.erase(it);
            ++stats_.gone;
            continue;
        }
        if (n.state == NodeState::Gathered && now >= n.until) {
            std::string error;
            bool wrote = false;
            registry_.modify(id, world::Change::State, [&](world::WorldActor& a) { wrote = a.kind == world::ActorKind::Npc ? corpse_reset(a, error) : node_reset(a, error); });
            if (!wrote) spdlog::warn("[gather] the node {} cannot be put back to available: {}", id, error);
            it = nodes_.erase(it);
            ++stats_.respawned;
            ++changed;
            continue;
        }
        if (n.state == NodeState::Channeling && now >= n.channel.completes + cfg_.stuck_grace) {
            spdlog::warn("[gather] the channel of player {} on node {} is {:.0f} s overdue: its session never completed it; the node is available again",
                         n.channel.player_id, id, std::chrono::duration<double>(now - n.channel.completes).count());
            std::string error;
            registry_.modify(id, world::Change::State, [&](world::WorldActor& a) { return a.kind == world::ActorKind::Npc ? corpse_reset(a, error) : node_reset(a, error); });
            it = nodes_.erase(it);
            ++stats_.stuck;
            ++changed;
            continue;
        }
        ++it;
    }
    return changed;
}

}  // namespace aoc::worldd::gather

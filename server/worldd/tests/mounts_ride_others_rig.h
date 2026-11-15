// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/mounts_ride_others_rig.h: other players watching a rider (the helpers of mounts_ride_others_tests.inc)
//
//  Several players in ONE world, each with a Harness of its own (its connection, its fake client, its session) that shares the first's PlayerWorld.  A step of the rig is one tick of
//  the world and one tick of every session, on one clock (the first harness's): the sessions tick in turn, as the server's loop ticks them.  What a watcher's client is sent is read
//  from its harness's `got` with the helpers of this file (a chain of the channel of an actor, the property blocks of a chain, the text of its exports).
//  Included by the .inc files that use it, in test_worldd_mounts_ride.cpp, after the program's own helpers.
// ============================================================================
#pragma once

#include <array>

/// The players of one world: the first is the clock's (Harness::group).
using Players = std::vector<Harness*>;

/// A player who joins the world of `first` now, with the same record's kit as any: its login is built and its client has phased in.  From now on a step of any of them is a step of all.
static std::unique_ptr<Harness> join_world(Harness& first, Players& all, svc::Character record) {
    if (all.empty()) all.push_back(&first);
    first.group = &all;
    auto h = std::make_unique<Harness>(std::move(record), nullptr, &first.fx, first.t);
    h->group = &all;
    all.push_back(h.get());
    h->play();
    return h;
}

/// A character other than cyb: its own id, name and account (two characters of one id cannot be in the world at once), at the place the world starts a character at, or at (x, y).
static svc::Character other_record(const std::string& id, const std::string& name, int account, std::optional<std::pair<double, double>> at = std::nullopt) {
    svc::Character c = cyb_record();
    c.id = id;
    c.name = name;
    c.account_id = account;
    if (at) {
        c.has_position = true;
        c.position.x = at->first;
        c.position.y = at->second;
        c.position.z = 0;
    }
    return c;
}

static std::string other_id(int n) {
    std::string id(32, '0');
    id[31] = "0123456789abcdef"[n & 15];
    id[30] = "0123456789abcdef"[(n >> 4) & 15];
    return id;
}

/// UPackageMapClient::SerializeNewActor in an open chain: the bit after it (0 when it does not read) and the place the actor opens at (cm).
struct NewActorAt {
    size_t                end = 0;
    std::optional<double> x, y, z;
};
static NewActorAt new_actor_at(const BunchWriter& w) {
    NewActorAt out;
    aoc::protocol::wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    for (int i = 0; i < 3; ++i) {   // the actor, its archetype and the level
        r.read_uint32();
        r.read_uint32();
        r.read_uint32();
        r.read_uint32();
    }
    if (r.read_bit()) {   // the location
        if (!r.read_bit()) return out;
        const auto q = repl::read_quantized_vector(r);
        if (!q) return out;
        const repl::Vec3d at = repl::dequantize_vector(*q, 10);
        out.x = at.x;
        out.y = at.y;
        out.z = at.z;
    }
    if (r.read_bit() && !repl::read_struct("FRotator", r)) return out;
    for (int i = 0; i < 2; ++i) {   // the scale and the velocity
        if (!r.read_bit()) continue;
        if (!r.read_bit() || !repl::read_quantized_vector(r)) return out;
    }
    out.end = r.overflowed() ? 0 : r.pos();
    return out;
}

/// The property blocks a chain carries by object (0: the actor), read with the schemas of `actor` and its subobjects; an open's blocks start after SerializeNewActor.
static States chain_states(const fake::Delivered& d, const repl::Actor& actor) {
    States out;
    const size_t start = d.open ? new_actor_at(d.content).end : 0;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), start);
    if (!blocks) return out;
    for (const repl::BlockView& b : *blocks) {
        const schema::ClassSchema* cls = nullptr;
        uint64_t id = 0;
        if (b.header.is_actor) {
            cls = actor.obj.schema;
        } else {
            const repl::Subobject* sub = actor.find(b.header.object);
            cls = sub ? sub->obj.schema : nullptr;
            id = b.header.object.ObjectId;
        }
        if (!cls) continue;
        BunchWriter w;
        w.write_bit_range(d.content.data(), b.payload_pos, b.payload_bits);
        auto st = repl::load_object_state(*cls, w.data(), b.payload_bits, b.header.has_rep_layout);
        if (st) out.emplace(id, std::move(*st));
    }
    return out;
}

// ---- what a watcher's client was sent ---------------------------------------------------------------------------------------------------

/// The registry's Player actor (the pawn the others are shown) of the player of `h`; null when there is none.
static const world::WorldActor* shown_pawn(const Harness& h) {
    const uint64_t id = static_cast<uint64_t>(h.s->bootstrap().setup().player_id);
    const world::WorldActor* found = nullptr;
    h.fx.world->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Player && a.owner == id) found = &a;
    });
    return found;
}

/// The chain that opened `channel` for the watcher last (an open at or after `from`), or null.
static const fake::Delivered* open_on(const Harness& watcher, uint32_t channel, size_t from = 0) {
    const fake::Delivered* found = nullptr;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        if (watcher.got[i].channel == channel && watcher.got[i].open) found = &watcher.got[i];
    }
    return found;
}

/// How many chains (opens included) the watcher's client got on `channel` from `from` on.
static size_t chains_count(const Harness& watcher, uint32_t channel, size_t from = 0) {
    size_t n = 0;
    for (size_t i = from; i < watcher.got.size(); ++i) n += watcher.got[i].channel == channel && !watcher.got[i].close;
    return n;
}

/// The closes the watcher's client got on `channel` from `from` on, with their reasons.
static std::vector<uint32_t> closes_on(const Harness& watcher, uint32_t channel, size_t from = 0) {
    std::vector<uint32_t> out;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        if (watcher.got[i].channel == channel && watcher.got[i].close) out.push_back(watcher.got[i].close_reason);
    }
    return out;
}

/// The value `path` of the property blocks a chain carries for the object `object` (0: the actor), nullptr when the chain holds none.
static const repl::Value* value_in(const States& s, uint64_t object, const char* path) {
    const auto it = s.find(object);
    if (it == s.end() || !it->second.schema) return nullptr;
    return it->second.state.get(*it->second.schema, path);
}

/// The object a chain's block says `path` is (nullopt: it does not say).
static std::optional<emit::FIntrepidNetworkGUID> object_in(const States& s, uint64_t object, const char* path) {
    const repl::Value* v = value_in(s, object, path);
    if (!v) return std::nullopt;
    if (const auto* g = v->get_if<emit::FIntrepidNetworkGUID>()) return *g;
    return std::nullopt;
}

static std::optional<int64_t> number_in(const States& s, uint64_t object, const char* path) {
    const auto it = s.find(object);
    if (it == s.end()) return std::nullopt;
    return int_of(it->second, path);
}

/// The ReplicatedMovement a chain carries for the actor: where it puts it (cm) and its velocity (cm/s); nullopt when the chain carries none.
struct SentMotion {
    double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
};
static std::optional<SentMotion> motion_in(const States& s) {
    const repl::Value* v = value_in(s, 0, "ReplicatedMovement");
    const auto* m = v ? v->get_if<repl::RepMovement>() : nullptr;
    if (!m) return std::nullopt;
    const repl::CodecOptions& opts = s.at(0).opts;
    const repl::Vec3d at = repl::dequantize_location(opts, m->location);
    const repl::Vec3d vel = repl::dequantize_velocity(opts, m->linear_velocity);
    return SentMotion{at.x, at.y, at.z, vel.x, vel.y, vel.z};
}

/// Whether the export bunches of a chain hold `text` as one byte per character, at any bit offset (the replicator packs its bits).
static bool exports_name(const fake::Delivered& d, const std::string& text) {
    for (const emit::BunchWriter& w : d.exports) {
        const size_t bytes = (w.bit_pos() + 7) / 8;
        const uint8_t* data = w.data();
        std::vector<uint8_t> shifted(bytes);
        for (int shift = 0; shift < 8; ++shift) {
            for (size_t i = 0; i < bytes; ++i) {
                const unsigned low = static_cast<unsigned>(data[i]) >> shift;
                const unsigned high = (shift == 0 || i + 1 >= bytes) ? 0u : (static_cast<unsigned>(data[i + 1]) << (8 - shift));
                shifted[i] = static_cast<uint8_t>((low | high) & 0xFFu);
            }
            if (std::search(shifted.begin(), shifted.end(), text.begin(), text.end()) != shifted.end()) return true;
        }
    }
    return false;
}

/// The latest value a watcher's client holds of `path` of the actor on `channel`, read from every chain it was sent since `from` (an open and the updates after it), oldest first: the
/// last one that carries it wins, as the client's copy does.  nullptr when no chain carried it.  `actor` is the registry's actor (the schemas of its objects).
struct ChannelState {
    States merged;     // by object: the values the chains carried, the latest of each
    size_t chains = 0;
};
static ChannelState channel_state(const Harness& watcher, uint32_t channel, const repl::Actor& actor, size_t from = 0) {
    ChannelState out;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close) continue;
        ++out.chains;
        States s = chain_states(d, actor);
        for (auto& [object, state] : s) {
            auto it = out.merged.find(object);
            if (it == out.merged.end()) {
                out.merged.emplace(object, std::move(state));
                continue;
            }
            for (const auto& [handle, value] : state.state.values()) it->second.state.set(*it->second.schema, static_cast<uint32_t>(handle), value);
        }
    }
    return out;
}

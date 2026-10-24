// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/map_stream.h  (the world stream, part 1: what is published)
//
//  The plain-C++ half of the world stream (svc/world_stream.h is its gRPC door): a compact snapshot of the world's players, NPCs, resource nodes, mounts and loot piles, the
//  snapshot builder the network thread runs five times a second, and the Cursor that works out, for one subscriber, what that subscriber is owed.  Nothing here takes a lock,
//  reads a clock or sends anything.
//
//  The snapshot.  One 32-byte record per entity (Rec).  The network thread builds it with a single pass over the actor registry and one over the players, so the cost grows with the
//  number of actors and not with the number of subscribers.  What the registry cannot say cheaply (an NPC's health, whether it is in a fight, whether it is dead) comes from the
//  fights themselves: CombatWorld lists the NPCs that have been in a fight (a few, not thousands), and the builder finds them by id in a small open-addressing table.  Every other
//  NPC is alive, at full health and out of combat, which is what it is.
//
//  The cursor.  A subscriber's Cursor keeps what it was last sent, per entity.  Given a snapshot it works out the entities that are new or differ and the ids that left the area, so a
//  subscriber that skipped snapshots is sent the difference against what it HAS, not against the snapshot before: nothing is lost by skipping.  Its first frame is a full snapshot.
//  A density cursor sends the counts per cell instead, whole, when they changed.
// ============================================================================
#pragma once

#include "world/actor_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::world::mapstream {

// The numbers below are the proto's (proto/internal/aoc/worldadmin/v1/worldadmin.proto).
enum Kind : uint8_t { kPlayer = 1, kNpc = 2, kResourceNode = 3, kMount = 4, kLootPile = 5 };
enum State : uint8_t { kInCombat = 1, kDead = 2, kMounted = 4 };
enum Layer : uint32_t { kLayerPlayers = 1, kLayerNpcs = 2, kLayerResourceNodes = 4, kLayerMounts = 8, kLayerLootPiles = 16, kLayerAll = 31 };

/// The health of an entity whose health is not tracked (a player, a resource node, a mount, a pile).
inline constexpr uint8_t kHealthUnknown = 255;
/// The largest side of an area a subscriber may ask for, and the most cells a density grid may have.
inline constexpr int64_t kMaxSideCm = 10'000'000;   // 100 km
inline constexpr int64_t kMaxCells = 65'536;

inline constexpr uint32_t layer_of(uint8_t kind) {
    switch (kind) {
        case kPlayer: return kLayerPlayers;
        case kNpc: return kLayerNpcs;
        case kResourceNode: return kLayerResourceNodes;
        case kMount: return kLayerMounts;
        case kLootPile: return kLayerLootPiles;
    }
    return 0;
}

/// The stream's kind of a registry actor; false for an actor the map does not show (a player's lingering effect, a town's replicator, a foliage cell, a station).
inline bool kind_of(ActorKind k, uint8_t& out) {
    switch (k) {
        case ActorKind::Npc: out = kNpc; return true;
        case ActorKind::Gatherable: out = kResourceNode; return true;
        case ActorKind::Mount: out = kMount; return true;
        case ActorKind::Pile: out = kLootPile; return true;
        default: return false;
    }
}

/// Whole centimetres, rounded to the nearest, clamped to what 32 bits hold; 0 for a number that is not a number.
inline int32_t to_cm(double v) {
    if (!(v == v)) return 0;
    const double r = std::floor(v + 0.5);
    if (r >= 2147483647.0) return INT32_MAX;
    if (r <= -2147483648.0) return INT32_MIN;
    return static_cast<int32_t>(r);
}

/// A facing in 8 bits: 0..255 is 0..360 degrees.
inline uint8_t yaw8(double degrees) {
    if (!std::isfinite(degrees)) return 0;
    double d = std::fmod(degrees, 360.0);
    if (d < 0) d += 360.0;
    return static_cast<uint8_t>(static_cast<int>(d * (256.0 / 360.0) + 0.5) & 0xFF);
}

/// One entity of a snapshot.
struct Rec {
    uint64_t id = 0;
    int32_t  x = 0, y = 0, z = 0;   // cm
    uint8_t  kind = 0, yaw = 0, state = 0, health = kHealthUnknown;
    uint16_t level = 0;

    bool operator==(const Rec& o) const {
        return id == o.id && x == o.x && y == o.y && z == o.z && kind == o.kind && yaw == o.yaw && state == o.state && health == o.health && level == o.level;
    }
};
static_assert(sizeof(Rec) <= 32, "a snapshot record is meant to stay small");

/// What the fights know of an NPC that has been in one: the registry has no cheap way to say it.
struct Overlay {
    uint64_t id = 0;
    uint8_t  health = 100;   // percent of its maximum
    uint8_t  state = 0;      // State bits
};

/// A player of the snapshot (the session's own numbers; players are not registry actors).
struct PlayerRow {
    uint64_t    id = 0;       // the pawn's actor id
    std::string name;
    double      x = 0, y = 0, z = 0, yaw_degrees = 0;
    bool        mounted = false, dead = false;
    int         level = 0;
};

struct PlayerName {
    uint64_t    id = 0;
    std::string name;
};

struct Counts {
    uint32_t players = 0, npcs = 0, resource_nodes = 0, mounts = 0, loot_piles = 0;
};

/// The world as the network thread saw it at one publish.
struct Snapshot {
    uint64_t                tick = 0;         // the publish's number, 1 for the first
    double                  world_time = 0;   // seconds
    std::vector<Rec>        recs;
    std::vector<PlayerName> names;            // the players' names (a few)
    Counts                  counts;
    float                   tick_ms = 0, publish_ms = 0, publish_max_ms = 0;   // the stream's own numbers at the time
};

/// A table from ids to the rows of an Overlay list, built once per publish.  Open addressing with a power-of-two size: a lookup is a multiply, a shift and a read or two of a table that
/// fits the cache, which is what keeps the pass over ten thousand NPCs cheap.
class OverlayIndex {
public:
    void build(const std::vector<Overlay>& rows) {
        rows_ = &rows;
        size_t n = 16;
        while (n < rows.size() * 2) n <<= 1;
        slots_.assign(n, 0);
        mask_ = n - 1;
        for (size_t i = 0; i < rows.size(); ++i) {
            size_t h = slot_of(rows[i].id);
            while (slots_[h] != 0) h = (h + 1) & mask_;
            slots_[h] = static_cast<uint32_t>(i + 1);
        }
    }

    const Overlay* find(uint64_t id) const {
        if (!rows_ || rows_->empty()) return nullptr;
        for (size_t h = slot_of(id); slots_[h] != 0; h = (h + 1) & mask_) {
            const Overlay& o = (*rows_)[slots_[h] - 1];
            if (o.id == id) return &o;
        }
        return nullptr;
    }

private:
    size_t slot_of(uint64_t id) const { return static_cast<size_t>((id * 0x9E3779B97F4A7C15ull) >> 32) & mask_; }

    const std::vector<Overlay>* rows_ = nullptr;
    std::vector<uint32_t>       slots_;
    size_t                      mask_ = 0;
};

/// The record of a registry actor.  `ov` is its row of the fights' overlay, if it has one.
inline Rec make_rec(const WorldActor& a, uint8_t kind, const Overlay* ov) {
    Rec r;
    r.id = a.id;
    r.kind = kind;
    r.x = to_cm(a.x);
    r.y = to_cm(a.y);
    r.z = to_cm(a.z);
    r.yaw = yaw8(a.yaw_degrees);
    r.level = static_cast<uint16_t>(std::clamp(a.npc.level, 0, 65535));
    if (kind == kNpc) {
        r.health = 100;
        if (ov) {
            r.health = ov->health;
            r.state = ov->state;
        }
    }
    return r;
}

inline Rec make_rec(const PlayerRow& p) {
    Rec r;
    r.id = p.id;
    r.kind = kPlayer;
    r.x = to_cm(p.x);
    r.y = to_cm(p.y);
    r.z = to_cm(p.z);
    r.yaw = yaw8(p.yaw_degrees);
    r.level = static_cast<uint16_t>(std::clamp(p.level, 0, 65535));
    r.state = static_cast<uint8_t>((p.mounted ? kMounted : 0) | (p.dead ? kDead : 0));
    return r;
}

/// The records of the registry's mapped actors (NPCs, resource nodes, mounts, loot piles), kept up to date by the registry's own change notifications, so that a publish copies an array
/// of 32-byte records (a few tens of microseconds for twelve thousand) and does not walk twelve thousand actors of a kilobyte each.  Every change of an actor that the replication counts
/// goes through ActorRegistry::touch (a move and a modify among them), which is where `touched` is called from; an actor that was never touched has not changed.  The records carry the
/// defaults of an NPC (alive, full health, out of combat): the fights' overlay is put on a copy at each publish.
class Mirror final : public ActorRegistry::Observer {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    Mirror() = default;
    Mirror(const Mirror&) = delete;
    Mirror& operator=(const Mirror&) = delete;
    ~Mirror() override { detach(); }

    /// Reads the registry as it is and has the registry tell this mirror of every change from now on.
    void attach(ActorRegistry& registry) {
        detach();
        registry_ = &registry;
        rebuild(registry);
        registry.set_observer(this);
    }

    void detach() {
        if (registry_) registry_->set_observer(nullptr);
        registry_ = nullptr;
    }

    /// Starts over from the registry's contents.
    void rebuild(const ActorRegistry& registry) {
        recs_.clear();
        slot_.clear();
        counts_ = {};
        registry.for_each([&](const WorldActor& a) { added(a); });
    }

    void added(const WorldActor& a) override {
        uint8_t kind = 0;
        if (!kind_of(a.kind, kind) || slot_.count(a.id) != 0) return;
        slot_[a.id] = static_cast<uint32_t>(recs_.size());
        recs_.push_back(make_rec(a, kind, nullptr));
        count(kind, 1);
    }

    void touched(const WorldActor& a) override {
        uint8_t kind = 0;
        if (!kind_of(a.kind, kind)) return;
        const auto it = slot_.find(a.id);
        if (it == slot_.end()) return;
        Rec& r = recs_[it->second];
        r.x = to_cm(a.x);
        r.y = to_cm(a.y);
        r.z = to_cm(a.z);
        r.yaw = yaw8(a.yaw_degrees);
    }

    void removed(const WorldActor& a) override {
        const auto it = slot_.find(a.id);
        if (it == slot_.end()) return;
        const uint32_t slot = it->second;
        count(recs_[slot].kind, -1);
        if (slot + 1 != recs_.size()) {   // the last record takes the hole
            recs_[slot] = recs_.back();
            slot_[recs_[slot].id] = slot;
        }
        recs_.pop_back();
        slot_.erase(a.id);
    }

    const std::vector<Rec>& recs() const { return recs_; }
    const Counts& counts() const { return counts_; }
    /// The slot of an entity in recs(); npos for an id the mirror does not hold.
    size_t slot_of(uint64_t id) const {
        const auto it = slot_.find(id);
        return it == slot_.end() ? npos : it->second;
    }

    /// How many records differ from what the registry says now (a missing one, an extra one, a different one): 0 when the mirror is right.  A walk over the registry; run it rarely.
    size_t verify(const ActorRegistry& registry) const {
        size_t different = 0, mapped = 0;
        registry.for_each([&](const WorldActor& a) {
            uint8_t kind = 0;
            if (!kind_of(a.kind, kind)) return;
            ++mapped;
            const size_t slot = slot_of(a.id);
            if (slot == npos || !(recs_[slot] == make_rec(a, kind, nullptr))) ++different;
        });
        if (mapped < recs_.size()) different += recs_.size() - mapped;   // records of actors that are gone
        return different;
    }

private:
    void count(uint8_t kind, int delta) {
        const auto step = [&](uint32_t& n) { n = static_cast<uint32_t>(static_cast<int64_t>(n) + delta); };
        switch (kind) {
            case kNpc: step(counts_.npcs); break;
            case kResourceNode: step(counts_.resource_nodes); break;
            case kMount: step(counts_.mounts); break;
            case kLootPile: step(counts_.loot_piles); break;
            default: break;
        }
    }

    ActorRegistry*                         registry_ = nullptr;
    std::vector<Rec>                       recs_;
    std::unordered_map<uint64_t, uint32_t> slot_;
    Counts                                 counts_;
};

/// Builds snapshots from the mirror.
class SnapshotBuilder {
public:
    /// `out` becomes the mirror's records with the fights' overlay put on the NPCs that have been in a fight, then the players.
    void build(Snapshot& out, const Mirror& mirror, const std::vector<Overlay>& overlay, const std::vector<PlayerRow>& players, double world_time, uint64_t tick) const {
        out.tick = tick;
        out.world_time = world_time;
        out.names.clear();
        out.recs = mirror.recs();   // reuses out.recs' room: a copy of plain bytes
        out.counts = mirror.counts();
        for (const Overlay& o : overlay) {
            const size_t slot = mirror.slot_of(o.id);
            if (slot == Mirror::npos) continue;
            out.recs[slot].health = o.health;
            out.recs[slot].state = o.state;
        }
        for (const PlayerRow& p : players) {
            if (p.id == 0) continue;
            out.recs.push_back(make_rec(p));
            out.names.push_back({p.id, p.name});
            ++out.counts.players;
        }
    }
};

// ---- what one subscriber asks for -------------------------------------------------------------------------------------------------------

/// A box in cm: min inclusive, max exclusive.
struct Area {
    int32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool contains(int32_t x, int32_t y) const { return x >= min_x && x < max_x && y >= min_y && y < max_y; }
};

struct Request {
    Area     area;
    bool     density = false;
    uint32_t cell_cm = 50000;       // density only: 20000, 50000 or 100000
    uint32_t layers = kLayerAll;
};

inline int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

/// Why a request is refused; empty when it is fine.
inline std::string validate(const Request& r) {
    const int64_t w = static_cast<int64_t>(r.area.max_x) - r.area.min_x, h = static_cast<int64_t>(r.area.max_y) - r.area.min_y;
    if (w <= 0 || h <= 0) return "the area is empty: its maximum must be above its minimum on both axes";
    if (w > kMaxSideCm || h > kMaxSideCm) return "the area is larger than 100 km on a side";
    if ((r.layers & ~static_cast<uint32_t>(kLayerAll)) != 0) return "unknown layer bits (the layers are 1 players, 2 NPCs, 4 resource nodes, 8 mounts, 16 loot piles)";
    if (r.density) {
        if (r.cell_cm != 20000 && r.cell_cm != 50000 && r.cell_cm != 100000) return "the density cell must be 20000, 50000 or 100000 cm";
        const int64_t cell = r.cell_cm;
        const int64_t ox = floor_div(r.area.min_x, cell) * cell, oy = floor_div(r.area.min_y, cell) * cell;
        const int64_t cols = (static_cast<int64_t>(r.area.max_x) - ox + cell - 1) / cell, rows = (static_cast<int64_t>(r.area.max_y) - oy + cell - 1) / cell;
        if (cols * rows > kMaxCells) return "the density grid would have more than 65,536 cells: ask for a smaller area or a larger cell";
    }
    return {};
}

/// Counts per cell.
struct Grid {
    uint32_t              cell_cm = 0;
    int32_t               origin_x = 0, origin_y = 0;
    uint32_t              cols = 0, rows = 0;
    std::vector<uint32_t> players, npcs, resource_nodes;

    bool operator==(const Grid& o) const {
        return cell_cm == o.cell_cm && origin_x == o.origin_x && origin_y == o.origin_y && cols == o.cols && rows == o.rows && players == o.players && npcs == o.npcs &&
               resource_nodes == o.resource_nodes;
    }
};

/// What a subscriber is owed.
struct Frame {
    enum class Type : uint8_t { None, Snapshot, Delta, Density };
    Type                    type = Type::None;
    std::vector<Rec>        changed;   // a Snapshot: every entity in the area; a Delta: the ones that are new or differ
    std::vector<PlayerName> names;     // the names of the players among `changed` that are new to this subscriber
    std::vector<uint64_t>   removed;   // a Delta: the ids that left the area or the world
    Grid                    grid;      // Density
};

/// One subscriber's side: what it was last sent, and the work of finding what it is owed.  The request must have passed validate().
class Cursor {
public:
    explicit Cursor(const Request& request) : req_(request) {
        if (req_.density) {
            const int64_t cell = req_.cell_cm;
            ox_ = floor_div(req_.area.min_x, cell) * cell;
            oy_ = floor_div(req_.area.min_y, cell) * cell;
            cols_ = static_cast<uint32_t>((static_cast<int64_t>(req_.area.max_x) - ox_ + cell - 1) / cell);
            rows_ = static_cast<uint32_t>((static_cast<int64_t>(req_.area.max_y) - oy_ + cell - 1) / cell);
        }
    }

    /// What the subscriber is owed from `snap`: a Snapshot the first time; after it a Delta when an entity is new, differs or left (or `heartbeat`, which asks for a frame anyway, empty
    /// when nothing changed); a Density grid when the counts changed (or `heartbeat`).  Type::None when there is nothing to send.
    void next(const Snapshot& snap, bool heartbeat, Frame& out) {
        out.type = Frame::Type::None;
        out.changed.clear();
        out.names.clear();
        out.removed.clear();
        if (req_.density) {
            next_density(snap, heartbeat, out);
        } else {
            next_entities(snap, heartbeat, out);
        }
    }

    /// Entities the subscriber has been sent and not told to forget.
    size_t held() const { return held_.size(); }

private:
    struct Held {
        Rec      rec;
        uint32_t gen = 0;
    };

    void next_entities(const Snapshot& snap, bool heartbeat, Frame& out) {
        ++gen_;
        const bool first = !started_;
        for (const Rec& r : snap.recs) {
            if ((req_.layers & layer_of(r.kind)) == 0 || !req_.area.contains(r.x, r.y)) continue;
            const auto [it, fresh] = held_.try_emplace(r.id);
            Held& h = it->second;
            h.gen = gen_;
            if (fresh || !(h.rec == r)) {
                h.rec = r;
                out.changed.push_back(r);
                if (fresh && r.kind == kPlayer) {
                    for (const PlayerName& n : snap.names) {
                        if (n.id == r.id) {
                            out.names.push_back(n);
                            break;
                        }
                    }
                }
            }
        }
        for (auto it = held_.begin(); it != held_.end();) {
            if (it->second.gen != gen_) {
                out.removed.push_back(it->first);
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
        started_ = true;
        if (first) {
            out.type = Frame::Type::Snapshot;
        } else if (!out.changed.empty() || !out.removed.empty() || heartbeat) {
            out.type = Frame::Type::Delta;
        }
    }

    void next_density(const Snapshot& snap, bool heartbeat, Frame& out) {
        Grid g;
        g.cell_cm = req_.cell_cm;
        g.origin_x = static_cast<int32_t>(ox_);
        g.origin_y = static_cast<int32_t>(oy_);
        g.cols = cols_;
        g.rows = rows_;
        const size_t cells = static_cast<size_t>(cols_) * rows_;
        g.players.assign(cells, 0);
        g.npcs.assign(cells, 0);
        g.resource_nodes.assign(cells, 0);
        const int64_t cell = req_.cell_cm;
        for (const Rec& r : snap.recs) {
            if ((req_.layers & layer_of(r.kind)) == 0 || !req_.area.contains(r.x, r.y)) continue;
            const int64_t col = (static_cast<int64_t>(r.x) - ox_) / cell, row = (static_cast<int64_t>(r.y) - oy_) / cell;
            if (col < 0 || row < 0 || col >= cols_ || row >= rows_) continue;
            const size_t i = static_cast<size_t>(row) * cols_ + static_cast<size_t>(col);
            switch (r.kind) {
                case kPlayer: ++g.players[i]; break;
                case kNpc:
                    if ((r.state & kDead) == 0) ++g.npcs[i];
                    break;
                case kResourceNode: ++g.resource_nodes[i]; break;
                default: break;
            }
        }
        const bool changed = !started_ || !(g == last_grid_);
        started_ = true;
        if (changed || heartbeat) {
            last_grid_ = g;
            out.grid = std::move(g);
            out.type = Frame::Type::Density;
        }
    }

    Request                              req_;
    bool                                 started_ = false;
    uint32_t                             gen_ = 0;
    std::unordered_map<uint64_t, Held>   held_;
    int64_t                              ox_ = 0, oy_ = 0;
    uint32_t                             cols_ = 0, rows_ = 0;
    Grid                                 last_grid_;
};

}  // namespace aoc::worldd::world::mapstream

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  svc/world_stream_impl.h  (the implementation of svc/world_stream.h)
//
//  A header of its own so that worldd's CMake file did not have to change: include it from exactly ONE .cpp (svc/world_admin.cpp does).  Its functions are defined here and are not
//  inline, so a second include would not link.  The snapshot, the cursor and the request checks are world/map_stream.h's.
// ============================================================================
#pragma once

#include "svc/world_stream.h"

#include "aoc/worldadmin/v1/worldadmin.grpc.pb.h"
#include "combat/combat_world.h"
#include "session/player_world.h"
#include "svc/service_token.h"
#include "world/map_stream.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/resource_quota.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace aoc { namespace worldd { namespace svc {

namespace wsv1 = ::aoc::worldadmin::v1;
namespace mstream = ::aoc::worldd::world::mapstream;

namespace stream_detail {

/// "/Game/.../Wolf.Wolf_C" -> "Wolf": the object's name without the Blueprint class suffix.
inline std::string leaf_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = leaf.find_last_of('.');
    if (dot != std::string::npos) leaf = leaf.substr(dot + 1);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    return leaf;
}

/// What an actor is called: the nameplate it shows, else its label, else its Blueprint's name.  `scratch` holds the name when it has to be made.
inline const std::string& display_name(const world::WorldActor& a, std::string& scratch) {
    if (!a.npc.name.empty()) return a.npc.name;
    scratch = a.label.empty() ? leaf_of(a.npc.archetype) : a.label;
    constexpr char kDefault[] = "Default__";   // a class default object's name: "Default__AlphaWolf" is an AlphaWolf
    if (scratch.compare(0, sizeof kDefault - 1, kDefault) == 0) scratch.erase(0, sizeof kDefault - 1);
    if (scratch.size() > 2 && scratch.compare(scratch.size() - 2, 2, "_C") == 0) scratch.resize(scratch.size() - 2);
    return scratch;
}

/// Whether `hay` holds `needle_lower` (already lower case), ignoring case.
inline bool ci_contains(const std::string& hay, const std::string& needle_lower) {
    if (needle_lower.empty()) return true;
    if (hay.size() < needle_lower.size()) return false;
    const size_t last = hay.size() - needle_lower.size();
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        while (j < needle_lower.size() && std::tolower(static_cast<unsigned char>(hay[i + j])) == static_cast<unsigned char>(needle_lower[j])) ++j;
        if (j == needle_lower.size()) return true;
    }
    return false;
}

inline bool ci_less(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int x = std::tolower(static_cast<unsigned char>(a[i])), y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}

inline const char* ai_state_name(npc::ai::State s) {
    switch (s) {
        case npc::ai::State::Idle: return "idle";
        case npc::ai::State::Chase: return "chase";
        case npc::ai::State::Attack: return "attack";
        case npc::ai::State::Reset: return "reset";
        case npc::ai::State::Dead: return "dead";
    }
    return "none";
}

inline const char* source_name(world::Source s) {
    switch (s) {
        case world::Source::Data: return "data";
        case world::Source::Captured: return "captured";
        case world::Source::Runtime: return "runtime";
    }
    return "";
}

inline void put_entity(wsv1::Entity* e, const mstream::Rec& r) {
    e->set_id(r.id);
    e->set_kind(static_cast<wsv1::EntityKind>(r.kind));
    e->set_x_cm(r.x);
    e->set_y_cm(r.y);
    e->set_z_cm(r.z);
    e->set_yaw(r.yaw);
    e->set_state(r.state);
    e->set_health_pct(r.health);
    e->set_level(r.level);
}

inline void put_stats(wsv1::StreamStats* s, const mstream::Snapshot& snap) {
    s->set_tick_ms(snap.tick_ms);
    s->set_publish_ms(snap.publish_ms);
    s->set_publish_max_ms(snap.publish_max_ms);
    s->set_players(snap.counts.players);
    s->set_npcs(snap.counts.npcs);
    s->set_resource_nodes(snap.counts.resource_nodes);
    s->set_mounts(snap.counts.mounts);
}

}  // namespace stream_detail

// ---- the implementation ---------------------------------------------------------------------------------------------------------------

struct WorldStreamServer::Impl final : wsv1::WorldStreamService::Service {
    using SteadyClock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kPublishPeriod{200};   // five a second
    static constexpr std::chrono::seconds      kHeartbeat{2};         // a stream with nothing to say still sends a frame this often
    static constexpr std::chrono::seconds      kLogEvery{60};

    Impl(PlayerWorld& w, Directory d, AdminQueue& q, Options o) : world(w), directory(std::move(d)), queue(q), options(std::move(o)) { mirror.attach(world.registry()); }

    PlayerWorld& world;
    Directory    directory;
    AdminQueue&  queue;
    Options      options;

    // ---- shared with the stream threads: the newest snapshot, and the flags
    std::mutex                              mu;
    std::condition_variable                 cv;
    std::shared_ptr<const mstream::Snapshot> latest;
    bool                                    stopping = false;
    std::atomic<size_t>                     open_streams{0};
    std::atomic<uint64_t>                   refused_calls{0};
    std::mutex                              log_mu;
    SteadyClock::time_point                 last_refusal_log{};
    uint64_t                                held_back = 0;

    // ---- the network thread's alone (publish and note_loop)
    std::vector<std::shared_ptr<mstream::Snapshot>> pool;   // every buffer in use or free to be built into
    mstream::Mirror                         mirror;     // the registry's mapped actors as records, kept by the registry's own notifications
    mstream::SnapshotBuilder                builder;
    std::vector<mstream::Overlay>           overlay;
    std::vector<combat::CombatWorld::Summary> summaries;
    std::vector<mstream::PlayerRow>         rows;
    SteadyClock::time_point                 next_publish{}, window_start{}, last_error_log{};
    uint64_t                                publishes = 0;
    double                                  publish_ms = 0, loop_ms = 0;                              // smoothed
    double                                  window_sum = 0, window_max = 0, last_window_max = 0;      // publish cost in the minute
    double                                  loop_sum = 0, loop_max = 0;                                // the loop's work in the minute
    uint64_t                                window_n = 0, loop_n = 0;
    mstream::Counts                         last_counts;
    size_t                                  last_recs = 0;

    // ---- the token ---------------------------------------------------------------------------------------------------------------------

    /// The token check: exactly one "authorization" header, equal to "Bearer <token>", compared without stopping at the first different byte.
    grpc::Status authorize(grpc::ServerContext* ctx, const char* method) {
        const auto range = ctx->client_metadata().equal_range(kAuthorizationMetadata);
        bool ok = false;
        if (!options.token.empty() && std::distance(range.first, range.second) == 1) {
            const std::string want = "Bearer " + options.token;
            const grpc::string_ref got = range.first->second;
            unsigned char diff = got.size() == want.size() ? 0 : 1;
            for (size_t i = 0; i < want.size(); ++i) diff |= static_cast<unsigned char>(i < got.size() ? got.data()[i] : 0) ^ static_cast<unsigned char>(want[i]);
            ok = diff == 0;
        }
        if (ok) return grpc::Status::OK;
        ++refused_calls;
        {
            std::lock_guard<std::mutex> lock(log_mu);
            ++held_back;
            const auto now = SteadyClock::now();
            if (last_refusal_log == SteadyClock::time_point{} || now - last_refusal_log >= std::chrono::seconds(1)) {
                spdlog::warn("[stream] {} from {} refused: missing or wrong service token ({} refusal(s) since the last line)", method, ctx->peer(), held_back);
                held_back = 0;
                last_refusal_log = now;
            }
        }
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "missing or wrong service token");
    }

    /// Runs `f` on the network thread and waits for it.
    template <class F>
    grpc::Status on_net_thread(F&& f) {
        if (!queue.run(std::forward<F>(f), options.call_timeout)) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "worldd's network thread did not take the call in time (is worldd stopping?)");
        }
        return grpc::Status::OK;
    }

    // ---- gathering (network thread) ----------------------------------------------------------------------------------------------------

    void gather_players(std::vector<mstream::PlayerRow>& out) {
        out.clear();
        for (const PlayerHandle& h : directory()) {
            if (!h) continue;
            const PlayerSession& p = *h.player;
            mstream::PlayerRow r;
            r.id = p.bootstrap().setup().guids.pawn.ObjectId;
            if (r.id == 0) continue;   // the pawn is not made yet
            const GoodPosition g = p.last_position();
            r.name = p.name();
            r.x = g.x;
            r.y = g.y;
            r.z = g.z;
            r.yaw_degrees = g.yaw_degrees;
            r.mounted = p.mounted();
            r.dead = p.dead();
            r.level = p.progress().level;
            out.push_back(std::move(r));
        }
    }

    /// The NPCs that have been in a fight, with their health and flags (a few, not thousands).
    void gather_overlay(std::vector<mstream::Overlay>& out, std::vector<combat::CombatWorld::Summary>& scratch) {
        out.clear();
        scratch.clear();
        combat::CombatWorld* cw = world.combat();
        if (!cw) return;
        cw->summaries(scratch);
        out.reserve(scratch.size());
        for (const auto& s : scratch) {
            mstream::Overlay o;
            o.id = s.id;
            o.health = s.health_pct;
            o.state = static_cast<uint8_t>((s.in_combat ? mstream::kInCombat : 0) | (s.dead ? mstream::kDead : 0));
            out.push_back(o);
        }
    }

    // ---- publishing (network thread) ---------------------------------------------------------------------------------------------------

    /// A snapshot to build into: one nobody holds any more, else a new one.
    std::shared_ptr<mstream::Snapshot> take_buffer() {
        for (auto& p : pool) {
            if (p.use_count() == 1) return p;
        }
        if (pool.size() >= 16) pool.erase(pool.begin());   // a stream holds a buffer for as long as it works out a frame; this is never reached in practice
        pool.push_back(std::make_shared<mstream::Snapshot>());
        return pool.back();
    }

    void publish() {
        const auto start = SteadyClock::now();
        {
            std::lock_guard<std::mutex> lock(mu);
            if (stopping) return;
        }
        if (start < next_publish) return;
        next_publish = (next_publish == SteadyClock::time_point{} || start - next_publish > 4 * kPublishPeriod) ? start + kPublishPeriod : next_publish + kPublishPeriod;
        try {
            gather_players(rows);
            gather_overlay(overlay, summaries);
            std::shared_ptr<mstream::Snapshot> snap = take_buffer();
            builder.build(*snap, mirror, overlay, rows, world.world_clock(), publishes + 1);
            ++publishes;
            snap->tick_ms = static_cast<float>(loop_ms);
            snap->publish_ms = static_cast<float>(publish_ms);
            snap->publish_max_ms = static_cast<float>(std::max(window_max, last_window_max));
            last_counts = snap->counts;
            last_recs = snap->recs.size();
            {
                std::lock_guard<std::mutex> lock(mu);
                latest = snap;
            }
            cv.notify_all();
        } catch (const std::exception& e) {
            if (last_error_log == SteadyClock::time_point{} || start - last_error_log >= std::chrono::seconds(10)) {
                spdlog::error("[stream] a publish threw {}: going on", e.what());
                last_error_log = start;
            }
        }
        note_publish(std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count());
    }

    void note_publish(double ms) {
        publish_ms = publishes <= 1 ? ms : publish_ms * 0.8 + ms * 0.2;
        window_sum += ms;
        ++window_n;
        window_max = std::max(window_max, ms);
        const auto now = SteadyClock::now();
        if (window_start == SteadyClock::time_point{}) window_start = now;
        if (now - window_start < kLogEvery) return;
        // once a minute the mirror is checked against the registry (a walk over it, not counted in the publish cost): a record that differs is a change the registry did not announce
        const size_t drift = mirror.verify(world.registry());
        if (drift != 0) {
            spdlog::warn("[stream] the mirror differed from the registry in {} record(s): rebuilt", drift);
            mirror.rebuild(world.registry());
        }
        spdlog::info("[stream] last {:.0f} s: {} publishes, {:.3f} ms on average and {:.3f} ms at most on the network thread; {} entities ({} players, {} NPCs, {} resource nodes, {} mounts, {} loot piles); "
                     "{} stream(s) open; the loop's work {:.2f} ms on average, {:.2f} ms at most; mirror check: {} difference(s)",
                     std::chrono::duration<double>(now - window_start).count(), window_n, window_n ? window_sum / static_cast<double>(window_n) : 0.0, window_max, last_recs,
                     last_counts.players, last_counts.npcs, last_counts.resource_nodes, last_counts.mounts, last_counts.loot_piles, open_streams.load(),
                     loop_n ? loop_sum / static_cast<double>(loop_n) : 0.0, loop_max, drift);
        last_window_max = window_max;
        window_sum = window_max = 0;
        window_n = 0;
        loop_sum = loop_max = 0;
        loop_n = 0;
        window_start = now;
    }

    void note_loop(std::chrono::microseconds busy) {
        const double ms = std::chrono::duration<double, std::milli>(busy).count();
        loop_ms = loop_n == 0 && loop_ms == 0 ? ms : loop_ms * 0.95 + ms * 0.05;
        loop_sum += ms;
        loop_max = std::max(loop_max, ms);
        ++loop_n;
    }

    // ---- the streams (gRPC threads) ----------------------------------------------------------------------------------------------------

    /// The newest snapshot once it is newer than `seen`; null after `timeout` or when stopping.
    std::shared_ptr<const mstream::Snapshot> wait_newer(uint64_t seen, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_for(lock, timeout, [&] { return stopping || (latest && latest->tick > seen); });
        if (stopping || !latest || latest->tick <= seen) return nullptr;
        return latest;
    }

    grpc::Status Subscribe(grpc::ServerContext* ctx, const wsv1::SubscribeRequest* req, grpc::ServerWriter<wsv1::SubscribeResponse>* writer) override {
        if (auto s = authorize(ctx, "Subscribe"); !s.ok()) return s;
        mstream::Request want;
        if (!req->has_area()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "give an area");
        want.area = {req->area().min_x_cm(), req->area().min_y_cm(), req->area().max_x_cm(), req->area().max_y_cm()};
        switch (req->detail()) {
            case wsv1::STREAM_DETAIL_UNSPECIFIED:
            case wsv1::STREAM_DETAIL_ENTITIES: want.density = false; break;
            case wsv1::STREAM_DETAIL_DENSITY: want.density = true; break;
            default: return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown detail");
        }
        want.cell_cm = req->density_cell_cm() == 0 ? 50000 : req->density_cell_cm();
        want.layers = req->layers() == 0 ? static_cast<uint32_t>(mstream::kLayerAll) : req->layers();
        if (const std::string why = mstream::validate(want); !why.empty()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, why);

        const size_t before = open_streams.fetch_add(1);
        struct Release {
            std::atomic<size_t>& n;
            ~Release() { --n; }
        } release{open_streams};
        if (before >= options.max_subscribers) {
            return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "the stream limit is reached: " + std::to_string(options.max_subscribers) + " streams are open");
        }

        spdlog::info("[stream] {} watches x {}..{}, y {}..{} cm: {}, layers {}", ctx->peer(), want.area.min_x, want.area.max_x, want.area.min_y, want.area.max_y,
                     want.density ? "a density grid of " + std::to_string(want.cell_cm) + " cm cells" : std::string("entities"), want.layers);
        mstream::Cursor cursor(want);
        mstream::Frame frame;
        wsv1::SubscribeResponse resp;
        uint64_t seen = 0, seq = 0;
        auto last_sent = SteadyClock::now();
        bool any_sent = false;
        while (!ctx->IsCancelled()) {
            std::shared_ptr<const mstream::Snapshot> snap = wait_newer(seen, std::chrono::milliseconds(500));
            if (!snap) {
                std::lock_guard<std::mutex> lock(mu);
                if (stopping) break;
                continue;
            }
            seen = snap->tick;
            cursor.next(*snap, any_sent && SteadyClock::now() - last_sent >= kHeartbeat, frame);
            if (frame.type == mstream::Frame::Type::None) continue;

            resp.Clear();
            resp.set_seq(++seq);
            resp.set_tick(snap->tick);
            resp.set_world_time(snap->world_time);
            stream_detail::put_stats(resp.mutable_stats(), *snap);
            snap.reset();   // a write may block for as long as the subscriber does not read: the buffer goes back to the pool meanwhile
            fill_body(frame, resp);
            if (!writer->Write(resp)) break;
            any_sent = true;
            last_sent = SteadyClock::now();
        }
        spdlog::info("[stream] {} left after {} frame(s)", ctx->peer(), seq);
        return grpc::Status::OK;
    }

    static void fill_body(const mstream::Frame& frame, wsv1::SubscribeResponse& resp) {
        const auto put_all = [&](const auto& add) {
            for (const mstream::Rec& r : frame.changed) {
                wsv1::Entity* e = add();
                stream_detail::put_entity(e, r);
                if (r.kind == mstream::kPlayer) {
                    for (const mstream::PlayerName& n : frame.names) {
                        if (n.id == r.id) {
                            e->set_name(n.name);
                            break;
                        }
                    }
                }
            }
        };
        switch (frame.type) {
            case mstream::Frame::Type::Snapshot: {
                wsv1::EntitySnapshot* s = resp.mutable_snapshot();
                s->mutable_entities()->Reserve(static_cast<int>(frame.changed.size()));
                put_all([&] { return s->add_entities(); });
                break;
            }
            case mstream::Frame::Type::Delta: {
                wsv1::EntityDelta* d = resp.mutable_delta();   // set even when empty: the heartbeat
                d->mutable_changed()->Reserve(static_cast<int>(frame.changed.size()));
                put_all([&] { return d->add_changed(); });
                d->mutable_removed()->Reserve(static_cast<int>(frame.removed.size()));
                for (const uint64_t id : frame.removed) d->add_removed(id);
                break;
            }
            case mstream::Frame::Type::Density: {
                wsv1::DensityGrid* g = resp.mutable_density();
                const mstream::Grid& src = frame.grid;
                g->set_cell_cm(src.cell_cm);
                g->set_origin_x_cm(src.origin_x);
                g->set_origin_y_cm(src.origin_y);
                g->set_cols(src.cols);
                g->set_rows(src.rows);
                g->mutable_players()->Reserve(static_cast<int>(src.players.size()));
                g->mutable_npcs()->Reserve(static_cast<int>(src.npcs.size()));
                g->mutable_resource_nodes()->Reserve(static_cast<int>(src.resource_nodes.size()));
                for (const uint32_t v : src.players) g->add_players(v);
                for (const uint32_t v : src.npcs) g->add_npcs(v);
                for (const uint32_t v : src.resource_nodes) g->add_resource_nodes(v);
                break;
            }
            case mstream::Frame::Type::None: break;
        }
    }

    // ---- Inspect, Search, GetStatus ----------------------------------------------------------------------------------------------------

    grpc::Status Inspect(grpc::ServerContext* ctx, const wsv1::InspectRequest* req, wsv1::InspectResponse* resp) override {
        if (auto s = authorize(ctx, "Inspect"); !s.ok()) return s;
        bool found = false;
        if (auto s = on_net_thread([&] { found = inspect(req->id(), resp); }); !s.ok()) return s;
        if (!found) return grpc::Status(grpc::StatusCode::NOT_FOUND, "no entity with that id is in the world");
        return grpc::Status::OK;
    }

    /// Network thread: fills the answer for the entity `id`; false when there is none.
    bool inspect(uint64_t id, wsv1::InspectResponse* resp) {
        namespace sd = stream_detail;
        if (const world::WorldActor* a = world.registry().find(id)) {
            uint8_t kind = 0;
            if (!mstream::kind_of(a->kind, kind)) return false;
            std::vector<mstream::Overlay> ov;
            std::vector<combat::CombatWorld::Summary> scratch;
            gather_overlay(ov, scratch);
            const mstream::Overlay* mine = nullptr;
            for (const mstream::Overlay& o : ov) {
                if (o.id == id) mine = &o;
            }
            sd::put_entity(resp->mutable_entity(), mstream::make_rec(*a, kind, kind == mstream::kNpc ? mine : nullptr));
            wsv1::ActorDetail* d = resp->mutable_actor();
            std::string scratch_name;
            d->set_name(sd::display_name(*a, scratch_name));
            d->set_archetype(a->npc.archetype);
            d->set_source(sd::source_name(a->npc.source));
            d->set_level(static_cast<uint32_t>(std::max(0, a->npc.level)));
            combat::CombatWorld* cw = world.combat();
            const npc::ai::NpcAi* ai = cw ? cw->ai_of(id) : nullptr;
            if (ai) {
                d->set_combatant(true);
                d->set_health(cw->health_of(id));
                d->set_max_health(cw->max_health_of(id));
                d->set_ai_state(sd::ai_state_name(ai->state()));
                d->set_target(ai->target());
                std::vector<npc::ai::ThreatTable::Entry> threat = ai->threat().entries();
                std::sort(threat.begin(), threat.end(), [](const auto& x, const auto& y) { return x.threat > y.threat; });
                for (const auto& t : threat) {
                    wsv1::ThreatEntry* out = d->add_threat();
                    out->set_id(t.id);
                    out->set_threat(t.threat);
                }
            } else {
                d->set_ai_state("none");
            }
            d->set_home_x_cm(mstream::to_cm(a->npc.home_x));
            d->set_home_y_cm(mstream::to_cm(a->npc.home_y));
            d->set_home_z_cm(mstream::to_cm(a->npc.home_z));
            d->set_distance_from_home_cm(std::hypot(a->x - a->npc.home_x, a->y - a->npc.home_y));
            d->set_aggro_radius_cm(a->npc.aggro_radius_cm);
            d->set_leash_radius_cm(ai ? ai->params().leash_radius_cm : a->npc.leash_radius_cm);
            d->set_respawn_seconds(a->npc.respawn_seconds);
            d->set_respawn_rule(a->npc.respawn_rule);
            for (const int64_t t : a->npc.loot_tables) d->add_loot_tables(t);
            npc::NpcWorld* nw = world.npcs();
            d->set_standing(kind == mstream::kNpc && nw && nw->has(id) && nw->stands(id));
            // its spawn point is where the NPC world made it: the same as its home until a game master moves it
            nav::Vec3 spawn_ground, spawn_ai{a->npc.home_x, a->npc.home_y, a->npc.home_z};
            if (nw) nw->spawn_point(id, &spawn_ground, &spawn_ai);
            d->set_spawn_x_cm(mstream::to_cm(spawn_ai.x));
            d->set_spawn_y_cm(mstream::to_cm(spawn_ai.y));
            d->set_spawn_z_cm(mstream::to_cm(spawn_ai.z));
            // a dead one: its corpse, what is left on it, and when its spawn point makes it again
            if (const combat::Aftermath* after = world.aftermath()) {
                const double now = world.world_clock();
                if (const combat::Corpse* c = after->corpse_of(id); c && !c->living && !c->is_pile) {
                    d->set_corpse(true);
                    d->set_corpse_loot_stacks(static_cast<uint32_t>(c->remaining()));
                    d->set_corpse_closes_in_seconds(std::max(0.0, c->close_at - now));
                }
                d->set_respawn_in_seconds(after->respawn_in(id, now));
            }
            return true;
        }
        gather_players(rows);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].id != id) continue;
            const mstream::PlayerRow& p = rows[i];
            stream_detail::put_entity(resp->mutable_entity(), mstream::make_rec(p));
            wsv1::PlayerDetail* d = resp->mutable_player();
            d->set_name(p.name);
            d->set_level(static_cast<uint32_t>(std::max(0, p.level)));
            d->set_mounted(p.mounted);
            d->set_dead(p.dead);
            for (const PlayerHandle& h : directory()) {
                if (h && h.player->bootstrap().setup().guids.pawn.ObjectId == id) {
                    d->set_character_id(h.player->character_id());
                    d->set_move_speed_multiplier(h.player->gm_speed());
                    break;
                }
            }
            return true;
        }
        return false;
    }

    grpc::Status Search(grpc::ServerContext* ctx, const wsv1::SearchRequest* req, wsv1::SearchResponse* resp) override {
        if (auto s = authorize(ctx, "Search"); !s.ok()) return s;
        std::string needle = req->query();
        if (needle.size() < 2) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "the query needs at least 2 characters");
        if (needle.size() > 64) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "the query is longer than 64 characters");
        for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const size_t limit = req->limit() == 0 ? 20 : std::min<size_t>(req->limit(), 50);
        if (auto s = on_net_thread([&] { search(needle, limit, resp); }); !s.ok()) return s;
        return grpc::Status::OK;
    }

    /// Network thread: players, then NPCs, resource nodes, mounts and piles, whose name holds `needle` (lower case).
    void search(const std::string& needle, size_t limit, wsv1::SearchResponse* resp) {
        namespace sd = stream_detail;
        struct Hit {
            std::string  name;
            mstream::Rec rec;
        };
        constexpr size_t kCollect = 500;   // the most matches kept before the sort: it bounds the work and the memory of a query like "a"
        std::vector<Hit> hits;
        bool over = false;
        gather_players(rows);
        for (const mstream::PlayerRow& p : rows) {
            if (!sd::ci_contains(p.name, needle)) continue;
            if (hits.size() >= kCollect) {
                over = true;
                break;
            }
            hits.push_back({p.name, mstream::make_rec(p)});
        }
        std::vector<mstream::Overlay> ov;
        std::vector<combat::CombatWorld::Summary> scratch;
        gather_overlay(ov, scratch);
        mstream::OverlayIndex index;
        index.build(ov);
        std::string scratch_name;
        world.registry().for_each([&](const world::WorldActor& a) {
            uint8_t kind = 0;
            if (!mstream::kind_of(a.kind, kind)) return;
            const std::string& name = sd::display_name(a, scratch_name);
            if (!sd::ci_contains(name, needle)) return;
            if (hits.size() >= kCollect) {
                over = true;
                return;
            }
            hits.push_back({name, mstream::make_rec(a, kind, kind == mstream::kNpc ? index.find(a.id) : nullptr)});
        });
        std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
            const bool xp = x.rec.kind == mstream::kPlayer, yp = y.rec.kind == mstream::kPlayer;
            if (xp != yp) return xp;
            if (sd::ci_less(x.name, y.name)) return true;
            if (sd::ci_less(y.name, x.name)) return false;
            return x.rec.id < y.rec.id;
        });
        resp->set_truncated(over || hits.size() > limit);
        for (size_t i = 0; i < hits.size() && i < limit; ++i) {
            wsv1::SearchResult* r = resp->add_results();
            sd::put_entity(r->mutable_entity(), hits[i].rec);
            r->set_name(hits[i].name);
        }
    }

    grpc::Status GetStatus(grpc::ServerContext* ctx, const wsv1::GetStatusRequest*, wsv1::GetStatusResponse* resp) override {
        if (auto s = authorize(ctx, "GetStatus"); !s.ok()) return s;
        std::shared_ptr<const mstream::Snapshot> snap;
        {
            std::lock_guard<std::mutex> lock(mu);
            snap = latest;
        }
        if (snap) {
            stream_detail::put_stats(resp->mutable_stats(), *snap);
            resp->set_world_time(snap->world_time);
            resp->set_tick(snap->tick);
        }
        resp->set_subscribers(static_cast<uint32_t>(open_streams.load()));
        return grpc::Status::OK;
    }
};

// ---- the server -----------------------------------------------------------------------------------------------------------------------

WorldStreamServer::WorldStreamServer(PlayerWorld& world, Directory directory, AdminQueue& queue, Options options)
    : impl_(std::make_unique<Impl>(world, std::move(directory), queue, std::move(options))) {}

WorldStreamServer::~WorldStreamServer() { stop(); }

bool WorldStreamServer::start(std::string* error) {
    if (server_) return true;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(impl_->options.address, grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(impl_.get());
    builder.SetMaxReceiveMessageSize(64 * 1024);
    builder.AddChannelArgument("grpc.keepalive_time_ms", 10000);     // a subscriber that vanished without closing its connection is found out and its stream freed
    builder.AddChannelArgument("grpc.keepalive_timeout_ms", 5000);
    grpc::ResourceQuota quota;
    quota.SetMaxThreads(static_cast<int>(impl_->options.max_subscribers) + 8);   // a thread per open stream, and a few for Inspect, Search and GetStatus
    builder.SetResourceQuota(quota);
    server_ = builder.BuildAndStart();
    if (!server_ || port == 0) {
        server_.reset();
        if (error) *error = "cannot listen on " + impl_->options.address + " (is another worldd running? set AOC_WORLD_STREAM_ADDR)";
        return false;
    }
    port_ = port;
    return true;
}

void WorldStreamServer::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    impl_->queue.close();
    if (server_) {
        server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        server_.reset();
    }
}

void WorldStreamServer::publish() { impl_->publish(); }

void WorldStreamServer::note_loop(std::chrono::microseconds busy) { impl_->note_loop(busy); }

size_t WorldStreamServer::subscribers() const { return impl_->open_streams.load(); }

uint64_t WorldStreamServer::refused() const { return impl_->refused_calls.load(); }

}}}  // namespace aoc::worldd::svc

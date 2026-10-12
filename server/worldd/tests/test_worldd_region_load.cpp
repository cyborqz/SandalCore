// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_region_load.cpp  (G1, stage 1, Part B)
//
//  The scale test: worldd's real world of the Riverlands (the real content.db, npc_motion.tsv and the 950 MB chunked area_nav.bin of the working tree) with its full population,
//  and synthetic sessions on it.  A synthetic session is an ActorView on a real Connection with a fake client (the part of a session that costs by the number of actors) whose
//  player is moved by the test; the world's own tick (PlayerWorld::tick_world: the ground window, the NPC world, the fights, the aftermath) runs as worldd's Endpoint runs it.
//
//    start         the time and the memory of each step of worldd's start: the population (load_area), the ground (load_ground)
//    one player    one client standing at the login spot, the densest start (the user's): the world tick and the client's bandwidth and channels in the steady state
//    crossing      six clients cross the region at a mount's 22 m/s on six seeded tours of the footprint's rectangles, for 15 simulated minutes: every world tick's time (the
//                  first pass included), each client's bandwidth and channels, the ground window's tiles and times
//
//    far start     (the investigation of 2026-10-10) a client that starts 3 km from the default spawn: the NPCs that were waiting for their chunk at the start are grounded, and reach the
//                  client's view (opened, on their grounded height)
//
//  Each budget the brief sets is a check, and the numbers are printed:
//
//    <= 1 ms per world tick at the login spot's density          <= 10 ms for any single world tick (p99.9; the worst tick may take three times that on a busy PC)
//    <= 100 KB/s and <= 700 channels per client                  every player stands in a resident chunk (the window shares its 4,096 tiles by distance), none failed
//    a window pump <= its 3 ms budget plus one tile (<= 5 ms; the check allows three times that)
//
//  The wall-clock checks are three times their design budget (review B I1: this runs on a PC that also builds and plays); the printed numbers are the measure.
//
//  Heavy: it is NOT in the default gate (scripts/run_tests.ps1 -Heavy runs it, and the plan's last task does).  Needs dist/content/{content.db,npc_motion.tsv,area_nav.bin} (version 2,
//  chunked); it skips (exit 0) without the ground.  Run alone: it loads 1 GB of ground and takes minutes.
// ============================================================================
#include "content/content_db.h"
#include "fake_client.h"
#include "nav/area_nav.h"
#include "net/connection.h"
#include "npc/npc_world.h"
#include "proc_stats.h"
#include "session/actor_view.h"
#include "session/player_world.h"
#include "spawns/region_data.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
using std::chrono::milliseconds;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const Clock::time_point t0{std::chrono::seconds(5000)};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;
// (C-O-A : C_Y_B)

using Wall = std::chrono::steady_clock;
static double since_ms(Wall::time_point a) { return std::chrono::duration<double, std::milli>(Wall::now() - a).count(); }

/// One synthetic session's connection side.
struct Rig {
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<ActorView> view;
    uint64_t player = 0;
    double x = 0, y = 0;
    size_t bytes = 0, packets = 0;
    std::vector<size_t> second_bytes;          // bytes sent in each simulated second
    size_t second_acc = 0;
    double view_ms_total = 0, view_ms_max = 0;
    size_t view_ticks = 0;
    std::vector<std::pair<double, double>> tour;   // the points it walks through
    size_t leg = 0;

    Rig(world::ActorRegistry& reg, uint64_t id, double sx, double sy) : player(id), x(sx), y(sy) {
        ActorView::Config cfg;
        cfg.player_id = id;
        view = std::make_unique<ActorView>(reg, cfg, [](uint32_t) { return false; });
    }
    void tick(Clock::time_point t) {
        const auto a = Wall::now();
        view->tick(conn, x, y, t);
        const double ms = since_ms(a);
        view_ms_total += ms;
        view_ms_max = std::max(view_ms_max, ms);
        ++view_ticks;
        for (auto& pkt : conn.flush(t)) {
            bytes += pkt.size();
            second_acc += pkt.size();
            ++packets;
            client.receive(pkt);
        }
        conn.receive(client.packet({}), t);
        for (const AckEvent& e : conn.take_ack_events()) view->on_ack(e);
    }
    void close_second() {
        second_bytes.push_back(second_acc);
        second_acc = 0;
    }
    /// Moves towards the next point of the tour by `step` cm; false when the tour is over.
    bool walk(double step) {
        while (leg < tour.size() && step > 0) {
            const double dx = tour[leg].first - x, dy = tour[leg].second - y, d = std::hypot(dx, dy);
            if (d <= step) {
                x = tour[leg].first;
                y = tour[leg].second;
                step -= d;
                ++leg;
            } else {
                x += dx / d * step;
                y += dy / d * step;
                step = 0;
            }
        }
        return leg < tour.size();
    }
};

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

static double best_window_kbps(const std::vector<size_t>& sec, size_t window) {
    double best = 0;
    for (size_t i = 0; i + window <= sec.size(); ++i) {
        size_t sum = 0;
        for (size_t k = 0; k < window; ++k) sum += sec[i + k];
        best = std::max(best, static_cast<double>(sum) / 1024.0 / static_cast<double>(window));
    }
    return best;
}

int main() {
    spdlog::set_level(spdlog::level::warn);
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string db_path = env && *env ? env : LOAD_CONTENT_DB_PATH;
    const char* env_nav = std::getenv("AOC_AREA_NAV");
    const std::string nav_path = env_nav && *env_nav ? env_nav : LOAD_NAV_PATH;
    const char* env_motion = std::getenv("AOC_NPC_MOTION");
    const std::string motion_path = env_motion && *env_motion ? env_motion : LOAD_MOTION_PATH;
    std::string error;
    {
        std::string e;
        auto probe = nav::AreaNav::load(nav_path, &e);
        if (!probe || !probe->windowed()) {
            std::printf("  [skip] %s: %s (the chunked ground: tools/mapc build, then aoc_navbuild)\n", nav_path.c_str(), probe ? "it is not a chunked file" : e.c_str());
            return 0;
        }
    }
    auto content = content::ContentDb::open(db_path, error);
    if (!content) {
        std::printf("  [FAIL] content.db %s does not open: %s\n", db_path.c_str(), error.c_str());
        return 2;
    }
// C-Y-B : C O A

    // ---- start ----
    std::printf("start\n");
    const tests::MemoryNow m0 = tests::memory_now();
    auto t_all = Wall::now();
    aoc::protocol::ServerGuidAllocator guids(kServerId, kRandomizer);
    auto t_step = Wall::now();
    PlayerWorld world(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
    const double ms_world = since_ms(t_step);
    if (!world.ready()) {
        std::printf("  [FAIL] the world is not ready: %s\n", world.content_problem().c_str());
        return 2;
    }
    spawns::SpawnContext ctx;
    ctx.hour = 12.0;
    world.set_spawn_context(ctx);
    t_step = Wall::now();
    world.load_blueprints(motion_path, error);
    const double ms_bp = since_ms(t_step);
    t_step = Wall::now();
    world.set_gm_garden(true);   // (--gm-garden: placed by load_ground, on the real ground)
    const bool area_ok = world.load_area(spawns::AreaConfig::riverlands(), error);
    const double ms_area = since_ms(t_step);
    const tests::MemoryNow m1 = tests::memory_now();
    CHECK(area_ok, "the Riverlands load (load_area)");
    if (!area_ok) {
        std::printf("    %s\n", error.c_str());
        return 2;
    }
    t_step = Wall::now();
    const bool ground_ok = world.load_ground(nav_path, motion_path, error);
    const double ms_ground = since_ms(t_step);
    const double ms_start = since_ms(t_all);
    const tests::MemoryNow m2 = tests::memory_now();
    CHECK(ground_ok, "the ground loads (load_ground: the terrain, the login spot's window, every NPC of the region placed or waiting for its chunk)");
    if (!ground_ok) {
        std::printf("    %s\n", error.c_str());
        return 2;
    }
    std::printf("    PlayerWorld %.0f ms, Blueprint table %.0f ms, load_area %.0f ms (%zu actors), load_ground %.0f ms; worldd's content-dependent start in all: %.2f s\n", ms_world, ms_bp, ms_area,
                world.registry().size(), ms_ground, ms_start / 1000.0);
    std::printf("    memory: %.0f MB before, %.0f MB after the area, %.0f MB after the ground (peak %.0f MB, private %.0f MB)\n", m0.working_set_mb, m1.working_set_mb, m2.working_set_mb, m2.peak_mb, m2.private_mb);
    const npc::GroundReport* gr = world.ground_report();
    if (gr) std::printf("    %s\n", gr->describe().c_str());
    const nav::WindowStats w0 = world.nav()->window_stats();
    std::printf("    the ground window at start: %zu tiles in %zu chunks resident\n", w0.resident_tiles, w0.resident_chunks);
    if (const spawns::GardenReport* garden = world.garden_report()) {
        bool on_ground = garden->nodes.size() == 10;
        for (const spawns::GardenNode& n : garden->nodes) {
            double z = 0;
            on_ground = on_ground && n.grounded && world.nav()->ground_height(n.x, n.y, n.z, &z) && std::fabs(z - n.z) < 1.0;
        }
        std::printf("    %s\n", garden->describe().c_str());
        CHECK(on_ground, "the GM garden: ten nodes, each on the real ground (the navmesh's or the terrain's height under it)");
    } else {
        CHECK(false, "the GM garden was placed by load_ground");
    }
    CHECK(ms_start < 60000.0, "worldd's start with the region takes under a minute (the content, the population and the ground)");

    // the world's clock: 20 Hz
    const double dt = 0.05;
    double now = 0;
    std::vector<double> world_ms;   // each world tick's time
    const auto world_tick = [&]() {
        now += dt;
        const auto a = Wall::now();
        world.tick_world(now);
        const double ms = since_ms(a);
        world_ms.push_back(ms);
        return ms;
    };
    const auto tick_time = [&]() { return t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(now)); };

    // ---- one player at the login spot ----
    std::printf("one player at the login spot (the densest start)\n");
    {
        Rig r(world.registry(), 1, spawns::AreaConfig{}.center_x, spawns::AreaConfig{}.center_y);
        world_ms.clear();
        // the opens first (16 a pass), then the steady state
        for (int i = 0; i < 20 * 60; ++i) {   // 60 s
            world.note_player(r.player, r.x, r.y);
            world_tick();
            r.tick(tick_time());
            if ((i + 1) % 20 == 0) r.close_second();
        }
        const size_t settle_ticks = world_ms.size();
        const double first_pass_max = *std::max_element(world_ms.begin(), world_ms.end());
        world_ms.clear();
        r.second_bytes.clear();
        r.second_acc = 0;
        const size_t bytes_before = r.bytes;
        for (int i = 0; i < 20 * 120; ++i) {   // 120 s of steady state
            world.note_player(r.player, r.x, r.y);
            world_tick();
            r.tick(tick_time());
            if ((i + 1) % 20 == 0) r.close_second();
        }
        double sum = 0;
        for (const double v : world_ms) sum += v;
        const double avg = sum / static_cast<double>(world_ms.size());
        const double mx = *std::max_element(world_ms.begin(), world_ms.end());
        const double kbps = static_cast<double>(r.bytes - bytes_before) / 1024.0 / 120.0;
        const ActorView::Stats& vs = r.view->stats();
        std::printf("    the first minute (%zu ticks): the worst world tick %.2f ms\n", settle_ticks, first_pass_max);
        std::printf("    steady state, 120 s: world tick %.3f ms on average, p99 %.3f ms, %.2f ms at most; the client's view pass %.3f ms on average, %.2f ms at most\n", avg,
                    percentile(world_ms, 0.99), mx, r.view_ms_total / static_cast<double>(std::max<size_t>(1, r.view_ticks)), r.view_ms_max);
        std::printf("    the client: %.1f KB/s over the steady state, %.1f KB/s in the busiest 10 s of the first minute, %zu channels in use (peak %zu); %zu awake, %zu dormant; %zu opens, %zu snapshots\n", kbps,
                    best_window_kbps(r.second_bytes, 10), r.view->channels_in_use(), vs.peak_channels, r.view->count(world::ClientState::Awake), r.view->count(world::ClientState::Dormant), vs.opens,
                    vs.snapshots);
        CHECK(avg <= 3.0, "<= 1 ms per world tick on average at the login spot's density (checked at 3 ms: a shared PC)");
        CHECK(percentile(world_ms, 0.999) <= 10.0 && std::max(first_pass_max, mx) <= 30.0, "<= 10 ms for a world tick (p99.9 of the steady state), the first pass included at three times that");
        CHECK(kbps <= 100.0, "<= 100 KB/s to the client");
        CHECK(vs.peak_channels <= 700, "<= 700 channels in use by the client");
        CHECK(vs.broken == 0, "no actor was broken");
    }
// [C_Y_B ~ C-O-A]

    // ---- a client that starts far from the default spawn ----
    std::printf("a client that starts 3 km from the default spawn\n");
    {
        const double sx = spawns::AreaConfig{}.center_x, sy = spawns::AreaConfig{}.center_y;
        // the place: an NPC that waits for its chunk at about 3 km from the default spawn (one with company: the nearest to 3 km of those that have another waiting NPC within 50 m)
        double best = 1e300, fx = sx, fy = sy;
        world.registry().for_each([&](const world::WorldActor& a) {
            if (a.kind != world::ActorKind::Npc || !world.npcs()->has(a.id) || world.nav()->ready_at(a.x, a.y)) return;
            const double d = std::fabs(std::hypot(a.x - sx, a.y - sy) - 300000.0);
            if (d >= best) return;
            std::vector<uint64_t> company;
            world.registry().query(a.x, a.y, 5000.0, company);
            if (company.size() < 4) return;
            best = d;
            fx = a.x;
            fy = a.y;
        });
        Rig r(world.registry(), 50, fx, fy);
        std::vector<uint64_t> near_ids;
        world.registry().query(fx, fy, 10000.0, near_ids);   // within 100 m: inside the awake ring
        std::vector<uint64_t> npcs_here;
        size_t waiting_at_start = 0;
        for (const uint64_t id : near_ids) {
            const world::WorldActor* a = world.registry().find(id);
            if (!a || a->kind != world::ActorKind::Npc || !world.npcs()->has(id)) continue;
            npcs_here.push_back(id);
            if (!world.nav()->ready_at(a->x, a->y)) ++waiting_at_start;
        }
        std::printf("    the client starts at (%.0f, %.0f), %.2f km from the default spawn; %zu NPCs within 100 m of it, %zu of them waiting for their chunk (the ground there is not loaded yet)\n", fx, fy,
                    std::hypot(fx - sx, fy - sy) / 100000.0, npcs_here.size(), waiting_at_start);
        for (int i = 0; i < 20 * 45; ++i) {   // 45 s
            world.note_player(r.player, r.x, r.y);
            world_tick();
            r.tick(tick_time());
        }
        size_t still_waiting = 0, opened = 0, on_ground = 0;
        for (const uint64_t id : npcs_here) {
            const world::WorldActor* a = world.registry().find(id);
            if (!a) continue;   // (killed or removed meanwhile)
            if (!world.nav()->ready_at(a->x, a->y)) ++still_waiting;
            if (r.view->channel_of(id) != 0) ++opened;
            const npc::Mover* m = world.npcs()->mover(id);
            if (m && std::fabs(m->origin_z() - a->z) < 1.0) ++on_ground;
        }
        std::printf("    after 45 s: %zu of them still waiting for their chunk, %zu opened to the client (it holds %zu awake, %zu dormant actors), %zu standing on the height the NPC world grounded them at\n", still_waiting,
                    opened, r.view->count(world::ClientState::Awake), r.view->count(world::ClientState::Dormant), on_ground);
        CHECK(!npcs_here.empty() && waiting_at_start * 2 >= npcs_here.size(), "(the start is far from the login spot: most NPCs near the client were waiting for their chunk)");
        CHECK(still_waiting == 0, "the chunks around the client came, and the NPCs that were waiting for them are grounded");
        CHECK(opened * 10 >= npcs_here.size() * 9, "the NPCs that were waiting at the start reach the client (at least nine in ten of those within 100 m are opened)");
        CHECK(on_ground == npcs_here.size() || on_ground * 100 >= npcs_here.size() * 99, "and each stands, in the registry, at the height the NPC world grounded it at (a client never keeps the terrain's height of a waiting NPC)");
        CHECK(r.view->stats().broken == 0, "no actor was broken");
        world.forget_player(50);
    }
// built by C.O.A with Cyb

    // ---- six players crossing ----
    std::printf("six players cross the Riverlands\n");
    {
        constexpr int kClients = 6;
        constexpr double kMount = 2200.0;       // cm/s
        constexpr int kSeconds = 900;           // simulated
        std::vector<std::pair<double, double>> centres;
        for (size_t i = 0; i < spawns::region::kRiverlandsRectCount; ++i) {
            const auto& r = spawns::region::kRiverlandsRects[i];
            centres.emplace_back((r.x0 + r.x1) * 0.5, (r.y0 + r.y1) * 0.5);
        }
        std::vector<std::unique_ptr<Rig>> rigs;
        for (int k = 0; k < kClients; ++k) {
            std::mt19937 rng(1234 + k);
            std::vector<std::pair<double, double>> tour;
            for (int p = 0; p < 40; ++p) tour.push_back(centres[rng() % centres.size()]);
            auto rig = std::make_unique<Rig>(world.registry(), 100 + k, tour[0].first, tour[0].second);
            rig->tour = tour;
            rigs.push_back(std::move(rig));
        }
        world_ms.clear();
        const nav::WindowStats before = world.nav()->window_stats();
        const auto t_run = Wall::now();
        size_t over_5 = 0, over_10 = 0;
        size_t peak_tiles = 0, samples = 0, not_ready = 0;
        for (int i = 0; i < kSeconds * 20; ++i) {
            for (auto& r : rigs) {
                r->walk(kMount * dt);
                world.note_player(r->player, r->x, r->y);
            }
            const double ms = world_tick();
            over_5 += ms > 5.0;
            over_10 += ms > 10.0;
            for (auto& r : rigs) {
                r->tick(tick_time());
                if ((i + 1) % 20 == 0) {
                    r->close_second();
                    if (i > 20 * 30) {   // (after the first half minute: the windows have formed)
                        ++samples;
                        if (!world.nav()->ready_at(r->x, r->y)) ++not_ready;
                    }
                }
            }
            peak_tiles = std::max(peak_tiles, world.nav()->tile_count());
        }
        const double wall_s = since_ms(t_run) / 1000.0;
        double sum = 0;
        for (const double v : world_ms) sum += v;
        const double mx = *std::max_element(world_ms.begin(), world_ms.end());
        const nav::WindowStats w = world.nav()->window_stats();
        std::printf("    %d simulated seconds in %.0f s of wall time; %zu world ticks: %.3f ms on average, p50 %.3f, p99 %.3f, p99.9 %.3f, %.2f ms at most; %zu ticks over 5 ms, %zu over 10 ms\n", kSeconds, wall_s, world_ms.size(),
                    sum / static_cast<double>(world_ms.size()), percentile(world_ms, 0.5), percentile(world_ms, 0.99), percentile(world_ms, 0.999), mx, over_5, over_10);
        double worst_kbps = 0, worst_window = 0, total_kbps = 0;
        size_t worst_channels = 0, broken = 0;
        for (auto& r : rigs) {
            const double kbps = static_cast<double>(r->bytes) / 1024.0 / kSeconds;
            worst_kbps = std::max(worst_kbps, kbps);
            total_kbps += kbps;
            worst_window = std::max(worst_window, best_window_kbps(r->second_bytes, 10));
            worst_channels = std::max(worst_channels, r->view->stats().peak_channels);
            broken += r->view->stats().broken;
        }
        std::printf("    the clients: %.1f KB/s on average (worst %.1f KB/s, busiest 10 s %.1f KB/s), peak %zu channels; the view pass %.3f ms on average, %.2f ms at most\n", total_kbps / kClients, worst_kbps,
                    worst_window, worst_channels, rigs[0]->view_ms_total / static_cast<double>(std::max<size_t>(1, rigs[0]->view_ticks)), rigs[0]->view_ms_max);
        std::printf("    the ground window: peak %zu tiles resident (%zu counting the ones on the way), %llu chunk loads and %llu unloads in the crossing, %llu failures; the longest pump of all (start-up's blocking preload included) %.2f ms, a chunk read %.1f ms at most on the worker, a chunk's tiles added in %.2f ms at most (summed over its pumps), removed in %.2f ms\n",
                    std::max(peak_tiles, w.peak_resident_tiles), w.peak_committed_tiles, static_cast<unsigned long long>(w.chunk_loads - before.chunk_loads),
                    static_cast<unsigned long long>(w.chunk_unloads - before.chunk_unloads), static_cast<unsigned long long>(w.load_failures), w.max_pump_ms, w.max_chunk_read_ms, w.max_chunk_add_ms,
                    w.max_chunk_remove_ms);
        const PlayerWorld::WindowCost& wc = world.window_cost();
        std::printf("    the world tick's share of the window: the interest set %.2f ms at most, the pump %.2f ms at most (one tile: add %.2f ms, remove %.2f ms at most), standing the NPCs of %zu arrived chunks on their ground %.2f ms at most\n",
                    wc.max_interest_ms, wc.max_pump_ms, w.max_tile_add_ms, w.max_tile_remove_ms, wc.grounded_chunks, wc.max_ground_ms);
        std::printf("    the rest of the world tick, the longest of each: the NPC world's step %.2f ms, the fights %.2f ms, the corpses and respawns %.2f ms\n", wc.max_npcs_ms, wc.max_combat_ms, wc.max_aftermath_ms);
        if (world.npcs()) {
            const auto ns = world.npcs()->stats();
            std::printf("    the NPC world: %llu steps, the longest %.2f ms (who is awake %.2f ms of it); the most awake in a step %zu, the most paths asked in a step %zu; %llu legs started\n", static_cast<unsigned long long>(ns.steps),
                        ns.max_step_ms, ns.max_refresh_ms, ns.max_active, ns.max_pathfinds_in_step, static_cast<unsigned long long>(ns.moves_started));
            // (review A I5) the searches that found no path, by kind: a patrol leg that is given up pauses its NPC for 5 s a failure (60 s at most); a wander leg is another try
            std::printf("    searches with no path: wander %llu of %llu asks (%llu with no point to walk to, %llu deferred by the step's budget), patrol %llu of %llu asks (%llu deferred)\n",
                        static_cast<unsigned long long>(ns.wander_no_path), static_cast<unsigned long long>(ns.wander_asks), static_cast<unsigned long long>(ns.wander_no_point), static_cast<unsigned long long>(ns.wander_deferred),
                        static_cast<unsigned long long>(ns.patrol_no_path), static_cast<unsigned long long>(ns.patrol_asks), static_cast<unsigned long long>(ns.patrol_deferred));
            // ... and why: the start or the end off the loaded navmesh, the search out of expansions (the cap), no way (a lake, a cliff, a border that is not loaded), a path longer than asked
            std::printf("    why: wander: start off the mesh %llu, end off it %llu, out of expansions %llu, no way %llu, too long %llu; patrol: start off the mesh %llu, end off it %llu, out of expansions %llu, no way %llu, too long %llu\n",
                        static_cast<unsigned long long>(ns.wander_fail[1]), static_cast<unsigned long long>(ns.wander_fail[2]), static_cast<unsigned long long>(ns.wander_fail[3]), static_cast<unsigned long long>(ns.wander_fail[4]),
                        static_cast<unsigned long long>(ns.wander_fail[5]), static_cast<unsigned long long>(ns.patrol_fail[1]), static_cast<unsigned long long>(ns.patrol_fail[2]), static_cast<unsigned long long>(ns.patrol_fail[3]),
                        static_cast<unsigned long long>(ns.patrol_fail[4]), static_cast<unsigned long long>(ns.patrol_fail[5]));
        }
        if (const npc::GroundReport* g = world.ground_report()) std::printf("    NPCs still waiting for their chunk at the end: %zu of %zu\n", world.npcs() ? world.npcs()->pending_ground() : 0, g->npcs);
        const tests::MemoryNow end = tests::memory_now();
        std::printf("    memory at the end: %.0f MB (peak %.0f MB)\n", end.working_set_mb, end.peak_mb);
        CHECK(percentile(world_ms, 0.999) <= 10.0 && mx <= 30.0, "<= 10 ms for a world tick while six players cross the region (p99.9; the worst tick at three times that)");
        CHECK(sum / static_cast<double>(world_ms.size()) <= 3.0, "<= 1 ms per world tick on average over the whole crossing (checked at 3 ms: a shared PC)");
        CHECK(worst_kbps <= 100.0, "<= 100 KB/s to every client on average over the crossing");
        CHECK(worst_channels <= 700, "<= 700 channels in use by every client");
        std::printf("    coverage: %zu of %zu player-seconds the player stood in a chunk that was not resident (%llu chunk plans, %llu skipped; %llu chunks deferred for room, %llu dropped for a nearer one)\n", not_ready, samples,
                    static_cast<unsigned long long>(w.plans), static_cast<unsigned long long>(w.plans_skipped), static_cast<unsigned long long>(w.deferred_by_cap), static_cast<unsigned long long>(w.evicted_for_room));
        CHECK(std::max(peak_tiles, w.peak_resident_tiles) <= 4096 && w.peak_committed_tiles <= 4096 && w.load_failures == 0, "no chunk failed and Detour never refused a tile (the cap itself holds by construction)");
        CHECK(samples > 1000 && not_ready * 100 <= samples * 2, "each of the six players stood in a resident chunk in 98 % of the seconds (six windows of 25 chunks do not fit in 4,096 tiles: the window shares the room by distance)");
        CHECK(wc.max_pump_ms <= 15.0 && wc.max_ground_ms <= 15.0, "the ground window costs the world tick <= 5 ms at a time (checked at 15): a pump (one tile at most is added or removed past its 3 ms budget) and the standing of a chunk's NPCs");
        CHECK(broken == 0, "no actor was broken");
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

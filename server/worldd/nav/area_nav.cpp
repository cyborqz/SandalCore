// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "nav/area_nav.h"

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace aoc::worldd::nav {

namespace {

static_assert(sizeof(dtPolyRef) == 4, "dtPolyRef is 32 bits (DT_POLYREF64 is not set)");

constexpr int kMaxPolys = 4096;       // polygons of a path
constexpr int kMaxStraight = 1024;    // points of a straight path
constexpr int kSearchNodes = 16384;
constexpr double kTerrainCellCm = 25600.0;

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

uint64_t cell_key(int64_t cx, int64_t cy) { return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy); }

/// Detour trusts a tile's header: a tile cut short or with a wrong count would be read past its end.  The layout is dtCreateNavMeshData's.
bool tile_is_sound(const NavTileBlob& t, int max_polys, std::string* why) {
    auto al4 = [](long long x) { return (x + 3) & ~3LL; };
    auto bad = [&](const char* m) { *why = m; return false; };
    if (t.data.size() < sizeof(dtMeshHeader)) return bad("shorter than a tile header");
    dtMeshHeader h;
    std::memcpy(&h, t.data.data(), sizeof h);
    if (h.magic != DT_NAVMESH_MAGIC || h.version != DT_NAVMESH_VERSION) return bad("not a Detour tile of this version");
    if (h.x != t.tx || h.y != t.ty) return bad("its header names another tile");
    if (h.vertCount < 0 || h.polyCount < 0 || h.maxLinkCount < 0 || h.detailMeshCount < 0 || h.detailVertCount < 0 || h.detailTriCount < 0 || h.bvNodeCount < 0 ||
        h.offMeshConCount < 0 || h.polyCount > 0xffff || h.vertCount > (1 << 24) || h.maxLinkCount > (1 << 26) || h.detailVertCount > (1 << 24) || h.detailTriCount > (1 << 24))
        return bad("negative or too large counts");
    const long long expect = static_cast<long long>(al4(sizeof(dtMeshHeader))) + al4(sizeof(float) * 3 * static_cast<long long>(h.vertCount)) +
                             al4(sizeof(dtPoly) * static_cast<long long>(h.polyCount)) + al4(sizeof(dtLink) * static_cast<long long>(h.maxLinkCount)) +
                             al4(sizeof(dtPolyDetail) * static_cast<long long>(h.detailMeshCount)) +
                             al4(sizeof(float) * 3 * static_cast<long long>(h.detailVertCount)) + al4(4 * static_cast<long long>(h.detailTriCount)) +
                             al4(sizeof(dtBVNode) * static_cast<long long>(h.bvNodeCount)) + al4(sizeof(dtOffMeshConnection) * static_cast<long long>(h.offMeshConCount));
    if (expect != static_cast<long long>(t.data.size())) return bad("its size does not match its header's counts");
    // (review A M6) Detour keeps a tile's polygons in a table of the mesh's maxPolys (the id's polygon bits): a tile with more would have its references alias
    if (max_polys > 0 && h.polyCount > max_polys) return bad("it has more polygons than the mesh allows");
    return true;
}

}  // namespace

AreaNav::~AreaNav() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
    // tiles read and never added
    for (auto& lc : done_)
        for (auto& t : lc.tiles) if (t.data) dtFree(t.data);
    for (auto& s : slots_)
        if (s.state == State::Arrived)
            for (size_t i = s.cursor; i < s.tiles.size(); ++i) if (s.tiles[i].data) dtFree(s.tiles[i].data);
    if (query_) dtFreeNavMeshQuery(query_);
    if (mesh_) dtFreeNavMesh(mesh_);
    delete filter_;
}
// CoA with C:Y:B

std::unique_ptr<AreaNav> AreaNav::load(const std::string& path, std::string* err) {
    const int version = nav_file_version(path);
    if (version == 2) {
        NavFile f;
        if (!read_nav_index(path, &f, err)) return nullptr;
        std::unique_ptr<AreaNav> n(new AreaNav());
        n->file_ = std::move(f);
        n->path_ = path;
        const NavSettings& s = n->file_.settings;
        n->mesh_ = dtAllocNavMesh();
        if (!n->mesh_) { if (err) *err = "out of memory"; return nullptr; }
        dtNavMeshParams p;
        std::memset(&p, 0, sizeof p);
        p.orig[0] = s.origin[0]; p.orig[1] = s.origin[1]; p.orig[2] = s.origin[2];
        p.tileWidth = s.tile_size;
        p.tileHeight = s.tile_size;
        p.maxTiles = s.max_tiles;
        p.maxPolys = s.max_polys;
        if (dtStatusFailed(n->mesh_->init(&p))) { if (err) *err = "Detour refused the navmesh parameters"; return nullptr; }
        n->query_ = dtAllocNavMeshQuery();
        if (!n->query_ || dtStatusFailed(n->query_->init(n->mesh_, kSearchNodes))) { if (err) *err = "Detour query init failed"; return nullptr; }
        n->filter_ = new dtQueryFilter();
        n->filter_->setIncludeFlags(1);
        n->filter_->setExcludeFlags(0);
        n->build_terrain_index();
        n->windowed_ = true;
        n->slots_.resize(n->file_.chunks.size());
        for (size_t i = 0; i < n->file_.chunks.size(); ++i) {
            n->slots_[i].entry = n->file_.chunks[i];
            n->slot_by_key_[pack({n->file_.chunks[i].cx, n->file_.chunks[i].cy})] = static_cast<uint32_t>(i);
        }
        n->worker_ = std::thread([raw = n.get()] { raw->worker_main(); });
        return n;
    }
    NavFile f;
    if (!read_nav_file(path, &f, err)) return nullptr;
    return from_file(std::move(f), err);
}

std::unique_ptr<AreaNav> AreaNav::from_file(NavFile file, std::string* err) {
    std::unique_ptr<AreaNav> n(new AreaNav());
    n->file_ = std::move(file);
    const NavSettings& s = n->file_.settings;
    n->mesh_ = dtAllocNavMesh();
    if (!n->mesh_) { if (err) *err = "out of memory"; return nullptr; }
    dtNavMeshParams p;
    std::memset(&p, 0, sizeof p);
    p.orig[0] = s.origin[0]; p.orig[1] = s.origin[1]; p.orig[2] = s.origin[2];
    p.tileWidth = s.tile_size;
    p.tileHeight = s.tile_size;
    p.maxTiles = s.max_tiles;
    p.maxPolys = s.max_polys;
    if (dtStatusFailed(n->mesh_->init(&p))) { if (err) *err = "Detour refused the navmesh parameters"; return nullptr; }
    for (const auto& t : n->file_.tiles) {
        std::string why;
        if (!tile_is_sound(t, s.max_polys, &why)) { if (err) *err = "tile (" + std::to_string(t.tx) + ", " + std::to_string(t.ty) + ") is damaged: " + why; return nullptr; }
        // the mesh owns its tile data (DT_TILE_FREE_DATA): a copy in Detour's allocator
        auto* data = static_cast<unsigned char*>(dtAlloc(t.data.size(), DT_ALLOC_PERM));
        if (!data) { if (err) *err = "out of memory"; return nullptr; }
        std::memcpy(data, t.data.data(), t.data.size());
        dtTileRef ref = 0;
        if (dtStatusFailed(n->mesh_->addTile(data, static_cast<int>(t.data.size()), DT_TILE_FREE_DATA, 0, &ref))) {
            dtFree(data);
            if (err) *err = "Detour refused a tile (" + std::to_string(t.tx) + ", " + std::to_string(t.ty) + ")";
            return nullptr;
        }
    }
    n->tile_count_ = n->file_.tiles.size();
    for (auto& t : n->file_.tiles) {
        const dtMeshTile* mt = n->mesh_->getTileAt(t.tx, t.ty, 0);
        if (mt && mt->header) n->polygons_ += static_cast<size_t>(mt->header->polyCount);
        std::vector<uint8_t>().swap(t.data);   // Detour holds the tile now: one copy in memory, not two
    }
    n->query_ = dtAllocNavMeshQuery();
    if (!n->query_ || dtStatusFailed(n->query_->init(n->mesh_, kSearchNodes))) { if (err) *err = "Detour query init failed"; return nullptr; }
    n->filter_ = new dtQueryFilter();
    n->filter_->setIncludeFlags(1);
    n->filter_->setExcludeFlags(0);
    n->build_terrain_index();
    return n;
}
// C.Y.B : CoA

void AreaNav::build_terrain_index() {
    terrain_cells_.clear();
    for (uint32_t i = 0; i < file_.terrain.size(); ++i) {
        const TerrainGrid& g = file_.terrain[i];
        const int64_t a0 = static_cast<int64_t>(std::floor(g.x0 / kTerrainCellCm)), a1 = static_cast<int64_t>(std::floor(g.x1() / kTerrainCellCm));
        const int64_t b0 = static_cast<int64_t>(std::floor(g.y0 / kTerrainCellCm)), b1 = static_cast<int64_t>(std::floor(g.y1() / kTerrainCellCm));
        for (int64_t by = b0; by <= b1; ++by)
            for (int64_t bx = a0; bx <= a1; ++bx) terrain_cells_[cell_key(bx, by)].push_back(i);
    }
}

void AreaNav::to_local(const Vec3& p, float* out) const {
    out[0] = static_cast<float>(p.x - file_.center_x);
    out[1] = static_cast<float>(p.z);
    out[2] = static_cast<float>(p.y - file_.center_y);
}

Vec3 AreaNav::to_world(const float* p) const {
    return {p[0] + file_.center_x, p[2] + file_.center_y, static_cast<double>(p[1])};
}

bool AreaNav::terrain_height(double x, double y, double* z) const {
    const auto it = terrain_cells_.find(cell_key(static_cast<int64_t>(std::floor(x / kTerrainCellCm)), static_cast<int64_t>(std::floor(y / kTerrainCellCm))));
    if (it == terrain_cells_.end()) return false;
    for (const uint32_t i : it->second)
        if (file_.terrain[i].height(x, y, z)) return true;
    return false;
}

bool AreaNav::find_poly(const Vec3& p, double half_h, double half_v, uint32_t* ref, float* nearest) const {
    float c[3];
    to_local(p, c);
    const float ext[3] = {static_cast<float>(half_h), static_cast<float>(half_v), static_cast<float>(half_h)};
    dtPolyRef r = 0;
    float np[3] = {c[0], c[1], c[2]};
    if (dtStatusFailed(query_->findNearestPoly(c, ext, filter_, &r, np)) || r == 0) return false;
    *ref = r;
    if (nearest) { nearest[0] = np[0]; nearest[1] = np[1]; nearest[2] = np[2]; }
    return true;
}

bool AreaNav::ground_height(double x, double y, double z_hint, double* z, double below, double above) const {
    float c[3];
    to_local({x, y, z_hint + (above - below) * 0.5}, c);
    const float ext[3] = {3.0f, static_cast<float>((above + below) * 0.5), 3.0f};
    dtPolyRef polys[32];
    int n = 0;
    double best = 0;
    bool have = false;
    if (dtStatusSucceed(query_->queryPolygons(c, ext, filter_, polys, &n, 32))) {
        for (int i = 0; i < n; ++i) {
            float pos[3] = {static_cast<float>(x - file_.center_x), c[1], static_cast<float>(y - file_.center_y)};
            float h = 0;
            if (dtStatusFailed(query_->getPolyHeight(polys[i], pos, &h))) {
                // on a polygon's edge (a path's corner, a tile's border) the point may be a rounding error outside it: the closest point of the polygon within
                // 3 cm sideways counts
                float closest[3];
                bool over = false;
                if (dtStatusFailed(query_->closestPointOnPoly(polys[i], pos, closest, &over)) || std::fabs(closest[0] - pos[0]) > 3.0f || std::fabs(closest[2] - pos[2]) > 3.0f) continue;
                h = closest[1];
            }
            if (h < z_hint - below || h > z_hint + above) continue;
            if (!have || std::fabs(h - z_hint) < std::fabs(best - z_hint)) { best = h; have = true; }
        }
    }
    double tz = 0;
    const bool has_terrain = terrain_height(x, y, &tz);
    if (has_terrain && (!have || std::fabs(best - tz) <= 40.0)) {
        // the terrain itself is the surface (or the only one): its own heights are exact
        if (tz >= z_hint - below && tz <= z_hint + above) { *z = tz; return true; }
        if (!have) return false;
    }
    if (!have) return false;
    *z = best;
    return true;
}

bool AreaNav::nearest_point(const Vec3& p, double half_h, double half_v, Vec3* out) const {
    uint32_t ref = 0;
    float np[3];
    if (!find_poly(p, half_h, half_v, &ref, np)) return false;
    *out = to_world(np);
    return true;
}

bool AreaNav::on_navmesh(const Vec3& p, double tol_h, double tol_v) const {
    uint32_t ref = 0;
    float np[3];
    if (!find_poly(p, tol_h, tol_v, &ref, np)) return false;
    const Vec3 w = to_world(np);
    return std::hypot(w.x - p.x, w.y - p.y) <= tol_h && std::fabs(w.z - p.z) <= tol_v;
}
// (CoA + C | Y | B)

PathResult AreaNav::find_path(const Vec3& from, const Vec3& to, double snap_h, double snap_v, int max_iterations) const {
    PathResult res;
    uint32_t sref = 0, eref = 0;
    float sp[3], ep[3];
    if (!find_poly(from, snap_h, snap_v, &sref, sp)) { res.fail = PathResult::Fail::NoStartPoly; return res; }
    if (!find_poly(to, snap_h, snap_v, &eref, ep)) { res.fail = PathResult::Fail::NoEndPoly; return res; }
    dtPolyRef polys[kMaxPolys];
    int npolys = 0;
    float sl[3], el[3];
    to_local(from, sl);
    to_local(to, el);
    // the path starts and ends on the surface, at the snapped points
    if (max_iterations > 0) {
        if (dtStatusFailed(query_->initSlicedFindPath(sref, eref, sp, ep, filter_, 0))) { res.fail = PathResult::Fail::Unreachable; return res; }
        int done = 0;
        const dtStatus us = query_->updateSlicedFindPath(max_iterations, &done);
        res.iterations = done;
        if (dtStatusFailed(us) || dtStatusInProgress(us)) {
            query_->finalizeSlicedFindPath(polys, &npolys, 1);   // (ends the slice; the search ran out of its iterations or failed: no path)
            res.fail = dtStatusInProgress(us) ? PathResult::Fail::OutOfIterations : PathResult::Fail::Unreachable;
            return res;
        }
        if (dtStatusFailed(query_->finalizeSlicedFindPath(polys, &npolys, kMaxPolys)) || npolys == 0) { res.fail = PathResult::Fail::Unreachable; return res; }
    } else if (dtStatusFailed(query_->findPath(sref, eref, sp, ep, filter_, polys, &npolys, kMaxPolys)) || npolys == 0) {
        res.fail = PathResult::Fail::Unreachable;
        return res;
    }
    float endp[3] = {ep[0], ep[1], ep[2]};
    res.complete = polys[npolys - 1] == eref;
    if (!res.complete) {
        res.fail = PathResult::Fail::Unreachable;
        // as close as the last polygon gets
        bool over = false;
        query_->closestPointOnPoly(polys[npolys - 1], ep, endp, &over);
    }
    float straight[kMaxStraight * 3];
    unsigned char flags[kMaxStraight];
    dtPolyRef srefs[kMaxStraight];
    int nstraight = 0;
    if (dtStatusFailed(query_->findStraightPath(sp, endp, polys, npolys, straight, flags, srefs, &nstraight, kMaxStraight)) || nstraight == 0) return res;
    res.found = true;
    res.points.reserve(static_cast<size_t>(nstraight));
    for (int i = 0; i < nstraight; ++i) res.points.push_back(to_world(&straight[i * 3]));
    for (size_t i = 1; i < res.points.size(); ++i)
        res.length += std::hypot(res.points[i].x - res.points[i - 1].x, res.points[i].y - res.points[i - 1].y);
    return res;
}

bool AreaNav::raycast(const Vec3& from, const Vec3& to, Vec3* hit) const {
    uint32_t sref = 0;
    float sp[3];
    if (!find_poly(from, 60, 200, &sref, sp)) { if (hit) *hit = from; return false; }
    float ep[3];
    to_local(to, ep);
    // the end point is judged on the plane of the start: the ray is a 2D one over the polygons
    ep[1] = sp[1];
    dtRaycastHit rh;
    std::memset(&rh, 0, sizeof rh);
    if (dtStatusFailed(query_->raycast(sref, sp, ep, filter_, 0, &rh, 0))) { if (hit) *hit = from; return false; }
    if (rh.t >= 1.0f) return true;   // (FLT_MAX for no hit)
    if (hit) {
        const double t = std::clamp(static_cast<double>(rh.t), 0.0, 1.0);
        hit->x = from.x + (to.x - from.x) * t;
        hit->y = from.y + (to.y - from.y) * t;
        hit->z = from.z + (to.z - from.z) * t;
    }
    return false;
}

bool AreaNav::random_point(const Vec3& center, double radius, double u1, double u2, Vec3* out) const {
    if (!(radius >= 0) || !std::isfinite(u1) || !std::isfinite(u2)) return false;
    const double a = 6.283185307179586 * std::clamp(u1, 0.0, 0.999999), r = radius * std::sqrt(std::clamp(u2, 0.0, 0.999999));
    const Vec3 p{center.x + std::cos(a) * r, center.y + std::sin(a) * r, center.z};
    Vec3 np;
    if (!nearest_point(p, 150, 300, &np)) return false;
    if (std::hypot(np.x - center.x, np.y - center.y) > radius) return false;
    *out = np;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
//  the region and the window
// ---------------------------------------------------------------------------------------------------------------------------------

bool AreaNav::in_region(double x, double y) const {
    if (!file_.region.empty()) {
        for (const RegionRect& r : file_.region)
            if (r.contains(x, y)) return true;
        return false;
    }
    return std::hypot(x - file_.center_x, y - file_.center_y) <= file_.radius;
}

ChunkKey AreaNav::chunk_at(double x, double y) const {
    const double ts = file_.settings.tile_size;
    const int32_t ct = file_.chunk_tiles ? static_cast<int32_t>(file_.chunk_tiles) : kChunkTiles;
    const double fx = (x - file_.center_x - file_.settings.origin[0]) / ts, fy = (y - file_.center_y - file_.settings.origin[2]) / ts;
    // clamp far outside the grid (a hostile number) before the cast
    const auto tile = [](double f) { return static_cast<int32_t>(std::floor(std::clamp(f, -1.0e6, 1.0e6))); };
    return chunk_of_tile(tile(fx), tile(fy), ct);
}

bool AreaNav::chunk_resident(ChunkKey k) const {
    if (!windowed_) return true;
    const auto it = slot_by_key_.find(pack(k));
    if (it == slot_by_key_.end()) return true;   // no tiles there: nothing to wait for
    return slots_[it->second].state == State::Resident;
}

bool AreaNav::loading() const {
    for (const Slot& s : slots_)
        if (s.state == State::Requested || s.state == State::Arrived) return true;
    return false;
}

WindowStats AreaNav::window_stats() const {
    WindowStats s = stats_;
    s.resident_tiles = resident_tiles_;
    s.resident_chunks = resident_chunks_;
    s.committed_tiles = committed_tiles_;
    {
        std::lock_guard<std::mutex> lk(mu_);
        s.max_chunk_read_ms = std::max(s.max_chunk_read_ms, worker_max_read_ms_);
    }
    return s;
}
// -- C:Y:B . C-O-A --

std::vector<ChunkKey> AreaNav::centers_of(const std::vector<std::pair<double, double>>& points) const {
    std::vector<ChunkKey> centers;
    centers.reserve(points.size());
    for (const auto& p : points) {
        if (!std::isfinite(p.first) || !std::isfinite(p.second)) continue;
        centers.push_back(chunk_at(p.first, p.second));
    }
    std::sort(centers.begin(), centers.end());
    centers.erase(std::unique(centers.begin(), centers.end()), centers.end());
    return centers;
}

void AreaNav::set_interest(const std::vector<std::pair<double, double>>& points) {
    if (!windowed_) return;
    const std::vector<ChunkKey> centers = centers_of(points);
    if (centers.empty()) return;   // no player (or none with a number): the ground stays as it is
    if (!replan_ && centers == last_centers_) {   // the same chunks as the last plan and nothing waits for room: the plan would come out the same
        ++stats_.plans_skipped;
        return;
    }
    plan_window(centers, kLoadRadius, kKeepRadius);
}

std::vector<std::string> AreaNav::take_load_errors() {
    std::vector<std::string> out;
    out.swap(load_errors_);
    return out;
}

void AreaNav::plan_window(const std::vector<ChunkKey>& centers, int load_radius, int keep_radius) {
    if (centers.empty()) return;
    ++stats_.plans;
    last_centers_ = centers;
    replan_ = false;
    const auto now = Clock::now();
    for (Slot& s : slots_) { s.want = false; s.keep = false; }
    for (const ChunkKey& c : centers) {
        for (int dy = -keep_radius; dy <= keep_radius; ++dy)
            for (int dx = -keep_radius; dx <= keep_radius; ++dx) {
                const auto it = slot_by_key_.find(pack({c.x + dx, c.y + dy}));
                if (it == slot_by_key_.end()) continue;
                Slot& s = slots_[it->second];
                s.keep = true;
                if (std::abs(dx) <= load_radius && std::abs(dy) <= load_radius) s.want = true;
            }
    }
    // what leaves: a resident chunk that no player is within keep_radius of
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        if (s.state == State::Resident && !s.keep && !s.unload_queued) { s.unload_queued = true; unload_queue_.push_back(i); }
    }
    // how far a chunk is from the nearest player (squared, in chunks)
    const auto nearest = [&](const Slot& s) {
        double best = 1e300;
        for (const ChunkKey& c : centers) {
            const double dx = s.entry.cx - c.x, dy = s.entry.cy - c.y;
            best = std::min(best, dx * dx + dy * dy);
        }
        return best;
    };
    // what comes: wanted chunks that are not here, nearest to a player first.  A chunk whose read failed is asked for again after retry_delay_ (up to kMaxLoadAttempts reads).
    struct Cand { double d; uint32_t slot; };
    std::vector<Cand> cands;
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        if (!s.want || s.state != State::Absent) continue;
        if (s.failed) {
            if (s.attempts >= kMaxLoadAttempts) continue;
            if (now < s.retry_at) { replan_ = true; continue; }   // (polled until its time comes)
            s.failed = false;
            ++stats_.retries;
        }
        cands.push_back({nearest(s), i});
    }
    if (cands.empty()) return;
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d != b.d ? a.d < b.d : a.slot < b.slot; });
    const size_t cap = static_cast<size_t>(file_.settings.max_tiles);
    // What gives way when a nearer chunk has no room (review A I1): any resident chunk, the farthest from its nearest player first, wanted or only kept alike.  A chunk is dropped for
    // another only when it is STRICTLY farther from its player than the newcomer is from its own, so two players at the same distance do not take each other's chunks in turn, and
    // every player keeps its own chunk (distance 0) unless the whole mesh is made of such chunks.
    std::vector<Cand> victims;
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        const Slot& s = slots_[i];
        if (s.state != State::Resident || s.unload_queued) continue;
        victims.push_back({nearest(s), i});
    }
    std::sort(victims.begin(), victims.end(), [](const Cand& a, const Cand& b) { return a.d != b.d ? a.d > b.d : a.slot < b.slot; });
    size_t next_victim = 0;
    // (the tiles of chunks already on their way out count as free: they will be)
    size_t leaving = 0;
    for (const Slot& s : slots_)
        if (s.unload_queued || s.state == State::Unloading) leaving += s.state == State::Unloading ? (s.tiles.size() - s.cursor) : s.entry.tiles;
    bool any = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const Cand& c : cands) {
            Slot& s = slots_[c.slot];
            while (committed_tiles_ - std::min(committed_tiles_, leaving) + s.entry.tiles > cap && next_victim < victims.size() && victims[next_victim].d > c.d) {
                Slot& v = slots_[victims[next_victim++].slot];
                v.evict = true;
                v.unload_queued = true;
                unload_queue_.push_back(static_cast<uint32_t>(&v - slots_.data()));
                leaving += v.entry.tiles;
                ++stats_.evicted_for_room;
            }
            if (committed_tiles_ - std::min(committed_tiles_, leaving) + s.entry.tiles > cap) { ++stats_.deferred_by_cap; replan_ = true; break; }
            if (committed_tiles_ + s.entry.tiles > cap) { ++stats_.deferred_by_cap; replan_ = true; break; }   // (room only once the leaving chunks have left: the next call asks again)
            s.state = State::Requested;
            committed_tiles_ += s.entry.tiles;
            requests_.push_back(c.slot);
            any = true;
        }
    }
    stats_.peak_committed_tiles = std::max(stats_.peak_committed_tiles, committed_tiles_);
    if (any) cv_.notify_one();
}
// [C_Y_B x C O A]

bool AreaNav::add_tile(LoadedTile* t, std::string* err) {
    dtTileRef ref = 0;
    const dtStatus st = mesh_->addTile(t->data, t->size, DT_TILE_FREE_DATA, 0, &ref);
    if (dtStatusFailed(st)) {
        dtFree(t->data);
        t->data = nullptr;
        if (err) *err = "Detour refused tile (" + std::to_string(t->tx) + ", " + std::to_string(t->ty) + ")";
        return false;
    }
    t->data = nullptr;   // Detour owns it
    const dtMeshTile* mt = mesh_->getTileByRef(ref);
    if (mt && mt->header) polygons_ += static_cast<size_t>(mt->header->polyCount);
    return true;
}

void AreaNav::remove_tile(const LoadedTile& t) {
    const dtTileRef ref = mesh_->getTileRefAt(t.tx, t.ty, 0);
    if (!ref) return;
    const dtMeshTile* mt = mesh_->getTileByRef(ref);
    if (mt && mt->header) polygons_ -= std::min(polygons_, static_cast<size_t>(mt->header->polyCount));
    mesh_->removeTile(ref, nullptr, nullptr);
}

std::vector<ChunkKey> AreaNav::pump(double budget_ms) {
    std::vector<ChunkKey> newly;
    if (!windowed_) return newly;
    const auto t0 = Clock::now();
    // 1. what the worker has read
    std::deque<LoadedChunk> got;
    {
        std::lock_guard<std::mutex> lk(mu_);
        got.swap(done_);
    }
    for (LoadedChunk& lc : got) {
        Slot& s = slots_[lc.slot];
        stats_.max_chunk_read_ms = std::max(stats_.max_chunk_read_ms, lc.read_ms);
        if (!lc.error.empty()) {
            s.state = State::Absent;
            s.failed = true;
            ++s.attempts;
            s.retry_at = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(retry_delay_));
            committed_tiles_ -= std::min<size_t>(committed_tiles_, s.entry.tiles);
            ++stats_.load_failures;
            load_errors_.push_back(lc.error + (s.attempts < kMaxLoadAttempts ? " (read " + std::to_string(s.attempts) + " of " + std::to_string(kMaxLoadAttempts) + "; asked again later)" : " (given up)"));
            replan_ = true;
            continue;
        }
        if (!s.keep) {
            for (auto& t : lc.tiles) if (t.data) dtFree(t.data);
            s.state = State::Absent;
            committed_tiles_ -= std::min<size_t>(committed_tiles_, s.entry.tiles);
            continue;
        }
        s.tiles = std::move(lc.tiles);
        s.cursor = 0;
        s.work_ms = 0;
        s.state = State::Arrived;
        arrived_.push_back(lc.slot);
    }
    // 2. what leaves (it frees room first)
    while (!unload_queue_.empty()) {
        const uint32_t idx = unload_queue_.front();
        Slot& s = slots_[idx];
        if (s.state == State::Resident && s.keep && !s.evict) { s.unload_queued = false; unload_queue_.pop_front(); continue; }   // a player came back
        if (s.state == State::Resident) { s.state = State::Unloading; s.cursor = 0; s.work_ms = 0; }
        if (s.state != State::Unloading) { s.unload_queued = false; unload_queue_.pop_front(); continue; }
        while (s.cursor < s.tiles.size()) {
            const auto ta = Clock::now();
            if (s.tiles[s.cursor].size > 0) {   // (a tile that Detour refused when it was added has size 0: nothing to remove)
                remove_tile(s.tiles[s.cursor]);
                --resident_tiles_;
                committed_tiles_ -= std::min<size_t>(committed_tiles_, 1);
            }
            stats_.max_tile_remove_ms = std::max(stats_.max_tile_remove_ms, ms_since(ta));
            s.work_ms += ms_since(ta);
            ++s.cursor;
            if (ms_since(t0) >= budget_ms) break;
        }
        if (s.cursor >= s.tiles.size()) {
            stats_.max_chunk_remove_ms = std::max(stats_.max_chunk_remove_ms, s.work_ms);
            std::vector<LoadedTile>().swap(s.tiles);
            s.state = State::Absent;
            s.unload_queued = false;
            s.evict = false;
            --resident_chunks_;
            ++stats_.chunk_unloads;
            unload_queue_.pop_front();
        }
        if (ms_since(t0) >= budget_ms) break;
    }
    // 3. what arrives: its tiles into Detour, a few at a time
    while (!arrived_.empty()) {
        const uint32_t idx = arrived_.front();
        Slot& s = slots_[idx];
        if (s.cursor == 0 && !s.keep) {
            // nobody is near any more: it never goes in
            for (auto& t : s.tiles) if (t.data) dtFree(t.data);
            std::vector<LoadedTile>().swap(s.tiles);
            s.state = State::Absent;
            committed_tiles_ -= std::min<size_t>(committed_tiles_, s.entry.tiles);
            arrived_.pop_front();
            continue;
        }
        while (s.cursor < s.tiles.size()) {
            const auto ta = Clock::now();
            std::string err;
            if (add_tile(&s.tiles[s.cursor], &err)) ++resident_tiles_;
            else { ++stats_.load_failures; load_errors_.push_back(err); committed_tiles_ -= std::min<size_t>(committed_tiles_, 1); s.tiles[s.cursor].size = 0; }
            stats_.max_tile_add_ms = std::max(stats_.max_tile_add_ms, ms_since(ta));
            s.work_ms += ms_since(ta);
            ++s.cursor;
            if (ms_since(t0) >= budget_ms) break;
        }
        if (s.cursor >= s.tiles.size()) {
            stats_.max_chunk_add_ms = std::max(stats_.max_chunk_add_ms, s.work_ms);
            stats_.total_add_ms += s.work_ms;
            s.state = State::Resident;
            s.attempts = 0;
            ++resident_chunks_;
            ++stats_.chunk_loads;
            newly.push_back({s.entry.cx, s.entry.cy});
            arrived_.pop_front();
        }
        if (ms_since(t0) >= budget_ms) break;
    }
    tile_count_ = resident_tiles_;
    stats_.peak_resident_tiles = std::max(stats_.peak_resident_tiles, resident_tiles_);
    stats_.peak_committed_tiles = std::max(stats_.peak_committed_tiles, committed_tiles_);
    stats_.max_pump_ms = std::max(stats_.max_pump_ms, ms_since(t0));
    return newly;
}

bool AreaNav::load_around(double x, double y, int radius, double timeout_seconds) {
    return load_around(std::vector<std::pair<double, double>>{{x, y}}, radius, timeout_seconds);
}

bool AreaNav::load_around(const std::vector<std::pair<double, double>>& points, int radius, double timeout_seconds) {
    if (!windowed_) return true;
    plan_window(centers_of(points), radius, radius + 1);
    const auto t0 = Clock::now();
    for (;;) {
        pump(50.0);
        bool waiting = false;
        for (const Slot& s : slots_)
            if (s.want && (s.state == State::Requested || s.state == State::Arrived)) waiting = true;
        if (!waiting) break;
        if (ms_since(t0) > timeout_seconds * 1000.0) { last_centers_.clear(); return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    last_centers_.clear();   // (this plan used its own radii: the next set_interest plans again with the window's)
    for (const Slot& s : slots_)
        if (s.want && s.state != State::Resident) return false;   // damaged, or no room
    return true;
}

void AreaNav::worker_main() {
    std::ifstream in(path_, std::ios::binary);
    const int max_polys = file_.settings.max_polys;
    const uint32_t chunk_tiles = file_.chunk_tiles;
    for (;;) {
        uint32_t idx = 0;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !requests_.empty(); });
            if (stop_) return;
            idx = requests_.front();
            requests_.pop_front();
        }
        const NavChunkEntry entry = slots_[idx].entry;   // never changes after load
        const auto t0 = Clock::now();
        LoadedChunk lc;
        lc.slot = idx;
        std::vector<NavTileBlob> blobs;
        std::string err;
        // (review A I3) a failed read leaves the stream's fail bit set: clear it (and reopen a file that would not open) before every chunk, or one I/O error would fail every chunk after it
        if (!in.is_open()) in.open(path_, std::ios::binary);
        in.clear();
        if (!in.is_open()) {
            lc.error = "chunk (" + std::to_string(entry.cx) + ", " + std::to_string(entry.cy) + ") cannot be read: " + path_ + " does not open";
        } else if (!read_nav_chunk(in, entry, &blobs, &err, chunk_tiles)) {
            lc.error = err.empty() ? "cannot read the chunk" : err;
            in.clear();
        } else {
            lc.tiles.reserve(blobs.size());
            for (const NavTileBlob& b : blobs) {
                std::string why;
                if (!tile_is_sound(b, max_polys, &why)) { lc.error = "chunk (" + std::to_string(entry.cx) + ", " + std::to_string(entry.cy) + ") tile (" + std::to_string(b.tx) + ", " + std::to_string(b.ty) + ") is damaged: " + why; break; }
                auto* data = static_cast<unsigned char*>(dtAlloc(b.data.size(), DT_ALLOC_PERM));
                if (!data) { lc.error = "out of memory"; break; }
                std::memcpy(data, b.data.data(), b.data.size());
                LoadedTile t;
                t.tx = b.tx; t.ty = b.ty; t.data = data; t.size = static_cast<int>(b.data.size());
                lc.tiles.push_back(t);
            }
            if (!lc.error.empty()) {
                for (auto& t : lc.tiles) dtFree(t.data);
                lc.tiles.clear();
            }
        }
        lc.read_ms = ms_since(t0);
        std::lock_guard<std::mutex> lk(mu_);
        worker_max_read_ms_ = std::max(worker_max_read_ms_, lc.read_ms);
        done_.push_back(std::move(lc));
    }
}

}  // namespace aoc::worldd::nav

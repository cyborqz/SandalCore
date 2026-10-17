// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "nav/nav_build.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

namespace aoc::worldd::nav {

// ---------------------------------------------------------------------------------------------------------------------------------
//  the geometry file
// ---------------------------------------------------------------------------------------------------------------------------------
namespace {

struct Rd {
    const uint8_t* p;
    size_t size, pos = 0;
    bool ok = true;
    template <typename T>
    T get() {
        T v{};
        if (pos > size || sizeof(T) > size - pos) { ok = false; return v; }
        std::memcpy(&v, p + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    bool bytes(void* dst, size_t n) {
        if (pos > size || n > size - pos) { ok = false; return false; }
        std::memcpy(dst, p + pos, n);
        pos += n;
        return true;
    }
};

}  // namespace
// -- C O A x C.Y.B --

bool parse_geometry(const uint8_t* data, size_t size, Geometry* out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    Rd r{data, size};
    char magic[8] = {};
    if (!r.bytes(magic, 8) || std::memcmp(magic, "AOCGEOM1", 8) != 0) return fail("not an area_geom.bin (bad magic)");
    const uint32_t version = r.get<uint32_t>();
    if (version != 1 && version != 2) return fail("unsupported area_geom.bin version");
    Geometry g;
    g.center_x = r.get<double>(); g.center_y = r.get<double>(); g.radius = r.get<double>(); g.margin = r.get<double>();
    if (version == 2) {
        g.region_hash = r.get<uint64_t>();
        const uint32_t nr = r.get<uint32_t>();
        if (!r.ok || nr > 100000) return fail("area_geom.bin region is out of range");
        g.region.resize(nr);
        for (auto& q : g.region) { q.x0 = r.get<double>(); q.y0 = r.get<double>(); q.x1 = r.get<double>(); q.y1 = r.get<double>(); }
    }
    const uint32_t nt = r.get<uint32_t>(), nm = r.get<uint32_t>(), np = r.get<uint32_t>();
    if (!r.ok || nt > 100000 || nm > 10000000 || np > 100000000) return fail("area_geom.bin header is out of range");
    // Check the minimum encoded size before allocating vectors from untrusted counts.
    if (56ull * nt + 10ull * nm + 64ull * np > r.size - r.pos) return fail("area_geom.bin is cut short (header counts)");
    g.terrain.resize(nt);
    for (auto& t : g.terrain) {
        t.x0 = r.get<double>(); t.y0 = r.get<double>(); t.step_x = r.get<double>(); t.step_y = r.get<double>();
        t.z_base = r.get<double>(); t.z_scale = r.get<double>();
        t.nx = r.get<uint32_t>(); t.ny = r.get<uint32_t>();
        if (!r.ok || t.nx < 2 || t.ny < 2 || t.nx > 4096 || t.ny > 4096) return fail("area_geom.bin terrain is out of range");
        const size_t n = static_cast<size_t>(t.nx) * t.ny;
        if (r.pos + n * 2 > r.size) return fail("area_geom.bin is cut short (terrain)");
        t.raw.resize(n);
        r.bytes(t.raw.data(), n * 2);
    }
    g.meshes.resize(nm);
    for (auto& m : g.meshes) {
        const uint16_t nl = r.get<uint16_t>();
        if (r.pos + nl > r.size) return fail("area_geom.bin is cut short (mesh name)");
        m.name.assign(reinterpret_cast<const char*>(data + r.pos), nl);
        r.pos += nl;
        const uint32_t nv = r.get<uint32_t>(), ni = r.get<uint32_t>();
        if (!r.ok || ni % 3 != 0 || r.pos + static_cast<size_t>(nv) * 12 + static_cast<size_t>(ni) * 4 > r.size) return fail("area_geom.bin is cut short (mesh)");
        m.v.resize(static_cast<size_t>(nv) * 3);
        r.bytes(m.v.data(), m.v.size() * 4);
        m.i.resize(ni);
        r.bytes(m.i.data(), static_cast<size_t>(ni) * 4);
        for (uint32_t idx : m.i) if (idx >= nv) return fail("area_geom.bin has a triangle index out of range");
    }
    g.placements.resize(np);
    for (auto& p : g.placements) {
        p.mesh = r.get<uint32_t>();
        for (float& f : p.r) f = r.get<float>();
        for (double& d : p.t) d = r.get<double>();
        if (!r.ok) return fail("area_geom.bin is cut short (placements)");
        if (p.mesh >= nm) return fail("area_geom.bin has a placement of an unknown mesh");
    }
    if (r.pos != size) return fail("area_geom.bin has trailing bytes");
    *out = std::move(g);
    return true;
}

std::vector<uint8_t> serialize_geometry(const Geometry& g) {
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { const auto* b = static_cast<const uint8_t*>(p); out.insert(out.end(), b, b + n); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto f64 = [&](double v) { put(&v, 8); };
    put("AOCGEOM1", 8);
    const bool v2 = !g.region.empty();
    u32(v2 ? 2 : 1);
    f64(g.center_x); f64(g.center_y); f64(g.radius); f64(g.margin);
    if (v2) {
        const uint64_t h = g.region_hash;
        put(&h, 8);
        u32(static_cast<uint32_t>(g.region.size()));
        for (const auto& q : g.region) { f64(q.x0); f64(q.y0); f64(q.x1); f64(q.y1); }
    }
    u32(static_cast<uint32_t>(g.terrain.size())); u32(static_cast<uint32_t>(g.meshes.size())); u32(static_cast<uint32_t>(g.placements.size()));
    for (const auto& t : g.terrain) {
        f64(t.x0); f64(t.y0); f64(t.step_x); f64(t.step_y); f64(t.z_base); f64(t.z_scale);
        u32(t.nx); u32(t.ny);
        put(t.raw.data(), t.raw.size() * 2);
    }
    for (const auto& m : g.meshes) {
        const uint16_t nl = static_cast<uint16_t>(m.name.size());
        put(&nl, 2); put(m.name.data(), nl);
        u32(static_cast<uint32_t>(m.v.size() / 3)); u32(static_cast<uint32_t>(m.i.size()));
        put(m.v.data(), m.v.size() * 4);
        put(m.i.data(), m.i.size() * 4);
    }
    for (const auto& p : g.placements) {
        u32(p.mesh);
        put(p.r, sizeof p.r);
        put(p.t, sizeof p.t);
    }
    return out;
}

bool read_geometry(const std::string& path, Geometry* out, std::vector<uint8_t>* raw, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (err) *err = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const auto len = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    if (len > 0) in.read(reinterpret_cast<char*>(buf.data()), len);
    if (!in) { if (err) *err = "cannot read " + path; return false; }
    const bool ok = parse_geometry(buf.data(), buf.size(), out, err);
    if (raw) *raw = std::move(buf);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
//  the build
// ---------------------------------------------------------------------------------------------------------------------------------
namespace {

struct Box { double x0, z0, x1, z1; };   // nav-local x and z

template <typename T, void (*Free)(T*)>
struct Rc {
    T* p = nullptr;
    ~Rc() { if (p) Free(p); }
    Rc() = default;
    Rc(const Rc&) = delete;
    Rc& operator=(const Rc&) = delete;
};
// C:Y:B / CoA

using HF = Rc<rcHeightfield, rcFreeHeightField>;
using CHF = Rc<rcCompactHeightfield, rcFreeCompactHeightfield>;
using CS = Rc<rcContourSet, rcFreeContourSet>;
using PM = Rc<rcPolyMesh, rcFreePolyMesh>;
using PMD = Rc<rcPolyMeshDetail, rcFreePolyMeshDetail>;

struct TileTris {
    std::vector<float> v;     // nav-local xyz
    std::vector<int> t;       // indices
    double zmin = 1e30, zmax = -1e30;
    void tri(const float* a, const float* b, const float* c) {
        const int base = static_cast<int>(v.size() / 3);
        for (const float* p : {a, b, c}) { v.push_back(p[0]); v.push_back(p[1]); v.push_back(p[2]); zmin = std::min<double>(zmin, p[1]); zmax = std::max<double>(zmax, p[1]); }
        t.push_back(base); t.push_back(base + 1); t.push_back(base + 2);
    }
};

struct Builder {
    const Geometry& g;
    const BuildOptions& opt;
    NavSettings s;
    double ox, oz;                 // the grid's corner in nav-local x, z (E1's: the centre - 112,000 cm)
    int tx0 = 0, ty0 = 0;          // the first tile index of the tables below (E1's grid starts at 0; the region's reaches into the negative)
    int ntx, nty;                  // the tables' size
    std::vector<uint8_t> wanted;   // region mode: per table tile, whether it touches the footprint
    std::vector<std::vector<uint32_t>> bucket;     // per tile: placements whose padded bounds overlap it
    std::vector<Box> pbox;                         // per placement: nav-local bounds
    int walkable_height, walkable_climb, walkable_radius, border;

    Builder(const Geometry& geo, const BuildOptions& o) : g(geo), opt(o), s(o.settings) {}

    // world -> nav-local
    void to_local(double wx, double wy, double wz, float* out) const {
        out[0] = static_cast<float>(wx - g.center_x);
        out[1] = static_cast<float>(wz);
        out[2] = static_cast<float>(wy - g.center_y);
    }

    void prepare() {
        const double ts = s.tile_size;
        const double half = std::ceil((g.radius + opt.nav_margin_cm) / ts) * ts;
        ox = -half; oz = -half;
        ntx = nty = static_cast<int>(std::lround(2 * half / ts));
        if (!g.region.empty()) {
            // the region: the tiles of E1's own grid (the same origin, so that the same tiles come out) that reach the footprint's bounding box
            double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
            for (const auto& q : g.region) { bx0 = std::min(bx0, q.x0); by0 = std::min(by0, q.y0); bx1 = std::max(bx1, q.x1); by1 = std::max(by1, q.y1); }
            tx0 = static_cast<int>(std::floor((bx0 - g.center_x - ox) / ts));
            ty0 = static_cast<int>(std::floor((by0 - g.center_y - oz) / ts));
            const int tx1 = static_cast<int>(std::floor((bx1 - g.center_x - ox) / ts)), ty1 = static_cast<int>(std::floor((by1 - g.center_y - oz) / ts));
            ntx = tx1 - tx0 + 1;
            nty = ty1 - ty0 + 1;
            wanted.assign(static_cast<size_t>(ntx) * nty, 0);
            for (const auto& q : g.region) {
                // the tiles [x0, x0 + ts) that overlap [q.x0, q.x1): a tile touching the rectangle's edge only is not wanted
                const int a0 = static_cast<int>(std::floor((q.x0 - g.center_x - ox) / ts)), a1 = static_cast<int>(std::ceil((q.x1 - g.center_x - ox) / ts)) - 1;
                const int b0 = static_cast<int>(std::floor((q.y0 - g.center_y - oz) / ts)), b1 = static_cast<int>(std::ceil((q.y1 - g.center_y - oz) / ts)) - 1;
                for (int ty = std::max(b0, ty0); ty <= std::min(b1, ty0 + nty - 1); ++ty)
                    for (int tx = std::max(a0, tx0); tx <= std::min(a1, tx0 + ntx - 1); ++tx) wanted[static_cast<size_t>(ty - ty0) * ntx + (tx - tx0)] = 1;
            }
        }
        s.origin[0] = static_cast<float>(ox);
        s.origin[2] = static_cast<float>(oz);
        walkable_height = static_cast<int>(std::ceil(s.agent_height / s.cell_height));
        walkable_climb = static_cast<int>(std::floor(s.agent_max_climb / s.cell_height));
        walkable_radius = static_cast<int>(std::ceil(s.agent_radius / s.cell_size));
        border = walkable_radius + 3;
        const double pad = border * s.cell_size + s.cell_size;
        // each placement's bounds in nav-local x, z
        std::vector<std::array<double, 6>> mb(g.meshes.size());
        for (size_t i = 0; i < g.meshes.size(); ++i) {
            const auto& m = g.meshes[i];
            std::array<double, 6> b = {1e30, 1e30, 1e30, -1e30, -1e30, -1e30};
            for (size_t k = 0; k + 2 < m.v.size(); k += 3)
                for (int a = 0; a < 3; ++a) { b[a] = std::min<double>(b[a], m.v[k + a]); b[3 + a] = std::max<double>(b[3 + a], m.v[k + a]); }
            mb[i] = b;
        }
        bucket.assign(static_cast<size_t>(ntx) * nty, {});
        pbox.resize(g.placements.size());
        for (size_t pi = 0; pi < g.placements.size(); ++pi) {
            const auto& p = g.placements[pi];
            const auto& b = mb[p.mesh];
            double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
            if (b[0] > b[3]) { pbox[pi] = {1, 1, 0, 0}; continue; }   // an empty mesh
            for (int c = 0; c < 8; ++c) {
                const double lx = (c & 1) ? b[3] : b[0], ly = (c & 2) ? b[4] : b[1], lz = (c & 4) ? b[5] : b[2];
                const double wx = p.r[0] * lx + p.r[1] * ly + p.r[2] * lz + p.t[0];
                const double wy = p.r[3] * lx + p.r[4] * ly + p.r[5] * lz + p.t[1];
                x0 = std::min(x0, wx - g.center_x); x1 = std::max(x1, wx - g.center_x);
                z0 = std::min(z0, wy - g.center_y); z1 = std::max(z1, wy - g.center_y);
            }
            pbox[pi] = {x0, z0, x1, z1};
            const int a0 = std::max(0, static_cast<int>(std::floor((x0 - pad - ox) / ts)) - tx0), a1 = std::min(ntx - 1, static_cast<int>(std::floor((x1 + pad - ox) / ts)) - tx0);
            const int b0 = std::max(0, static_cast<int>(std::floor((z0 - pad - oz) / ts)) - ty0), b1 = std::min(nty - 1, static_cast<int>(std::floor((z1 + pad - oz) / ts)) - ty0);
            for (int by = b0; by <= b1; ++by)
                for (int bx = a0; bx <= a1; ++bx) bucket[static_cast<size_t>(by) * ntx + bx].push_back(static_cast<uint32_t>(pi));
        }
    }

    bool tile_wanted(int tx, int ty) const {
        if (!g.region.empty()) return wanted[static_cast<size_t>(ty - ty0) * ntx + (tx - tx0)] != 0;
        const double ts = s.tile_size;
        const double x0 = ox + tx * ts, z0 = oz + ty * ts;
        const double nx = std::clamp(0.0, x0, x0 + ts), nz = std::clamp(0.0, z0, z0 + ts);
        const double r = g.radius + opt.nav_margin_cm;
        return nx * nx + nz * nz <= r * r;
    }

    void gather(int tx, int ty, TileTris* out) const {
        const double ts = s.tile_size;
        const double pad = border * s.cell_size;
        const double bx0 = ox + tx * ts - pad, bx1 = ox + (tx + 1) * ts + pad;
        const double bz0 = oz + ty * ts - pad, bz1 = oz + (ty + 1) * ts + pad;
        // the terrain: both triangles of every quad that touches the box, wound so that the nav-local mapping (which swaps y and z) keeps them facing up
        for (const auto& t : g.terrain) {
            const double wx0 = bx0 + g.center_x, wx1 = bx1 + g.center_x, wy0 = bz0 + g.center_y, wy1 = bz1 + g.center_y;
            if (wx1 < t.x0 || wx0 > t.x1() || wy1 < t.y0 || wy0 > t.y1()) continue;
            const int ix0 = std::max(0, static_cast<int>(std::floor((wx0 - t.x0) / t.step_x))), ix1 = std::min<int>(t.nx - 2, static_cast<int>(std::floor((wx1 - t.x0) / t.step_x)));
            const int iy0 = std::max(0, static_cast<int>(std::floor((wy0 - t.y0) / t.step_y))), iy1 = std::min<int>(t.ny - 2, static_cast<int>(std::floor((wy1 - t.y0) / t.step_y)));
            for (int iy = iy0; iy <= iy1; ++iy)
                for (int ix = ix0; ix <= ix1; ++ix) {
                    float p00[3], p10[3], p01[3], p11[3];
                    to_local(t.x0 + ix * t.step_x, t.y0 + iy * t.step_y, t.sample(ix, iy), p00);
                    to_local(t.x0 + (ix + 1) * t.step_x, t.y0 + iy * t.step_y, t.sample(ix + 1, iy), p10);
                    to_local(t.x0 + ix * t.step_x, t.y0 + (iy + 1) * t.step_y, t.sample(ix, iy + 1), p01);
                    to_local(t.x0 + (ix + 1) * t.step_x, t.y0 + (iy + 1) * t.step_y, t.sample(ix + 1, iy + 1), p11);
                    out->tri(p00, p11, p10);
                    out->tri(p00, p01, p11);
                }
        }
        // the placed meshes
        std::vector<float> tv;
        for (uint32_t pi : bucket[static_cast<size_t>(ty - ty0) * ntx + (tx - tx0)]) {
            const auto& pl = g.placements[pi];
            const Box& pb = pbox[pi];
            if (pb.x1 < bx0 || pb.x0 > bx1 || pb.z1 < bz0 || pb.z0 > bz1) continue;
            const auto& m = g.meshes[pl.mesh];
            const size_t nv = m.v.size() / 3;
            tv.resize(nv * 3);
            for (size_t k = 0; k < nv; ++k) {
                const double lx = m.v[k * 3], ly = m.v[k * 3 + 1], lz = m.v[k * 3 + 2];
                const double wx = pl.r[0] * lx + pl.r[1] * ly + pl.r[2] * lz + pl.t[0];
                const double wy = pl.r[3] * lx + pl.r[4] * ly + pl.r[5] * lz + pl.t[1];
                const double wz = pl.r[6] * lx + pl.r[7] * ly + pl.r[8] * lz + pl.t[2];
                to_local(wx, wy, wz, &tv[k * 3]);
            }
            const double det = static_cast<double>(pl.r[0]) * (static_cast<double>(pl.r[4]) * pl.r[8] - static_cast<double>(pl.r[5]) * pl.r[7]) -
                               static_cast<double>(pl.r[1]) * (static_cast<double>(pl.r[3]) * pl.r[8] - static_cast<double>(pl.r[5]) * pl.r[6]) +
                               static_cast<double>(pl.r[2]) * (static_cast<double>(pl.r[3]) * pl.r[7] - static_cast<double>(pl.r[4]) * pl.r[6]);
            for (size_t k = 0; k + 2 < m.i.size(); k += 3) {
                const float* a = &tv[m.i[k] * 3];
                const float* b = &tv[m.i[k + 1] * 3];
                const float* c = &tv[m.i[k + 2] * 3];
                const float xmin = std::min({a[0], b[0], c[0]}), xmax = std::max({a[0], b[0], c[0]});
                if (xmax < bx0 || xmin > bx1) continue;
                const float zmin = std::min({a[2], b[2], c[2]}), zmax = std::max({a[2], b[2], c[2]});
                if (zmax < bz0 || zmin > bz1) continue;
                // outward in the world's winding; the swap into nav-local flips it, a mirrored placement flips it once more
                if (det >= 0) out->tri(a, c, b); else out->tri(a, b, c);
            }
        }
    }
// -- CoA & C:Y:B --

    // returns 1 built, 0 empty, -1 failed; `retried` counts a tile that had to be simplified more to fit Detour's polygon limit
    int build_tile(int tx, int ty, std::vector<uint8_t>* blob, long long* polys, long long* tris, std::string* err, int* retried = nullptr, int* tile_polys = nullptr) const {
        TileTris tt;
        gather(tx, ty, &tt);
        if (tt.t.empty()) return 0;
        *tris += static_cast<long long>(tt.t.size() / 3);
        rcContext ctx(false);
        const double ts = s.tile_size;
        rcConfig cfg;
        std::memset(&cfg, 0, sizeof cfg);
        cfg.cs = s.cell_size;
        cfg.ch = s.cell_height;
        cfg.walkableSlopeAngle = s.agent_max_slope;
        cfg.walkableHeight = walkable_height;
        cfg.walkableClimb = walkable_climb;
        cfg.walkableRadius = walkable_radius;
        cfg.maxEdgeLen = static_cast<int>(1200.0f / s.cell_size);
        cfg.maxSimplificationError = 1.3f;
        cfg.minRegionArea = rcSqr(8);
        cfg.mergeRegionArea = rcSqr(20);
        cfg.maxVertsPerPoly = 6;
        cfg.tileSize = static_cast<int>(std::lround(ts / s.cell_size));
        cfg.borderSize = border;
        cfg.width = cfg.tileSize + border * 2;
        cfg.height = cfg.tileSize + border * 2;
        cfg.detailSampleDist = s.cell_size * 8.0f;
        cfg.detailSampleMaxError = s.cell_height * 1.5f;
        const float pad = border * s.cell_size;
        cfg.bmin[0] = static_cast<float>(ox + tx * ts) - pad;
        cfg.bmax[0] = static_cast<float>(ox + (tx + 1) * ts) + pad;
        cfg.bmin[2] = static_cast<float>(oz + ty * ts) - pad;
        cfg.bmax[2] = static_cast<float>(oz + (ty + 1) * ts) + pad;
        cfg.bmin[1] = static_cast<float>(std::floor(tt.zmin / s.cell_height) * s.cell_height) - s.cell_height;
        cfg.bmax[1] = static_cast<float>(std::ceil(tt.zmax / s.cell_height) * s.cell_height) + s.cell_height * (walkable_height + 2);

        HF solid;
        solid.p = rcAllocHeightfield();
        if (!solid.p || !rcCreateHeightfield(&ctx, *solid.p, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) { *err = "heightfield"; return -1; }
        const int nt = static_cast<int>(tt.t.size() / 3);
        std::vector<unsigned char> areas(static_cast<size_t>(nt), 0);
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, tt.v.data(), static_cast<int>(tt.v.size() / 3), tt.t.data(), nt, areas.data());
        if (!rcRasterizeTriangles(&ctx, tt.v.data(), static_cast<int>(tt.v.size() / 3), tt.t.data(), areas.data(), nt, *solid.p, cfg.walkableClimb)) { *err = "rasterize"; return -1; }
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid.p);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid.p);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid.p);

        CHF chf;
        chf.p = rcAllocCompactHeightfield();
        if (!chf.p || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid.p, *chf.p)) { *err = "compact heightfield"; return -1; }
        if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf.p)) { *err = "erode"; return -1; }
        if (!rcBuildDistanceField(&ctx, *chf.p)) { *err = "distance field"; return -1; }
        if (!rcBuildRegions(&ctx, *chf.p, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) { *err = "regions"; return -1; }
        CS cset;
        PM pmesh;
        PMD dmesh;
        // A tile with more polygons than a Detour tile may hold (the limit is the mesh's polygon bits: 1,024 with 4,096 tiles) is built again with a coarser simplification
        // (a deterministic fallback: the same tile always takes the same steps; E1's 2,488 tiles never needed it, their largest has 800).
        float simplification = cfg.maxSimplificationError;
        for (int attempt = 0;; ++attempt) {
            cset.p = rcAllocContourSet();
            if (!cset.p || !rcBuildContours(&ctx, *chf.p, simplification, cfg.maxEdgeLen, *cset.p)) { *err = "contours"; return -1; }
            if (cset.p->nconts == 0) return 0;
            pmesh.p = rcAllocPolyMesh();
            if (!pmesh.p || !rcBuildPolyMesh(&ctx, *cset.p, cfg.maxVertsPerPoly, *pmesh.p)) { *err = "poly mesh"; return -1; }
            if (pmesh.p->npolys == 0) return 0;
            if (pmesh.p->npolys <= s.max_polys) break;
            if (attempt >= 6) { *err = "tile has " + std::to_string(pmesh.p->npolys) + " polygons (the limit is " + std::to_string(s.max_polys) + ") even simplified " + std::to_string(simplification); return -1; }
            if (retried && attempt == 0) ++*retried;
            simplification *= 1.6f;
            rcFreePolyMesh(pmesh.p); pmesh.p = nullptr;
            rcFreeContourSet(cset.p); cset.p = nullptr;
        }
        dmesh.p = rcAllocPolyMeshDetail();
        if (!dmesh.p || !rcBuildPolyMeshDetail(&ctx, *pmesh.p, *chf.p, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh.p)) { *err = "detail mesh"; return -1; }
        if (tile_polys) *tile_polys = pmesh.p->npolys;
        for (int i = 0; i < pmesh.p->npolys; ++i) {
            if (pmesh.p->areas[i] == RC_WALKABLE_AREA) pmesh.p->areas[i] = 1;
            pmesh.p->flags[i] = pmesh.p->areas[i] != 0 ? 1 : 0;
        }
        dtNavMeshCreateParams params;
        std::memset(&params, 0, sizeof params);
        params.verts = pmesh.p->verts;
        params.vertCount = pmesh.p->nverts;
        params.polys = pmesh.p->polys;
        params.polyAreas = pmesh.p->areas;
        params.polyFlags = pmesh.p->flags;
        params.polyCount = pmesh.p->npolys;
        params.nvp = pmesh.p->nvp;
        params.detailMeshes = dmesh.p->meshes;
        params.detailVerts = dmesh.p->verts;
        params.detailVertsCount = dmesh.p->nverts;
        params.detailTris = dmesh.p->tris;
        params.detailTriCount = dmesh.p->ntris;
        params.walkableHeight = s.agent_height;
        params.walkableRadius = s.agent_radius;
        params.walkableClimb = s.agent_max_climb;
        params.tileX = tx;
        params.tileY = ty;
        params.tileLayer = 0;
        rcVcopy(params.bmin, pmesh.p->bmin);
        rcVcopy(params.bmax, pmesh.p->bmax);
        params.cs = cfg.cs;
        params.ch = cfg.ch;
        params.buildBvTree = true;
        unsigned char* data = nullptr;
        int size = 0;
        if (!dtCreateNavMeshData(&params, &data, &size)) { *err = "dtCreateNavMeshData"; return -1; }
        blob->assign(data, data + size);
        dtFree(data);
        *polys += pmesh.p->npolys;
        return 1;
    }
};

}  // namespace

bool build_nav(const Geometry& g, uint64_t geometry_hash, const BuildOptions& opt, NavFile* out, BuildStats* stats, std::string* err) {
    const auto t0 = std::chrono::steady_clock::now();
    Builder b(g, opt);
    b.prepare();
    // the lowest ground gives the grid's floor (the tile origin's y is not used for addressing; it is recorded)
    double zmin = 0;
    {
        bool any = false;
        for (const auto& t : g.terrain)
            for (uint16_t r : t.raw) { const double z = t.z_base + (static_cast<int>(r) - 32768) * t.z_scale; if (!any || z < zmin) { zmin = z; any = true; } }
    }
    b.s.origin[1] = static_cast<float>(std::floor(zmin));
// [C.Y.B x CoA]

    struct Job { int tx, ty; };
    std::vector<Job> jobs;
    for (int ty = b.ty0; ty < b.ty0 + b.nty; ++ty)
        for (int tx = b.tx0; tx < b.tx0 + b.ntx; ++tx)
            if (b.tile_wanted(tx, ty)) jobs.push_back({tx, ty});

    struct Result { int status = 0; std::vector<uint8_t> blob; long long polys = 0, tris = 0; std::string err; int retried = 0, tile_polys = 0; };
    std::vector<Result> results(jobs.size());
    std::atomic<size_t> next{0};
    int nthreads = opt.threads > 0 ? opt.threads : static_cast<int>(std::thread::hardware_concurrency());
    nthreads = std::clamp(nthreads, 1, 64);
    auto worker = [&] {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= jobs.size()) return;
            Result& r = results[i];
            r.status = b.build_tile(jobs[i].tx, jobs[i].ty, &r.blob, &r.polys, &r.tris, &r.err, &r.retried, &r.tile_polys);
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < nthreads; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    NavFile f;
    f.center_x = g.center_x; f.center_y = g.center_y; f.radius = g.radius; f.margin = g.margin;
    f.geometry_hash = geometry_hash;
    f.settings = b.s;
    f.terrain = g.terrain;
    if (!g.region.empty()) {
        f.chunk_tiles = static_cast<uint32_t>(kChunkTiles);
        f.region = g.region;
        f.region_hash = g.region_hash;
    }
    BuildStats st;
    std::string first_err;
    for (size_t i = 0; i < jobs.size(); ++i) {
        auto& r = results[i];
        st.input_triangles += r.tris;
        st.retried_tiles += r.retried;
        st.max_tile_polys = std::max(st.max_tile_polys, r.tile_polys);
        if (r.status < 0) {
            ++st.failed_tiles;
            if (first_err.empty()) first_err = "tile (" + std::to_string(jobs[i].tx) + ", " + std::to_string(jobs[i].ty) + "): " + r.err;
        } else if (r.status == 0) {
            ++st.empty_tiles;
        } else {
            NavTileBlob tb;
            tb.tx = jobs[i].tx; tb.ty = jobs[i].ty;
            tb.data = std::move(r.blob);
            st.bytes += static_cast<long long>(tb.data.size());
            st.polys += r.polys;
            ++st.tiles;
            f.tiles.push_back(std::move(tb));
        }
    }
    st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (stats) *stats = st;
    if (st.failed_tiles > 0) {
        if (err) *err = std::to_string(st.failed_tiles) + " tiles failed, the first " + first_err;
        return false;
    }
    *out = std::move(f);
    return true;
}

}  // namespace aoc::worldd::nav

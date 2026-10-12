// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  aoc_navbuild  (plan E1, stage 2; chunked output: G1 stage 1): mapc's area_geom.bin into area_nav.bin.
//
//      aoc_navbuild --geometry dist/content/area_geom.bin --out dist/content/area_nav.bin [--threads N] [--nav-margin-cm 10000]
//      aoc_navbuild --compare A.bin B.bin      two chunked files: the same header, region, terrain and chunk hashes?  (exit 0 / 1; `mapc --check` uses it)
//      aoc_navbuild --info FILE.bin            the numbers of a file (tiles, chunks, sizes, terrain); with --verify every chunk is read back and its hash checked
//
//  mapc runs it ("mapc build"); run alone it rebuilds the navmesh from a geometry file.  The output does not depend on the number of threads.  A geometry with a
//  region (mapc's version 2 file) gives the chunked file (nav/nav_file.h version 2); a geometry of E1's circle gives the flat version 1.
// ============================================================================
#include "nav/nav_build.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

using namespace aoc::worldd::nav;

static int compare_files(const std::string& a_path, const std::string& b_path) {
    NavFile a, b;
    std::string err;
    if (!read_nav_index(a_path, &a, &err)) { std::fprintf(stderr, "aoc_navbuild: %s\n", err.c_str()); return 2; }
    if (!read_nav_index(b_path, &b, &err)) { std::fprintf(stderr, "aoc_navbuild: %s\n", err.c_str()); return 2; }
    int differences = 0;
    auto note = [&](const char* what) { std::printf("  differs: %s\n", what); ++differences; };
    if (a.center_x != b.center_x || a.center_y != b.center_y || a.radius != b.radius || a.margin != b.margin) note("the centre, radius or margin");
    if (a.geometry_hash != b.geometry_hash) note("the geometry hash");
    if (std::memcmp(&a.settings, &b.settings, sizeof a.settings) != 0) note("the navmesh settings");
    if (a.chunk_tiles != b.chunk_tiles) note("the chunk size");
    if (a.region_hash != b.region_hash || a.region.size() != b.region.size()) note("the region");
    if (a.terrain.size() != b.terrain.size()) note("the number of terrain grids");
    else
        for (size_t i = 0; i < a.terrain.size(); ++i) {
            const auto &x = a.terrain[i], &y = b.terrain[i];
            if (x.x0 != y.x0 || x.y0 != y.y0 || x.step_x != y.step_x || x.step_y != y.step_y || x.z_base != y.z_base || x.z_scale != y.z_scale || x.nx != y.nx || x.ny != y.ny ||
                fnv1a64(reinterpret_cast<const uint8_t*>(x.raw.data()), x.raw.size() * 2) != fnv1a64(reinterpret_cast<const uint8_t*>(y.raw.data()), y.raw.size() * 2)) {
                std::printf("  differs: terrain grid %zu at (%.0f, %.0f)\n", i, x.x0, x.y0);
                ++differences;
            }
        }
    size_t ia = 0, ib = 0, shown = 0, chunk_diffs = 0;
    auto key = [](const NavChunkEntry& e) { return ChunkKey{e.cx, e.cy}; };
    while (ia < a.chunks.size() || ib < b.chunks.size()) {
        const bool only_a = ib >= b.chunks.size() || (ia < a.chunks.size() && key(a.chunks[ia]) < key(b.chunks[ib]));
        const bool only_b = ia >= a.chunks.size() || (ib < b.chunks.size() && key(b.chunks[ib]) < key(a.chunks[ia]));
        if (only_a || only_b) {
            const NavChunkEntry& e = only_a ? a.chunks[ia] : b.chunks[ib];
            if (shown++ < 20) std::printf("  chunk (%d, %d) only in %s\n", e.cx, e.cy, only_a ? "the first file" : "the second");
            ++chunk_diffs;
            (only_a ? ia : ib)++;
            continue;
        }
        const NavChunkEntry &x = a.chunks[ia], &y = b.chunks[ib];
        if (x.tiles != y.tiles || x.size != y.size || x.hash != y.hash) {
            if (shown++ < 20) std::printf("  chunk (%d, %d) differs: %u tiles / %u bytes against %u / %u\n", x.cx, x.cy, x.tiles, x.size, y.tiles, y.size);
            ++chunk_diffs;
        }
        ++ia; ++ib;
    }
    if (chunk_diffs) { std::printf("  %zu chunks differ\n", chunk_diffs); ++differences; }
    std::printf("compare: %zu and %zu chunks, %zu and %zu terrain grids: %s\n", a.chunks.size(), b.chunks.size(), a.terrain.size(), b.terrain.size(), differences ? "DIFFER" : "identical");
    return differences ? 1 : 0;
}
// CoA.C.Y.B

static int info_file(const std::string& path, bool verify) {
    const int v = nav_file_version(path);
    if (v == 0) { std::fprintf(stderr, "aoc_navbuild: %s is not a nav file\n", path.c_str()); return 2; }
    NavFile f;
    std::string err;
    if (v == 1) {
        if (!read_nav_file(path, &f, &err)) { std::fprintf(stderr, "aoc_navbuild: %s\n", err.c_str()); return 1; }
        size_t bytes = 0;
        for (const auto& t : f.tiles) bytes += t.data.size();
        std::printf("%s: version 1 (flat), %zu terrain grids, %zu tiles, tile bytes %zu, centre (%.1f, %.1f) radius %.0f\n", path.c_str(), f.terrain.size(), f.tiles.size(), bytes, f.center_x, f.center_y, f.radius);
        return 0;
    }
    if (!read_nav_index(path, &f, &err)) { std::fprintf(stderr, "aoc_navbuild: %s\n", err.c_str()); return 1; }
    uint64_t bytes = 0, max_chunk = 0, tiles = 0, max_tiles = 0;
    for (const auto& c : f.chunks) { bytes += c.size; max_chunk = std::max<uint64_t>(max_chunk, c.size); tiles += c.tiles; max_tiles = std::max<uint64_t>(max_tiles, c.tiles); }
    uint64_t terrain_bytes = 0;
    for (const auto& g : f.terrain) terrain_bytes += 2ull * g.raw.size();
    std::printf("%s: version 2 (chunked, %u tiles per side), %zu region rectangles (hash %016llx), %zu terrain grids (%.1f MB), %zu chunks, %llu tiles\n", path.c_str(), f.chunk_tiles, f.region.size(),
                static_cast<unsigned long long>(f.region_hash), f.terrain.size(), terrain_bytes / 1048576.0, f.chunks.size(), static_cast<unsigned long long>(tiles));
    std::printf("  tile bytes %.1f MB (mean chunk %.2f MB, largest %.2f MB; mean %.1f tiles per chunk, most %llu); centre (%.1f, %.1f)\n", bytes / 1048576.0, f.chunks.empty() ? 0.0 : bytes / 1048576.0 / f.chunks.size(),
                max_chunk / 1048576.0, f.chunks.empty() ? 0.0 : static_cast<double>(tiles) / f.chunks.size(), static_cast<unsigned long long>(max_tiles), f.center_x, f.center_y);
    if (verify) {
        std::ifstream in(path, std::ios::binary);
        size_t bad = 0;
        for (const auto& c : f.chunks) {
            std::vector<NavTileBlob> t;
            if (!read_nav_chunk(in, c, &t, &err, f.chunk_tiles)) { std::printf("  %s\n", err.c_str()); ++bad; }
        }
        std::printf("  verify: %zu chunks read back, %zu bad\n", f.chunks.size(), bad);
        return bad ? 1 : 0;
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string geom, out;
    BuildOptions opt;
    std::string cmp_a, cmp_b, info;
    bool verify = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "aoc_navbuild: %s needs a value\n", name); std::exit(2); }
            return argv[++i];
        };
        if (a == "--geometry") geom = val("--geometry");
        else if (a == "--out") out = val("--out");
        else if (a == "--threads") opt.threads = std::atoi(val("--threads"));
        else if (a == "--nav-margin-cm") opt.nav_margin_cm = std::atof(val("--nav-margin-cm"));
        else if (a == "--verbose") opt.verbose = true;
        else if (a == "--compare") { cmp_a = val("--compare"); cmp_b = val("--compare"); }
        else if (a == "--info") info = val("--info");
        else if (a == "--verify") verify = true;
        else { std::fprintf(stderr, "aoc_navbuild: unknown argument %s\n", a.c_str()); return 2; }
    }
    if (!cmp_a.empty()) return compare_files(cmp_a, cmp_b);
    if (!info.empty()) return info_file(info, verify);
    if (geom.empty() || out.empty()) {
        std::fprintf(stderr, "usage: aoc_navbuild --geometry area_geom.bin --out area_nav.bin [--threads N] [--nav-margin-cm N]\n"
                             "       aoc_navbuild --compare A.bin B.bin | --info FILE.bin [--verify]\n");
        return 2;
    }
    Geometry g;
    uint64_t geometry_hash = 0;
    std::string err;
    {
        std::vector<uint8_t> raw;
        if (!read_geometry(geom, &g, &raw, &err)) { std::fprintf(stderr, "aoc_navbuild: %s\n", err.c_str()); return 1; }
        geometry_hash = fnv1a64(raw.data(), raw.size());
    }
    std::printf("geometry: %zu terrain grids, %zu meshes, %zu placements%s\n", g.terrain.size(), g.meshes.size(), g.placements.size(),
                g.region.empty() ? " (E1's circle)" : (", a region of " + std::to_string(g.region.size()) + " rectangles").c_str());
    NavFile f;
    BuildStats st;
    if (!build_nav(g, geometry_hash, opt, &f, &st, &err)) {
        std::fprintf(stderr, "aoc_navbuild: %s\n", err.c_str());
        return 1;
    }
    g = Geometry{};   // the meshes and placements are not needed again
    if (!write_nav_file(f, out, &err)) { std::fprintf(stderr, "aoc_navbuild: %s\n", err.c_str()); return 1; }
    std::ifstream sz(out, std::ios::binary | std::ios::ate);
    const long long file_bytes = static_cast<long long>(sz.tellg());
    std::printf("navmesh: %d tiles (%d empty), %lld polygons, %lld triangles in, tiles %lld bytes, file %lld bytes, %.1f s\n", st.tiles, st.empty_tiles, st.polys, st.input_triangles, st.bytes,
                file_bytes, st.seconds);
    std::printf("navmesh: the largest tile has %d polygons (the limit is %d)%s%d tiles needed a coarser simplification to fit\n", st.max_tile_polys, f.settings.max_polys, ", ", st.retried_tiles);
    if (f.chunked()) {
        NavFile idx;
        if (read_nav_index(out, &idx, &err)) {
            uint64_t most = 0;
            for (const auto& c : idx.chunks) most = std::max<uint64_t>(most, c.tiles);
            std::printf("navmesh: chunked, %zu chunks of up to %u x %u tiles (%llu at most), %zu terrain grids\n", idx.chunks.size(), f.chunk_tiles, f.chunk_tiles, static_cast<unsigned long long>(most), idx.terrain.size());
        } else {
            std::fprintf(stderr, "aoc_navbuild: the file just written does not read back: %s\n", err.c_str());
            return 1;
        }
    }
    return 0;
}

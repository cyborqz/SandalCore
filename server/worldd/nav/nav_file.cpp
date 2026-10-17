// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "nav/nav_file.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>

namespace aoc::worldd::nav {

bool TerrainGrid::height(double x, double y, double* z) const {
    if (nx < 2 || ny < 2) return false;
    const double fx = (x - x0) / step_x, fy = (y - y0) / step_y;
    if (!(fx >= 0 && fy >= 0 && fx <= nx - 1 && fy <= ny - 1)) return false;
    uint32_t ix = static_cast<uint32_t>(fx), iy = static_cast<uint32_t>(fy);
    if (ix > nx - 2) ix = nx - 2;
    if (iy > ny - 2) iy = ny - 2;
    const double tx = fx - ix, ty = fy - iy;
    const double z00 = sample(ix, iy), z10 = sample(ix + 1, iy), z01 = sample(ix, iy + 1), z11 = sample(ix + 1, iy + 1);
    *z = tx >= ty ? z00 + (z10 - z00) * tx + (z11 - z10) * ty : z00 + (z11 - z01) * tx + (z01 - z00) * ty;
    return true;
}

uint64_t fnv1a64(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

namespace {

constexpr size_t kTerrainDirRow = 64, kChunkDirRow = 32;

struct Writer {
    std::vector<uint8_t> out;
    template <typename T>
    void put(T v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
};

struct Reader {
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
// -- CoA . C-Y-B --

// --- the version 2 layout, shared by the in-memory and the streaming writer ---------------------------------------------------------

struct ChunkPlan {
    NavChunkEntry entry;
    std::vector<const NavTileBlob*> tiles;   // (ty, tx) order
};

struct Plan2 {
    std::vector<ChunkPlan> chunks;                    // (cy, cx) order
    std::vector<uint64_t> terrain_offsets;            // of each grid's samples
    uint64_t terrain_dir = 0, chunk_dir = 0, blobs = 0, total = 0;
    uint32_t tiles = 0;
};

uint64_t blob_size(const std::vector<const NavTileBlob*>& tiles) {
    uint64_t n = 0;
    for (const NavTileBlob* t : tiles) n += 12 + t->data.size();
    return n;
}

void put_blob(const std::vector<const NavTileBlob*>& tiles, const std::function<void(const void*, size_t)>& sink) {
    for (const NavTileBlob* t : tiles) {
        const int32_t tx = t->tx, ty = t->ty;
        const uint32_t sz = static_cast<uint32_t>(t->data.size());
        sink(&tx, 4); sink(&ty, 4); sink(&sz, 4);
        sink(t->data.data(), t->data.size());
    }
}

Plan2 plan_v2(const NavFile& f) {
    Plan2 p;
    std::map<ChunkKey, std::vector<const NavTileBlob*>> by_chunk;
    const int32_t ct = static_cast<int32_t>(f.chunk_tiles);
    for (const auto& t : f.tiles) by_chunk[chunk_of_tile(t.tx, t.ty, ct)].push_back(&t);
    for (auto& [key, tiles] : by_chunk) {
        std::sort(tiles.begin(), tiles.end(), [](const NavTileBlob* a, const NavTileBlob* b) { return a->ty != b->ty ? a->ty < b->ty : a->tx < b->tx; });
        ChunkPlan c;
        c.entry.cx = key.x; c.entry.cy = key.y;
        c.entry.tiles = static_cast<uint32_t>(tiles.size());
        c.entry.size = static_cast<uint32_t>(blob_size(tiles));
        // the hash of the blob's bytes
        uint64_t h = 1469598103934665603ull;
        put_blob(tiles, [&](const void* d, size_t n) {
            const auto* b = static_cast<const uint8_t*>(d);
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        });
        c.entry.hash = h;
        c.tiles = std::move(tiles);
        p.chunks.push_back(std::move(c));
        p.tiles += static_cast<uint32_t>(p.chunks.back().tiles.size());
    }
    std::sort(p.chunks.begin(), p.chunks.end(), [](const ChunkPlan& a, const ChunkPlan& b) { return ChunkKey{a.entry.cx, a.entry.cy} < ChunkKey{b.entry.cx, b.entry.cy}; });
    p.terrain_dir = kNavHeaderSize2 + 32ull * f.region.size();
    uint64_t pos = p.terrain_dir + kTerrainDirRow * f.terrain.size();
    for (const auto& g : f.terrain) { p.terrain_offsets.push_back(pos); pos += 2ull * g.raw.size(); }
    p.chunk_dir = pos;
    p.blobs = pos + kChunkDirRow * p.chunks.size();
    uint64_t off = p.blobs;
    for (auto& c : p.chunks) { c.entry.offset = off; off += c.entry.size; }
    p.total = off;
    return p;
}

void emit_v2(const NavFile& f, const std::function<void(const void*, size_t)>& sink) {
    const Plan2 p = plan_v2(f);
    auto put = [&](auto v) { sink(&v, sizeof v); };
    sink(kNavMagic2, 8);
    put(kNavVersion2);
    put(f.center_x); put(f.center_y); put(f.radius); put(f.margin);
    put(f.geometry_hash);
    const NavSettings& s = f.settings;
    put(s.cell_size); put(s.cell_height); put(s.agent_radius); put(s.agent_height); put(s.agent_max_climb); put(s.agent_max_slope);
    put(s.tile_size); put(s.max_tiles); put(s.max_polys);
    put(s.origin[0]); put(s.origin[1]); put(s.origin[2]);
    put(static_cast<uint32_t>(f.chunk_tiles));
    put(f.region_hash);
    put(static_cast<uint32_t>(f.region.size()));
    put(static_cast<uint32_t>(f.terrain.size()));
    put(static_cast<uint32_t>(p.chunks.size()));
    put(p.tiles);
    put(p.terrain_dir); put(p.chunk_dir); put(p.blobs); put(p.total);
    for (const auto& r : f.region) { put(r.x0); put(r.y0); put(r.x1); put(r.y1); }
    for (size_t i = 0; i < f.terrain.size(); ++i) {
        const auto& g = f.terrain[i];
        put(g.x0); put(g.y0); put(g.step_x); put(g.step_y); put(g.z_base); put(g.z_scale);
        put(g.nx); put(g.ny);
        put(p.terrain_offsets[i]);
    }
    for (const auto& g : f.terrain) sink(g.raw.data(), g.raw.size() * 2);
    for (const auto& c : p.chunks) {
        put(c.entry.cx); put(c.entry.cy); put(c.entry.tiles); put(c.entry.size); put(c.entry.offset); put(c.entry.hash);
    }
    for (const auto& c : p.chunks) put_blob(c.tiles, sink);
}
// C Y B and C.O.A

void emit_v1(const NavFile& f, const std::function<void(const void*, size_t)>& sink) {
    auto put = [&](auto v) { sink(&v, sizeof v); };
    sink(kNavMagic, 8);
    put(kNavVersion);
    put(f.center_x); put(f.center_y); put(f.radius); put(f.margin);
    put(f.geometry_hash);
    const NavSettings& s = f.settings;
    put(s.cell_size); put(s.cell_height); put(s.agent_radius); put(s.agent_height);
    put(s.agent_max_climb); put(s.agent_max_slope);
    put(s.tile_size); put(s.max_tiles); put(s.max_polys);
    put(s.origin[0]); put(s.origin[1]); put(s.origin[2]);
    put(static_cast<uint32_t>(f.terrain.size()));
    put(static_cast<uint32_t>(f.tiles.size()));
    for (const auto& g : f.terrain) {
        put(g.x0); put(g.y0); put(g.step_x); put(g.step_y);
        put(g.z_base); put(g.z_scale);
        put(g.nx); put(g.ny);
        sink(g.raw.data(), g.raw.size() * 2);
    }
    for (const auto& t : f.tiles) {
        put(t.tx); put(t.ty);
        put(static_cast<uint32_t>(t.data.size()));
        sink(t.data.data(), t.data.size());
    }
}

/// The header's common part, read from a reader positioned after magic and version.
bool read_common(Reader& r, NavFile* f) {
    f->center_x = r.get<double>(); f->center_y = r.get<double>(); f->radius = r.get<double>(); f->margin = r.get<double>();
    f->geometry_hash = r.get<uint64_t>();
    NavSettings& s = f->settings;
    s.cell_size = r.get<float>(); s.cell_height = r.get<float>(); s.agent_radius = r.get<float>(); s.agent_height = r.get<float>();
    s.agent_max_climb = r.get<float>(); s.agent_max_slope = r.get<float>();
    s.tile_size = r.get<float>(); s.max_tiles = r.get<int32_t>(); s.max_polys = r.get<int32_t>();
    s.origin[0] = r.get<float>(); s.origin[1] = r.get<float>(); s.origin[2] = r.get<float>();
    return r.ok;
}

bool settings_sane(const NavFile& f) {
    return f.radius > 0 && f.settings.tile_size > 0 && f.settings.max_tiles > 0 && f.settings.max_polys > 0;
}

}  // namespace

std::vector<uint8_t> serialize_nav_file(const NavFile& f) {
    Writer w;
    auto sink = [&](const void* p, size_t n) { w.bytes(p, n); };
    if (f.chunked()) emit_v2(f, sink); else emit_v1(f, sink);
    return std::move(w.out);
}

bool write_nav_file(const NavFile& f, const std::string& path, std::string* err) {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) { if (err) *err = "cannot write " + path; return false; }
    auto sink = [&](const void* p, size_t n) { if (n) o.write(static_cast<const char*>(p), static_cast<std::streamsize>(n)); };
    if (f.chunked()) emit_v2(f, sink); else emit_v1(f, sink);
    o.flush();
    if (!o) { if (err) *err = "cannot write " + path; return false; }
    return true;
}

static bool parse_v1(Reader& r, NavFile* out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    NavFile f;
    if (!read_common(r, &f)) return fail("area_nav.bin is cut short (header)");
    const uint32_t nt = r.get<uint32_t>(), nn = r.get<uint32_t>();
    if (!r.ok) return fail("area_nav.bin is cut short (header)");
    if (!settings_sane(f) || nt > 100000 || nn > 1000000) return fail("area_nav.bin header values are out of range");
    f.terrain.resize(nt);
    for (auto& g : f.terrain) {
        g.x0 = r.get<double>(); g.y0 = r.get<double>(); g.step_x = r.get<double>(); g.step_y = r.get<double>();
        g.z_base = r.get<double>(); g.z_scale = r.get<double>();
        g.nx = r.get<uint32_t>(); g.ny = r.get<uint32_t>();
        if (!r.ok) return fail("area_nav.bin is cut short (terrain)");
        if (g.nx < 2 || g.ny < 2 || g.nx > 4096 || g.ny > 4096 || !(g.step_x > 0) || !(g.step_y > 0)) return fail("area_nav.bin terrain grid is out of range");
        const size_t n = static_cast<size_t>(g.nx) * g.ny;
        if (r.pos + n * 2 > r.size) return fail("area_nav.bin is cut short (terrain heights)");
        g.raw.resize(n);
        r.bytes(g.raw.data(), n * 2);
    }
    f.tiles.resize(nn);
    for (auto& t : f.tiles) {
        t.tx = r.get<int32_t>(); t.ty = r.get<int32_t>();
        const uint32_t sz = r.get<uint32_t>();
        if (!r.ok || r.pos + sz > r.size) return fail("area_nav.bin is cut short (tiles)");
        t.data.resize(sz);
        r.bytes(t.data.data(), sz);
    }
    if (r.pos != r.size) return fail("area_nav.bin has trailing bytes");
    *out = std::move(f);
    return true;
}

/// Version 2: the index from a buffer that starts at the header and holds at least the terrain and the directories (the whole file, or what read_nav_index reads).
static bool parse_v2_index(const uint8_t* data, size_t size, NavFile* out, uint64_t* file_size, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    Reader r{data, size};
    char magic[8] = {};
    if (!r.bytes(magic, 8) || std::memcmp(magic, kNavMagic2, 8) != 0) return fail("not a chunked area_nav.bin (bad magic)");
    if (r.get<uint32_t>() != kNavVersion2) return fail("unsupported area_nav.bin version");
    NavFile f;
    if (!read_common(r, &f)) return fail("area_nav.bin is cut short (header)");
    f.chunk_tiles = r.get<uint32_t>();
    f.region_hash = r.get<uint64_t>();
    const uint32_t nr = r.get<uint32_t>(), ng = r.get<uint32_t>(), nc = r.get<uint32_t>(), nt = r.get<uint32_t>();
    const uint64_t terrain_dir = r.get<uint64_t>(), chunk_dir = r.get<uint64_t>(), blobs = r.get<uint64_t>(), total = r.get<uint64_t>();
    if (!r.ok || r.pos != kNavHeaderSize2) return fail("area_nav.bin is cut short (header)");
    if (!settings_sane(f) || f.chunk_tiles == 0 || f.chunk_tiles > 64 || nr > 100000 || ng > 100000 || nc > 1000000 || nt > 10000000) return fail("area_nav.bin header values are out of range");
    if (terrain_dir != kNavHeaderSize2 + 32ull * nr || chunk_dir < terrain_dir + kTerrainDirRow * ng ||
        chunk_dir > blobs || blobs > size || blobs - chunk_dir != kChunkDirRow * nc || total < blobs) return fail("area_nav.bin offsets are inconsistent");
    const uint64_t terrain_samples = terrain_dir + kTerrainDirRow * ng;
    if (file_size) *file_size = total;
    f.region.resize(nr);
    for (auto& q : f.region) { q.x0 = r.get<double>(); q.y0 = r.get<double>(); q.x1 = r.get<double>(); q.y1 = r.get<double>(); }
    if (!r.ok) return fail("area_nav.bin is cut short (region)");
    f.terrain.resize(ng);
    std::vector<uint64_t> offsets(ng);
    for (uint32_t i = 0; i < ng; ++i) {
        auto& g = f.terrain[i];
        g.x0 = r.get<double>(); g.y0 = r.get<double>(); g.step_x = r.get<double>(); g.step_y = r.get<double>();
        g.z_base = r.get<double>(); g.z_scale = r.get<double>();
        g.nx = r.get<uint32_t>(); g.ny = r.get<uint32_t>();
        offsets[i] = r.get<uint64_t>();
        if (!r.ok) return fail("area_nav.bin is cut short (terrain directory)");
        if (g.nx < 2 || g.ny < 2 || g.nx > 4096 || g.ny > 4096 || !(g.step_x > 0) || !(g.step_y > 0)) return fail("area_nav.bin terrain grid is out of range");
        if (offsets[i] < terrain_samples || offsets[i] > chunk_dir || 2ull * g.nx * g.ny > chunk_dir - offsets[i]) return fail("area_nav.bin terrain samples are out of place");
    }
    for (uint32_t i = 0; i < ng; ++i) {
        auto& g = f.terrain[i];
        const size_t n = static_cast<size_t>(g.nx) * g.ny;
        if (offsets[i] > size || n * 2 > size - offsets[i]) return fail("area_nav.bin is cut short (terrain heights)");
        g.raw.resize(n);
        std::memcpy(g.raw.data(), data + offsets[i], n * 2);
    }
    if (chunk_dir > size || kChunkDirRow * nc > size - chunk_dir) return fail("area_nav.bin is cut short (chunk directory)");
    Reader cr{data, size, static_cast<size_t>(chunk_dir)};
    f.chunks.resize(nc);
    uint64_t tiles = 0;
    for (auto& c : f.chunks) {
        c.cx = cr.get<int32_t>(); c.cy = cr.get<int32_t>(); c.tiles = cr.get<uint32_t>(); c.size = cr.get<uint32_t>(); c.offset = cr.get<uint64_t>(); c.hash = cr.get<uint64_t>();
        if (c.offset < blobs || c.offset > total || c.size > total - c.offset || c.tiles == 0 || c.size < 12ull * c.tiles) return fail("area_nav.bin has a chunk out of place");
        // (review A M6, B M3) a row cannot hold more tiles than its chunk has room for, and a chunk is listed once, in the directory's order
        if (c.tiles > static_cast<uint64_t>(f.chunk_tiles) * f.chunk_tiles) return fail("area_nav.bin has a chunk with more tiles than a chunk holds");
        if (&c != &f.chunks.front() && !(ChunkKey{(&c - 1)->cx, (&c - 1)->cy} < ChunkKey{c.cx, c.cy})) return fail("area_nav.bin's chunk directory is not sorted or lists a chunk twice");
        tiles += c.tiles;
    }
    if (tiles != nt) return fail("area_nav.bin: the chunks' tiles do not add up to the header's");
    f.tile_count_v2 = nt;
    *out = std::move(f);
    return true;
}
// (CoA) (C_Y_B)

/// One chunk's tiles from its blob bytes.
static bool split_chunk(const uint8_t* blob, size_t size, const NavChunkEntry& e, std::vector<NavTileBlob>* out, std::string* err, uint32_t chunk_tiles) {
    auto fail = [&](const std::string& m) { if (err) *err = "chunk (" + std::to_string(e.cx) + ", " + std::to_string(e.cy) + "): " + m; return false; };
    if (size != e.size) return fail("its size differs from the directory's");
    if (fnv1a64(blob, size) != e.hash) return fail("its hash differs from the directory's (the file is damaged)");
    Reader r{blob, size};
    out->clear();
    out->reserve(e.tiles);
    for (uint32_t i = 0; i < e.tiles; ++i) {
        NavTileBlob t;
        t.tx = r.get<int32_t>(); t.ty = r.get<int32_t>();
        const uint32_t sz = r.get<uint32_t>();
        if (!r.ok || r.pos + sz > r.size) return fail("a tile is cut short");
        t.data.resize(sz);
        r.bytes(t.data.data(), sz);
        // (review B M3) a tile belongs to its chunk, and the tiles are in (ty, tx) order without repeats
        if (chunk_tiles != 0) {
            if (chunk_of_tile(t.tx, t.ty, static_cast<int32_t>(chunk_tiles)) != ChunkKey{e.cx, e.cy}) return fail("a tile lies in another chunk");
            if (!out->empty() && !(std::pair<int32_t, int32_t>{out->back().ty, out->back().tx} < std::pair<int32_t, int32_t>{t.ty, t.tx})) return fail("its tiles are not sorted or repeat");
        }
        out->push_back(std::move(t));
    }
    if (r.pos != r.size) return fail("trailing bytes after its tiles");
    return true;
}

bool parse_nav_file(const uint8_t* data, size_t size, NavFile* out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    Reader r{data, size};
    char magic[8] = {};
    if (!r.bytes(magic, 8)) return fail("not an area_nav.bin (bad magic)");
    if (std::memcmp(magic, kNavMagic2, 8) == 0) {
        NavFile f;
        uint64_t total = 0;
        if (!parse_v2_index(data, size, &f, &total, err)) return false;
        if (total != size) return fail("area_nav.bin has a different size than its header says");
        for (const auto& c : f.chunks) {
            std::vector<NavTileBlob> tiles;
            if (!split_chunk(data + c.offset, c.size, c, &tiles, err, f.chunk_tiles)) return false;
            for (auto& t : tiles) f.tiles.push_back(std::move(t));
        }
        *out = std::move(f);
        return true;
    }
    if (std::memcmp(magic, kNavMagic, 8) != 0) return fail("not an area_nav.bin (bad magic)");
    if (r.get<uint32_t>() != kNavVersion) return fail("unsupported area_nav.bin version");
    return parse_v1(r, out, err);
}

int nav_file_version(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    char magic[8] = {};
    uint32_t v = 0;
    in.read(magic, 8);
    in.read(reinterpret_cast<char*>(&v), 4);
    if (!in) return 0;
    if (std::memcmp(magic, kNavMagic, 8) == 0 && v == kNavVersion) return 1;
    if (std::memcmp(magic, kNavMagic2, 8) == 0 && v == kNavVersion2) return 2;
    return 0;
}

bool read_nav_index(const std::string& path, NavFile* out, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (err) *err = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const auto len = static_cast<uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> head(kNavHeaderSize2);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    if (!in) { if (err) *err = path + " is cut short (header)"; return false; }
    if (std::memcmp(head.data(), kNavMagic2, 8) != 0) { if (err) *err = path + " is not a chunked area_nav.bin"; return false; }
    uint64_t terrain_dir = 0, chunk_dir = 0, blobs = 0, total = 0;
    std::memcpy(&terrain_dir, head.data() + 128, 8); std::memcpy(&chunk_dir, head.data() + 136, 8); std::memcpy(&blobs, head.data() + 144, 8); std::memcpy(&total, head.data() + 152, 8);
    if (total != len || blobs > len || chunk_dir > blobs || terrain_dir < kNavHeaderSize2 || terrain_dir > chunk_dir) { if (err) *err = path + " has inconsistent offsets or was cut short"; return false; }
    // everything before the blobs: the region, the terrain and the directories
    std::vector<uint8_t> index(static_cast<size_t>(blobs));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(index.size()));
    if (!in) { if (err) *err = path + " is cut short (index)"; return false; }
    NavFile f;
    uint64_t t2 = 0;
    // the chunk rows' offsets and sizes are checked against `total` by the parser
    if (!parse_v2_index(index.data(), index.size(), &f, &t2, err)) return false;
    *out = std::move(f);
    return true;
}

bool read_nav_chunk(std::ifstream& in, const NavChunkEntry& e, std::vector<NavTileBlob>* out, std::string* err, uint32_t chunk_tiles) {
    std::vector<uint8_t> blob(e.size);
    in.clear();
    in.seekg(static_cast<std::streamoff>(e.offset), std::ios::beg);
    in.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    if (!in) { if (err) *err = "chunk (" + std::to_string(e.cx) + ", " + std::to_string(e.cy) + ") cannot be read"; return false; }
    return split_chunk(blob.data(), blob.size(), e, out, err, chunk_tiles);
}

bool read_nav_file(const std::string& path, NavFile* out, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (err) *err = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const auto len = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    if (len > 0) in.read(reinterpret_cast<char*>(buf.data()), len);
    if (!in) { if (err) *err = "cannot read " + path; return false; }
    return parse_nav_file(buf.data(), buf.size(), out, err);
}

}  // namespace aoc::worldd::nav

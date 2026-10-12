// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  nav/area_nav.h  (plan E1, stage 2; the chunk window: G1 stage 1)
//
//  The area's ground at run time: dist/content/area_nav.bin (nav/nav_file.h) loaded into Detour, in world centimetres.
//
//    ground_height   the surface under a point: a navmesh floor (a bridge, a building's floor, the terrain's own polygons) nearest to the
//                    height asked, else the landscape's heightfield;  the terrain is exact (the client's own samples), a floor is the
//                    navmesh's detail surface (within about 10 cm)
//    find_path       a straight path along walkable ground (Detour's A*), partial when the end is not reachable
//    raycast         a line along the walkable surface: how far it goes before it leaves the navmesh (a wall, a ledge)
//    random_point    a walkable point within a radius of another
//
//  A single thread uses an AreaNav (the net thread: Detour's query object is not shared).  Nothing here allocates per query beyond the
//  returned vectors; the scratch buffers are members.
//
//  THE WINDOW (G1 stage 1).  A flat file (version 1: E1's 2,488 tiles) is loaded whole.  A chunked file (version 2: the Riverlands' ~31,000 tiles, 161 MB of terrain and
//  ~1 GB of tiles on disk) is not: Detour holds at most 4,096 tiles (its polygon references have 10 bits of salt), and the net thread must not wait for a disk.  So
//    * the terrain (the heightfield: a height query must never wait) and the chunk directory are resident from the start; no tile is;
//    * the owner says where its players stand (`set_interest`): the chunks within 2 of any player's chunk are loaded, a chunk stays until it is more than 3 away from every
//      player (one more chunk of hysteresis: a player on a border does not make the same chunk come and go), and never more than the mesh's 4,096 tiles are resident, loading
//      or waiting.  When the players' windows do not all fit, the room is shared by distance (review A I1, "water-filling"): a chunk nearer to a player than a resident chunk is
//      to its own player takes that chunk's place, so every player keeps at least its own chunk and the outer rings of every window give way together (the mesh holds about 70
//      chunks: about 70 players are covered; for more, more meshes);
//    * `set_interest` does nothing when the players' chunks are the same as at the last call and nothing is waiting for room (review A I2), so it is cheap to call at every tick;
//    * a worker thread reads a chunk's tiles off the disk (and checks them: hash, Detour's header); the net thread adds them with `pump` (Detour's addTile / removeTile), a
//      few tiles at a time under a time budget (a chunk takes about a millisecond);
//    * Detour joins a new tile to its loaded neighbours: a path and a ray cross a chunk border that both sides have loaded, and an unloaded border is a wall;
//    * a poly reference held by the caller across a pump may be stale (a removed tile's salt changes): AreaNav's queries never keep one between calls, and `Mover` keeps world
//      points, so the answer to "my ref is stale" is always "ask again".
//  `ready_at` says whether the ground under a point is there to be asked (its chunk is resident, or has no tiles); an NPC whose chunk is not ready is dormant and is grounded
//  when `pump` reports the chunk (npc/npc_world.h).
// ============================================================================
#pragma once

#include "nav/nav_file.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class dtQueryFilter;

namespace aoc::worldd::nav {

struct Vec3 {
    double x = 0, y = 0, z = 0;   // world cm
};
// (C | O | A : C | Y | B)

struct PathResult {
    bool found = false;       // a path of at least the start point exists
    bool complete = false;    // it reaches the end (otherwise: as close as the navmesh gets)
    std::vector<Vec3> points; // start .. end (or the closest reachable point)
    double length = 0;        // cm, along the points
    int iterations = 0;       // A* node expansions a bounded search used (found or not); 0 for an unbounded one
    /// Why there is no complete path (review A I5: nothing counted the searches that gave up against the ones that had no way): the start or the end is off the loaded navmesh,
    /// the bounded search ran out of expansions, or the end cannot be reached from the start (a lake, a cliff, a chunk that is not loaded: an unloaded border is a wall).
    /// TooLong is the caller's (a path longer than it asked for).
    enum class Fail : uint8_t { None, NoStartPoly, NoEndPoly, OutOfIterations, Unreachable, TooLong };
    Fail fail = Fail::None;
};

/// What the window did (tests and the cost lines of the README read it).
struct WindowStats {
    size_t resident_tiles = 0, resident_chunks = 0, committed_tiles = 0;   // committed: resident, being read, or waiting to be added
    size_t peak_resident_tiles = 0, peak_committed_tiles = 0;
    uint64_t chunk_loads = 0, chunk_unloads = 0, load_failures = 0, deferred_by_cap = 0, evicted_for_room = 0;
    uint64_t plans = 0, plans_skipped = 0;   // set_interest calls that planned the window / found the players' chunks unchanged and nothing waiting
    uint64_t retries = 0;                    // chunks asked for again after a failed read (review A I3)
    double max_chunk_add_ms = 0;      // the net thread's time to add all the tiles of one chunk (summed over its addTile calls)
    double max_chunk_remove_ms = 0;
    double max_pump_ms = 0;           // the longest single pump() call
    double max_tile_add_ms = 0, max_tile_remove_ms = 0;   // the longest single addTile / removeTile (a pump cannot be shorter than one tile)
    double max_chunk_read_ms = 0;     // the worker's: read and check one chunk
    double total_add_ms = 0;
};

class AreaNav {
public:
    static constexpr int kLoadRadius = 2;    // chunks around a player that are loaded
    static constexpr int kKeepRadius = 3;    // chunks around every player that stay (the hysteresis)

    ~AreaNav();
    AreaNav(const AreaNav&) = delete;
    AreaNav& operator=(const AreaNav&) = delete;

    /// Loads a file (flat: whole; chunked: the window, empty); null with `err` on failure.
    static std::unique_ptr<AreaNav> load(const std::string& path, std::string* err);
    /// From a file held in memory: every tile is added (a chunked file in memory is loaded whole: it must not hold more tiles than the mesh may).
    static std::unique_ptr<AreaNav> from_file(NavFile file, std::string* err);

    double center_x() const { return file_.center_x; }
    double center_y() const { return file_.center_y; }
    double radius() const { return file_.radius; }
    const NavFile& file() const { return file_; }
    /// Tiles in Detour now (a flat file: all of them).
    size_t tile_count() const { return tile_count_; }
    /// Tiles the file holds (a chunked file: all chunks').
    size_t total_tiles() const { return file_.chunked() ? static_cast<size_t>(file_.tile_count_v2 ? file_.tile_count_v2 : file_.tiles.size()) : file_.tiles.size(); }
    size_t terrain_grids() const { return file_.terrain.size(); }
    size_t polygon_count() const { return polygons_; }

    /// The landscape's height at (x, y); false outside the extracted terrain.
    bool terrain_height(double x, double y, double* z) const;

    /// The walkable surface under (x, y) nearest to `z_hint` within [z_hint - below, z_hint + above] (cm); the terrain when it is the surface
    /// (a navmesh floor within 40 cm of it) or when no floor is there.  False when neither exists.
    bool ground_height(double x, double y, double z_hint, double* z, double below = 400, double above = 150) const;

    /// The navmesh point nearest to p within a box of half extents (horizontal, vertical); false when there is none.
    bool nearest_point(const Vec3& p, double half_h, double half_v, Vec3* out) const;

    /// True when p stands on the navmesh within `tol_h` horizontally and `tol_v` vertically.
    bool on_navmesh(const Vec3& p, double tol_h = 40, double tol_v = 80) const;

    /// `max_iterations` > 0 bounds the A* search (Detour's sliced find-path: that many node expansions, then it gives up with no path): a walk that cannot be found is
    /// then cheap on a navmesh of 4,096 tiles instead of exploring all of it (G1 stage 1: a failing search of 16,384 nodes took milliseconds).  0: unbounded.
    PathResult find_path(const Vec3& from, const Vec3& to, double snap_h = 150, double snap_v = 300, int max_iterations = 0) const;

    /// A ray along the navmesh from `from` towards `to`: true when it reaches `to` without leaving the walkable surface; otherwise `hit` is
    /// where it left (a boundary edge) when given.
    bool raycast(const Vec3& from, const Vec3& to, Vec3* hit = nullptr) const;
    /// Line of sight along the ground (raycast without the hit).
    bool line_of_sight(const Vec3& from, const Vec3& to) const { return raycast(from, to, nullptr); }

    /// A walkable point within `radius` of `center`: the point of the disk that `u1` (the angle) and `u2` (the distance, uniform over the area)
    /// pick, moved onto the navmesh (the nearest point within 1.5 m), false when there is none or it lands outside the circle.  It is not
    /// proven to be connected to `center`: the caller asks find_path.  Numbers in [0, 1).
    bool random_point(const Vec3& center, double radius, double u1, double u2, Vec3* out) const;

    // ---- the region ---------------------------------------------------------------------------------------------------------------

    /// True when the file was cut for a footprint (a chunked file: the Riverlands' rectangles).
    bool has_region() const { return !file_.region.empty(); }
    /// A point inside the footprint (the rectangles; a flat file: the circle it was cut for).  Beyond it there is no navmesh and nothing spawns.
    bool in_region(double x, double y) const;

    // ---- the window ---------------------------------------------------------------------------------------------------------------

    /// A chunked file with a window (a flat file, or a file held in memory, is whole).
    bool windowed() const { return windowed_; }
    /// The chunk a world point is in.
    ChunkKey chunk_at(double x, double y) const;
    /// True when the chunk's tiles are in Detour (or the chunk has none: nothing to wait for).
    bool chunk_resident(ChunkKey k) const;
    /// True when the ground at a point can be asked: its chunk is resident or empty (always true without a window).
    bool ready_at(double x, double y) const { return !windowed_ || chunk_resident(chunk_at(x, y)); }
    /// The chunks that have tiles.
    size_t chunk_count() const { return slots_.size(); }

    /// Where the players stand (world x, y): the chunks within kLoadRadius of any of them are requested (nearest first, within the tile cap; a chunk that is nearer to a player than
    /// a resident one is to its own takes its place); the chunks more than kKeepRadius from all of them are queued for unloading.  No point (or no finite one): nothing changes (an empty
    /// server keeps its ground).  Cheap: call it every world tick; it plans only when a player's chunk changed or some wanted chunk is still waiting for room or for a retry.
    void set_interest(const std::vector<std::pair<double, double>>& points);
    /// How long after a failed read a chunk is asked for again (seconds, default 30; at most kMaxLoadAttempts tries in all: a damaged file is not read for ever).
    void set_retry_delay(double seconds) { retry_delay_ = seconds; }
    static constexpr int kMaxLoadAttempts = 4;
    /// The messages of the chunk reads and adds that failed since the last call (the owner logs them).
    std::vector<std::string> take_load_errors();
    /// The net thread's share, within `budget_ms`: takes the chunks the worker has read, adds their tiles, removes the queued ones' tiles.  At least one tile of work is done when there
    /// is any.  Returns the chunks that became resident in this call.
    std::vector<ChunkKey> pump(double budget_ms = 4.0);
    /// Blocking: loads the chunks within `radius` chunks of a point and waits for them (start-up and tests; the worker still reads them).  False on a timeout or a damaged chunk.
    bool load_around(double x, double y, int radius, double timeout_seconds = 30);
    /// The same for several points at once (the chunks within `radius` of any of them; the window of the players it replaces is forgotten).
    bool load_around(const std::vector<std::pair<double, double>>& points, int radius, double timeout_seconds = 60);
    /// True while a requested chunk has not been added yet.
    bool loading() const;

    WindowStats window_stats() const;

private:
    AreaNav() = default;

    struct LoadedTile {
        int32_t tx = 0, ty = 0;
        unsigned char* data = nullptr;   // dtAlloc'ed: Detour takes it over (DT_TILE_FREE_DATA)
        int size = 0;
    };
    struct LoadedChunk {
        uint32_t slot = 0;
        std::vector<LoadedTile> tiles;
        std::string error;
        double read_ms = 0;
    };
    enum class State : uint8_t { Absent, Requested, Arrived, Resident, Unloading };
    struct Slot {
        NavChunkEntry entry;
        State state = State::Absent;
        bool want = false, keep = false, failed = false, unload_queued = false, evict = false;   // evict: dropped to make room for a nearer chunk although a player is within the keep radius
        int attempts = 0;                                    // reads tried (a failed one is tried again after retry_delay_, up to kMaxLoadAttempts)
        std::chrono::steady_clock::time_point retry_at{};    // when a failed chunk may be asked for again
        std::vector<LoadedTile> tiles;                       // Arrived: waiting to be added; Resident/Unloading: their coordinates (data null)
        uint32_t cursor = 0;                                 // tiles added (Arrived) or removed (Unloading) so far
        double work_ms = 0;                                  // addTile / removeTile time of this chunk so far
    };

    void to_local(const Vec3& p, float* out) const;
    Vec3 to_world(const float* p) const;
    bool find_poly(const Vec3& p, double half_h, double half_v, uint32_t* ref, float* nearest) const;
    bool add_tile(LoadedTile* t, std::string* err);
    void remove_tile(const LoadedTile& t);
    void build_terrain_index();
    std::vector<ChunkKey> centers_of(const std::vector<std::pair<double, double>>& points) const;
    void plan_window(const std::vector<ChunkKey>& centers, int load_radius, int keep_radius);
    void worker_main();
    static uint64_t pack(ChunkKey k) { return (static_cast<uint64_t>(static_cast<uint32_t>(k.x)) << 32) | static_cast<uint32_t>(k.y); }

    NavFile file_;
    dtNavMesh* mesh_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
    dtQueryFilter* filter_ = nullptr;
    size_t polygons_ = 0;
    size_t tile_count_ = 0;

    // the terrain's lookup: 25,600 cm cells -> the grids that overlap them (in file order: the first one that holds a point answers, as a scan of the file would)
    std::unordered_map<uint64_t, std::vector<uint32_t>> terrain_cells_;

    // the window
    bool windowed_ = false;
    std::string path_;
    std::vector<Slot> slots_;                                // one per directory row
    std::unordered_map<uint64_t, uint32_t> slot_by_key_;
    std::deque<uint32_t> arrived_, unload_queue_;
    size_t resident_tiles_ = 0, committed_tiles_ = 0, resident_chunks_ = 0;
    WindowStats stats_;
    std::vector<ChunkKey> last_centers_;                     // the players' chunks at the last plan (sorted, unique)
    bool replan_ = false;                                    // some wanted chunk was deferred (no room) or waits for a retry: plan again even if the players did not move
    double retry_delay_ = 30.0;
    std::vector<std::string> load_errors_;
// (Cyb ~ C | O | A)

    std::thread worker_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<uint32_t> requests_;
    std::deque<LoadedChunk> done_;
    bool stop_ = false;
    double worker_max_read_ms_ = 0;
};

}  // namespace aoc::worldd::nav

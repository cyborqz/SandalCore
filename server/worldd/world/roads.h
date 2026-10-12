// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/roads.h  (the mounts milestone, stage 3)
//
//  The roads of the area: what a mount rides faster on.  A road is a polyline of the world's cm with a tier (0 to 4: Road_T0..T4_Speed_Mount, +10 % to
//  +30 % of the mount's speed) and a half width; a mount whose position lies within a road's half width of its line is on that road (the highest tier
//  wins where two roads meet).  The file is `dist/content/roads.tsv`: tools/mapc (`mapc roads`) reads the BP_RoadActor_C splines of the cooked map into
//  roads_raw.tsv and `contentc roads` resolves each road's tier and width at a node level (stage 4):
//
//      # roads v1: N roads for a mount at node level L (Name) ...      (comment lines; the node level the tiers were resolved at)
//      road <TAB> tier <TAB> half_width_cm <TAB> x,y <TAB> x,y <TAB> ...      one polyline per line, its points in order
//
//  Nodes 1, stage 3b: roads by node level.  A road's worth depends on the node level of the zone of influence that owns it (RoadPresetRecord.RoadAppearancesIds[node level]): `contentc roads
//  --levels` writes dist/content/roads_levels.tsv, one line per road with its owner zone and its tier and half width at each of the seven node levels,
//
//      roadn <TAB> zoi_guid <TAB> t0,t1,..,t6 <TAB> h0,h1,..,h6 <TAB> x,y <TAB> x,y ...      (tier -1 and half width 0: the road gives a mount nothing at that level)
//
//  and `apply_levels` sets every such road to the level its zone's node has now (a zone with no node, a starting area, takes the world's default level): a node's level-up changes the
//  tiers of its roads at once.  The client redraws the roads itself when the node's level changes (the GameState's node maps); only the mounts' speed is the server's.
//
//  Without the file (or with it empty) there are no roads: a mount rides at its own speed.  The map is read once at start and held read-only: sessions
//  share it.  A grid of 2,000 cm cells holds the segments, each in every cell its widened box touches; a query reads only the cell it falls in.
// ============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::world {

class RoadMap {
public:
    static constexpr int kLevels = 7;   // ENodeLevel: Wilderness .. Metropolis
// Cyb captures, C O A code

    struct Polyline {
        int                 tier = 0;
        double              half_width_cm = 0;
        std::vector<double> xy;   // x0, y0, x1, y1, ...
        // Nodes 1, stage 3b: a road whose worth follows its owner zone's node level.  `by_level` roads carry a tier (-1: none) and a half width for each level; tier and
        // half_width_cm above are then the worth at the level the road is at now.
        bool                          by_level = false;
        int64_t                       zoi = 0;
        std::array<int, kLevels>      tiers{};
        std::array<double, kLevels>   halves{};
    };

    /// Reads the file (roads.tsv's `road` lines, roads_levels.tsv's `roadn` lines). False, with `error`, when it does not open or a line is not a road. A file with no road is fine
    /// (no roads).  A by-level road starts at `initial_level`.
    bool load(const std::string& path, std::string& error, int initial_level = 0);
    /// Adds a road (the tests'); false for a tier outside 0..4, a width that is not positive or fewer than two points.  A by-level road (Polyline::by_level) needs a tier of -1..4 and a
    /// positive half width at every level that has a tier, and starts at `initial_level`.
    bool add(Polyline road, int initial_level = 0);

    /// Sets every by-level road to the node level `level_of_zoi(its zone)` says (a negative answer: `default_level`).  Returns the roads whose tier or width changed.
    size_t apply_levels(const std::function<int(int64_t zoi)>& level_of_zoi, int default_level);
    /// Roads that follow a node level, and the level and tier road `i` (the order of the file) is at now (-1: none; a road that is not by-level has level -1).
    size_t  level_roads() const { return by_level_; }
    int     level_of_road(size_t i) const { return i < level_.size() ? level_[i] : -1; }
    int     tier_of_road(size_t i) const;
    int64_t zoi_of_road(size_t i) const { return i < roads_.size() ? roads_[i].zoi : 0; }

    /// The tier of the best road under (x, y): 0 to 4, or -1 for none.
    int tier_at(double x, double y) const;

    size_t roads() const { return roads_.size(); }
    /// The node level the file's tiers were resolved at (its header says; -1: it does not).  worldd warns when it is not the level the world runs at.
    int    node_level() const { return node_level_; }
    size_t segments() const { return segments_; }
    bool   empty() const { return roads_.empty(); }
    /// The horizontal extent of what is loaded (cm): min x, min y, max x, max y; all 0 for none.
    struct Bounds {
        double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    };
    Bounds bounds() const { return bounds_; }

    static constexpr double kCellCm = 2000.0;

private:
    struct Seg {
        float  x0, y0, x1, y1;
        float  half_width;
        int8_t tier;
    };
    static int64_t key(int32_t cx, int32_t cy) { return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cy); }
    void           index(const Polyline& road, size_t road_index);
    void           set_level(size_t road, int level);

    std::vector<Polyline>                           roads_;
    std::vector<uint32_t>                           first_seg_, seg_count_;   // the segments of each road (consecutive in segs_)
    std::vector<int>                                level_;                   // each road's level now (-1: not by-level)
    size_t                                          by_level_ = 0;
    std::vector<Seg>                                segs_;
    std::unordered_map<int64_t, std::vector<uint32_t>> grid_;
    size_t                                          segments_ = 0;
    int                                             node_level_ = -1;
    Bounds                                          bounds_;
};

}  // namespace aoc::worldd::world

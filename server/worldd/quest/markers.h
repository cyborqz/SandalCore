// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  quest/markers.h  (quests milestone, "map markers")
//
//  Where the map pins of an objective go.  The client draws a pin or an area for every objective that is active and has at least one place in the marker array of its quest
//  component (quest_wire.h); this file makes those places from the data, at the moment an objective becomes active.  Header-only and free of the network: the positions of the
//  people and the creatures come in through a PointSource.
//
//  The places of an objective, in the order of the rules:
//    * the record switches its base markers off (an override): the record's own list, verbatim; an empty list is no pin at all;
//    * GoTo: the record's list of places, verbatim (the ones that complete the objective are the same places);
//    * Dialogue and TurnIn: one pin (a SpecificLocations of radius 5,000 cm) for every place of every NPC the objective names;
//    * Interact, Collect, TurnIn, EventNotification: the same pin for every place of every population instance the objective tracks;
//    * Kill: the spawn points of the creatures it names, clustered: a cluster is an area (RadiusArea) whose centre is the points' centroid and whose radius is the largest
//      horizontal distance from the centre to a point (at least the record's MinRadius); a point with no neighbour is a pin;
//    * anything else (Custom, UseAbility, SubQuest ...): no place.
//  A drawn objective is never redrawn by the client, so the places are final when they are made.
// ============================================================================
#pragma once

#include "content/content_db.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aoc::worldd::quest {

/// One place of an objective's marker (the client's FObjectiveIndicatorCluster).
struct MarkerLocation {
    enum class Display : uint8_t { Specific = 0, Area = 1, Hidden = 2 };   // EMarkerDisplayType: an icon, a circle and an icon, nothing
    Display display = Display::Specific;
    double  x = 0, y = 0, z = 0;       // cm
    double  radius = 0;                // cm
    bool    completes = true;          // the place completes a GoTo objective (the client draws it the same)
    int64_t id = 0;                    // QuestLocationId
};

struct MarkerPoint {
    double x = 0, y = 0, z = 0;
};

/// Where the world's people and creatures stand.  The implementation reads the live world; the lookups may be slow the first time (an index is built) and cheap after.
class PointSource {
public:
    virtual ~PointSource() = default;
    /// The places the NPC with population instance `instance` stands at, one per placement, appended to `out`.
    virtual void npc_places(int64_t instance, std::vector<MarkerPoint>& out) = 0;
    /// The spawn points of the creatures of the population `instances` and `sets` (a creature counts when it is one of the instances or stands in one of the sets), appended to `out`.
    virtual void creature_places(const std::vector<int64_t>& instances, const std::vector<int64_t>& sets, std::vector<MarkerPoint>& out) = 0;
};

namespace markers_detail {

/// The radius of a pin that stands for a person or a tracked instance: the record's MinRadius, which every record has as 5,000 cm.
inline constexpr double kPersonRadius = 5000.0;

inline MarkerLocation::Display display_of(const std::string& data_name) {
    if (data_name == "RadiusArea") return MarkerLocation::Display::Area;
    if (data_name == "Hidden") return MarkerLocation::Display::Hidden;
    return MarkerLocation::Display::Specific;
}

inline MarkerLocation from_data(const content::QuestLocation& l) {
    MarkerLocation m;
    m.display = display_of(l.display);
    m.x = l.x;
    m.y = l.y;
    m.z = l.z;
    m.radius = l.radius;
    m.completes = l.completes;
    m.id = l.id;
    return m;
}

inline MarkerLocation person_pin(const MarkerPoint& p) {
    MarkerLocation m;
    m.display = MarkerLocation::Display::Specific;
    m.x = p.x;
    m.y = p.y;
    m.z = p.z;
    m.radius = kPersonRadius;
    m.completes = true;
    return m;
}

inline double flat_distance(const MarkerPoint& a, const MarkerPoint& b) { return std::hypot(a.x - b.x, a.y - b.y); }

/// The cluster a set of points makes: the centroid, and the radius that holds every point (never below `min_radius`, never above `max_radius`).
inline MarkerLocation area_of(const std::vector<MarkerPoint>& pts, const std::vector<size_t>& members, const content::QuestObjectiveRow::Cluster& cl) {
    MarkerPoint c;
    for (const size_t i : members) {
        c.x += pts[i].x;
        c.y += pts[i].y;
        c.z += pts[i].z;
    }
    const double n = static_cast<double>(members.size());
    c.x /= n;
    c.y /= n;
    c.z /= n;
    double reach = 0;
    for (const size_t i : members) reach = std::max(reach, flat_distance(c, pts[i]));
    MarkerLocation m;
    m.display = MarkerLocation::Display::Area;
    m.x = c.x;
    m.y = c.y;
    m.z = c.z;
    m.radius = std::min(std::max(reach, cl.min_radius), cl.max_radius);
    m.completes = true;
    return m;
}

/// DBSCAN over the horizontal distance: a point with at least `min_points` points (itself among them) within `epsilon` is a core point; a cluster is the core points that reach one
/// another and the points within reach of them; a point in no cluster is noise.  Returns the clusters (indices into `pts`) and the noise.
inline void dbscan(const std::vector<MarkerPoint>& pts, double epsilon, int min_points, std::vector<std::vector<size_t>>& clusters, std::vector<size_t>& noise) {
    const size_t n = pts.size();
    // neighbours by a grid of cells as wide as epsilon: a point's neighbours are in its cell and the eight around it
    const double cell = std::max(epsilon, 1.0);
    const auto key = [&](const MarkerPoint& p) {
        const int64_t cx = static_cast<int64_t>(std::floor(p.x / cell)), cy = static_cast<int64_t>(std::floor(p.y / cell));
        return std::pair<int64_t, int64_t>(cx, cy);
    };
    struct PairHash {
        size_t operator()(const std::pair<int64_t, int64_t>& k) const { return std::hash<int64_t>()(k.first * 73856093LL) ^ std::hash<int64_t>()(k.second * 19349663LL); }
    };
    std::unordered_map<std::pair<int64_t, int64_t>, std::vector<size_t>, PairHash> grid;
    for (size_t i = 0; i < n; ++i) grid[key(pts[i])].push_back(i);
    const auto neighbours = [&](size_t i) {
        std::vector<size_t> out;
        const auto k = key(pts[i]);
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dy = -1; dy <= 1; ++dy) {
                const auto it = grid.find({k.first + dx, k.second + dy});
                if (it == grid.end()) continue;
                for (const size_t j : it->second) {
                    if (flat_distance(pts[i], pts[j]) <= epsilon) out.push_back(j);
                }
            }
        }
        return out;
    };
    constexpr int kUnseen = -2, kNoise = -1;
    std::vector<int> label(n, kUnseen);
    for (size_t i = 0; i < n; ++i) {
        if (label[i] != kUnseen) continue;
        std::vector<size_t> seeds = neighbours(i);
        if (static_cast<int>(seeds.size()) < min_points) {
            label[i] = kNoise;
            continue;
        }
        const int id = static_cast<int>(clusters.size());
        clusters.emplace_back();
        label[i] = id;
        clusters[static_cast<size_t>(id)].push_back(i);
        for (size_t s = 0; s < seeds.size(); ++s) {
            const size_t j = seeds[s];
            if (label[j] == kNoise) {   // a border point of this cluster
                label[j] = id;
                clusters[static_cast<size_t>(id)].push_back(j);
                continue;
            }
            if (label[j] != kUnseen) continue;
            label[j] = id;
            clusters[static_cast<size_t>(id)].push_back(j);
            const std::vector<size_t> more = neighbours(j);
            if (static_cast<int>(more.size()) >= min_points) seeds.insert(seeds.end(), more.begin(), more.end());
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (label[i] == kNoise) noise.push_back(i);
    }
}

/// The places of a Kill objective: its creatures' spawn points, clustered.
inline void kill_places(const content::QuestObjectiveRow& o, PointSource& src, std::vector<MarkerLocation>& out) {
    if (o.no_dynamic_markers) return;
    if (o.kill_sets.empty() && o.kill_instances.empty()) return;   // (a kill named by tags only is not modelled)
    std::vector<MarkerPoint> pts;
    src.creature_places(o.kill_instances, o.kill_sets, pts);
    if (pts.empty()) return;
    const content::QuestObjectiveRow::Cluster& cl = o.cluster;
    std::vector<std::vector<size_t>> clusters;
    std::vector<size_t> noise;
    dbscan(pts, cl.epsilon, std::max(cl.min_points, 1), clusters, noise);
    // clusters whose centres are within the merge distance become one
    std::vector<MarkerLocation> areas;
    bool merged = true;
    while (merged) {
        merged = false;
        areas.clear();
        for (const auto& c : clusters) areas.push_back(area_of(pts, c, cl));
        for (size_t a = 0; a < clusters.size() && !merged; ++a) {
            for (size_t b = a + 1; b < clusters.size() && !merged; ++b) {
                if (std::hypot(areas[a].x - areas[b].x, areas[a].y - areas[b].y) > cl.merge) continue;
                clusters[a].insert(clusters[a].end(), clusters[b].begin(), clusters[b].end());
                clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(b));
                merged = true;
            }
        }
    }
    // the biggest cluster first, then the points that belong to none
    std::vector<size_t> order(clusters.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return clusters[a].size() > clusters[b].size(); });
    for (const size_t k : order) out.push_back(area_of(pts, clusters[k], cl));
    if (cl.default_type != "Hidden") {
        for (const size_t i : noise) {
            MarkerLocation m;
            m.display = display_of(cl.default_type);
            m.x = pts[i].x;
            m.y = pts[i].y;
            m.z = pts[i].z;
            m.radius = cl.min_radius;
            m.completes = true;
            out.push_back(m);
        }
    }
    const size_t cap = static_cast<size_t>(std::max(cl.max_clusters, 1));
    if (out.size() > cap) out.resize(cap);
}

}  // namespace markers_detail

/// The places of `o`'s marker as they are now (empty: the objective gets no pin).
inline std::vector<MarkerLocation> compute_locations(const content::QuestObjectiveRow& o, PointSource& src) {
    using namespace markers_detail;
    std::vector<MarkerLocation> out;
    if (o.override_markers) {
        for (const content::QuestLocation& l : o.override_locations) out.push_back(from_data(l));
        return out;
    }
    const auto people = [&](int64_t instance) {
        if (instance == 0) return;
        std::vector<MarkerPoint> pts;
        src.npc_places(instance, pts);
        for (const MarkerPoint& p : pts) out.push_back(person_pin(p));
    };
    if (o.type == "GoTo") {
        for (const content::QuestLocation& l : o.locations) out.push_back(from_data(l));
    } else if (o.type == "Dialogue" || o.type == "TurnIn") {
        if (!o.no_dynamic_markers) {
            for (const content::QuestSource& s : o.sources) {
                if (s.kind.empty() || s.kind == "NPC") people(s.instance);
            }
        }
        if (o.type == "TurnIn") {
            for (const int64_t t : o.trackers) people(t);
        }
    } else if (o.type == "Interact" || o.type == "Collect" || o.type == "EventNotification") {
        for (const int64_t t : o.trackers) people(t);
    } else if (o.type == "Kill") {
        kill_places(o, src, out);
    }
    return out;
}

}  // namespace aoc::worldd::quest

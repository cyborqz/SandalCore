// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  fishing/water_map.h  (Fishing, milestone 1)
//
//  The water of the Riverlands that a line can be cast into: dist/content/fishing_water.tsv (tools/waterbodies, `waterbodies read`; the extractor's `waterbodies` step).  The map's Water
//  grid holds the lakes and rivers as actors; the ones that name fishing data (the GatheringInteractionData of that water: the rod, the certification, the length of a catch and the
//  table of fish) are in the file, each with
//
//    an outline   a lake's shape, a closed polygon in world cm (the Hermite curve through the spline's points, a point about every 2 m): the lake's surface is the actor's height
//    volumes      the swim volumes the game made for the body (boxes with a yaw): a river is the chain of its boxes, and a lake with no outline is the union of its boxes
//
//  A point is on the water when it is inside the outline of a lake or inside a box of a river.  The swim volumes of a lake are a coarse axis-aligned partition that overshoots the shore (a
//  box centre is outside the outline in about a third of the lakes' boxes), which is why a lake is tested by its outline.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::fishing {

/// What a point of water is: which body, its fishing data (the guid of a GatheringInteractionData) and the height of its surface.
struct WaterHit {
    size_t  body = 0;          // the body's number in the file
    int64_t fishing = 0;       // the interaction the body names
    double  surface_z = 0;     // cm
    bool    river = false;
};

class WaterMap {
public:
    /// Reads the file.  False, with `error`, when it does not open, a line is not one of the three kinds or holds no number where one belongs, a body has neither an outline nor a swim volume, or the file holds no body.
    bool load(const std::string& path, std::string& error);
    /// The same from text (the tests' small maps).
    bool parse(const std::string& text, std::string& error);

    size_t bodies() const { return bodies_.size(); }
    size_t outlines() const { return outlines_; }
    size_t boxes() const { return boxes_; }
    size_t lakes() const;
    size_t rivers() const;
    bool   empty() const { return bodies_.empty(); }

    /// The water the point (x, y) is on; nullopt for dry land.  Where two bodies overlap the lower numbered one answers.
    std::optional<WaterHit> at(double x, double y) const;

    /// A body's facts, for the log and the tests.
    struct BodyInfo {
        std::string actor;
        int64_t     fishing = 0;
        bool        river = false;
        double      x = 0, y = 0, z = 0;
        size_t      outline_points = 0, boxes = 0;
    };
    BodyInfo info(size_t body) const;
    /// The distinct fishing data guids and how many bodies name each (the log of the start).
    std::vector<std::pair<int64_t, size_t>> fishing_data() const;

    /// The centre of a body's swim volume number `index`; nullopt when the body has fewer.
    std::optional<std::pair<double, double>> box_centre(size_t body, size_t index = 0) const;

    /// The closest water to (x, y) within `radius_cm`: nullopt for none (the tests and the GM's help).  A point of the water's edge, found by walking an outline's vertices and a box's sides.
    struct Near {
        double x = 0, y = 0, distance = 0;
        size_t body = 0;
    };
    std::optional<Near> nearest(double x, double y, double radius_cm) const;

private:
    struct Box {
        double cx = 0, cy = 0, cz = 0, hx = 0, hy = 0, hz = 0, cos_yaw = 1, sin_yaw = 0;
    };
    struct Body {
        std::string                          actor;
        bool                                 river = false;
        int64_t                              fishing = 0;
        double                               x = 0, y = 0, z = 0;
        std::vector<std::pair<double, double>> outline;
        std::vector<Box>                     boxes;
        double                               min_x = 0, min_y = 0, max_x = 0, max_y = 0;   // the bounds of what answers
    };

    void index();
    static bool inside_outline(const Body& b, double x, double y);
    static bool inside_box(const Box& b, double x, double y);

    std::vector<Body>                    bodies_;
    size_t                               outlines_ = 0, boxes_ = 0;
    static constexpr double              kCellCm = 5000.0;   // the grid's cell: bodies by the cells their bounds touch
    std::unordered_map<int64_t, std::vector<uint32_t>> grid_;   // the cell's key -> the bodies whose bounds touch it, in body order
};

}  // namespace aoc::worldd::fishing

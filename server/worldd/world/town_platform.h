// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/town_platform.h  (the floor of the five city nodes' bowls)
//
//  The floor of the five city nodes' bowls (Winstead, Joeva, New Aela, Halcyon, Miraleth).  A town's floor is not the baked landscape lifted by 50 m: it is the layout's own "Landscape
//  Platform" sublevel, one per LAYOUT (Layout_Flat_D for Winstead, Joeva and New Aela, Layout_River_C for Halcyon and Miraleth), which the client's ANodeLayoutReplicator loads at the node.
//  `mapc town-floors` reads the two umaps (Landscape_Flat_D_Master_01, Landscape_River_C_Master_01) into dist/content/town_platforms.bin; this is the reader and the sampler.
//
//  The frame.  A platform stands at the node RECORD's x, y, z and yaw (city_node of content.db): its own coordinates (u, v) are the node's
//  frame, so   world = record + R(yaw) * (u, v)   in x and y, and   world z = record z + the platform's z.   Against the baked pit under each town the platform in this frame
//  differs by a median absolute deviation of 1.7 to 4.9 cm (2.6 m and more for any other yaw).
//
//  The footprint.  The platform is 1,008 m on a side (256 landscape components of 64 x 64 vertices at 100 cm), but the town is the part of it that is not a HOLE: the visibility layer
//  cuts the outside (253,873 and 253,869 vertices, 24.9 %, one region on the border, none inside, the nearest 451 m from the origin), so the footprint is a rounded square of about 900 m.
//  Inside it the floor is the platform's (PlayerWorld takes the higher of the platform and the baked landscape: a hill of the landscape pokes through Joeva's town, 115 m over its platform);
//  a query that falls on a hole, or outside the lattice, is not in the town (the baked ground meets the platform at the rim).
//
//  What it is not.  The platform is the lowest floor of the town: the walls, terraces, stairs, upper floors and towers are meshes the platform does not hold.
//
//  File (little-endian; tools/mapc/TownFloors.cs writes it):
//    "AOCTPF01", u32 version (1), u32 layouts;  per layout  u16 name length + name, u16 package length + package, u32 components;
//    per component  f64 x0 y0 stepX stepY zBase zScale, u32 nx ny, u32 flags (bit 0: it has holes), u16 raw[nx*ny] (world z = zBase + (raw - 32768) * zScale, x fastest),
//    then, with the holes flag, a bit per vertex (LSB first; 1: the visibility weight is 128 or more).
//  The components are one lattice (neighbours share a border row of vertices): the reader joins them into one grid per layout.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::world {

/// One layout's platform joined into one lattice: the heights (cm, the platform's own z) and the holes, in the platform's own (u, v) coordinates.
struct PlatformGrid {
    double   step_x = 100, step_y = 100;   // cm between vertices
    double   x0 = 0, y0 = 0;               // the platform's own coordinates of vertex (0, 0)
    uint32_t nx = 0, ny = 0;               // vertices
    std::vector<float>   z;                // nx * ny, x fastest
    std::vector<uint8_t> hole;             // nx * ny: 1 where the vertex is a hole (or no component covers it)
    double   reach = 0;                    // the farthest vertex that is not a hole from the platform's origin (cm): a query beyond it is not in the town

    /// Fills `reach` from the holes; call it once the grid is complete.
    void finish();
    /// The floor at the platform's own (u, v): true when the point lies on a quad of the lattice none of whose corners is a hole.  UE splits each quad along its (0,0)-(1,1) diagonal.
    bool height(double u, double v, double* out) const;
    size_t holes() const;
};

/// The reader: false with `err` for a short, wrong or inconsistent file.
struct PlatformLayout {
    std::string  name;      // "Layout_Flat_D"
    std::string  package;   // the umap it was read from
    PlatformGrid grid;
};
bool parse_town_platforms(const uint8_t* data, size_t size, std::vector<PlatformLayout>* out, std::string* err);
bool read_town_platforms(const std::string& path, std::vector<PlatformLayout>* out, std::string* err);

/// A city node whose floor is a platform: its record, and the layout whose platform stands at it.  `asset_set` is the layout prop's NodeAssetSet that holds the platform's umap
/// (NS_Flat_D_Master_01_Landscape: prop 14 of Layout_Flat_D; NS_River_C_Landscape: prop 53 of Layout_River_C; identity transforms, River_C's is 0.32 cm off): its item is in the
/// replicator's LayoutAssetSetGuids at every node level 0 to 6, which is how the client gets the floor (the test checks it on the real data).
struct TownBowl {
    int64_t     node;      // CityNodeRecordBase
    const char* name;      // the record
    const char* layout;
    int64_t     asset_set;
};
inline constexpr int64_t kFlatDPlatformSet = 6064632779982176256ll, kRiverCPlatformSet = 6064632757347942400ll;
inline constexpr TownBowl kTownBowls[] = {
    {108646108518008ll, "Verra_RVR_Winstead", "Layout_Flat_D", kFlatDPlatformSet},
    {108646121231993ll, "Verra_RVR_Joeva", "Layout_Flat_D", kFlatDPlatformSet},
    {108646608033403ll, "Verra_RVR_NewAela", "Layout_Flat_D", kFlatDPlatformSet},
    {108646610851452ll, "Verra_RVR_Halcyon", "Layout_River_C", kRiverCPlatformSet},
    {108646615701117ll, "Verra_RVR_Miraleth", "Layout_River_C", kRiverCPlatformSet},
};
// ~ C | O | A & Cyb ~

/// The platforms placed at their nodes.
class TownPlatforms {
public:
    struct Frame {
        double x = 0, y = 0, z = 0;   // the node record's place (cm)
        double yaw_degrees = 0;       // ... and its rotation
    };
    struct Hit {
        int64_t node = 0;
        double  z = 0;                // the floor (where a player's feet are), world cm
        double  u = 0, v = 0;         // the point in the node's frame
    };

    /// Adds layouts (the file's, or a test's).  A name that is there is replaced.
    void add_layout(const std::string& name, PlatformGrid grid);
    bool load(const std::string& path, std::string* err);
    /// Stands `layout`'s platform at `node` in `frame`; false when the layout is not here.
    bool place(int64_t node, const std::string& layout, const Frame& frame);

    /// The floor of the town whose footprint holds (x, y); none outside every footprint.  Two footprints on the same point (none in the game): the nearer node's.
    std::optional<Hit> floor_at(double x, double y) const;

    bool         has_layout(const std::string& name) const { return layouts_.count(name) != 0; }
    const PlatformGrid* layout(const std::string& name) const;
    bool         has_node(int64_t node) const;
    const Frame* frame_of(int64_t node) const;
    std::string  layout_of(int64_t node) const;
    size_t       layouts() const { return layouts_.size(); }
    size_t       nodes() const { return placed_.size(); }

private:
    struct Placed {
        int64_t     node = 0;
        std::string layout;
        Frame       frame;
        double      cos_yaw = 1, sin_yaw = 0;
        double      reach = 0;
    };
    std::map<std::string, PlatformGrid> layouts_;
    std::vector<Placed>                 placed_;
};

}  // namespace aoc::worldd::world

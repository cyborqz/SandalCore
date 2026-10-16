// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  foliage/foliage_map.h  (Gathering stage 3, part "Foliage gathering")
//
//  The painted trees, plants and stones of the Riverlands: dist/content/region_foliage.tsv (tools/mapc, `mapc extract`; version 2), 84,218 instances of the region's gatherable
//  foliage.  An instance is named the way the client names it in ServerStartInteractionWithFoliage and in its state actors: the OWNER actor (InstancedFoliageActor_25600_<cx>_<cy>_0:
//  one per 25,600 cm cell, 34 cells have two), the COMPONENT (IntrepidGatherableFISMC_<n>: 10,613 pairs, at most 38 to an owner) and the INDEX in the component's per-instance
//  data.  Those three are the key [R: captures 1428, 1451 and 1519 name rows of this file exactly: capture_foliage.h kStarts]; the row's `def_name` is its
//  GatheringFoliageData (the file's def_id is 0 or a state record's for 6,007 rows, so the name decides).
//
//  A row is an index into rows(): the other foliage code works on those indexes (the store's state, a lock, a cell actor's item).  The owners' components are sorted by name as
//  the retail server lists them in an actor's open (FISMC_0, _1, _10, _11 ... _19, _2, _20 ...: byte order).
//
//  The file is read once at start; a damaged line (the wrong column count, a number that is none, a key twice) stops the load with its line number.
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::foliage {

class FoliageMap {
public:
    struct Row {
        uint32_t def = 0;          // index into def_names()
        uint16_t owner = 0;        // index into owners()
        uint16_t component = 0;    // index into that owner's components
        uint32_t index = 0;        // the instance's index in its component
        float    x = 0, y = 0, z = 0;   // cm
    };
    struct Owner {
        std::string               name;         // InstancedFoliageActor_25600_-41_19_0
        int                       cell_x = 0, cell_y = 0;   // the name's cell (25,600 cm squares)
        int64_t                   z_lo_tenths = 0, z_hi_tenths = 0;   // the lowest and the highest height of its instances, in tenths of a cm (the file's own precision)
        std::vector<std::string>  components;   // IntrepidGatherableFISMC_<n>, sorted by name
        std::vector<uint32_t>     rows;         // its rows, in the file's order

        /// Where retail puts the cell's state actor in z: the middle of its instances' lowest and highest, in tenths of a cm (SerializeNewActor's location is at scale 10).  [R: the seven
        /// cells of capture 1198's login and 1428's, nine of nine to the tenth of a cm: the file's own precision, so a half tenth is cut off]
        int64_t centre_z_tenths() const { return (z_lo_tenths + z_hi_tenths) >> 1; }
        /// ... and the whole cm that ReplicatedMovement (an AActor's: a whole number) carries: the nearest cm, a tie going down [R: 1198's cell (-31, 16) is 12807.5 in the header and 12807 in the block].
        int64_t centre_z_cm() const {
            const int64_t t = centre_z_tenths() + 4;
            return t >= 0 ? t / 10 : -((-t + 9) / 10);
        }
    };

    /// The cell's size in cm: the owners' names say it (InstancedFoliageActor_25600_...).
    static constexpr double kCellCm = 25600.0;

    /// nullopt, with `error` naming the line, for a file that is missing or damaged.
    static std::optional<FoliageMap> load(const std::string& path, std::string& error);
    static std::optional<FoliageMap> parse(std::string_view text, std::string& error);

    const std::vector<Row>&         rows() const { return rows_; }
    const std::vector<Owner>&       owners() const { return owners_; }
    const std::vector<std::string>& def_names() const { return def_names_; }
    size_t                          components() const { return components_; }
// C-Y-B.C.O.A

    /// The row an instance is, by the names the client sends; nullopt when there is none.
    std::optional<uint32_t> find(std::string_view owner, std::string_view component, uint32_t index) const;
    std::optional<uint16_t> owner_of(std::string_view name) const;
    /// The center of an owner's cell in cm: ((cx + 0.5) * 25,600, (cy + 0.5) * 25,600).
    void cell_center(uint16_t owner, double& x, double& y) const;

    std::string describe() const;

private:
    static uint64_t key_of(uint16_t owner, uint16_t component, uint32_t index) { return (static_cast<uint64_t>(owner) << 48) | (static_cast<uint64_t>(component) << 32) | index; }

    std::vector<Row>                          rows_;
    std::vector<Owner>                        owners_;
    std::vector<std::string>                  def_names_;
    std::unordered_map<std::string, uint16_t> owner_by_name_;
    std::unordered_map<uint64_t, uint32_t>    row_by_key_;
    size_t                                    components_ = 0;
};

}  // namespace aoc::worldd::foliage

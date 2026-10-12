// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  stats/gear.h: what the worn items add to the stats (plan B3)
//
//  The stat engine's equipment input (StatInputs::mods and ::equipped) from the items a character
//  wears: the same rules as services/internal/statprog's gear.go, which read the same rows of
//  content.db (contentc version 4: quality_curve, stat_block_stat, item_set, set_bonus, item_tag), and the
//  shared corpus (stats/testdata/corpus.txt) holds the items of the cases they were made from, so the
//  C++ and the Go reading are compared on the real file.
//
//    - An item's ItemStatBlock gives, per stat, a Min and a Max at each rarity (Common, Uncommon, Rare,
//      Heroic, Epic, Legendary, Artifact). The item's Quality picks the rarity band of its QualityCurve
//      (Common 1-5, Uncommon 6-15, Rare 16-30, Heroic 31-50, Epic 51-70, Legendary 71-100, Artifact
//      101-200) and the value is Min + (Max - Min) * (q - X) / (Y - X) in it; an item with no curve reads
//      CommonMin. With Rounding::HalfUp each value is made floor(v + 0.5) first (the research's best fit
//      of a retail level-20 Ranger's 29 items, 376 of 442 stats against 358 unrounded: not proven).
//    - A SetBonus adds, for each tier (the pieces of the set that must be worn), stat bonuses by rarity:
//      every tier up to the worn count applies, at the lowest rarity among the worn pieces of the set.
//    - Every gameplay tag of an item is given under the name of the slot it is worn in.
//
//  The sums are per stat, in double, in the order Gear.Mods adds them: the items in the order given, then
//  the sets by ascending guid; the mods come out ordered by stat guid. A stat that is not in the engine's
//  bundle (an item's MaxDurability) stays in the list: the engine ignores it and counts it.
// ============================================================================
#pragma once

#include "stats/stat_engine.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
// [C O A :: C-Y-B]

namespace aoc::worldd::stats {

inline constexpr int kRarities = 7;   // Common, Uncommon, Rare, Heroic, Epic, Legendary, Artifact

struct QualityBand {
    double x = 0, y = 0;   // Quality x..y is this rarity
};

/// One stat of an item stat block: its Min and Max at each rarity.
struct BlockStat {
    int64_t                      stat = 0;
    std::array<double, kRarities> min{}, max{};
};

/// One bonus of a set tier: the stat and its value at each rarity (has[r] false: none, adds 0).
struct SetEntry {
    int64_t                       stat = 0;
    std::array<bool, kRarities>   has{};
    std::array<double, kRarities> value{};
};

struct SetTier {
    int                    pieces = 0;
    std::vector<SetEntry>  entries;
};

struct SetBonus {
    std::vector<SetTier> tiers;
};

/// What the stat engine needs of one ItemRecord.
struct ItemGear {
    int64_t                         record = 0;
    bool                            has_curve = false;
    std::array<QualityBand, kRarities> bands{};
    std::vector<BlockStat>          stats;
    std::vector<int64_t>            sets;   // the SetBonus guids, in the record's order
    std::vector<std::string>        tags;   // GameplayTags, in the record's order
};

/// Where the item facts come from: content.db in worldd, a table in a test. A pointer is valid for the catalog's life.
class GearCatalog {
public:
    virtual ~GearCatalog() = default;
    /// nullptr: the catalog does not know this record.
    virtual const ItemGear* item(int64_t record) const = 0;
    /// nullptr: the catalog does not know this set.
    virtual const SetBonus* set(int64_t guid) const = 0;
    /// The EEquipSlot enumerator name of a slot value ("Chest" for 2, "Primary" for 15); "Slot<N>" for a value that has none.
    virtual std::string slot_name(int slot) const = 0;
};

enum class Rounding : uint8_t { None, HalfUp };

/// An item worn: its record, its Quality (an index into the record's quality curve) and the EEquipSlot it is in.
struct WornItem {
    int64_t record = 0;
    int     quality = 1;
    int     slot = 0;
};

struct GearInputs {
    std::vector<StatMod>     mods;       // by stat guid, ascending
    std::vector<EquippedTag> equipped;
    /// Worn records the catalog does not know (they add nothing) and sets it does not know (no bonus).
    int                      unknown_items = 0;
    int                      unknown_sets = 0;
};

GearInputs gear_inputs(const GearCatalog& catalog, std::span<const WornItem> worn, Rounding rounding);

}  // namespace aoc::worldd::stats

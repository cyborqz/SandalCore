// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/rarity.h: the one rule that turns an item's quality into its rarity, its stats and its durability (item rarities, step 0; header only)
//
//  The game client works an item's rarity out by itself, from the item's record and the Quality the server sent, and the server has to say the same wherever it speaks of it: the
//  pick-up and sale toasts, the experience a harvest pays, a shop's price, the stats of worn gear, the durability of a new item.  The rule:
//    * the rarity is the band the quality falls in on the record's quality curve (Common 1-5, Uncommon 6-15, Rare 16-30, Heroic 31-50, Epic 51-70, Legendary 71-100, Artifact
//      101-200), held inside the record's own RarityMin..RarityMax; a record with no curve is its RarityMin.  So a Glint of tier 2 (always Uncommon) is Uncommon at quality 21, and
//      Bee's Wax (never below Rare) is Rare at quality 1.
//    * a stat of a gear item is its stat block's Min and Max AT THAT RARITY, interpolated by where the quality sits inside the rarity's band (0 at the band's start, 1 at its end,
//      never outside it), and rounded half up.  An item's maximum durability is such a stat (kMaxDurabilityStat).
//  Everything here is a pure function of the facts; nothing reads a file.  The readers of content.db fill the facts (content/content_db.h) and every record that has them derives
//  from ItemRarityFacts, so there is one rule and no second copy of it.
// ============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace aoc::worldd::items {

/// The rarities: Common 0, Uncommon 1, Rare 2, Heroic 3, Epic 4, Legendary 5, Artifact 6.
inline constexpr int kRarityCount = 7;
/// The qualities an item can hold (the character service's table allows 1 to 200).
inline constexpr int kMinQuality = 1;
inline constexpr int kMaxQuality = 200;

/// The first quality of each rarity's band on the common quality curve: what an item is given when only its rarity is asked for.
inline constexpr std::array<int, kRarityCount> kFirstQuality = {1, 6, 16, 31, 51, 71, 101};

inline constexpr std::array<std::string_view, kRarityCount> kRarityNames = {"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"};

/// The stat that is an item's maximum durability (the record of StatTypeDef MaxDurability).
inline constexpr int64_t kMaxDurabilityStat = 6064629444242636800ll;

/// The first quality of a rarity's band on the common curve (1, 6, 16, 31, 51, 71, 101); a rarity outside 0 to 6 is held inside it.
constexpr int first_quality_of(int rarity) { return kFirstQuality[static_cast<size_t>(std::clamp(rarity, 0, kRarityCount - 1))]; }

/// The last quality of a rarity's band on the common curve (5, 15, 30, 50, 70, 100, 200).
constexpr int last_quality_of(int rarity) { return rarity >= kRarityCount - 1 ? kMaxQuality : first_quality_of(rarity + 1) - 1; }

/// The name of a rarity ("Common" ... "Artifact"), as the client's rich-text styles (Item_Uncommon ...) and the players know it; a rarity outside 0 to 6 is held inside it.
constexpr std::string_view rarity_name(int rarity) { return kRarityNames[static_cast<size_t>(std::clamp(rarity, 0, kRarityCount - 1))]; }

/// The rarity a name stands for ("rare", "Rare", "RARE"), -1 for a word that is none.
inline int rarity_named(std::string_view name) {
    for (int r = 0; r < kRarityCount; ++r) {
        const std::string_view full = kRarityNames[static_cast<size_t>(r)];
        if (name.size() != full.size()) continue;
        bool same = true;
        for (size_t i = 0; i < name.size() && same; ++i) same = std::tolower(static_cast<unsigned char>(name[i])) == std::tolower(static_cast<unsigned char>(full[i]));
        if (same) return r;
    }
    return -1;
}

/// A range of rarities in words, for a refusal: "Rare", "Rare or better", "Common to Epic", "any rarity"; the ends are held inside Common to Artifact.
inline std::string rarity_range_text(int lo, int hi) {
    lo = std::clamp(lo, 0, kRarityCount - 1);
    hi = std::clamp(hi, 0, kRarityCount - 1);
    if (lo == hi) return std::string(rarity_name(lo));
    if (lo == 0 && hi == kRarityCount - 1) return "any rarity";
    if (hi == kRarityCount - 1) return std::string(rarity_name(lo)) + " or better";
    return std::string(rarity_name(lo)) + " to " + std::string(rarity_name(hi));
}

/// The Quality range (x, y) of one rarity on an item's quality curve.
struct QualityBand {
    double x = 0, y = 0;   // quality x..y is this rarity
};

/// The values of one stat at each rarity: its Min and its Max.
struct RarityRange {
    std::array<double, kRarityCount> min{}, max{};
};

/// What decides the rarity of an item at a quality: its quality curve and the range its own rarity is held inside.  The default is the common curve, held nowhere.
struct ItemRarityFacts {
    bool                                  has_curve = false;       // false: the record has no quality curve (it is always rarity_min)
    std::array<QualityBand, kRarityCount> bands{};                 // the curve: the Quality range of each rarity, in order
    int                                   rarity_min = 0;          // RarityMin and RarityMax as rarity ordinals (Common 0 ... Artifact 6)
    int                                   rarity_max = kRarityCount - 1;

    /// The rarity whose band holds the quality (the first of Common to Legendary with x <= quality <= y); Artifact for a quality at or past its start, else Common.  Not held
    /// inside rarity_min..rarity_max yet: rarity_of does that.
    int band_of(double quality) const {
        for (int i = 0; i < kRarityCount - 1; ++i) {
            const QualityBand& b = bands[static_cast<size_t>(i)];
            if (b.x <= quality && quality <= b.y) return i;
        }
        return bands[kRarityCount - 1].x <= quality ? kRarityCount - 1 : 0;
    }

    /// The rarity of an item of this quality, as the client shows it: its band held inside rarity_min..rarity_max; a record with no curve is rarity_min.
    int rarity_of(double quality) const {
        if (!has_curve) return rarity_min;
        const int band = band_of(quality);
        return band < rarity_min ? rarity_min : (rarity_max < band ? rarity_max : band);
    }

    /// Where the quality sits inside the band of the rarity the item shows, 0 to 1 (never outside it); 0 for a record with no curve or a band that is a single point.
    double position_of(double quality) const {
        if (!has_curve) return 0.0;
        const QualityBand& b = bands[static_cast<size_t>(rarity_of(quality))];
        return b.y > b.x ? std::clamp((quality - b.x) / (b.y - b.x), 0.0, 1.0) : 0.0;
    }

    /// A stat of the item at this quality: its Min and Max at the rarity the item shows, interpolated by the quality's place in that rarity's band.  `round_half_up`: the client
    /// rounds a stat half up (floor(v + 0.5)); the research readers of the stat engine ask for the plain value.
    double stat_at(const RarityRange& range, double quality, bool round_half_up = true) const {
        const size_t r = static_cast<size_t>(rarity_of(quality));
        const double v = range.min[r] + (range.max[r] - range.min[r]) * position_of(quality);
        return round_half_up ? std::floor(v + 0.5) : v;
    }
};

/// The common quality curve (Common 1-5 ... Artifact 101-200) with no bound on the rarity: what a record the data does not hold, a test without data and a quality with no item
/// attached read.
inline const ItemRarityFacts& default_rarity_facts() {
    static const ItemRarityFacts facts = [] {
        ItemRarityFacts f;
        f.has_curve = true;
        for (int r = 0; r < kRarityCount; ++r) f.bands[static_cast<size_t>(r)] = {static_cast<double>(first_quality_of(r)), static_cast<double>(last_quality_of(r))};
        return f;
    }();
    return facts;
}

/// The maximum durability a new item has at `quality`: the MaxDurability stat of its stat block at the rarity the item shows; an item with no such stat keeps the record's own value
/// (the durability at its default quality, rounded half up; 0 for an item that does not wear).  A repair restores the same maximum.
inline double max_durability_at(const ItemRarityFacts& facts, const RarityRange* durability_stat, double record_value, double quality) {
    return durability_stat ? facts.stat_at(*durability_stat, quality) : std::floor(record_value + 0.5);
}

/// The quality a game master's request asks for: a rarity by name ("legendary": the lowest quality of that colour), an exact quality (1 to 200), or both.  `item_name` is for the
/// message only.  Returns the quality, or 0 with `why` set when the request is refused: a word that is no rarity, a number out of range, a rarity or a quality the item can never
/// show (the answer lists the rarities the item allows), or a rarity and a quality that disagree.  Nothing is asked when `rarity` is empty and `quality` is 0.
inline int quality_for_request(const ItemRarityFacts& facts, std::string_view item_name, std::string_view rarity, int quality, std::string& why) {
    why.clear();
    int named = -1;
    if (!rarity.empty()) {
        named = rarity_named(rarity);
        if (named < 0) {
            why = "\"" + std::string(rarity) + "\" is no rarity: use Common, Uncommon, Rare, Heroic, Epic, Legendary or Artifact, or q<number> for an exact quality (1 to 200)";
            return 0;
        }
    }
    if (quality != 0 && (quality < kMinQuality || quality > kMaxQuality)) {
        why = "the quality " + std::to_string(quality) + " is not 1 to " + std::to_string(kMaxQuality);
        return 0;
    }
    // the rarities the item can show: its RarityMin to RarityMax with a quality curve; without one the client always reads RarityMin
    const int lo = facts.rarity_min, hi = facts.has_curve ? std::max(facts.rarity_min, facts.rarity_max) : facts.rarity_min;
    const auto allows = [&] {
        if (lo == hi) return "it is always " + std::string(rarity_name(lo));
        std::string list;
        for (int r = lo; r <= hi; ++r) {
            if (!list.empty()) list += r == hi ? " or " : ", ";
            list += rarity_name(r);
        }
        return "it can be " + list;
    };
    if (quality != 0) {
        const int band = facts.has_curve ? facts.band_of(quality) : lo;
        if (band < lo || band > hi) {
            why = "quality " + std::to_string(quality) + " is " + std::string(rarity_name(band)) + ", which " + std::string(item_name) + " is never: " + allows();
            return 0;
        }
        if (named >= 0 && named != band) {
            why = std::string(rarity_name(named)) + " does not fit quality " + std::to_string(quality) + " (that is " + std::string(rarity_name(band)) + ")";
            return 0;
        }
        return quality;
    }
    if (named < lo || named > hi) {
        why = std::string(item_name) + " cannot be " + std::string(rarity_name(named)) + ": " + allows();
        return 0;
    }
    const int first = facts.has_curve ? static_cast<int>(std::ceil(facts.bands[static_cast<size_t>(named)].x)) : first_quality_of(named);
    return std::clamp(first, kMinQuality, kMaxQuality);
}

}  // namespace aoc::worldd::items

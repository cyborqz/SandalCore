// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  gather/gathering.h  (Gathering stage 2, part "Harvesting")
//
//  Everything a harvest needs that is the world's and not one player's: the gather tables of content.db (which node needs what, which tool, how long), the reward tables
//  of the nodes (their own RewardBook: the interaction's line, the experience lines and the bonus drops of the Random_Mining_* tables), the shared node state (gather/node_board.h)
//  and the rolls (the channel's length, the harvest).  The player's session asks it and applies the answer to the player's items and experience.
//
//  The rolls are the design data's equations run as they are written (the user's rule: the data is the truth), on the stat engine with the harvesting player's own run:
//
//    channel      the GatheringInteractionData's duration formula (EvalFormula of the Speed equation: BaseGatheringDuration x the GatheringSpeed curve of the player's ratings)
//    quality      each item's ItemVariation Quality formula (Artisan_Gathering_<Profession>_<Tool>_Rarity or its _T1_Only variant): the rarity roll per harvest, the quality
//                 that gives the colour in the bag (1, 6, 16, 31, 51, 71, 101 for Common to Artifact)
//    quantity     the reward line's quantity (a base + the Quantity equation, which reads GetRolledItemQuality())
//    experience   the profession's (the Artisanship_Gathering_BaseXP equation: the tier's base x the item's rarity x the experience rating) and Adventuring's, as the reward table
//                 lines give them (GetItemRarity() and GetSourceCertificationTier() read the harvest's own item)
//    wear         the tool's durability formula (1; 2 for a Medium axe node)
//
//  What the rating stats (Artisan_Gathering_*_Rating, 112 of them) are for a player is the stat engine's settled value of the stat, equipment included: 0 for everyone until
//  skills and talents exist, which gives exactly the data's base: the rarity table of `Artisan_Gathering_Rarity` at rating 0 (81.9 % Common, 85.4 % for _T1_Only), a quantity of
//  the base (the chance of the Quantity equation is 0 % at rating 0), the full channel length.
//
//  GetRolledItemQuality() in the Quantity equation is compared with 0 to 6 (the rarity): by default it answers the index of the rarity BAND of the rolled quality (Common 1-5,
//  Uncommon 6-15, Rare 16-30, Heroic 31-50, Epic 51-70, Legendary 71-100, Artifact 101-200); `quality_literal` switches to the literal quality (AOC_GATHER_QUALITY_LITERAL),
//  which makes a Common q1 read as rarity 1.  Both fit the one captured bonus (an Oak x6 = 5 + 1).
// ============================================================================
#pragma once

#include "combat/rewards.h"
#include "content/content_db.h"
#include "gather/node_board.h"
#include "session/ability_catalog.h"
#include "stats/character_stats.h"
#include "world/actor_registry.h"
#include "world/city_nodes.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::gather {

/// Gathering stage 3, "Hunting": the PlayerInteractionDefs of the huntable creatures (the Hunting_<kind> defs: every def whose gathering interaction is the Hunting profession's).  The
/// asset of a huntable creature names its def (NpcAssetRow::interaction_def), and the def names the interaction its corpse is harvested with.
std::set<int64_t> hunting_defs(const content::FoliageData& data);
/// The name a huntable creature's nameplate shows, by its PlayerInteractionDef: the def's name without "Hunting_" and with a space before each capital ("Hunting_Raven" -> "Raven", "Hunting_PallasCat" ->
/// "Pallas Cat").  Retail's four living opens carry it as UNPCInfo.CharacterName ("Raven", "Spider", "Tidesnapper", "Bear": captures 914, 758, 1458); the Blueprints of these creatures name nothing.
/// The multi-word names are inferred from that rule.
std::map<int64_t, std::string> hunting_names(const content::FoliageData& data);
// -- CoA & C_Y_B --

struct Config {
    NodeBoard::Config board;
    bool              quality_literal = false;
    uint64_t          seed = 0x6A7E5EEDull;
};

/// What a harvest gave.
struct Harvest {
    std::vector<combat::RolledItem> items;       // in roll order: items[0] is the interaction's own line (when it rolled one), the rest are bonus drops
    int64_t  profession_xp = 0;                   // the Profession lines of the reward tables, summed
    int64_t  adventuring_xp = 0;                  // the Adventuring lines, summed (as the table gives it: no decay by level)
    double   durability_loss = 1.0;               // the tool's wear
    int      primary_quality = 0, primary_rarity = 0;   // the interaction's own item: the colour of the harvest
    size_t   tables = 0, gated = 0, quality_failures = 0;
};

class Gathering {
public:
    /// nullptr, with `error`, when content.db has no gather tables, no reward table of a node reads, or a piece is missing. `catalog` (the programs of the formulas) must outlive it.
    static std::unique_ptr<Gathering> make(const content::ContentDb& db, world::ActorRegistry& registry, const abilities::Catalog& catalog, Config config, std::string& error);

    /// The ratings the rolls read instead of the stat engine's settled values (a stat guid -> its rating): the tests' way to give a player a rating until skills and talents exist.
    /// Empty (the default): the settled values of the stats.
    void set_ratings(std::map<int64_t, double> ratings) { ratings_ = std::move(ratings); }
    const std::map<int64_t, double>& ratings() const { return ratings_; }

    NodeBoard&       board() { return board_; }
    const NodeBoard& board() const { return board_; }
    const Config&    config() const { return cfg_; }
    stats::SplitMix& rng() { return rng_; }

    size_t defs() const { return data_.defs.size(); }
    size_t tools() const { return data_.tools.size(); }
    size_t tables() const { return rewards_ ? rewards_->tables() : 0; }
    const content::GatherData& data() const { return data_; }

    const content::GatherDefRow*  def(int64_t actor_def) const;
    const content::GatherToolRow* tool(int64_t item) const;
    /// The def a resource node's actor carries (GatherableActorDef.Guid); nullptr for an actor that is no node or whose def has no gather row.
    const content::GatherDefRow*  def_of(const world::WorldActor& node) const;

    /// Gathering stage 3, "Foliage gathering": a gathering interaction (a GatheringInteractionData: a tree's or plant's per state, a Hunting corpse's) as the row the node rules and rolls
    /// take (check_requirements, channel_seconds, harvest): its profession, certification, tool slot, durations and reward table, the range of the PlayerInteractionDef named like it.
    /// nullptr for a guid that is none.  The reward tables of all 236 are loaded with the nodes'.
    const content::GatherDefRow*  interaction(int64_t interaction_guid) const;
    /// The foliage defs and states and every gathering interaction (content.db version 15); nullptr for a file without them.
    const content::FoliageData*   foliage_data() const { return foliage_ ? &*foliage_ : nullptr; }
    size_t                        interactions() const { return interactions_.size(); }

    /// Gathering stage 3, "Hunting": a kind of huntable creature.  `row` is the Hunting_<kind> gathering interaction (the Bow, 15 s, a Novice, Apprentice or Journeyman certification, the
    /// kind's reward table).  `gives_nothing`: nothing in the reward table is an item a player can use (an empty table, or only RETIRED_ and NOTUSED_ records): its corpse is hunted all the
    /// same, for the experience, and the session drops the items.
    struct HuntKind {
        int64_t                      def = 0;       // the PlayerInteractionDef
        std::string                  name;          // Hunting_Wolf
        const content::GatherDefRow* row = nullptr;
        bool                         gives_nothing = false;
    };
    /// Runs every kind's table 60 times for `probe` (a character's stat run) and marks the kinds that never give a usable item `gives_nothing` too (a weight of 0, a predicate that reads the
    /// creature's wounds, which the roller answers with "no"); returns how many kinds give nothing in all.  The world calls it once when gathering starts.
    size_t classify_hunting(stats::CharacterStats::Evaluation& probe);
    /// How many hunting roots had their NumberOfSubtablesToSelect read as 1 (the data says 2: see gathering.cpp).
    size_t hunting_one_of() const { return hunting_one_of_; }
    /// The kind a PlayerInteractionDef names; nullptr when it is no hunting one (a vendor's, a quest's ...).
    const HuntKind* hunt_kind(int64_t interaction_def) const;
    /// Every kind, by PlayerInteractionDef.
    const std::map<int64_t, HuntKind>& hunting() const { return hunting_; }

    /// The EItemRarity ordinal of a quality of an item (the band of its record's quality curve; the common curve for an item without one).
    int rarity_of(int64_t item, int quality) const;

    /// The channel's length in seconds: the interaction's duration formula run for the player (null when the formula does not run, `why` says why).
    std::optional<double> channel_seconds(const content::GatherDefRow& def, stats::CharacterStats::Evaluation& player, std::string& why);

    /// One harvest: the reward table of the node's def rolled for the player (`player` is its stat run, `level` its level); `site` is the node's place (the biome the bonus tables read).
    /// nullopt, with `why`, when the table is missing.
    std::optional<Harvest> harvest(const content::GatherDefRow& def, stats::CharacterStats::Evaluation& player, double level, const world::NodeSite& site,
                                   const world::CityNodes* nodes, stats::SplitMix& rng, std::string& why);

private:
    Gathering(NodeBoard::Config board, world::ActorRegistry& registry, Config config) : board_(registry, board), cfg_(config) {}

    NodeBoard                       board_;
    Config                          cfg_;
    const content::ContentDb*       db_ = nullptr;
    const abilities::Catalog*       catalog_ = nullptr;
    content::GatherData             data_;
    std::optional<content::FoliageData> foliage_;
    std::map<int64_t, content::GatherDefRow> interactions_;   // by GatheringInteractionData guid
    std::map<int64_t, HuntKind>     hunting_;                  // by PlayerInteractionDef guid
    size_t                          hunting_one_of_ = 0;
    std::optional<combat::RewardBook> rewards_;
    stats::SplitMix                 rng_;
    std::map<int64_t, double>       ratings_;
    mutable std::map<int64_t, std::array<int, stats::kRarities>> floors_;   // item -> the lowest quality of each rarity band
};

}  // namespace aoc::worldd::gather

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/rewards.h  (plan E1, stage 3, part 2)
//
//  What a kill hands out, from the design data's reward tables (content.db loot_table: every RewardTableRecordBase, its expressions compiled into the
//  stat program).  A creature's Blueprint names the tables (npc_motion.tsv's `rewards`; the population's asset and instance may add some): nearly every
//  one has Base_Kill_Reward_XP+Glint (experience of three kinds from Base_Kill_XP at the creature's level times its reward multiplier, and the Glint
//  certificates by Glint_Chance and Glint_QMin / QMax), and the kinds of creature their own (Goblin_Base, Bandit_Base, BaseLoot_Melee ...: gear, recipes and
//  reagents by percentages).  The expressions run as the dead creature (the owner: its level, its elite rank, its Health) on the stat engine, with this
//  host: RandDouble and RandIntRange from the world's seed, the level of the highest player that may loot and its luck, the creature's gameplay tags.
//
//  The selection rules [modelled: the algorithm names are the data's, what they do is not written down]:
//    table      its Predicate must hold; its own containers roll; then its sub tables are chosen by the table's algorithm and rolled the same way
//    All        every element                  None / empty   no sub table (a container: all)
//    PureRandom                                  NumberToSelect (at least 1) distinct elements, equal chance
//    PerElementWeightedRandom                    the same, by the weights (WeightsPerSubTable / WeightsPerReward)
//    PerElementWeightedExpressionRandom          the same, by the weights the expressions give
//    RollEachByPercent                           each element on its own: the percentage the expression gives (a fraction; none: never)
//    container  its GrantPredicate must hold; its rewards are chosen by the container's algorithm; each reward gives its experience (an amount),
//               its items (a quantity: a fraction is rounded down, none for 0) and its currency (counted, not given: no currency in E1)
//  An expression that does not run (a quest state, a story phase: the compiler left it out) is "no": a predicate that is false, a weight
//  or percentage of 0, a quantity of 0.  Depth is bounded.  The node reads (Nodes 1: the biome of the node the kill is in, so the "_RVR" gear tables
//  roll in the Riverlands and the "_TUR" ones in the Tropics) run on the host over world/city_nodes.h.
//
//  Experience is rolled as the table gives it (the creature's own worth); what a player of another level gets is that times the Adventuring leveling
//  type's ExperienceDecayCurve (1 for a creature of the player's level up to 9 above, 0.05 at 9 below and more, the level-17 Rabid of the captures gave 19
//  where the table says 450).
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "session/ability_catalog.h"
#include "stats/character_stats.h"
#include "stats/gather_host.h"
#include "world/city_nodes.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aoc::worldd::combat {

struct RolledItem {
    int64_t item = 0;
    int64_t variation = 0;
    int64_t quantity = 0;
    /// The quality the item's variation rolled: a harvest's (Gathering stage 2) or a kill's (item rarities, step 1); 0: none rolled, the item record's default stands.
    int     quality = 0;
};

struct RolledXp {
    std::string type;       // Adventuring, Weapon, NodeActivity
    int64_t     amount = 0; // as the table gives it, before the decay of the player's level
};

struct Rolled {
    std::vector<RolledItem> loot;     // grant "Always": into the corpse's loot (the same item and variation in one stack)
    std::vector<RolledItem> direct;   // grant "Auto" items: straight to the players (none in the data)
    std::vector<RolledXp>   xp;
    size_t tables = 0;                // tables rolled (the roots and the sub tables they chose)
    size_t gated = 0;                 // expressions that did not run, read as "no"
    size_t currency = 0;              // currency rewards left out
    /// A kill (item rarities, step 1): how many items had their quality rolled by their line's variation, how many formulas did not run (the item keeps the record's quality), and how many
    /// Common outcomes the test boost threw back (RollContext::rarity_boost; 0 at 1).
    size_t quality_rolls = 0;
    size_t quality_failures = 0;
    size_t boost_redraws = 0;
};
// (C-O-A : Cyb)

/// Gathering stage 2: a harvest's rolls (the reward tables of a node).  The table's items carry an ItemVariation whose Quality formula rolls the item's rarity (Artisan_Gathering_Rarity)
/// and a quantity formula that reads what that roll gave (GetRolledItemQuality) and, for the experience, the rarity of the harvest's item (GetItemRarity): for each item the roller runs
/// the variation's formula first, then the quantity, then the experience tables read the rarity of the FIRST item rolled (the interaction's own line, not a bonus drop).
struct GatherRoll {
    stats::GatherContext context;                              // the host's answers; the roller sets quality and item_rarity as it goes; ratings, source_tier and base_duration are the caller's
    const std::map<int64_t, int64_t>* quality_expr = nullptr;  // ItemVariation guid -> the ab_expr id of its Quality formula
    /// The EItemRarity ordinal (0 Common .. 6 Artifact) of an item of `item` at `quality`: the band of its record's quality curve.
    std::function<int(int64_t item, int quality)> rarity_of;
    /// GetRolledItemQuality() answers the quality itself (true) or the index of its rarity band (false: the default; the equation compares it with 0 to 6).
    bool quality_literal = false;
    bool primary_set = false;                                  // the first item has been rolled
    int  primary_quality = 0, primary_rarity = 0;              // ... its quality and rarity
    size_t quality_failures = 0;                               // a Quality formula that did not run (the item keeps the record's quality)
};

struct RollContext {
    stats::CharacterStats::Evaluation* npc = nullptr;   // the dead creature's run: the owner of the expressions (a harvest: the player's)
    /// Gathering stage 2: set for a harvest (see GatherRoll); null for a kill.
    GatherRoll* gather = nullptr;
    const std::vector<std::string>*    npc_tags = nullptr;
    double  loot_max_level = 1;       // GetPlayerLootMaxLevel
    double  avg_luck = 0;             // GetPlayerLootAvgLuck
    double  player_level = 1;         // GetPlayerLevel (the decay)
    /// Multiplies the chance of every RollEachByPercent element (the draw is `rand < min(1, percent x chance_mult)`); never a quantity, a weight or a predicate.  1: the data's own odds
    /// (worldd --loot-chance-mult / AOC_LOOT_CHANCE_MULT; the user's test of 2026-10-10 found drops too rare).
    double  chance_mult = 1.0;
    /// Item rarities, step 1 (a kill, never a harvest): the test boost.  Multiplies the weight of every outcome above Common in the quality roll of a reward line's variation (a gear or a resource
    /// drop): a roll that comes out Common is thrown back with the chance 1 - 1 / rarity_boost and drawn again, so each better outcome is that many times as likely against Common and their
    /// odds among themselves stay the data's.  Never a quantity, a percentage or which Glint tier drops.  1: the data's own odds, the formula runs once and nothing else is drawn
    /// (worldd --loot-rarity-boost / AOC_LOOT_RARITY_BOOST).
    double  rarity_boost = 1.0;
    stats::SplitMix* rng = nullptr;
    /// Nodes 1, stage 1: where the kill is.  The node reads of the tables' expressions (GetNodeBiome, GetCurrentNode, GetNodeRecord, Node_GetLevel ...) are
    /// answered over `nodes` (null: a world with no nodes) in the zone of influence of the creature's body: `node` is the rank of that zone's node (-1:
    /// none) and `biome` its biome (EBiomeType; Default for none).  The player is read as being in the same zone.
    const world::CityNodes* nodes = nullptr;
    int     node = -1;
    int     biome = 0;
};

class RewardBook {
public:
    /// `catalog` (its programs run the expressions) must outlive the book; `decay_expr` is the xp_rule "adventuring_decay" (0: no decay).
    RewardBook(const abilities::Catalog& catalog, std::map<int64_t, content::LootTable> tables, int64_t decay_expr)
        : catalog_(catalog), tables_(std::move(tables)), decay_expr_(decay_expr) {}

    size_t tables() const { return tables_.size(); }
    const content::LootTable* table(int64_t guid) const {
        const auto it = tables_.find(guid);
        return it == tables_.end() ? nullptr : &it->second;
    }

    /// Item rarities, step 1: what a KILL's rolls need to give an item its quality (a harvest carries its own, see GatherRoll): the Quality formula of each item variation (the variation's
    /// guid -> the ab_expr id of the formula, content::GatherData::quality_expr) and the rarity an item shows at a quality (items/rarity.h).  The creature's own numbers run the formula, so
    /// its elite rank adds to a gear roll.  A book given no rules rolls no quality: its items keep the item record's.
    void set_quality_rules(std::map<int64_t, int64_t> quality_expr, std::function<int(int64_t item, int quality)> rarity_of) {
        quality_expr_ = std::move(quality_expr);
        rarity_of_ = std::move(rarity_of);
    }
    size_t quality_formulas() const { return quality_expr_.size(); }
    /// The ab_expr id of the Quality formula of an item variation; 0 for a variation that has none and for a book given no rules.
    int64_t quality_program(int64_t variation) const {
        const auto it = quality_expr_.find(variation);
        return it == quality_expr_.end() ? 0 : it->second;
    }
    /// The rarity (0 Common ... 6 Artifact) the client shows for an item of `item` at `quality`; the common quality curve held nowhere when the book was given no function.
    int rarity_of(int64_t item, int quality) const;

    /// Rolls the tables `roots` for one death.
    Rolled roll(const std::vector<int64_t>& roots, RollContext& ctx) const;

    /// What `amount` experience of a creature is worth to a player of `ctx.player_level`: the decay of the level difference, rounded.
    int64_t xp_for(int64_t amount, RollContext& ctx) const;

private:
    const abilities::Catalog&               catalog_;
    std::map<int64_t, content::LootTable>   tables_;
    int64_t                                 decay_expr_;
    std::map<int64_t, int64_t>              quality_expr_;   // item variation guid -> its Quality formula (a kill's rolls: set_quality_rules)
    std::function<int(int64_t, int)>        rarity_of_;
};

}  // namespace aoc::worldd::combat

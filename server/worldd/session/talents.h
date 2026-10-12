// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/talents.h
//
//  The character's talents (plan D1, stage 2) as pure logic over content.db: the skill trees, what each node needs and each rank costs, the
//  points a level gives, what a purchase request may do, and what the character's ranks make of its KnownAbilities.
//
//  What the retail captures show [R: scratchpad research/stage2/evidence.md]: ServerPurchaseSkillBuild lists ranks of ONE tree with
//  bPurchase 1 or 0 (a respec lists every owned, non-auto rank with bPurchase 0); the tree list the server keeps is the auto-granted ranks
//  of the level and the bought ranks appended in request order; points are earned 1 Archetype and 1 Stamina per level from level 2 and
//  24 Weapon points per line; a rank costs the points its SkillCost lists (a class rank 1 Archetype, a Universal rank 2 or 3 Stamina, a
//  weapon rank 1 to 5 Weapon); an auto-granted rank costs nothing and is never refunded. Every call was answered: no refusal and no
//  partial acceptance is in any capture, so what a hostile or stale request gets is modelled here, not captured.
//
//  Modelled, not captured (the part-2 spec's "Settled in D1, stage 2"):
//    - a node can offer a choice (a SkillTreeSlot, the talents of a class tree and the picks of a weapon tree): each skill the slot offers is a
//      Node of the tree with the node's guid, and the character owns one of a node's options at a time (buying another option is refused until
//      the first is refunded; the captures never show two options of one node owned);
//    - the gates are the records' own: LevelRequirement (the character's level; a weapon line is treated as being at the character's level,
//      as weapon XP is not tracked), PointRequirement (points of any type spent in the tree before the rank), SkillsRequired (any of them,
//      or all with bAllAreRequired; a requirement that names nothing is met), MutuallyExclusiveSkills (either way round), the ranks of one
//      skill in order, and the points of each type the rank costs;
//    - a request is judged whole: its refunds first, then its purchases as a set (a rank whose prerequisite is bought later in the same request
//      waits for it), each against what the request has done so far; one refused entry refuses the request and nothing changes; the tree's list gets
//      the purchases in the order of the request. After refunds, no rank that stays may be left without its prerequisite;
//    - only the character's own class tree, the Universal tree and the weapon trees can be bought (a profession tree, a staging tree or
//      another class's tree is refused), and a rank is looked up in the tree the request names (the tree's Guid, never its Name).
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "protocol/repl/skill_rpc.h"
#include "stats/stat_engine.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>
// (C | Y | B) (CoA)

namespace aoc::worldd::talents {

enum class Kind : uint8_t { Class, Universal, Weapon, Profession, Other };

struct Rank {
    int64_t                        guid = 0;
    int                            ord = 0;
    std::string                    name;
    int64_t                        ability = 0;           // 0: a passive
    int64_t                        effect = 0;            // 0: no effect
    bool                           effect_infinite = false;
    double                         effect_duration = 0;   // seconds, when finite
    std::vector<content::SkillCost> costs;
    std::vector<stats::StatMod>    mods;                  // the constant stat mods of an infinite effect
};

struct Node {
    int64_t              guid = 0;
    int                  ord = 0;
    std::string          name;
    int64_t              skill = 0;
    std::string          skill_name;
    int                  level_requirement = 0;
    int                  point_requirement = 0;
    bool                 auto_granted = false;
    bool                 auto_hotbar = false;
    bool                 all_required = false;
    int                  opt = 0;                         // a slot node (a talent choice) is one Node per option, all with this node's guid and ord
    int64_t              slot = 0;                        // the SkillTreeSlot of a slot node; 0 for a plain node
    std::vector<int64_t> requires_skills;
    std::vector<int64_t> requires_nodes;                  // slot nodes of the tree: a requirement met by any option of the node that is owned
    std::vector<int64_t> exclusive_skills;
    std::vector<Rank>    ranks;                           // by ord
};

struct Tree {
    int64_t           guid = 0;
    std::string       name;
    Kind              kind = Kind::Other;
    int64_t           leveling_type = 0;
    int64_t           class_guid = 0;
    int64_t           weapon_line = 0;
    std::vector<Node> nodes;                              // in the tree's order
};

/// A rank found in a tree.
struct RankRef {
    const Tree* tree = nullptr;
    const Node* node = nullptr;
    const Rank* rank = nullptr;
    bool ok() const { return rank != nullptr; }
    /// An auto-granted node's first rank: a level grants it and it is never bought or refunded.
    bool is_auto() const { return node && rank && node->auto_granted && rank->ord == 0; }
};

/// The point types the rules read.
enum class Point : uint8_t { Archetype, Stamina, Weapon };

class Catalog {
public:
    /// From content.db's rows; nullopt (with `error`) when the data lacks what the rules need: the three point types, the Universal tree,
    /// and a class tree for each class row it lists.
    static std::optional<Catalog> from(content::TalentData data, std::string& error);

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    Catalog(Catalog&&) noexcept = default;
    Catalog& operator=(Catalog&&) noexcept = default;

    const Tree* tree(int64_t guid) const;
    /// A tree by its name ("Bard", "Universal", "Weapon_1H_Scepter"); null for a name that is none (names are unique among the trees that are loaded).
    const Tree* tree_named(const std::string& name) const;
    const std::vector<Tree>& trees() const { return trees_; }
    /// The class tree of a class record (the class's ActiveSkillTreeId); null for a guid that is no class.
    const Tree* class_tree(int64_t class_guid) const;
    const Tree* universal() const { return universal_; }
    /// The profession trees that hold an auto-granted node a character has from the start (the gathering trees: Mining, Herbalism,
    /// Lumberjacking, Hunting, Fishing), by tree guid.
    const std::vector<const Tree*>& professions() const { return professions_; }
    /// The rank `rank_guid` of the tree `tree_guid` (the tree's node whose skill has that rank); !ok() when the tree has no such rank.
    RankRef find(int64_t tree_guid, int64_t rank_guid) const;
    /// The auto-granted ranks a character of `level` has in `tree`: each auto-granted node's first rank whose level requirement is
    /// reached, by level (a level-0 node with the level-1 ones) and then the node's place in the tree.
    std::vector<RankRef> auto_ranks(const Tree& tree, int level) const;
    int64_t point_type(Point p) const { return points_[static_cast<size_t>(p)]; }
    /// The points of a type a leveling type has given by `level`.
    int64_t earned(int64_t leveling_type, int level, int64_t point_type) const;
    size_t  tree_count() const { return trees_.size(); }

private:
    Catalog() = default;
    std::vector<Tree>                trees_;
    std::map<int64_t, size_t>        by_guid_;
    std::map<int64_t, size_t>        by_class_;
    const Tree*                      universal_ = nullptr;
    std::vector<const Tree*>         professions_;
    int64_t                          points_[3] = {0, 0, 0};
    std::map<int64_t, std::map<int, std::map<int64_t, int64_t>>> level_points_;
};

/// The most entries one request may carry (a respec of the biggest retail tree is 49; the codec's own bound is 1,024).
inline constexpr size_t kMaxBuildChanges = 256;

/// One entry of a judged request.
struct Change {
    RankRef ref;
    bool    purchase = false;
};

/// What a request does, or why it does nothing.
struct Plan {
    bool                                    ok = false;
    std::string                             refusal;
    std::vector<int64_t>                    trees;     // the trees it touches, in the order the request names them
    std::vector<Change>                     changes;   // the refunds in request order, then the purchases in request order
    std::map<int64_t, std::vector<int64_t>> after;     // the bought ranks of each touched tree once it is applied
};

/// The character's ranks: what it bought, per tree in the order it bought them (the stored form: auto-granted ranks are the level's, not
/// stored), and what the rules need to know of the character.
class Book {
public:
    /// `class_guid` is the character's class record, `level` its Adventuring level.
    Book(const Catalog& catalog, int64_t class_guid, int level) : catalog_(&catalog), class_guid_(class_guid), level_(level) {}
// [C-O-A - C-Y-B]

    const Catalog& catalog() const { return *catalog_; }
    int64_t class_guid() const { return class_guid_; }
    int     level() const { return level_; }
    void    set_level(int level) { level_ = level; }
    void    set_class(int64_t class_guid) { class_guid_ = class_guid; }
    const Tree* class_tree() const { return catalog_->class_tree(class_guid_); }

    /// The bought ranks per tree; a tree with none is absent.
    const std::map<int64_t, std::vector<int64_t>>& purchased() const { return purchased_; }
    const std::vector<int64_t>& purchased(int64_t tree) const;
    /// Loads stored ranks, keeping the ones the catalog knows in a tree that can be bought, once each, in order: what was dropped is
    /// counted in `dropped` when it is given. An empty list removes the tree's entry.
    void load(int64_t tree, const std::vector<int64_t>& ranks, size_t* dropped = nullptr);

    /// The tree list the XPTracker shows: the auto-granted ranks of the level, then the bought ranks. Empty for a tree that is none.
    std::vector<int64_t> list(const Tree& tree) const;
    /// The same, as ranks.
    std::vector<RankRef> listed(const Tree& tree) const;
    /// The trees the XPTracker lists at the join, in order: the class tree, the professions with a rank, the Universal tree, then the weapon
    /// trees with a bought rank (by guid). A tree whose list is empty is left out.
    std::vector<const Tree*> listed_trees() const;
    /// The abilities of a tree's listed ranks, in list order.
    std::vector<int64_t> abilities(const Tree& tree) const;
    /// The passives of the ranks that are on the character as effects for the stat engine (plan D2: the one path for every stat mod): one
    /// effect per rank whose infinite effect has constant stat mods (Endurance Training's MaxStamina +10), from the class tree's, the
    /// Universal tree's and the professions', and the weapon trees' of the line `wielded_line` (a weapon passive applies only for the weapon
    /// in hand).
    std::vector<stats::ActiveEffect> passives(int64_t wielded_line) const;
    /// Every skill the character holds, as GetSkillPurchased reads them (the tree and the skill): the listed ranks of every tree it has, the
    /// auto-granted ranks of its level and the bought ones, whichever weapon is in hand.
    std::vector<stats::SkillRank> held_ranks() const;

    /// What is left of each point type in a tree: earned by the character's level minus what the bought ranks cost.
    int64_t points_left(const Tree& tree, int64_t point_type) const;

    /// Judges a request against the character as it is; see the header. Nothing changes: apply() does that.
    Plan plan(const protocol::repl::SkillBuild& build) const;
    void apply(const Plan& plan);

private:
    bool purchasable(const Tree& tree) const;
    std::set<int64_t> owned_skills(const Tree& tree, const std::vector<int64_t>& bought) const;
    int64_t spent(const Tree& tree, const std::vector<int64_t>& bought, int64_t point_type) const;   // point_type 0: of any type
    std::string gate(const RankRef& ref, const std::vector<int64_t>& bought) const;

    const Catalog*                          catalog_;
    int64_t                                 class_guid_;
    int                                     level_;
    std::map<int64_t, std::vector<int64_t>> purchased_;
};

// ---- KnownAbilities ------------------------------------------------------------------------------------------------------------

/// The AbilityComponent's two lists as the join sends them (research evidence section 7): General_Unstuck, the abilities of the ranks the
/// character's trees list (class tree first, the professions', the Universal tree's), the combo of the weapon in hand (0: none), then the
/// static tail (the fishing abilities, Rest, the emotes, Dodge, Guard, Sprint ... 45 of them); `records` is the same list with the combos
/// of the other weapons the character carries (that it does not know) after the combo, before the tail (the captured Bard's: its Book combo is
/// element 12, after the Scepter's at 11).  `item_abilities` (the mounts milestone, stage 3 fix 1) are the abilities the carried items bring (a mount item's
/// Summon_Mount): the records only, after the combos and before the tail, as in the login capture 1198, where the bag's Consumable_*, Summon_Mount and
/// Deploy_Vehicle abilities lie between the weapon combos and General_Rest and are in no KnownAbilities.
struct AbilityLists {
    std::vector<int64_t> known;
    std::vector<int64_t> records;
};

AbilityLists join_abilities(const Book& book, int64_t unstuck, int64_t combo, std::span<const int64_t> carried_combos, std::span<const int64_t> tail,
                            std::span<const int64_t> item_abilities = {});

}  // namespace aoc::worldd::talents

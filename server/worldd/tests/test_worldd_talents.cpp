// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_talents.cpp: the talents catalog, a character's ranks, what a purchase request may do, KnownAbilities and the action bar
//  (worldd plan D1, stage 2). Pure logic: no connection, no session (the session's side is test_worldd_items_session).
//
//  On a synthetic tree (the rules, one at a time):
//    the points               a level gives 1 Archetype and 1 Stamina point from level 2 and a weapon line one Weapon point from level 2; a rank costs what its
//                             record says; a purchase that needs more than is left is refused
//    the gates                level, the points spent in the tree, SkillsRequired (any, or all), a requirement that names nothing, mutually exclusive skills
//                             (either way round), the ranks of one skill in order
//    a request                judged whole (a bad entry refuses the rest, nothing changes), refunds first, a swap, a respec, a refund of an auto-granted rank
//                             or of one that is not bought, a rank twice, an unknown tree or rank, a tree that cannot be bought, too many entries
//    the lists                the XPTracker's list is the level's auto-granted ranks then the bought ones; the abilities and the stat mods of the ranks
//  On the real trees of the fixture (talent_fixture.sql, the real rows of content.db):
//    the catalog              the eight class trees, the Universal tree, the 14 weapon trees, the five gathering trees; the points by level
//    KnownAbilities           a level-1 Bard's lists are the captured Bard's, element for element (57 and 58)
//    purchases                real Bard ranks bought at levels 2 and 3, the point and prerequisite rules on real data
//    talent slots             the Bard's talent choices (nodes that name a SkillTreeSlot, not a skill): each option is a rank of the tree, one option of a node
//                             at a time, a requirement that names a slot node is met by any option
//    retail requests          all 128 ServerPurchaseSkillBuild requests of the retail captures (retail_purchases.h), replayed capture by capture: each is accepted
//  and the readers: talent_data on the fixture and on files whose rows are of the wrong storage class, item_weapon, an old file refused.
//  The action bar: set, clear, the class pages, a slot out of range, the default, placing a skill, removing a class's skills.
// ============================================================================
#include "bootstrap/captured_setup.h"
#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "content_db_fixture.h"
#include "retail_purchases.h"
#include "session/action_bar.h"
#include "session/talents.h"
#include "stats/stat_engine.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace content = aoc::worldd::content;
namespace talents = aoc::worldd::talents;
namespace stats = aoc::worldd::stats;
namespace actionbar = aoc::worldd::actionbar;
namespace repl = aoc::protocol::repl;
namespace cf = content_fixture;
// ~ Cyb :: C | O | A ~

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---- a synthetic catalog ---------------------------------------------------------------------------------------------------------------

namespace syn {
constexpr int64_t kArchetype = 11, kStamina = 12, kWeapon = 13;
constexpr int64_t kAdventuring = 2001, kWeaponType = 2002;
constexpr int64_t kClassTree = 1001, kUniversalTree = 1002, kWeaponTree = 1003, kOtherClassTree = 1004, kProfessionTree = 1005, kStageTree = 1006;
constexpr int64_t kClass = 7001, kOtherClass = 7002, kLine = 8001, kOtherLine = 8002;
// ranks (also the nodes' ids + 100 and the skills' + 200)
constexpr int64_t rA = 3001, rB = 3002, rC = 3003, rD = 3004, rE = 3005, rF = 3006, rG1 = 3007, rG2 = 3008, rH = 3009, rI = 3010, rJ = 3011;
constexpr int64_t rU1 = 3101, rUbuy = 3102, rW = 3201, rOther = 3301, rProf = 3401, rWeaponMod = 3202;
constexpr int64_t kAbilityA = 9001, kAbilityB = 9002, kAbilityC = 9003, kAbilityProf = 9004, kAbilityG = 9005, kEffectE = 9101, kEffectU = 9102, kEffectW = 9103;
constexpr int64_t kStatStamina = 6064631747701440948ll;

content::SkillRankRow rank(int64_t guid, int ord, int64_t ability, int64_t effect, bool infinite, double seconds, std::vector<content::SkillCost> costs,
                           std::vector<content::RankStatMod> mods = {}) {
    content::SkillRankRow r;
    r.guid = guid;
    r.ord = ord;
    r.name = "rank" + std::to_string(guid);
    r.ability = ability;
    r.effect = effect;
    r.effect_infinite = infinite;
    r.effect_duration = seconds;
    r.costs = std::move(costs);
    r.mods = std::move(mods);
    return r;
}

content::SkillNodeRow node(int64_t guid, const char* name, int level, int points, bool autog, bool hotbar, std::vector<content::SkillRankRow> ranks,
                           std::vector<int64_t> requires_skills = {}, bool all = false, std::vector<int64_t> exclusive = {}) {
    content::SkillNodeRow n;
    n.node = guid;
    n.name = name;
    n.skill = guid + 200;
    n.skill_name = name;
    n.level_requirement = level;
    n.point_requirement = points;
    n.auto_granted = autog;
    n.auto_hotbar = hotbar;
    n.all_required = all;
    n.requires_skills = std::move(requires_skills);
    n.exclusive_skills = std::move(exclusive);
    n.ranks = std::move(ranks);
    return n;
}

content::SkillTreeRow tree(int64_t guid, const char* name, const char* kind, int64_t type, int64_t cls, int64_t line, std::vector<content::SkillNodeRow> nodes) {
    content::SkillTreeRow t;
    t.guid = guid;
    t.name = name;
    t.kind = kind;
    t.leveling_type_guid = type;
    t.class_guid = cls;
    t.weapon_line_guid = line;
    int ord = 0;
    for (content::SkillNodeRow& n : nodes) n.ord = ord++;
    t.nodes = std::move(nodes);
    return t;
}

const std::vector<content::SkillCost> one = {{kArchetype, 1}};

content::TalentData data() {
    content::TalentData d;
    d.point_types = {{content::kPointArchetype, kArchetype}, {content::kPointStamina, kStamina}, {content::kPointWeapon, kWeapon}};
    for (int level = 2; level <= 25; ++level) {
        d.points[kAdventuring][level] = {{kArchetype, 1}, {kStamina, 1}};
        d.points[kWeaponType][level] = {{kWeapon, 1}};
    }
    // nodes: the ids are rank - 100 (a node), the skill is the node + 200
    std::vector<content::SkillNodeRow> class_nodes;
    class_nodes.push_back(node(rA - 100, "A_auto", 1, 0, true, true, {rank(rA, 0, kAbilityA, 0, false, 0, {})}));
    class_nodes.push_back(node(rB - 100, "B_needs_A", 0, 0, false, true, {rank(rB, 0, kAbilityB, 0, false, 0, one)}, {rA - 100 + 200}));
    class_nodes.push_back(node(rC - 100, "C_excl_D", 0, 0, false, true, {rank(rC, 0, kAbilityC, 0, false, 0, one)}, {}, false, {rD - 100 + 200}));
    class_nodes.push_back(node(rD - 100, "D_excl_C", 0, 0, false, true, {rank(rD, 0, 0, 0, false, 0, one)}, {}, false, {rC - 100 + 200}));
    class_nodes.push_back(node(rE - 100, "E_two_spent", 0, 2, false, true,
                               {rank(rE, 0, 0, kEffectE, true, 0, one, {{kStatStamina, "MaxStamina", 7.5}})}));
    class_nodes.push_back(node(rF - 100, "F_level5", 5, 0, false, false, {rank(rF, 0, 0, 0, false, 0, one)}));
    class_nodes.push_back(node(rG1 - 100, "G_two_ranks", 0, 0, false, true,
                               {rank(rG1, 0, kAbilityG, 0, false, 0, one), rank(rG2, 1, 0, 0, false, 0, {{kArchetype, 2}})}));
    class_nodes.push_back(node(rH - 100, "H_all_of_B_C", 0, 0, false, false, {rank(rH, 0, 0, 0, false, 0, one)}, {rB - 100 + 200, rC - 100 + 200}, true));
    class_nodes.push_back(node(rI - 100, "I_any_of_B_E", 0, 0, false, false, {rank(rI, 0, 0, 0, false, 0, one)}, {rB - 100 + 200, rE - 100 + 200}, false));
    class_nodes.push_back(node(rJ - 100, "J_empty_requirement", 0, 0, false, false, {rank(rJ, 0, 0, 0, false, 0, one)}, {0}, true));
    class_nodes.push_back(node(1, "A_listed_again", 1, 0, true, true, {rank(rA, 0, kAbilityA, 0, false, 0, {})}));   // the same skill: granted once
    d.trees.push_back(tree(kClassTree, "ClassTree", "class", kAdventuring, kClass, 0, std::move(class_nodes)));
    d.trees.back().nodes.back().skill = rA - 100 + 200;
    d.trees.push_back(tree(kUniversalTree, "Universal", "universal", kAdventuring, 0, 0,
                           {node(rU1 - 100, "U_auto_l2", 2, 0, true, true, {rank(rU1, 0, 0, kEffectU, false, 1.0, {})}),
                            node(rUbuy - 100, "U_buy", 0, 0, false, true, {rank(rUbuy, 0, 0, kEffectU, false, 1.0, {{kStamina, 2}})})}));
    d.trees.push_back(tree(kWeaponTree, "Weapon_Test", "weapon", kWeaponType, 0, kLine,
                           {node(rW - 100, "W_buy", 0, 0, false, true, {rank(rW, 0, 0, kEffectW, false, 0.1, {{kWeapon, 2}})}),
                            node(rWeaponMod - 100, "W_mod", 0, 0, false, false,
                                 {rank(rWeaponMod, 0, 0, kEffectW, true, 0, {{kWeapon, 1}}, {{kStatStamina, "MaxStamina", 3}})})}));
    d.trees.push_back(tree(kOtherClassTree, "OtherClassTree", "class", kAdventuring, kOtherClass, 0,
                           {node(rOther - 100, "Other_buy", 0, 0, false, true, {rank(rOther, 0, 0, 0, false, 0, one)}),
                            node(rOther - 99, "Other_auto", 0, 0, true, true, {rank(rOther + 1, 0, 9900, 0, false, 0, {})})}));
    d.trees.push_back(tree(kProfessionTree, "Mining_Test", "profession", 2003, 0, 0, {node(rProf - 100, "Prof_auto", 0, 0, true, false, {rank(rProf, 0, kAbilityProf, 0, false, 0, {})})}));
    d.trees.push_back(tree(kStageTree, "Stage", "other", kAdventuring, 0, 0, {node(rOther + 50, "Stage_buy", 0, 0, false, true, {rank(rOther + 51, 0, 0, 0, false, 0, one)})}));
    return d;
}
}  // namespace syn

static std::optional<talents::Catalog> synthetic_catalog() {
    std::string error;
    auto c = talents::Catalog::from(syn::data(), error);
    if (!c) std::printf("  [FAIL] the synthetic catalog does not build: %s\n", error.c_str());
    return c;
}

static repl::DesignDataId id_of(int64_t guid, int64_t type) {
    repl::DesignDataId d;
    d.guid = guid;
    d.type_id = type;
    return d;
}

/// A request: rank, tree, bPurchase.
struct Entry {
    int64_t rank, tree;
    bool    purchase;
};
static repl::SkillBuild build_of(std::initializer_list<Entry> entries) {
    repl::SkillBuild b;
    for (const Entry& e : entries) {
        repl::SkillRankChange c;
        c.rank = id_of(e.rank, content::kTypeSkillRank);
        c.tree = id_of(e.tree, content::kTypeSkillTree);
        c.leveling_type = id_of(syn::kAdventuring, content::kTypeLevelingTypeDef);
        c.purchase = e.purchase;
        b.changes.push_back(c);
    }
    return b;
}

static bool refused_with(const talents::Plan& p, const char* part) { return !p.ok && contains(p.refusal, part); }

/// Plans and, when it is fine, applies.
static talents::Plan run(talents::Book& book, std::initializer_list<Entry> entries) {
    talents::Plan p = book.plan(build_of(entries));
    if (p.ok) book.apply(p);
    return p;
}

// ---- tests -----------------------------------------------------------------------------------------------------------------------------

static void test_catalog_synthetic() {
    std::printf("the catalog\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    CHECK(cat->tree_count() == 6 && cat->universal() && cat->universal()->guid == syn::kUniversalTree, "six trees, the Universal tree found by its kind");
    CHECK(cat->class_tree(syn::kClass) && cat->class_tree(syn::kClass)->guid == syn::kClassTree && !cat->class_tree(12345) && cat->class_tree(syn::kOtherClass)->guid == syn::kOtherClassTree,
          "class_tree: a class's tree by the class record, none for a guid that is no class");
    CHECK(cat->professions().size() == 1 && cat->professions()[0]->guid == syn::kProfessionTree, "the profession trees with a rank from the start: the one with an auto-granted node");
    CHECK(cat->find(syn::kClassTree, syn::rB).ok() && cat->find(syn::kClassTree, syn::rB).node->name == std::string("B_needs_A") && !cat->find(syn::kClassTree, syn::rW).ok() &&
              !cat->find(999, syn::rB).ok(),
          "find: a rank in the tree it is in; not in another tree, none in a tree that is none");
    CHECK(cat->find(syn::kClassTree, syn::rG2).rank->ord == 1 && cat->find(syn::kClassTree, syn::rG2).node->name == std::string("G_two_ranks"), "the second rank of a skill is found with its node");
    CHECK(cat->point_type(talents::Point::Archetype) == syn::kArchetype && cat->point_type(talents::Point::Stamina) == syn::kStamina && cat->point_type(talents::Point::Weapon) == syn::kWeapon,
          "the three point types by name");
    CHECK(cat->earned(syn::kAdventuring, 1, syn::kArchetype) == 0 && cat->earned(syn::kAdventuring, 2, syn::kArchetype) == 1 && cat->earned(syn::kAdventuring, 25, syn::kStamina) == 24 &&
              cat->earned(syn::kAdventuring, 99, syn::kStamina) == 24 && cat->earned(syn::kWeaponType, 25, syn::kWeapon) == 24 && cat->earned(777, 25, syn::kWeapon) == 0,
          "earned: nothing at level 1, one of each Adventuring type per level from 2, 24 by level 25, nothing for a type that is none");
    // the auto ranks: by level then node order, a skill granted once
    const auto a1 = cat->auto_ranks(*cat->class_tree(syn::kClass), 1);
    const auto a0 = cat->auto_ranks(*cat->class_tree(syn::kClass), 0);
    CHECK(a1.size() == 1 && a1[0].rank->guid == syn::rA && a0.empty(), "auto_ranks: the class tree's one auto skill at level 1 (listed twice in the data, granted once), none at level 0");
    CHECK(cat->auto_ranks(*cat->universal(), 1).empty() && cat->auto_ranks(*cat->universal(), 2).size() == 1, "the Universal tree's auto node at level 2");
// CoA - C | Y | B

    // a catalog without what the rules need
    std::string error;
    content::TalentData bad = syn::data();
    bad.point_types.erase(content::kPointWeapon);
    CHECK(!talents::Catalog::from(bad, error) && contains(error, content::kPointWeapon), "no Combat_Weapon point type: refused, naming it");
    bad = syn::data();
    bad.trees[1].kind = "other";
    CHECK(!talents::Catalog::from(bad, error) && contains(error, "Universal"), "no Universal tree: refused");
}

static void test_points_and_costs() {
    std::printf("points and costs\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    talents::Book b(*cat, syn::kClass, 1);
    CHECK(b.points_left(*cat->class_tree(syn::kClass), syn::kArchetype) == 0, "a level-1 character has no points");
    auto p = run(b, {{syn::rB, syn::kClassTree, true}});
    CHECK(refused_with(p, "costs 1 point(s) and the character has 0 left"), "buying a rank with no points is refused, and says what is left");
    b.set_level(2);
    p = run(b, {{syn::rB, syn::kClassTree, true}});
    CHECK(p.ok && p.changes.size() == 1 && p.after.at(syn::kClassTree) == std::vector<int64_t>{syn::rB} && b.points_left(*cat->class_tree(syn::kClass), syn::kArchetype) == 0,
          "at level 2 one point buys one rank and none is left");
    p = run(b, {{syn::rC, syn::kClassTree, true}});
    CHECK(refused_with(p, "0 left") && b.purchased(syn::kClassTree).size() == 1, "a second rank is refused: nothing left, nothing changed");
    // a refund gives the point back
    p = run(b, {{syn::rB, syn::kClassTree, false}});
    CHECK(p.ok && b.purchased(syn::kClassTree).empty() && b.points_left(*cat->class_tree(syn::kClass), syn::kArchetype) == 1, "a refund gives the point back");
    // the third rank of a request that the points do not cover refuses the whole request
    b.set_level(3);   // two points
    const auto before = b.purchased();
    p = run(b, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}, {syn::rF + 0, syn::kClassTree, true}});
    CHECK(!p.ok && b.purchased() == before, "three ranks for two points: the whole request is refused and nothing is bought");
    p = run(b, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}});
    CHECK(p.ok && b.purchased(syn::kClassTree) == (std::vector<int64_t>{syn::rB, syn::rC}), "two ranks for two points, kept in the order of the request");
    // the Universal tree's points are Stamina; a weapon tree's are the line's own
    talents::Book u(*cat, syn::kClass, 2);
    p = run(u, {{syn::rUbuy, syn::kUniversalTree, true}});
    CHECK(refused_with(p, "costs 2 point(s)"), "a Universal rank costs 2 Stamina points: one level's worth is not enough");
    u.set_level(3);
    p = run(u, {{syn::rUbuy, syn::kUniversalTree, true}});
    CHECK(p.ok && u.purchased(syn::kUniversalTree) == std::vector<int64_t>{syn::rUbuy} && u.points_left(*cat->universal(), syn::kStamina) == 0 &&
              u.points_left(*cat->class_tree(syn::kClass), syn::kArchetype) == 2,
          "at level 3 two Stamina points buy it; the Archetype points are another pool");
    talents::Book w(*cat, syn::kClass, 2);
    p = run(w, {{syn::rW, syn::kWeaponTree, true}});
    CHECK(refused_with(p, "costs 2 point(s)"), "a weapon rank costs 2 Weapon points");
    w.set_level(25);
    p = run(w, {{syn::rW, syn::kWeaponTree, true}, {syn::rWeaponMod, syn::kWeaponTree, true}});
    CHECK(p.ok && w.points_left(*cat->tree(syn::kWeaponTree), syn::kWeapon) == 24 - 3, "at level 25 both weapon ranks are bought: 3 of the 24 Weapon points are spent");
}

static void test_gates() {
    std::printf("the gates\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    const talents::Tree& ct = *cat->class_tree(syn::kClass);
    talents::Book b(*cat, syn::kClass, 25);   // points are no issue: 24
    // the level
    talents::Book low(*cat, syn::kClass, 4);
    auto p = run(low, {{syn::rF, syn::kClassTree, true}});
    CHECK(refused_with(p, "needs level 5 (the character is level 4)"), "LevelRequirement: a level-5 rank at level 4 is refused");
    low.set_level(5);
    CHECK(run(low, {{syn::rF, syn::kClassTree, true}}).ok, "...and bought at 5");
    // the points spent in the tree
    p = run(b, {{syn::rE, syn::kClassTree, true}});
    CHECK(refused_with(p, "needs 2 point(s) spent in the tree (0 are)"), "PointRequirement: 2 points must be spent in the tree first");
    p = run(b, {{syn::rC, syn::kClassTree, true}});
    p = run(b, {{syn::rE, syn::kClassTree, true}});
    CHECK(refused_with(p, "(1 are)"), "...one spent is not enough");
    CHECK(run(b, {{syn::rB, syn::kClassTree, true}, {syn::rE, syn::kClassTree, true}}).ok, "...the second purchase of one request counts what the first spent");
    // SkillsRequired: B needs A (the auto-granted skill): met by the level's grant
    talents::Book fresh(*cat, syn::kClass, 25);
    CHECK(run(fresh, {{syn::rB, syn::kClassTree, true}}).ok, "SkillsRequired: B needs the auto-granted A, which a level-25 character has");
    talents::Book l1(*cat, syn::kClass, 1);
    // at level 1 A (level 1) is there too, but no points: the refusal is the points', not the prerequisite
    CHECK(refused_with(l1.plan(build_of({{syn::rB, syn::kClassTree, true}})), "0 left"), "...at level 1 the prerequisite is met and the points are the problem");
    // all of B and C, and any of B and E
    talents::Book h(*cat, syn::kClass, 25);
    p = run(h, {{syn::rH, syn::kClassTree, true}});
    CHECK(refused_with(p, "prerequisite"), "bAllAreRequired: H needs B and C, owning neither is refused");
    run(h, {{syn::rB, syn::kClassTree, true}});
    p = run(h, {{syn::rH, syn::kClassTree, true}});
    CHECK(refused_with(p, "prerequisite"), "...owning only B is refused");
    run(h, {{syn::rC, syn::kClassTree, true}});
    CHECK(run(h, {{syn::rH, syn::kClassTree, true}}).ok, "...owning B and C buys it");
    // the purchases of one request are judged as a set: H before the B and C it needs, in one request
    talents::Book set(*cat, syn::kClass, 25);
    p = run(set, {{syn::rH, syn::kClassTree, true}, {syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}});
    CHECK(p.ok && set.purchased(syn::kClassTree) == (std::vector<int64_t>{syn::rH, syn::rB, syn::rC}), "a request that lists H before the B and C it needs is accepted; the list keeps the request's order");
    p = run(set, {{syn::rE, syn::kClassTree, true}, {syn::rD, syn::kClassTree, true}, {syn::rF, syn::kClassTree, true}});
    CHECK(refused_with(p, "excludes") && set.purchased(syn::kClassTree).size() == 3, "...but a rank that is never met is still refused: D excludes the owned C, and nothing of the request is bought");
    talents::Book any(*cat, syn::kClass, 25);
    p = run(any, {{syn::rI, syn::kClassTree, true}});
    CHECK(refused_with(p, "prerequisite"), "any of B or E: owning neither is refused");
    run(any, {{syn::rB, syn::kClassTree, true}});
    CHECK(run(any, {{syn::rI, syn::kClassTree, true}}).ok, "...owning one of them buys it");
    CHECK(run(fresh, {{syn::rJ, syn::kClassTree, true}}).ok, "a requirement that names nothing is met");
    // mutually exclusive, either way round
    talents::Book x(*cat, syn::kClass, 25);
    run(x, {{syn::rC, syn::kClassTree, true}});
    p = run(x, {{syn::rD, syn::kClassTree, true}});
    CHECK(refused_with(p, "excludes a skill the character owns"), "MutuallyExclusiveSkills: D excludes C, which is owned");
    talents::Book y(*cat, syn::kClass, 25);
    run(y, {{syn::rD, syn::kClassTree, true}});
    p = run(y, {{syn::rC, syn::kClassTree, true}});
    CHECK(refused_with(p, "excludes"), "...and the other way round");
    p = run(y, {{syn::rC, syn::kClassTree, true}, {syn::rD, syn::kClassTree, false}});
    CHECK(p.ok && y.purchased(syn::kClassTree) == std::vector<int64_t>{syn::rC}, "a swap in one request (the refund of D first) buys C");
    talents::Book both(*cat, syn::kClass, 25);
    p = run(both, {{syn::rC, syn::kClassTree, true}, {syn::rD, syn::kClassTree, true}});
    CHECK(!p.ok && both.purchased().empty(), "both exclusive ranks in one request: refused whole");
    // the ranks of one skill in order
    talents::Book g(*cat, syn::kClass, 25);
    p = run(g, {{syn::rG2, syn::kClassTree, true}});
    CHECK(refused_with(p, "earlier rank is not bought"), "the second rank before the first is refused");
    CHECK(run(g, {{syn::rG1, syn::kClassTree, true}, {syn::rG2, syn::kClassTree, true}}).ok && g.points_left(ct, syn::kArchetype) == 24 - 3, "the two ranks in order, in one request: 1 + 2 points");
    p = run(g, {{syn::rG1, syn::kClassTree, false}});
    CHECK(refused_with(p, "without its earlier rank") && g.purchased(syn::kClassTree).size() == 2, "refunding the first rank of a skill whose second stays is refused");
    CHECK(run(g, {{syn::rG2, syn::kClassTree, false}, {syn::rG1, syn::kClassTree, false}}).ok && g.purchased(syn::kClassTree).empty(), "refunding both ranks of the skill is fine");
}

static void test_requests() {
    std::printf("what a request may do\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    talents::Book b(*cat, syn::kClass, 25);
    auto p = b.plan(repl::SkillBuild{});
    CHECK(refused_with(p, "no rank"), "an empty request is refused");
    p = run(b, {{syn::rB, syn::kClassTree, true}, {syn::rB, syn::kClassTree, true}});
    CHECK(refused_with(p, "repeats") && b.purchased().empty(), "a rank twice in one request is refused whole");
    run(b, {{syn::rB, syn::kClassTree, true}});
    p = run(b, {{syn::rB, syn::kClassTree, true}});
    CHECK(refused_with(p, "bought already"), "a rank that is bought already");
    p = run(b, {{syn::rA, syn::kClassTree, true}});
    CHECK(refused_with(p, "granted by the level"), "the auto-granted rank cannot be bought");
    p = run(b, {{syn::rA, syn::kClassTree, false}});
    CHECK(refused_with(p, "granted by the level") && refused_with(p, "refunded"), "...nor refunded: no refund of an auto-granted rank");
    p = run(b, {{syn::rC, syn::kClassTree, false}});
    CHECK(refused_with(p, "not bought"), "a refund of a rank that is not bought");
    // ids
    repl::SkillBuild bad = build_of({{syn::rB, syn::kClassTree, true}});
    bad.changes[0].rank.type_id = 12345;
    CHECK(refused_with(b.plan(bad), "no skill rank"), "a rank whose TypeId is none of a skill rank's");
    bad = build_of({{syn::rC, syn::kClassTree, true}});
    bad.changes[0].tree.type_id = content::kTypeSkillRank;
    CHECK(refused_with(b.plan(bad), "no skill tree"), "a tree whose TypeId is none of a skill tree's");
    CHECK(refused_with(b.plan(build_of({{syn::rC, 999999, true}})), "does not know"), "an unknown tree");
    CHECK(refused_with(b.plan(build_of({{999999, syn::kClassTree, true}})), "not in tree"), "an unknown rank");
    CHECK(refused_with(b.plan(build_of({{syn::rW, syn::kClassTree, true}})), "not in tree"), "a rank of another tree, named in this one");
    // the name of the request's tree is not read: a class tree id carries a plain string that differs between builds
    bad = build_of({{syn::rC, syn::kClassTree, true}});
    bad.changes[0].tree.name = repl::NameValue::plain("Prototype_Assassin_SkillTable");
    bad.changes[0].rank.name = repl::NameValue::plain("anything");
    CHECK(b.plan(bad).ok, "the Name of the ids is never read: a tree is its Guid");
    // trees that cannot be bought
    CHECK(refused_with(b.plan(build_of({{syn::rOther, syn::kOtherClassTree, true}})), "cannot be bought here"), "another class's tree");
    CHECK(refused_with(b.plan(build_of({{syn::rProf, syn::kProfessionTree, true}})), "cannot be bought here"), "a profession tree");
    CHECK(refused_with(b.plan(build_of({{syn::rOther + 51, syn::kStageTree, true}})), "cannot be bought here"), "a staging tree");
    talents::Book other(*cat, syn::kOtherClass, 25);
    CHECK(other.plan(build_of({{syn::rOther, syn::kOtherClassTree, true}})).ok, "...but it is bought by a character of that class");
    // size
    repl::SkillBuild huge;
    for (size_t i = 0; i < talents::kMaxBuildChanges + 1; ++i) huge.changes.push_back(build_of({{syn::rB, syn::kClassTree, false}}).changes[0]);
    CHECK(refused_with(b.plan(huge), "at most"), "a request over the cap is refused before anything else");
    // respec: every bought rank refunded in one call; the auto-granted ranks stay
    talents::Book r(*cat, syn::kClass, 25);
    run(r, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}, {syn::rE, syn::kClassTree, true}});
    const std::vector<int64_t> list_before = r.list(*cat->class_tree(syn::kClass));
    CHECK(list_before == (std::vector<int64_t>{syn::rA, syn::rB, syn::rC, syn::rE}), "the tree's list: the auto-granted rank then the bought ones in purchase order");
    p = run(r, {{syn::rE, syn::kClassTree, false}, {syn::rB, syn::kClassTree, false}, {syn::rC, syn::kClassTree, false}});
    CHECK(p.ok && r.purchased().empty() && r.list(*cat->class_tree(syn::kClass)) == std::vector<int64_t>{syn::rA} && build_of({{syn::rB, syn::kClassTree, false}}).refund_only(),
          "a respec: every bought rank refunded, the auto-granted one stays in the list");
    // a refund that leaves a rank without its prerequisite
    talents::Book d(*cat, syn::kClass, 25);
    run(d, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}, {syn::rH, syn::kClassTree, true}});
    p = run(d, {{syn::rB, syn::kClassTree, false}});
    CHECK(refused_with(p, "without its prerequisite") && d.purchased(syn::kClassTree).size() == 3, "refunding B while H, which needs it, stays: refused, nothing changes");
    CHECK(run(d, {{syn::rH, syn::kClassTree, false}, {syn::rB, syn::kClassTree, false}}).ok, "...refunding H with it is fine");
    // refunds come first whatever the order of the request: a swap names the buy first
    talents::Book s(*cat, syn::kClass, 3);   // 2 points
    run(s, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}});
    p = run(s, {{syn::rF + 0, syn::kClassTree, true}, {syn::rB, syn::kClassTree, false}});
    CHECK(refused_with(p, "needs level 5"), "(a swap names the purchase first: the level is judged all the same)");
    p = run(s, {{syn::rG1, syn::kClassTree, true}, {syn::rB, syn::kClassTree, false}});
    CHECK(p.ok && p.changes.size() == 2 && !p.changes[0].purchase && p.changes[1].purchase && s.purchased(syn::kClassTree) == (std::vector<int64_t>{syn::rC, syn::rG1}),
          "a swap: the refund is applied first, so its point pays for the purchase; the changes come refunds first");
    // one request names several trees
    talents::Book m(*cat, syn::kClass, 25);
    p = run(m, {{syn::rB, syn::kClassTree, true}, {syn::rUbuy, syn::kUniversalTree, true}, {syn::rW, syn::kWeaponTree, true}});
    CHECK(p.ok && p.trees == (std::vector<int64_t>{syn::kClassTree, syn::kUniversalTree, syn::kWeaponTree}) && p.after.size() == 3, "a request over three trees: all judged, all applied");
    // a plan changes nothing until it is applied
    talents::Book n(*cat, syn::kClass, 25);
    const auto pl = n.plan(build_of({{syn::rB, syn::kClassTree, true}}));
    CHECK(pl.ok && n.purchased().empty(), "plan() changes nothing; apply() does");
    n.apply(pl);
    CHECK(n.purchased(syn::kClassTree) == std::vector<int64_t>{syn::rB}, "apply() stores the ranks");
    talents::Plan refused;
    n.apply(refused);
    CHECK(n.purchased(syn::kClassTree) == std::vector<int64_t>{syn::rB}, "apply() of a refused plan does nothing");
}
// C Y B x C-O-A

static void test_lists_and_mods() {
    std::printf("the lists, the abilities and the mods\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    talents::Book b(*cat, syn::kClass, 2);
    // the lists: the class tree (auto A), the profession, the Universal tree (auto at 2)
    const auto trees = b.listed_trees();
    CHECK(trees.size() == 3 && trees[0]->guid == syn::kClassTree && trees[1]->guid == syn::kProfessionTree && trees[2]->guid == syn::kUniversalTree,
          "the trees the XPTracker lists: the class tree, the professions with a rank, the Universal tree");
    talents::Book l1(*cat, syn::kClass, 1);
    CHECK(l1.listed_trees().size() == 2 && l1.list(*cat->universal()).empty(), "at level 1 the Universal tree has no rank to list");
    // a bought weapon rank puts its tree on the list
    b.set_level(25);
    run(b, {{syn::rW, syn::kWeaponTree, true}, {syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}});
    CHECK(b.listed_trees().size() == 4 && b.listed_trees().back()->guid == syn::kWeaponTree && b.list(*cat->tree(syn::kWeaponTree)) == std::vector<int64_t>{syn::rW},
          "a weapon tree is listed once it has a bought rank, and its list is only what was bought (no auto ranks)");
    // another class's tree is not on the character
    CHECK(b.list(*cat->tree(syn::kOtherClassTree)).empty(), "another class's tree has no list");
    // abilities
    CHECK(b.abilities(*cat->class_tree(syn::kClass)) == (std::vector<int64_t>{syn::kAbilityA, syn::kAbilityB, syn::kAbilityC}), "the class tree's abilities in list order: the auto-granted, then the bought");
    // mods: the infinite effect's constant mod, a weapon rank's only with its line in hand
    run(b, {{syn::rE, syn::kClassTree, true}, {syn::rWeaponMod, syn::kWeaponTree, true}});
    // (plan D2: the passives are effects for the stat engine, one per rank with constant mods; the test flattens them to the mods)
    const auto flat = [](const std::vector<stats::ActiveEffect>& effects) {
        std::vector<stats::EffectMod> out;
        for (const auto& e : effects) out.insert(out.end(), e.mods.begin(), e.mods.end());
        return out;
    };
    const auto none = flat(b.passives(0));
    const auto with_line = flat(b.passives(syn::kLine));
    CHECK(none.size() == 1 && none[0].stat == syn::kStatStamina && none[0].value == 7.5 && with_line.size() == 2 && with_line[1].value == 3,
          "mods: the class passive's constant always, a weapon rank's while its line is in hand");
    CHECK(flat(b.passives(syn::kOtherLine)).size() == 1, "...not with another weapon line in hand");
    CHECK(b.passives(syn::kLine).size() == 2 && b.passives(syn::kLine)[0].stacks == 1 && b.passives(syn::kLine)[0].effect.type_id == stats::kRefTypeEffect,
          "each is an infinite effect of one stack, referenced as an Effect record");
    // load: what is stored is filtered by the catalog
    talents::Book l(*cat, syn::kClass, 25);
    size_t dropped = 0;
    l.load(syn::kClassTree, {syn::rB, 424242, syn::rA, syn::rB, syn::rC}, &dropped);
    CHECK(l.purchased(syn::kClassTree) == (std::vector<int64_t>{syn::rB, syn::rC}) && dropped == 3, "load keeps the known ranks once each in order; an unknown one, an auto-granted one and a duplicate are dropped");
    dropped = 0;
    l.load(424242, {syn::rB}, &dropped);
    CHECK(dropped == 1 && l.purchased(424242).empty(), "load of a tree that is none drops its ranks");
    l.load(syn::kClassTree, {});
    CHECK(l.purchased().empty(), "load of an empty list removes the tree");
    // a class change keeps each class's ranks under its own tree
    talents::Book c(*cat, syn::kClass, 25);
    run(c, {{syn::rB, syn::kClassTree, true}});
    c.set_class(syn::kOtherClass);
    CHECK(c.list(*cat->class_tree(syn::kClass)).empty() && c.purchased(syn::kClassTree) == std::vector<int64_t>{syn::rB} && c.class_tree()->guid == syn::kOtherClassTree,
          "after a class change the old class tree is not on the character, its bought ranks are kept");
    c.set_class(syn::kClass);
    CHECK(c.list(*cat->class_tree(syn::kClass)) == (std::vector<int64_t>{syn::rA, syn::rB}), "...and come back with the class");
}

// a talent slot (a node that names a SkillTreeSlot): one Node per option, all with the node's guid
static void test_slots_synthetic() {
    std::printf("talent slots, synthetic\n");
    constexpr int64_t rS1 = 3501, rS2a = 3502, rS2b = 3503, rS3 = 3504, rNeedSlot = 3510, rNeedOpt = 3511, rNeedStranger = 3512;
    constexpr int64_t kSlotNode = 400, kSlotGuid = 777;
    content::TalentData d = syn::data();
    const auto option = [&](int64_t skill, int opt, std::vector<content::SkillRankRow> ranks) {
        content::SkillNodeRow n = syn::node(kSlotNode, "Slot", 0, 0, false, true, std::move(ranks));
        n.skill = skill;
        n.skill_name = "option" + std::to_string(skill);
        n.opt = opt;
        n.slot = kSlotGuid;
        return n;
    };
    content::SkillTreeRow& cls = d.trees[0];
    const int ord = static_cast<int>(cls.nodes.size());
    for (auto n : {option(5001, 0, {syn::rank(rS1, 0, syn::kAbilityA + 50, 0, false, 0, syn::one)}),
                   option(5002, 1, {syn::rank(rS2a, 0, 0, 0, false, 0, syn::one), syn::rank(rS2b, 1, 0, 0, false, 0, syn::one)}),
                   option(5003, 2, {syn::rank(rS3, 0, 0, 0, false, 0, syn::one)})}) {
        n.ord = ord;
        cls.nodes.push_back(std::move(n));
    }
    content::SkillNodeRow need_slot = syn::node(rNeedSlot - 100, "NeedsTheSlot", 0, 0, false, false, {syn::rank(rNeedSlot, 0, 0, 0, false, 0, syn::one)});
    need_slot.requires_nodes = {kSlotNode};
    need_slot.ord = ord + 1;
    content::SkillNodeRow need_opt = syn::node(rNeedOpt - 100, "NeedsOption2", 0, 0, false, false, {syn::rank(rNeedOpt, 0, 0, 0, false, 0, syn::one)}, {5002});
    need_opt.ord = ord + 2;
    content::SkillNodeRow need_stranger = syn::node(rNeedStranger - 100, "NeedsAnAbsentSlot", 0, 0, false, false, {syn::rank(rNeedStranger, 0, 0, 0, false, 0, syn::one)});
    need_stranger.requires_nodes = {123456789};
    need_stranger.ord = ord + 3;
    cls.nodes.push_back(std::move(need_slot));
    cls.nodes.push_back(std::move(need_opt));
    cls.nodes.push_back(std::move(need_stranger));
    std::string error;
    auto cat = talents::Catalog::from(std::move(d), error);
    if (!cat) {
        std::printf("  [FAIL] the slot catalog does not build: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    CHECK(cat->find(syn::kClassTree, rS1).ok() && cat->find(syn::kClassTree, rS2b).ok() && cat->find(syn::kClassTree, rS2b).node->skill == 5002 &&
              cat->find(syn::kClassTree, rS2b).rank->ord == 1 && cat->find(syn::kClassTree, rS3).node->guid == kSlotNode,
          "every option of a slot is a rank of the tree, found with its option's node");
    talents::Book b(*cat, syn::kClass, 25);
    auto p = run(b, {{rS1, syn::kClassTree, true}});
    CHECK(p.ok && b.purchased(syn::kClassTree) == std::vector<int64_t>{rS1}, "an option of a slot is bought like any rank");
    p = run(b, {{rS2a, syn::kClassTree, true}});
    CHECK(refused_with(p, "its slot already holds") && b.purchased(syn::kClassTree).size() == 1, "another option of the same slot is refused while the first is owned");
    p = run(b, {{rS1, syn::kClassTree, false}, {rS3, syn::kClassTree, true}});
    CHECK(p.ok && b.purchased(syn::kClassTree) == std::vector<int64_t>{rS3}, "a swap (refund of the owned option, purchase of another) in one request is accepted");
    p = run(b, {{rS3, syn::kClassTree, false}, {rS2a, syn::kClassTree, true}, {rS2b, syn::kClassTree, true}});
    CHECK(p.ok && b.purchased(syn::kClassTree) == (std::vector<int64_t>{rS2a, rS2b}), "both ranks of one option (the second rank follows the first) are bought together");
    talents::Book n(*cat, syn::kClass, 25);
    p = run(n, {{rNeedSlot, syn::kClassTree, true}});
    CHECK(refused_with(p, "prerequisite"), "a requirement that names a slot node is not met while no option of it is owned");
    run(n, {{rS3, syn::kClassTree, true}});
    CHECK(run(n, {{rNeedSlot, syn::kClassTree, true}}).ok, "...and is met by any option of it");
    p = run(n, {{rS3, syn::kClassTree, false}});
    CHECK(refused_with(p, "without its prerequisite") && n.purchased(syn::kClassTree).size() == 2, "refunding the only option owned while a rank needs the slot is refused");
    (void)rNeedOpt;   // (a requirement that names one option of a slot node is read as the node: contentc writes it as requires_nodes)
    CHECK(run(n, {{rNeedStranger, syn::kClassTree, true}}).ok, "a requirement that names a node the tree does not hold is met (the data cannot be bought toward)");
    CHECK(n.list(*cat->class_tree(syn::kClass)).back() == rNeedStranger && n.abilities(*cat->class_tree(syn::kClass)).size() == 1,
          "a picked option is in the tree's list; an option without an ability adds none");
    talents::Book a(*cat, syn::kClass, 25);
    run(a, {{rS1, syn::kClassTree, true}});
    CHECK(a.abilities(*cat->class_tree(syn::kClass)) == (std::vector<int64_t>{syn::kAbilityA, syn::kAbilityA + 50}), "...and the option's ability joins the tree's abilities");
    // a refusal names the entry's Guids
    talents::Book g(*cat, syn::kClass, 25);
    p = g.plan(build_of({{987654321, syn::kClassTree, true}}));
    CHECK(refused_with(p, "entry 1 (rank Guid 987654321, tree Guid 1001, LevelingType Guid 2001) names a rank that is not in tree ClassTree"),
          "the refusal of a rank the tree does not hold names the entry's rank, tree and LevelingType Guids");
}
// C-O-A.Cyb

static void test_known_abilities_synthetic() {
    std::printf("KnownAbilities and the records, synthetic\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    talents::Book b(*cat, syn::kClass, 25);
    run(b, {{syn::rB, syn::kClassTree, true}, {syn::rD, syn::kClassTree, true}});
    const std::vector<int64_t> tail = {501, 502, 503};
    const std::vector<int64_t> carried = {syn::kAbilityC + 1000, 7777};
    const talents::AbilityLists l = talents::join_abilities(b, 100, 4242, carried, tail);
    // General_Unstuck, the class tree's abilities (A auto, B bought; D has none), the profession's, the combo, the tail
    CHECK(l.known == (std::vector<int64_t>{100, syn::kAbilityA, syn::kAbilityB, syn::kAbilityProf, 4242, 501, 502, 503}),
          "known: Unstuck, the class tree's abilities, the professions', the combo of the weapon in hand, the static tail");
    // (the carried combos go after the combo, before the tail: [100, A, B, Prof, 4242, carried..., 501, 502, 503])
    CHECK(l.records == (std::vector<int64_t>{100, syn::kAbilityA, syn::kAbilityB, syn::kAbilityProf, 4242, carried[0], 7777, 501, 502, 503}),
          "records: the same with the combos of the other weapons carried after the combo, before the tail");
    const talents::AbilityLists none = talents::join_abilities(b, 100, 0, {}, tail);
    CHECK(none.known.size() == 7 && none.records == none.known, "no weapon in hand and none carried: no combo, no extra");
    const talents::AbilityLists dup = talents::join_abilities(b, 100, syn::kAbilityA, std::vector<int64_t>{syn::kAbilityA, 501}, tail);
    CHECK(std::count(dup.known.begin(), dup.known.end(), syn::kAbilityA) == 1 && dup.records.size() == dup.known.size(), "an ability twice is listed once, a carried combo already known adds nothing");
    // the abilities the carried items bring (a mount item's Summon_Mount): records only, after the combos and before the tail, once
    const std::vector<int64_t> item_abilities = {9100, 7777, 9100};
    const talents::AbilityLists with_items = talents::join_abilities(b, 100, 4242, carried, tail, item_abilities);
    CHECK(with_items.known == l.known, "an item's ability is in no KnownAbilities (retail's login: Summon_Mount is in the records only)");
    CHECK(with_items.records == (std::vector<int64_t>{100, syn::kAbilityA, syn::kAbilityB, syn::kAbilityProf, 4242, carried[0], 7777, 9100, 501, 502, 503}),
          "the records: the combos carried, then the items' abilities (once each, one already listed adds nothing), then the tail");
    CHECK(talents::join_abilities(b, 100, 4242, carried, tail, {}).records == l.records, "no item ability: the lists are as before");
    // a class change: the old class's abilities are not listed
    b.set_class(syn::kOtherClass);
    const talents::AbilityLists other = talents::join_abilities(b, 100, 0, {}, tail);
    CHECK(other.known == (std::vector<int64_t>{100, 9900, syn::kAbilityProf, 501, 502, 503}), "after a class change: the new class's tree, the old class's abilities gone");
}

// ---- the real trees ---------------------------------------------------------------------------------------------------------------------

struct RealWorld {
    fs::path                          dir;
    std::optional<content::ContentDb> db;
    std::optional<talents::Catalog>   catalog;
    RealWorld() {
        dir = cf::make_temp_dir("worldd_talents_");
        const fs::path path = dir / "content.db";
        if (!cf::make_db(path)) std::exit(2);
        std::string error;
        db = content::ContentDb::open(path.string(), error);
        if (!db) {
            std::printf("  [FAIL] the fixture does not open: %s\n", error.c_str());
            std::exit(2);
        }
        auto data = db->talent_data(error);
        if (!data) {
            std::printf("  [FAIL] talent_data: %s\n", error.c_str());
            std::exit(2);
        }
        catalog = talents::Catalog::from(std::move(*data), error);
        if (!catalog) {
            std::printf("  [FAIL] the catalog: %s\n", error.c_str());
            std::exit(2);
        }
    }
    ~RealWorld() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

static constexpr int64_t kBardClass = 108640158570105ll;
static constexpr int64_t kTankClass = 108640201692798ll;
static constexpr int64_t kScepterCombo = 6064631845843960261ll;
static constexpr int64_t kBookCombo = 6064631012536680449ll;

static void test_real_catalog() {
    std::printf("the real trees of the fixture\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    CHECK(cat.tree_count() == 28, "28 trees: the eight class trees, the Universal tree, the 14 weapon trees and the five gathering trees");
    const talents::Tree* bard = cat.class_tree(kBardClass);
    const talents::Tree* tank = cat.class_tree(kTankClass);
    CHECK(bard && bard->name == "Bard" && bard->guid == 6064629885900499588ll && tank && tank->name == "Tank" && cat.universal() && cat.universal()->guid == content::kGuidUniversalTree,
          "the Bard's, the Tank's and the Universal tree");
    CHECK(cat.professions().size() == 5, "five profession trees have a rank a character has from the start");
    std::vector<std::string> names;
    for (const talents::Tree* t : cat.professions()) names.push_back(t->name);
    CHECK(names == (std::vector<std::string>{"Mining", "Herbalism", "Lumberjacking", "Hunting", "Fishing"}), "...Mining, Herbalism, Lumberjacking, Hunting, Fishing, in that order (the captured Bard's)");
    CHECK(cat.earned(content::kGuidAdventuring, 25, cat.point_type(talents::Point::Archetype)) == 24 && cat.earned(content::kGuidAdventuring, 25, cat.point_type(talents::Point::Stamina)) == 24 &&
              cat.earned(content::kGuidAdventuring, 1, cat.point_type(talents::Point::Archetype)) == 0 && cat.earned(content::kGuidAdventuring, 50, cat.point_type(talents::Point::Archetype)) == 49,
          "the points by level: 24 Archetype and 24 Stamina by level 25 (49 by level 50, which the cap never lets a character reach)");
    // the Bard's four auto-granted skills
    const std::vector<talents::RankRef> autos = cat.auto_ranks(*bard, 25);
    CHECK(autos.size() == 4 && autos[0].node->name == "Bard_Rhetoric" && autos[1].node->name == "Bard_Flourish" && autos[2].node->name == "Bard_Passive_ResonantWeapon" &&
              autos[3].node->name == "Bard_Fortissimo",
          "the Bard's auto-granted ranks by level: Rhetoric (1), Flourish (3), Resonant Weapon (5), Fortissimo (10)");
    CHECK(cat.auto_ranks(*bard, 1).size() == 1 && cat.auto_ranks(*bard, 4).size() == 2 && cat.auto_ranks(*bard, 10).size() == 4, "...at levels 1, 4 and 10: one, two and four");
    // the Ranger's order is the research's: Salvo, Hunt of the Raven, Mark Prey (level 1), then Camouflage (3), Thorns (6), Strider (8), Regeneration (10)
    const talents::Tree* ranger = cat.class_tree(108640156341880ll);
    std::vector<std::string> order;
    if (ranger) {
        for (const talents::RankRef& r : cat.auto_ranks(*ranger, 20)) order.push_back(r.node->name);
    }
    CHECK(order == (std::vector<std::string>{"Ranger_Salvo", "Ranger_HuntOfTheRaven", "Ranger_MarkPrey", "Ranger_Passive_WeaponFocus_Bows", "Ranger_Camouflage", "Ranger_Thorns",
                                             "Ranger_Strider", "Ranger_Regeneration"}),
          "the Ranger's auto-granted ranks by level, then by the tree's order: the retail Ranger's list order (Salvo, Hunt of the Raven, Mark Prey, Camouflage ...)");
    const talents::Tree* summoner = cat.class_tree(108640185046653ll);
    CHECK(summoner && cat.auto_ranks(*summoner, 1).size() == 21 && cat.auto_ranks(*summoner, 0).size() == 19, "the Summoner has 21 auto-granted ranks at level 1, 19 of them at level 0 (the pet commands)");
}

static void test_real_join_lists() {
    std::printf("KnownAbilities of the real Bard: the captured Bard's, element for element\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    talents::Book bard(cat, kBardClass, 1);
    const talents::AbilityLists l = talents::join_abilities(bard, aoc::worldd::bootstrap::general_unstuck(), kScepterCombo, std::vector<int64_t>{kScepterCombo, kBookCombo},
                                                            aoc::worldd::bootstrap::static_known_tail());
    const std::vector<int64_t> captured_known = aoc::worldd::bootstrap::captured_bard_known_abilities();
    const std::vector<aoc::worldd::bootstrap::AbilityRecord> captured_records = aoc::worldd::bootstrap::captured_bard_records_to_load();
    if (l.known != captured_known) {
        std::printf("    derived %zu, captured %zu\n", l.known.size(), captured_known.size());
        for (size_t i = 0; i < std::min(l.known.size(), captured_known.size()); ++i) {
            if (l.known[i] != captured_known[i]) {
                std::printf("    first difference at %zu: %lld against %lld\n", i, static_cast<long long>(l.known[i]), static_cast<long long>(captured_known[i]));
                break;
            }
        }
    }
    CHECK(captured_known.size() == 57 && l.known == captured_known, "a level-1 Bard's KnownAbilities are the captured 57, in the captured order");
    bool same_records = l.records.size() == captured_records.size();
    for (size_t i = 0; same_records && i < l.records.size(); ++i) same_records = l.records[i] == captured_records[i].guid && captured_records[i].type_id == content::kTypeAbilityRecord;
    CHECK(captured_records.size() == 58 && same_records, "...and the records the captured 58 (the Book's combo after the Scepter's), every type the ability record type");
    // a level-10 Bard: Flourish, Resonant Weapon (a passive: no ability) and Fortissimo join, after QuickWit, before the professions
    talents::Book b10(cat, kBardClass, 10);
    const talents::AbilityLists l10 = talents::join_abilities(b10, aoc::worldd::bootstrap::general_unstuck(), kScepterCombo, std::vector<int64_t>{kScepterCombo, kBookCombo},
                                                              aoc::worldd::bootstrap::static_known_tail());
    CHECK(l10.known.size() == 57 + 2 && l10.known[1] == captured_known[1] && l10.known[2] != captured_known[2] && l10.known[3] != captured_known[2],
          "a level-10 Bard knows two more (Flourish and Fortissimo; Resonant Weapon is a passive), after QuickWit");
    // a Tank: its own abilities, the same professions, the same tail
    talents::Book tank(cat, kTankClass, 1);
    const talents::AbilityLists lt = talents::join_abilities(tank, aoc::worldd::bootstrap::general_unstuck(), 0, {}, aoc::worldd::bootstrap::static_known_tail());
    CHECK(lt.known[0] == captured_known[0] && lt.known.size() == 1 + 2 + 9 + 45 && std::equal(lt.known.end() - 45, lt.known.end(), captured_known.end() - 45),
          "a level-1 Tank: Unstuck, Vengeance and Grit, the nine professions' abilities, no combo, the same tail");
    CHECK(std::find(lt.known.begin(), lt.known.end(), captured_known[1]) == lt.known.end(), "...and not the Bard's QuickWit");
}
// [CoA ~ C:Y:B]

static void test_real_purchases() {
    std::printf("real Bard ranks\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    const int64_t bard_tree = cat.class_tree(kBardClass)->guid;
    const auto rank_named = [&](const char* name) -> int64_t {
        for (const talents::Node& n : cat.tree(bard_tree)->nodes) {
            if (n.name == name) return n.ranks.front().guid;
        }
        return 0;
    };
    const int64_t menacing = rank_named("Bard_Music_Menacing"), shielding = rank_named("Bard_Dance_Shielding"), mighty = rank_named("Bard_Passive_Dance_Shielding_Mighty"),
                  conflict = rank_named("Bard_Story_Conflict"), saga = rank_named("Bard_Saga"), instrument = rank_named("Bard_Passive_Instrument");
    CHECK(menacing && shielding && mighty && conflict && saga && instrument, "the ranks are in the fixture");
    talents::Book b(cat, kBardClass, 1);
    auto p = run(b, {{menacing, bard_tree, true}});
    CHECK(refused_with(p, "prerequisite"), "the instrument songs need an Instrument talent picked (a slot of Flute, Fiddle and Drum), which none is yet");
    p = run(b, {{instrument, bard_tree, true}});
    CHECK(p.ok && b.points_left(*cat.tree(bard_tree), cat.point_type(talents::Point::Archetype)) == 0, "the Instrument talent costs no point: a level-1 Bard picks it");
    p = run(b, {{menacing, bard_tree, true}});
    CHECK(refused_with(p, "0 left"), "a level-1 Bard has no points");
    b.set_level(2);
    p = run(b, {{shielding, bard_tree, true}});
    CHECK(refused_with(p, "needs 1 point(s) spent in the tree (0 are)"), "Dance of Shielding needs a point spent in the tree first");
    p = run(b, {{menacing, bard_tree, true}});
    CHECK(p.ok && b.points_left(*cat.tree(bard_tree), cat.point_type(talents::Point::Archetype)) == 0, "Menacing costs the one point of level 2");
    p = run(b, {{shielding, bard_tree, true}});
    CHECK(refused_with(p, "0 left"), "...Shielding then has the point spent and none to pay");
    b.set_level(3);
    p = run(b, {{shielding, bard_tree, true}});
    CHECK(p.ok && b.list(*cat.tree(bard_tree)).size() == 5, "at level 3 Shielding is bought; the list is Rhetoric, Flourish (granted at 3) and the three bought: Instrument, Menacing, Shielding");
    CHECK(b.list(*cat.tree(bard_tree)) == (std::vector<int64_t>{cat.auto_ranks(*cat.tree(bard_tree), 3)[0].rank->guid, cat.auto_ranks(*cat.tree(bard_tree), 3)[1].rank->guid, instrument, menacing,
                                                                  shielding}),
          "(the list is the two auto ranks of level 3 and then the bought, in purchase order)");
    b.set_level(25);
    p = run(b, {{mighty, bard_tree, true}});
    CHECK(refused_with(p, "point(s) spent"), "Mighty needs 4 points spent in the tree: 2 are");
    p = run(b, {{conflict, bard_tree, true}});
    CHECK(refused_with(p, "10 point(s) spent"), "a story needs 10 points spent in the tree");
    p = run(b, {{saga, bard_tree, true}});
    CHECK(refused_with(p, "prerequisite"), "the Saga needs one of the six stories first");
    // ten points spent in the tree: five instrument songs (no gate), four dances and songs that need a point spent, one that needs four
    for (const char* name : {"Bard_Music_Leech", "Bard_Music_Hopeful", "Bard_Music_Pensive", "Bard_Music_Epic", "Bard_Misc_Discordance", "Bard_Dance_Maddening", "Bard_GetOffTheStage",
                              "Bard_Dance_Mesmerizing"}) {
        p = run(b, {{rank_named(name), bard_tree, true}});
        if (!p.ok) std::printf("    %s: %s\n", name, p.refusal.c_str());
    }
    p = run(b, {{mighty, bard_tree, true}});
    CHECK(p.ok, "with ten ranks bought (10 points spent in the tree), Mighty (4 points spent) is bought");
    p = run(b, {{conflict, bard_tree, true}});
    CHECK(p.ok, "...and a story, which needs 10 points spent");
    p = run(b, {{saga, bard_tree, true}});
    CHECK(p.ok, "...and then the Saga, which needs one of the stories");
    p = run(b, {{conflict, bard_tree, false}});
    CHECK(refused_with(p, "without its prerequisite") && b.purchased(bard_tree).size() == 14, "refunding the story while the Saga stays is refused");
    p = run(b, {{saga, bard_tree, false}, {conflict, bard_tree, false}});
    CHECK(p.ok && b.purchased(bard_tree).size() == 12, "...refunding the Saga with it is fine");
}

// The Bard's talents: the nodes that name a SkillTreeSlot (five songs, Instrument, three masteries), each option a rank of the tree.
static void test_real_slots() {
    std::printf("the real Bard's talent slots\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    const talents::Tree* bard = cat.class_tree(kBardClass);
    size_t slot_options = 0;
    std::set<int64_t> slot_nodes;
    for (const talents::Node& n : bard->nodes) {
        if (n.slot == 0) continue;
        ++slot_options;
        slot_nodes.insert(n.guid);
    }
    CHECK(slot_nodes.size() == 9 && slot_options > slot_nodes.size(), "the Bard's tree has nine talent nodes (slots), each with more than one option");
    const auto node_named = [&](const char* name) {
        std::vector<const talents::Node*> out;
        for (const talents::Node& n : bard->nodes) {
            if (n.name == name) out.push_back(&n);
        }
        return out;
    };
    const auto hymn = node_named("Bard_Song_Hymn");
    CHECK(hymn.size() == 2 && hymn[0]->slot != 0 && hymn[0]->slot == hymn[1]->slot && hymn[0]->skill != hymn[1]->skill && hymn[0]->ord == hymn[1]->ord &&
              hymn[0]->point_requirement == 1 && !hymn[0]->ranks.empty() && !hymn[1]->ranks.empty(),
          "the Hymn slot offers two skills, each with its ranks, at the node's place and with its gates");
    if (hymn.size() != 2) return;
    talents::Book b(cat, kBardClass, 25);
    const int64_t first = hymn[0]->ranks.front().guid, second = hymn[1]->ranks.front().guid;
    auto p = run(b, {{first, bard->guid, true}});
    CHECK(refused_with(p, "needs 1 point(s) spent in the tree (0 are)"), "a talent has the node's gates: the Hymn slot needs a point spent in the tree");
    const auto instrument = node_named("Bard_Passive_Instrument"), menacing = node_named("Bard_Music_Menacing");
    CHECK(instrument.size() == 3 && menacing.size() == 1, "the Instrument slot offers three skills (Flute, Fiddle, Drum); Menacing is a plain node");
    if (instrument.empty() || menacing.empty()) return;
    p = run(b, {{menacing[0]->ranks.front().guid, bard->guid, true}});
    CHECK(refused_with(p, "prerequisite"), "Menacing needs an Instrument picked: a requirement that names a slot node");
    p = run(b, {{menacing[0]->ranks.front().guid, bard->guid, true}, {instrument[1]->ranks.front().guid, bard->guid, true}});
    CHECK(p.ok && b.list(*bard).back() == instrument[1]->ranks.front().guid,
          "...and any option of the slot meets it, also when the build lists Menacing before the Fiddle (the client lists a tree's nodes in the tree's order)");
    p = run(b, {{first, bard->guid, true}});
    CHECK(p.ok && b.list(*bard).back() == first, "with a point spent, the first option is bought and is in the tree's list");
    p = run(b, {{second, bard->guid, true}});
    CHECK(refused_with(p, "its slot already holds"), "the second option of the same slot is refused");
    p = run(b, {{first, bard->guid, false}, {second, bard->guid, true}});
    CHECK(p.ok && b.list(*bard).back() == second, "a swap (refund, purchase) in one request moves the pick");
}
// ~ C:Y:B :: C.O.A ~

// All 128 ServerPurchaseSkillBuild requests of the retail captures against a character with the level and the points (a level-25 character: 24
// Archetype, 24 Stamina and, for a weapon line treated as at that level, 24 Weapon points). What the character owned before each call is what the
// server's own answer says it owned after (the tree list) without the call's purchases and with its refunds, so every gate is judged on the real
// state of the real character. Retail answered every one of them: the validator must accept every one, and the list it makes of the tree must be
// the server's own list (the ranks the catalog knows, bought ones in the order of the retail list).
static void test_retail_requests() {
    std::printf("the retail requests\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    size_t accepted = 0, refused = 0, entries = 0, differing_lists = 0;
    for (const retail_purchases::Call& call : retail_purchases::kCalls) {
        const int64_t tree_guid = retail_purchases::kEntries[call.first].tree;
        const talents::Tree* tree = cat.tree(tree_guid);
        if (!tree) {
            std::printf("    capture %d line %d: the tree %lld is not in the catalog\n", call.capture, call.line, static_cast<long long>(tree_guid));
            ++refused;
            continue;
        }
        // the class of the capture: the class tree this call buys in (a Universal or weapon call is any class's)
        talents::Book book(cat, tree->kind == talents::Kind::Class ? tree->class_guid : kBardClass, 25);
        repl::SkillBuild build;
        std::vector<int64_t> before(retail_purchases::kAfter + call.after_first, retail_purchases::kAfter + call.after_first + call.after_count);
        for (size_t i = 0; i < call.count; ++i) {
            const retail_purchases::Entry& e = retail_purchases::kEntries[call.first + i];
            repl::SkillRankChange c;
            c.rank = id_of(e.rank, content::kTypeSkillRank);
            c.tree = id_of(e.tree, content::kTypeSkillTree);
            c.leveling_type = id_of(e.leveling_type, content::kTypeLevelingTypeDef);
            c.purchase = e.purchase;
            build.changes.push_back(c);
            ++entries;
            if (e.purchase) before.erase(std::remove(before.begin(), before.end(), e.rank), before.end());
        }
        for (size_t i = 0; i < call.count; ++i) {
            const retail_purchases::Entry& e = retail_purchases::kEntries[call.first + i];
            if (!e.purchase && std::find(before.begin(), before.end(), e.rank) == before.end()) before.push_back(e.rank);
        }
        book.load(tree_guid, before);
        const talents::Plan plan = book.plan(build);
        if (!plan.ok) {
            ++refused;
            std::printf("    capture %d line %d (%zu entries, tree %s): %s\n", call.capture, call.line, call.count, tree->name.c_str(), plan.refusal.c_str());
            continue;
        }
        ++accepted;
        book.apply(plan);
        std::vector<int64_t> expected;   // the retail list, the ranks the catalog holds as bought ranks
        for (size_t i = 0; i < call.after_count; ++i) {
            const talents::RankRef r = cat.find(tree_guid, retail_purchases::kAfter[call.after_first + i]);
            if (r.ok() && !r.is_auto()) expected.push_back(r.rank->guid);
        }
        if (book.purchased(tree_guid) != expected) {
            ++differing_lists;
            std::printf("    capture %d line %d: our bought ranks (%zu) are not the retail list's (%zu)\n", call.capture, call.line, book.purchased(tree_guid).size(), expected.size());
        }
    }
    CHECK(std::size(retail_purchases::kCalls) == 128 && entries == 411, "the corpus is 128 requests and 411 entries");
    CHECK(refused == 0 && accepted == 128, "every one of the 128 retail requests is accepted");
    CHECK(differing_lists == 0, "...and leaves the tree's bought ranks as the server's own list had them (the order of the rest kept, the purchases after)");
}

static void test_readers() {
    std::printf("the readers\n");
    const fs::path dir = cf::make_temp_dir("worldd_talents_readers_");
    std::string error;
    {
        const fs::path path = dir / "good.db";
        cf::make_db(path);
        auto db = content::ContentDb::open(path.string(), error);
        CHECK(db.has_value(), "the fixture opens");
        if (db) {
            const auto data = db->talent_data(error);
            CHECK(data && data->trees.size() == 28 && data->point_types.size() == 11 && data->point_types.at("Combat_Archetype") == 6064631448935923712ll, "talent_data: 28 trees, 11 point types, Combat_Archetype by name");
            size_t nodes = 0, ranks = 0;
            if (data) {
                for (const auto& t : data->trees) {
                    nodes += t.nodes.size();
                    for (const auto& n : t.nodes) ranks += n.ranks.size();
                }
            }
            CHECK(nodes > 600 && ranks >= nodes - 5, "...with their nodes and the ranks of each node's skill");
            const auto sw = db->item_weapon(6064631131688402948ll, error);
            CHECK(sw && sw->attack_ability == kScepterCombo && sw->skill_line == 6064632329144307267ll && sw->skill_line_name == "Weapon_1H_Scepter", "item_weapon: the starter scepter's combo and weapon line");
            error.clear();
            CHECK(!db->item_weapon(6064629382556287782ll, error) && error.empty(), "item_weapon: a bag is no weapon (nullopt, no error)");
        }
    }
    // a column of another storage class than its table's: refused, as the Go readers refuse
    struct Case {
        const char* name;
        const char* sql;
        const char* part;
    };
    for (const Case& c : {Case{"a node's level as text", "UPDATE skill_node SET level_requirement = 'five' WHERE node_guid = (SELECT min(node_guid) FROM skill_node)", "skill_node"},
                          Case{"a node's level as a fraction", "UPDATE skill_node SET point_requirement = 1.5 WHERE node_guid = (SELECT min(node_guid) FROM skill_node)", "skill_node"},
                          Case{"a rank's ability as text", "UPDATE skill_rank SET ability_guid = 'x' WHERE rank_guid = (SELECT min(rank_guid) FROM skill_rank)", "skill_rank"},
                          Case{"a rank's duration as text", "UPDATE skill_rank SET effect_duration = 'long' WHERE rank_guid = (SELECT min(rank_guid) FROM skill_rank)", "skill_rank"},
                          Case{"a cost of text", "UPDATE skill_rank_cost SET quantity = 'two' WHERE rank_guid = (SELECT min(rank_guid) FROM skill_rank_cost)", "skill_rank_cost"},
                          Case{"a cost out of range", "UPDATE skill_rank_cost SET quantity = 5000 WHERE rank_guid = (SELECT min(rank_guid) FROM skill_rank_cost)", "skill_rank_cost"},
                          Case{"a point as text", "UPDATE leveling_point SET quantity = 'one' WHERE type_guid = (SELECT min(type_guid) FROM leveling_point)", "leveling_point"},
                          Case{"a tree's leveling type as text", "UPDATE skill_tree SET leveling_type_guid = 'x' WHERE guid = (SELECT min(guid) FROM skill_tree)", "skill_tree"},
                          Case{"a requirement as text", "UPDATE skill_node_require SET skill_guid = 'x' WHERE node_guid = (SELECT min(node_guid) FROM skill_node_require)", "skill_node_require"},
                          Case{"a node of a tree that is none", "UPDATE skill_node SET tree_guid = 42 WHERE node_guid = (SELECT min(node_guid) FROM skill_node)", "tree"}}) {
        const fs::path path = dir / "bad.db";
        std::error_code ec;
        fs::remove(path, ec);
        cf::make_db(path);
        sqlite3* raw = nullptr;
        sqlite3_open(path.string().c_str(), &raw);
        cf::exec(raw, c.sql);
        sqlite3_close(raw);
        auto db = content::ContentDb::open(path.string(), error);
        if (!db) {
            std::printf("  [FAIL] %s: the file does not open: %s\n", c.name, error.c_str());
            ++g_fail;
            continue;
        }
        error.clear();
        const auto data = db->talent_data(error);
        const std::string msg = std::string(c.name) + ": refused, naming the table";
        CHECK(!data && contains(error, c.part), msg.c_str());
    }
    // a file without the talent tables is refused at open, with the way to rebuild it
    {
        const fs::path path = dir / "old.db";
        cf::DbOptions o;
        o.talent_tables = false;
        cf::make_db(path, o);
        const auto db = content::ContentDb::open(path.string(), error);
        CHECK(!db && contains(error, "tables this worldd reads") && contains(error, "contentc"), "a content.db built before the talents is refused at open, with how to rebuild it");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
// built by C.O.A with Cyb

static void test_action_bar() {
    std::printf("the action bar\n");
    using namespace actionbar;
    const auto slot = [](int bank, int s, uint8_t type, int64_t ability = 0, int64_t rank = 0) {
        repl::ActionbarSlot x;
        x.bank_index = bank;
        x.slot_index = s;
        x.use.use_type = type;
        x.use.ability_id = ability;
        x.use.skill_rank_id = rank;
        return x;
    };
    Bar b;
    std::string why;
    CHECK(b.empty() && b.slots().empty() && b.first_free() == std::make_pair(0, 0), "an empty bar: the first free slot is bank 0 slot 0");
    CHECK(b.set(slot(0, 0, repl::ActionbarUseType::Skill, 11, 111)) && b.set(slot(4, 3, repl::ActionbarUseType::SwapWeapons, 5)) && b.size() == 2 && b.at(0, 0)->skill_rank_id == 111,
          "set stores a slot");
    CHECK(b.first_free() == std::make_pair(0, 1), "the first free slot of the class pages is after it");
    CHECK(b.set(slot(0, 0, repl::ActionbarUseType::None)) && b.size() == 1 && !b.at(0, 0), "a UseType of None clears the slot");
    CHECK(b.set(slot(0, 0, repl::ActionbarUseType::None)), "...a clear of a slot that is empty is fine");
    // out of range, enumerations
    for (const auto& s : {slot(-1, 0, 1), slot(20, 0, 1), slot(0, -1, 1), slot(0, 16, 1), slot(0, 0, 10), slot(2147483647, 0, 1)}) {
        why.clear();
        CHECK(!b.set(s, &why) && !why.empty(), "a slot outside the banks 0-19 and slots 0-15, or a use type past 9, is refused with a reason");
    }
    repl::ActionbarSlot bad_instigator = slot(0, 2, 1);
    bad_instigator.use.instigator = 4;
    repl::ActionbarSlot bad_quality = slot(0, 2, 3);
    bad_quality.use.item_quality = -1;
    CHECK(!b.set(bad_instigator) && !b.set(bad_quality) && b.size() == 1, "an instigator past 3 and a negative item quality are refused");
    // a rarity or combo button past the wire's 4 bits would make the character service refuse every progress save of the session
    repl::ActionbarSlot bad_rarity = slot(0, 2, 3), bad_combo = slot(0, 2, 2), top_rarity = slot(0, 3, 3), top_combo = slot(0, 4, 2);
    bad_rarity.use.item_rarity = 200;
    bad_combo.use.combo_button = 16;
    top_rarity.use.item_rarity = 15;
    top_combo.use.combo_button = 15;
    why.clear();
    CHECK(!b.set(bad_rarity, &why) && !why.empty() && !b.at(0, 2), "an item rarity of 200 is refused with a reason, nothing stored");
    why.clear();
    CHECK(!b.set(bad_combo, &why) && !why.empty() && !b.at(0, 2) && b.size() == 1, "a combo button of 16 is refused with a reason, nothing stored");
    CHECK(b.set(top_rarity) && b.set(top_combo) && b.size() == 3 && b.set(slot(0, 3, repl::ActionbarUseType::None)) && b.set(slot(0, 4, repl::ActionbarUseType::None)) && b.size() == 1,
          "15 is the highest rarity and combo button: stored (and cleared again)");
    // full
    Bar full;
    size_t placed = 0;
    for (int bank = 0; bank <= kMaxBank && placed < kMaxSlots; ++bank) {
        for (int s = 0; s <= kMaxSlot && placed < kMaxSlots; ++s) placed += full.set(slot(bank, s, 1, 5)) ? 1 : 0;
    }
    CHECK(full.size() == kMaxSlots && !full.set(slot(19, 15, 1, 5)) && full.set(slot(0, 0, 1, 6)), "a bar holds at most kMaxSlots slots: one more is refused, one that replaces a slot is not");
    // placing skills
    Bar p;
    const auto a = p.place_skill(900, 9001);
    const auto c = p.place_skill(901, 9002);
    CHECK(a && a->bank_index == 0 && a->slot_index == 0 && a->use.use_type == repl::ActionbarUseType::Skill && a->use.ability_id == 900 && a->use.skill_rank_id == 9001 &&
              c && c->slot_index == 1,
          "place_skill: the first empty slots of bank 0, a Skill slot with the ability and the rank");
    CHECK(!p.place_skill(900, 9001) && p.has_skill(9001) && !p.has_skill(1), "a rank on the bar is not placed twice");
    for (int i = 0; i < kClassBanks * kClassSlots; ++i) p.place_skill(1000 + i, 20000 + i);
    CHECK(!p.place_skill(5, 5) && p.size() == static_cast<size_t>(kClassBanks * kClassSlots), "a full set of class pages: nothing is placed");
    // removing a class's skills
    const auto freed = p.remove_skills({9001, 20003, 777});
    CHECK(freed.size() == 2 && freed[0] == std::make_pair(0, 0) && p.first_free() == std::make_pair(0, 0), "remove_skills frees the slots of the ranks it is given, in order");
    // a skill moved by the client is on one slot
    Bar m;
    m.place_skill(500, 5000);
    m.set(slot(1, 5, repl::ActionbarUseType::Skill, 500, 5000));
    const std::vector<std::pair<int, int>> others = m.remove_other_copies(5000, 1, 5);
    CHECK(others.size() == 1 && others[0] == std::make_pair(0, 0) && m.size() == 1 && m.at(1, 5), "remove_other_copies leaves the skill on the slot it was just put on");
    // the default
    const Bar d = Bar::defaults({107887054050936ll, content::kGuidSprint, content::kGuidDodge});
    CHECK(d.size() == 4 && d.at(4, 1)->ability_id == 107887054050936ll && d.at(4, 2)->ability_id == content::kGuidSprint && d.at(4, 3)->use_type == repl::ActionbarUseType::SwapWeapons &&
              d.at(4, 3)->ability_id == content::kGuidDodge && d.at(4, 4)->use_type == repl::ActionbarUseType::Ability && d.at(4, 4)->ability_id == content::kGuidDodge,
          "the default weapon bank: Guard, Sprint, the weapon swap (Dodge's id as captured), Dodge");
    const auto sl = d.slots();
    CHECK(sl.size() == 4 && sl[0].slot_index == 1 && sl[3].slot_index == 4, "the slots come by bank then slot");
    // the wire's list: the weapon bank three times in retail's order (1, 2, 4, 3), the rest once
    Bar w = d;
    w.set(slot(0, 0, repl::ActionbarUseType::Skill, 11, 111));
    w.set(slot(2, 5, repl::ActionbarUseType::Ability, 22));
    const auto wire = w.wire_slots();
    CHECK(wire.size() == 2 + 12 && wire[0].bank_index == 0 && wire[1].bank_index == 2, "wire_slots: the class slots once, by bank then slot, ahead of the weapon bank here");
    bool order = true;
    const int expect[] = {1, 2, 4, 3};
    for (size_t i = 0; i < 12; ++i) {
        const repl::ActionbarSlot& x = wire[2 + i];
        order = order && x.bank_index == 4 && x.slot_index == expect[i % 4] && x.use == *d.at(4, x.slot_index);
    }
    CHECK(order, "...then the weapon bank's slots 1, 2, 4, 3 and again twice, each with its stored use");
    Bar after;   // a bank after the weapon bank, and a weapon bank with a missing slot and an extra one
    after.set(slot(4, 2, repl::ActionbarUseType::Ability, 7));
    after.set(slot(4, 6, repl::ActionbarUseType::Ability, 8));
    after.set(slot(16, 0, repl::ActionbarUseType::Ability, 9));
    const auto wire2 = after.wire_slots();
    CHECK(wire2.size() == 3 + 1 + 1 && wire2[0].slot_index == 2 && wire2[1].slot_index == 2 && wire2[2].slot_index == 2 && wire2[3].slot_index == 6 && wire2[4].bank_index == 16,
          "a weapon bank with only slot 2 lists it three times; slot 6, not one of the four, once; the pet bank after it");
    CHECK(Bar().wire_slots().empty() && w.slots().size() == 6, "an empty bar has no wire slots, and the stored slots are not tripled");
}
// Cyb captures, C-O-A code

int main() {
    test_catalog_synthetic();
    test_points_and_costs();
    test_gates();
    test_requests();
    test_lists_and_mods();
    test_slots_synthetic();
    test_known_abilities_synthetic();
    test_real_catalog();
    test_real_join_lists();
    test_real_purchases();
    test_real_slots();
    test_retail_requests();
    test_readers();
    test_action_bar();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

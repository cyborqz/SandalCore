// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/talents.cpp (see talents.h)
// ============================================================================
#include "session/talents.h"

#include <algorithm>

namespace aoc::worldd::talents {

namespace {

Kind kind_of(const std::string& k) {
    if (k == "class") return Kind::Class;
    if (k == "universal") return Kind::Universal;
    if (k == "weapon") return Kind::Weapon;
    if (k == "profession") return Kind::Profession;
    return Kind::Other;
}

const std::vector<int64_t> kNone;

}  // namespace

// ============================================================================================================================
//  the catalog
// ============================================================================================================================

std::optional<Catalog> Catalog::from(content::TalentData data, std::string& error) {
    Catalog c;
    c.level_points_ = std::move(data.points);
    for (const auto& [i, name] : {std::pair{Point::Archetype, content::kPointArchetype}, std::pair{Point::Stamina, content::kPointStamina},
                                   std::pair{Point::Weapon, content::kPointWeapon}}) {
        const auto it = data.point_types.find(name);
        if (it == data.point_types.end()) {
            error = std::string("content.db has no skill point type ") + name;
            return std::nullopt;
        }
        c.points_[static_cast<size_t>(i)] = it->second;
    }
    for (content::SkillTreeRow& row : data.trees) {
        Tree t;
        t.guid = row.guid;
        t.name = std::move(row.name);
        t.kind = kind_of(row.kind);
        t.leveling_type = row.leveling_type_guid;
        t.class_guid = row.class_guid;
        t.weapon_line = row.weapon_line_guid;
        for (content::SkillNodeRow& n : row.nodes) {
            Node node;
            node.guid = n.node;
            node.ord = n.ord;
            node.name = std::move(n.name);
            node.skill = n.skill;
            node.skill_name = std::move(n.skill_name);
            node.level_requirement = n.level_requirement;
            node.point_requirement = n.point_requirement;
            node.auto_granted = n.auto_granted;
            node.auto_hotbar = n.auto_hotbar;
            node.all_required = n.all_required;
            node.opt = n.opt;
            node.slot = n.slot;
            node.requires_skills = std::move(n.requires_skills);
            node.requires_nodes = std::move(n.requires_nodes);
            node.exclusive_skills = std::move(n.exclusive_skills);
            for (content::SkillRankRow& r : n.ranks) {
                Rank rank;
                rank.guid = r.guid;
                rank.ord = r.ord;
                rank.name = std::move(r.name);
                rank.ability = r.ability;
                rank.effect = r.effect;
                rank.effect_infinite = r.effect_infinite;
                rank.effect_duration = r.effect_duration;
                rank.costs = std::move(r.costs);
                for (const content::RankStatMod& m : r.mods) rank.mods.push_back({m.stat_guid, m.value});
                node.ranks.push_back(std::move(rank));
            }
            std::stable_sort(node.ranks.begin(), node.ranks.end(), [](const Rank& a, const Rank& b) { return a.ord < b.ord; });
            t.nodes.push_back(std::move(node));
        }
        c.trees_.push_back(std::move(t));
    }
    for (size_t i = 0; i < c.trees_.size(); ++i) {
        const Tree& t = c.trees_[i];
        c.by_guid_[t.guid] = i;
        if (t.kind == Kind::Class && t.class_guid != 0) c.by_class_[t.class_guid] = i;
        if (t.kind == Kind::Universal) c.universal_ = &t;
    }
    if (!c.universal_) {
        error = "content.db has no Universal skill tree";
        return std::nullopt;
    }
    for (const Tree& t : c.trees_) {
        if (t.kind != Kind::Profession) continue;
        const bool autos = std::any_of(t.nodes.begin(), t.nodes.end(), [](const Node& n) { return n.auto_granted && n.level_requirement <= 1 && !n.ranks.empty(); });
        if (autos) c.professions_.push_back(&t);
    }
    std::sort(c.professions_.begin(), c.professions_.end(), [](const Tree* a, const Tree* b) { return a->guid < b->guid; });
    return c;
}
// C-O-A - C | Y | B

const Tree* Catalog::tree(int64_t guid) const {
    const auto it = by_guid_.find(guid);
    return it == by_guid_.end() ? nullptr : &trees_[it->second];
}

const Tree* Catalog::tree_named(const std::string& name) const {
    for (const Tree& t : trees_) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

const Tree* Catalog::class_tree(int64_t class_guid) const {
    const auto it = by_class_.find(class_guid);
    return it == by_class_.end() ? nullptr : &trees_[it->second];
}

RankRef Catalog::find(int64_t tree_guid, int64_t rank_guid) const {
    const Tree* t = tree(tree_guid);
    if (!t) return {};
    for (const Node& n : t->nodes) {
        for (const Rank& r : n.ranks) {
            if (r.guid == rank_guid) return {t, &n, &r};
        }
    }
    return {};
}

std::vector<RankRef> Catalog::auto_ranks(const Tree& tree, int level) const {
    std::vector<RankRef> out;
    for (const Node& n : tree.nodes) {
        if (!n.auto_granted || n.ranks.empty() || n.level_requirement > level) continue;
        // a node can be listed twice in a tree's data; one skill is granted once
        const bool again = std::any_of(out.begin(), out.end(), [&](const RankRef& r) { return r.node->skill == n.skill; });
        if (again) continue;
        out.push_back({&tree, &n, &n.ranks.front()});
    }
    // by level, a level-0 node with the level-1 ones, then by the node's place in the tree: the order of the retail lists (a Ranger's Salvo, Hunt of the
    // Raven, Mark Prey, then Camouflage at 3, Thorns at 6, Strider at 8; a Bard's Fishing_FishingPole (1), Survey (0), Sport (0) as they stand in the tree)
    std::stable_sort(out.begin(), out.end(), [](const RankRef& a, const RankRef& b) {
        const int la = std::max(1, a.node->level_requirement), lb = std::max(1, b.node->level_requirement);
        return la != lb ? la < lb : a.node->ord < b.node->ord;
    });
    return out;
}

int64_t Catalog::earned(int64_t leveling_type, int level, int64_t point_type) const {
    const auto t = level_points_.find(leveling_type);
    if (t == level_points_.end()) return 0;
    int64_t sum = 0;
    for (const auto& [l, by_type] : t->second) {
        if (l > level) break;
        const auto p = by_type.find(point_type);
        if (p != by_type.end()) sum += p->second;
    }
    return sum;
}

// ============================================================================================================================
//  the book
// ============================================================================================================================

const std::vector<int64_t>& Book::purchased(int64_t tree) const {
    const auto it = purchased_.find(tree);
    return it == purchased_.end() ? kNone : it->second;
}

bool Book::purchasable(const Tree& tree) const {
    switch (tree.kind) {
        case Kind::Universal:
        case Kind::Weapon: return true;
        case Kind::Class: return tree.class_guid == class_guid_;
        default: return false;
    }
}

void Book::load(int64_t tree_guid, const std::vector<int64_t>& ranks, size_t* dropped) {
    const Tree* tree = catalog_->tree(tree_guid);
    purchased_.erase(tree_guid);
    if (!tree) {
        if (dropped) *dropped += ranks.size();
        return;
    }
    std::vector<int64_t> keep;
    for (const int64_t r : ranks) {
        const RankRef ref = catalog_->find(tree_guid, r);
        const bool fine = ref.ok() && !ref.is_auto() && std::find(keep.begin(), keep.end(), r) == keep.end();
        if (fine) keep.push_back(r);
        else if (dropped) ++*dropped;
    }
    if (!keep.empty()) purchased_[tree_guid] = std::move(keep);
}
// ~ C_Y_B & CoA ~

std::vector<RankRef> Book::listed(const Tree& tree) const {
    std::vector<RankRef> out;
    if (tree.kind == Kind::Class && tree.class_guid != class_guid_) return out;   // another class's tree is not on the character
    if (tree.kind == Kind::Other) return out;
    if (tree.kind != Kind::Weapon) out = catalog_->auto_ranks(tree, level_);
    if (tree.kind == Kind::Profession && std::find(catalog_->professions().begin(), catalog_->professions().end(), &tree) == catalog_->professions().end()) out.clear();
    for (const int64_t r : purchased(tree.guid)) {
        const RankRef ref = catalog_->find(tree.guid, r);
        if (ref.ok()) out.push_back(ref);
    }
    return out;
}

std::vector<int64_t> Book::list(const Tree& tree) const {
    std::vector<int64_t> out;
    for (const RankRef& r : listed(tree)) out.push_back(r.rank->guid);
    return out;
}

std::vector<const Tree*> Book::listed_trees() const {
    std::vector<const Tree*> out;
    if (const Tree* c = class_tree(); c && !listed(*c).empty()) out.push_back(c);
    for (const Tree* p : catalog_->professions()) {
        if (!listed(*p).empty()) out.push_back(p);
    }
    if (const Tree* u = catalog_->universal(); u && !listed(*u).empty()) out.push_back(u);
    for (const auto& [guid, ranks] : purchased_) {   // by tree guid
        const Tree* t = catalog_->tree(guid);
        if (t && t->kind == Kind::Weapon && !ranks.empty()) out.push_back(t);
    }
    return out;
}

std::vector<int64_t> Book::abilities(const Tree& tree) const {
    std::vector<int64_t> out;
    for (const RankRef& r : listed(tree)) {
        if (r.rank->ability != 0) out.push_back(r.rank->ability);
    }
    return out;
}

std::vector<stats::ActiveEffect> Book::passives(int64_t wielded_line) const {
    std::vector<stats::ActiveEffect> out;
    for (const Tree* t : listed_trees()) {
        if (t->kind == Kind::Weapon && t->weapon_line != wielded_line) continue;
        for (const RankRef& r : listed(*t)) {
            if (r.rank->mods.empty()) continue;
            stats::ActiveEffect e;
            e.effect = {stats::kRefTypeEffect, r.rank->effect};
            e.stacks = 1;
            for (const stats::StatMod& m : r.rank->mods) e.mods.push_back({m.stat, m.value});
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::vector<stats::SkillRank> Book::held_ranks() const {
    std::vector<stats::SkillRank> out;
    for (const Tree* t : listed_trees()) {
        for (const RankRef& r : listed(*t)) {
            // a formula names a held skill (GetSkillPurchased(tree, skill): most of them) or one rank of it (a talent's upgrade): both are held
            out.push_back({{stats::kRefTypeSkillTree, t->guid}, {stats::kRefTypeSkill, r.node->skill}});
            out.push_back({{stats::kRefTypeSkillTree, t->guid}, {stats::kRefTypeSkillRank, r.rank->guid}});
        }
    }
    return out;
}
// C O A : C:Y:B

std::set<int64_t> Book::owned_skills(const Tree& tree, const std::vector<int64_t>& bought) const {
    std::set<int64_t> out;
    if (tree.kind != Kind::Weapon) {
        for (const RankRef& r : catalog_->auto_ranks(tree, level_)) out.insert(r.node->skill);
    }
    for (const int64_t r : bought) {
        const RankRef ref = catalog_->find(tree.guid, r);
        if (ref.ok()) out.insert(ref.node->skill);
    }
    return out;
}

int64_t Book::spent(const Tree& tree, const std::vector<int64_t>& bought, int64_t point_type) const {
    int64_t sum = 0;
    for (const int64_t r : bought) {
        const RankRef ref = catalog_->find(tree.guid, r);
        if (!ref.ok()) continue;
        for (const content::SkillCost& c : ref.rank->costs) {
            if (point_type == 0 || c.point_type == point_type) sum += c.quantity;
        }
    }
    return sum;
}

int64_t Book::points_left(const Tree& tree, int64_t point_type) const {
    return catalog_->earned(tree.leveling_type, level_, point_type) - spent(tree, purchased(tree.guid), point_type);
}

namespace {

/// Whether a slot node of `tree` has one of its options in `owned`; a node that is not in the tree has none to own, so the requirement that
/// names it is met (the records name nodes of other trees now and then: such a requirement could never be met by buying).
bool slot_node_owned(const Tree& tree, int64_t node, const std::set<int64_t>& owned) {
    bool listed = false;
    for (const Node& other : tree.nodes) {
        if (other.guid != node) continue;
        listed = true;
        if (owned.count(other.skill)) return true;
    }
    return !listed;
}

/// Whether the requirements of a node are met by `owned` (a requirement that names no skill is met; one that names a slot node is met by any
/// option of it that is owned).
bool requirements_met(const Tree& tree, const Node& n, const std::set<int64_t>& owned) {
    if (n.requires_skills.empty() && n.requires_nodes.empty()) return true;
    for (const int64_t r : n.requires_skills) {
        const bool met = r == 0 || owned.count(r) != 0;
        if (n.all_required && !met) return false;
        if (!n.all_required && met) return true;
    }
    for (const int64_t r : n.requires_nodes) {
        const bool met = slot_node_owned(tree, r, owned);
        if (n.all_required && !met) return false;
        if (!n.all_required && met) return true;
    }
    return n.all_required;
}

}  // namespace

std::string Book::gate(const RankRef& ref, const std::vector<int64_t>& bought) const {
    const Node& n = *ref.node;
    const Tree& tree = *ref.tree;
    if (level_ < n.level_requirement) {
        return "needs level " + std::to_string(n.level_requirement) + " (the character is level " + std::to_string(level_) + ")";
    }
    if (const int64_t in_tree = spent(tree, bought, 0); in_tree < n.point_requirement) {
        return "needs " + std::to_string(n.point_requirement) + " point(s) spent in the tree (" + std::to_string(in_tree) + " are)";
    }
    const std::set<int64_t> owned = owned_skills(tree, bought);
    if (!requirements_met(tree, n, owned)) return "its prerequisite skill is not owned";
    if (n.slot != 0) {   // a talent choice: one option of the node at a time
        for (const int64_t r : bought) {
            const RankRef held = catalog_->find(tree.guid, r);
            if (held.ok() && held.node->guid == n.guid && held.node->skill != n.skill) {
                return "its slot already holds " + held.node->skill_name + " (refund it to pick another)";
            }
        }
    }
    for (const int64_t ex : n.exclusive_skills) {
        if (owned.count(ex)) return "it excludes a skill the character owns";
    }
    for (const Node& other : tree.nodes) {
        if (owned.count(other.skill) == 0) continue;
        if (std::find(other.exclusive_skills.begin(), other.exclusive_skills.end(), n.skill) != other.exclusive_skills.end()) {
            return "an owned skill (" + other.skill_name + ") excludes it";
        }
    }
    if (ref.rank->ord > 0) {
        const Rank* before = nullptr;
        for (const Rank& r : n.ranks) {
            if (r.ord == ref.rank->ord - 1) before = &r;
        }
        // an auto-granted node's first rank is the level's, not bought
        const bool granted = before && n.auto_granted && before->ord == 0;
        if (!before || (!granted && std::find(bought.begin(), bought.end(), before->guid) == bought.end())) return "the skill's earlier rank is not bought";
    }
    for (const content::SkillCost& c : ref.rank->costs) {
        const int64_t left = catalog_->earned(tree.leveling_type, level_, c.point_type) - spent(tree, bought, c.point_type);
        if (left < c.quantity) {
            return "costs " + std::to_string(c.quantity) + " point(s) and the character has " + std::to_string(left < 0 ? 0 : left) + " left of that type";
        }
    }
    return {};
}
// C Y B / CoA

Plan Book::plan(const protocol::repl::SkillBuild& build) const {
    Plan p;
    const auto refuse = [&](std::string why) {
        p.ok = false;
        p.refusal = std::move(why);
        p.trees.clear();
        p.changes.clear();
        p.after.clear();
        return p;
    };
    if (build.changes.empty()) return refuse("the request lists no rank");
    if (build.changes.size() > kMaxBuildChanges) {
        return refuse("the request lists " + std::to_string(build.changes.size()) + " ranks (at most " + std::to_string(kMaxBuildChanges) + ")");
    }
    std::vector<Change> refunds, buys;
    std::set<std::pair<int64_t, int64_t>> seen;
    for (size_t i = 0; i < build.changes.size(); ++i) {
        const protocol::repl::SkillRankChange& c = build.changes[i];
        // the refusal names the entry's Guids: a rank or a tree the data does not hold can then be found in the records
        const std::string at = "entry " + std::to_string(i + 1) + " (rank Guid " + std::to_string(c.rank.guid) + ", tree Guid " + std::to_string(c.tree.guid) +
                               ", LevelingType Guid " + std::to_string(c.leveling_type.guid) + ")";
        if (c.rank.type_id != content::kTypeSkillRank) return refuse(at + " names a record that is no skill rank");
        if (c.tree.type_id != content::kTypeSkillTree) return refuse(at + " names a record that is no skill tree");
        const Tree* tree = catalog_->tree(c.tree.guid);
        if (!tree) return refuse(at + " names a skill tree this server does not know");
        const RankRef ref = catalog_->find(c.tree.guid, c.rank.guid);
        if (!ref.ok()) return refuse(at + " names a rank that is not in tree " + tree->name);
        if (!purchasable(*tree)) {
            return refuse(at + ": the tree " + tree->name + " cannot be bought here (only the character's own class tree, the Universal tree and the weapon trees can)");
        }
        if (!seen.insert({c.tree.guid, c.rank.guid}).second) return refuse(at + " repeats " + ref.rank->name);
        if (std::find(p.trees.begin(), p.trees.end(), c.tree.guid) == p.trees.end()) p.trees.push_back(c.tree.guid);
        (c.purchase ? buys : refunds).push_back({ref, c.purchase});
    }
    std::map<int64_t, std::vector<int64_t>> work;
    for (const int64_t t : p.trees) work[t] = purchased(t);
    for (const Change& c : refunds) {
        std::vector<int64_t>& list = work[c.ref.tree->guid];
        const auto it = std::find(list.begin(), list.end(), c.ref.rank->guid);
        if (it == list.end()) {
            return refuse(c.ref.is_auto() ? "the rank " + c.ref.rank->name + " is granted by the level and cannot be refunded"
                                          : "the rank " + c.ref.rank->name + " is not bought, so it cannot be refunded");
        }
        list.erase(it);
        p.changes.push_back(c);
    }
    for (const Change& c : buys) {
        const std::vector<int64_t>& list = work[c.ref.tree->guid];
        if (c.ref.is_auto()) return refuse("the rank " + c.ref.rank->name + " is granted by the level, not bought");
        if (std::find(list.begin(), list.end(), c.ref.rank->guid) != list.end()) return refuse("the rank " + c.ref.rank->name + " is bought already");
    }
    // The purchases are judged as a set, not in the order the client lists them: a build can name a rank before the one it needs (the client lists a
    // tree's nodes in the tree's order, and the Bard's instrument songs come before the Instrument they need), so a rank whose gate is not met yet
    // waits for the others, and the request is refused only when some rank is still not met once nothing more can be bought. (Every gate but the
    // exclusions only gets easier with more bought, and the points' total does not depend on the order.) The tree's list gets them in request order.
    {
        std::map<int64_t, std::vector<int64_t>> trial = work;
        std::vector<const Change*> waiting;
        for (const Change& c : buys) waiting.push_back(&c);
        while (!waiting.empty()) {
            bool progress = false;
            std::string first_refusal;
            for (auto it = waiting.begin(); it != waiting.end();) {
                const Change& c = **it;
                std::vector<int64_t>& list = trial[c.ref.tree->guid];
                const std::string why = gate(c.ref, list);
                if (why.empty()) {
                    list.push_back(c.ref.rank->guid);
                    it = waiting.erase(it);
                    progress = true;
                } else {
                    if (first_refusal.empty()) first_refusal = "the rank " + c.ref.rank->name + " " + why;
                    ++it;
                }
            }
            if (!progress) return refuse(first_refusal);
        }
    }
    for (const Change& c : buys) {
        work[c.ref.tree->guid].push_back(c.ref.rank->guid);
        p.changes.push_back(c);
    }
    // after refunds, no rank that stays may lose its prerequisite or sit beside a rank it excludes
    if (!refunds.empty()) {
        for (const int64_t t : p.trees) {
            const Tree& tree = *catalog_->tree(t);
            const std::vector<int64_t>& list = work[t];
            const std::set<int64_t> owned = owned_skills(tree, list);
            for (const int64_t r : list) {
                const RankRef ref = catalog_->find(t, r);
                if (!ref.ok()) continue;
                if (!requirements_met(tree, *ref.node, owned)) return refuse("the refunds would leave " + ref.rank->name + " without its prerequisite");
                if (ref.rank->ord > 0) {   // a later rank of a skill needs the one before it
                    const Rank* before = nullptr;
                    for (const Rank& other : ref.node->ranks) {
                        if (other.ord == ref.rank->ord - 1) before = &other;
                    }
                    const bool granted = before && ref.node->auto_granted && before->ord == 0;
                    if (!before || (!granted && std::find(list.begin(), list.end(), before->guid) == list.end())) {
                        return refuse("the refunds would leave " + ref.rank->name + " without its earlier rank");
                    }
                }
            }
        }
    }
    p.after = std::move(work);
    p.ok = true;
    return p;
}

void Book::apply(const Plan& plan) {
    if (!plan.ok) return;
    for (const auto& [tree, list] : plan.after) {
        if (list.empty()) purchased_.erase(tree);
        else purchased_[tree] = list;
    }
}

// ============================================================================================================================
//  KnownAbilities
// ============================================================================================================================

AbilityLists join_abilities(const Book& book, int64_t unstuck, int64_t combo, std::span<const int64_t> carried_combos, std::span<const int64_t> tail,
                            std::span<const int64_t> item_abilities) {
    AbilityLists out;
    const auto add = [&](std::vector<int64_t>& to, int64_t a) {
        if (a == 0) return;
        if (std::find(to.begin(), to.end(), a) == to.end()) to.push_back(a);
    };
    // everything before the tail: Unstuck, the trees' abilities, the combo of the weapon in hand
    std::vector<int64_t> head;
    add(head, unstuck);
    for (const Tree* t : book.listed_trees()) {
        for (const int64_t a : book.abilities(*t)) add(head, a);
    }
    add(head, combo);
    // the records hold the combos of the other weapons the character carries right after the combo (the captured Bard's Book combo is element 12,
    // after the Scepter's at 11), before the tail
    std::vector<int64_t> extras;
    for (const int64_t c : carried_combos) {
        if (std::find(head.begin(), head.end(), c) == head.end()) add(extras, c);
    }
    out.known = head;
    for (const int64_t a : tail) add(out.known, a);
    out.records = head;
    for (const int64_t a : extras) add(out.records, a);
    for (const int64_t a : item_abilities) add(out.records, a);   // (known to no list: the client loads them with the item)
    for (const int64_t a : tail) {
        if (std::find(out.records.begin(), out.records.end(), a) == out.records.end() && a != 0) out.records.push_back(a);
    }
    return out;
}

}  // namespace aoc::worldd::talents

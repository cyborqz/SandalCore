// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "world/node_buildings.h"

#include "world/elections.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace aoc::worldd::world {
namespace node_buildings {

namespace {

template <typename T, size_t N>
const std::unordered_map<int64_t, const T*>& index_of(const T (&rows)[N], size_t count, int64_t T::*key) {
    static const std::unordered_map<int64_t, const T*> by = [&] {
        std::unordered_map<int64_t, const T*> m;
        for (size_t i = 0; i < count && i < N; ++i) m.emplace(rows[i].*key, &rows[i]);
        return m;
    }();
    return by;
}

template <typename T>
const T* find_in(const std::unordered_map<int64_t, const T*>& m, int64_t guid) {
    if (guid == 0) return nullptr;
    const auto it = m.find(guid);
    return it == m.end() ? nullptr : it->second;
}

const std::unordered_map<int64_t, const TreeNode*>& nodes_by_rank() {
    static const std::unordered_map<int64_t, const TreeNode*> by = [] {
        std::unordered_map<int64_t, const TreeNode*> m;
        for (size_t i = 0; i < kTreeNodes_count; ++i) m.emplace(kTreeNodes[i].rank, &kTreeNodes[i]);
        return m;
    }();
    return by;
}

const std::unordered_map<int64_t, const TreeNode*>& nodes_by_node() {
    static const std::unordered_map<int64_t, const TreeNode*> by = [] {
        std::unordered_map<int64_t, const TreeNode*> m;
        for (size_t i = 0; i < kTreeNodes_count; ++i) m.emplace(kTreeNodes[i].node, &kTreeNodes[i]);
        return m;
    }();
    return by;
}

int32_t scaled(int32_t q, double scale) {
    if (scale == 1.0) return q;
    return static_cast<int32_t>(std::ceil(static_cast<double>(q) * std::max(0.0, scale) - 1e-9));
}

int64_t scaled64(int64_t q, double scale) {
    if (scale == 1.0) return q;
    return static_cast<int64_t>(std::ceil(static_cast<double>(q) * std::max(0.0, scale) - 1e-9));
}

/// What a set of costs asks for together: copper, and each entry (item, alternative, quantity).
struct Bill {
    int64_t copper = 0;
    struct Line {
        int64_t item = 0, alt = 0;
        int32_t quantity = 0;
    };
    std::vector<Line> lines;
    void add(int64_t cost_guid, double scale) {
        const Cost* c = cost(cost_guid);
        if (!c) return;
        copper += scaled64(c->copper, scale);
        for (size_t i = c->first; i < c->first + c->count && i < kCostItems_count; ++i) {
            const int32_t q = scaled(kCostItems[i].quantity, scale);
            if (q > 0) lines.push_back({kCostItems[i].item, kCostItems[i].alt, q});
        }
    }
    void add_items(const CostList& items, int64_t money) {
        copper += money;
        for (const auto& [item, q] : items) {
            if (q > 0) lines.push_back({item, 0, q});
        }
    }
};

bool can_pay_bill(const NodeElection& n, const Bill& bill) {
    if (bill.copper > n.treasury) return false;
    std::map<int64_t, int64_t> left(n.stored.begin(), n.stored.end());
    for (const Bill::Line& l : bill.lines) {
        int64_t want = l.quantity;
        for (const int64_t item : {l.item, l.alt}) {
            if (item == 0 || want <= 0) continue;
            int64_t& have = left[item];
            const int64_t take = std::min(have, want);
            have -= take;
            want -= take;
        }
        if (want > 0) return false;
    }
    return true;
}

int64_t pay_bill(NodeElection& n, const Bill& bill) {
    const int64_t copper = std::min(bill.copper, std::max<int64_t>(0, n.treasury));
    n.treasury -= copper;
    for (const Bill::Line& l : bill.lines) {
        int32_t want = l.quantity;
        for (const int64_t item : {l.item, l.alt}) {
            if (item == 0 || want <= 0) continue;
            const auto it = n.stored.find(item);
            if (it == n.stored.end()) continue;
            const int32_t take = std::min(it->second, want);
            it->second -= take;
            want -= take;
            if (it->second <= 0) n.stored.erase(it);
        }
    }
    return copper;
}

const PlotInfo* info_of(const std::vector<PlotInfo>& plots, int64_t plot) {
    for (const PlotInfo& p : plots) {
        if (p.plot == plot) return &p;
    }
    return nullptr;
}

/// The plot's live state, made from the data's row when nothing happened to it yet.
PlotState& live_of(NodeBuildings& b, const PlotInfo& p) {
    const auto it = b.plots.find(p.plot);
    if (it != b.plots.end()) return it->second;
    PlotState s;
    s.building = p.state == kPlotEmpty ? 0 : p.building;
    s.state = p.state;
    return b.plots.emplace(p.plot, std::move(s)).first->second;
}

void clear_project(PlotState& s) {
    s.exp_state = kExpNone;
    s.exp_type = kExpTypeNone;
    s.exp_rank = 0;
    s.exp_started_ms = s.exp_ends_ms = 0;
    s.exp_given.clear();
    s.votes.clear();
}

/// A purchase's players' phase (or its construction when there is no players' cost or it is skipped).
void start_players_phase(PlotState& s, const Expansion* ex, int64_t now_ms, bool skip_players) {
    s.exp_started_ms = now_ms;
    s.exp_given.clear();
    s.votes.clear();
    if (ex && !skip_players && !cost_items(ex->player_cost).empty()) {
        s.exp_state = kExpPlayerContribution;
        s.exp_ends_ms = ex->player_phase_ms > 0 ? now_ms + ex->player_phase_ms : 0;
    } else {
        s.exp_state = kExpConstruction;
        s.exp_ends_ms = now_ms + (ex ? std::max<int64_t>(0, ex->construction_ms) : 0);
    }
}

bool bought(const PlotState& s, int64_t node) {
    for (const int64_t r : s.ranks) {
        const TreeNode* t = rank_node(r);
        if (t && t->node == node) return true;
    }
    return false;
}

void add_points_of_mod(int64_t mod_guid, std::map<int64_t, int32_t>& out, int sign) {
    const Mod* m = mod(mod_guid);
    if (!m) return;
    for (size_t i = m->first; i < m->first + m->count && i < kModPoints_count; ++i) out[kModPoints[i].type] += sign * kModPoints[i].quantity;
}

void add_rank_points(int64_t rank, std::map<int64_t, int32_t>& out, int sign) {
    const TreeNode* t = rank_node(rank);
    if (!t) return;
    for (size_t i = t->points_first; i < t->points_first + t->points_count && i < kRankPoints_count; ++i) out[kRankPoints[i].type] += sign * kRankPoints[i].quantity;
}

/// The cost list of `cost` as entries with their gifts: (first item, alternative, asked).
std::vector<Bill::Line> entries_of(int64_t cost_guid) {
    Bill b;
    b.add(cost_guid, 1.0);
    return b.lines;
}

/// The players' cost the plot gives to now: its construction's (the map the gifts go to) or its expansion's; null for none.
struct Target {
    int64_t                     cost = 0;
    std::map<int64_t, int32_t>* given = nullptr;
    bool                        expansion = false;
};

}  // namespace

const Building* building(int64_t guid) { return find_in(index_of(kBuildings, kBuildings_count, &Building::guid), guid); }
const Cost* cost(int64_t guid) { return find_in(index_of(kCosts, kCosts_count, &Cost::guid), guid); }
const Expansion* expansion(int64_t guid) { return find_in(index_of(kExpansions, kExpansions_count, &Expansion::guid), guid); }
const Phases* phases(int64_t guid) { return find_in(index_of(kPhases, kPhases_count, &Phases::guid), guid); }
const Mod* mod(int64_t guid) { return find_in(index_of(kMods, kMods_count, &Mod::guid), guid); }
const TreeNode* rank_node(int64_t rank) { return find_in(nodes_by_rank(), rank); }
const TreeNode* tree_node(int64_t node) { return find_in(nodes_by_node(), node); }

std::string point_type_name(int64_t type) {
    for (size_t i = 0; i < kPointTypes_count; ++i) {
        if (kPointTypes[i].guid == type) return kPointTypes[i].name;
    }
    return std::string();
}

const std::vector<int64_t>& node_point_types() {
    static const std::vector<int64_t> types = [] {
        std::vector<int64_t> v;
        for (size_t i = 0; i < kPointTypes_count; ++i) {
            if (std::string(kPointTypes[i].name).rfind("Node_", 0) == 0) v.push_back(kPointTypes[i].guid);
        }
        return v;
    }();
    return types;
}

CostList cost_items(int64_t cost_guid, double scale) {
    CostList out;
    const Cost* c = cost(cost_guid);
    if (!c) return out;
    for (size_t i = c->first; i < c->first + c->count && i < kCostItems_count; ++i) {
        const int32_t q = scaled(kCostItems[i].quantity, scale);
        if (q > 0) out.emplace_back(kCostItems[i].item, q);
    }
    return out;
}

int64_t cost_copper(int64_t cost_guid, double scale) {
    const Cost* c = cost(cost_guid);
    return c ? scaled64(c->copper, scale) : 0;
}

int64_t interpolation_ms(int64_t phases_guid) {
    const Phases* p = phases(phases_guid);
    if (!p) return 0;
    int64_t total = 0;
    for (size_t i = p->first; i < p->first + p->count && i < kStages_count; ++i) total += std::max<int64_t>(0, kStages[i].interpolation_ms);
    return total;
}

bool buyable(const Building& b) { return b.in_manager && b.type != 0 && (b.node_cost != 0 || b.player_cost != 0); }

PlotNow now_of(const NodeBuildings& b, const PlotInfo& p) {
    PlotNow n;
    const auto it = b.plots.find(p.plot);
    if (it == b.plots.end()) {
        n.building = p.building;
        n.state = p.state;
        return n;
    }
    n.live = &it->second;
    n.state = it->second.state;
    n.building = it->second.building != 0 ? it->second.building : (p.empty_building != 0 ? p.empty_building : p.building);
    return n;
}

bool can_pay(const NodeElection& n, int64_t cost_guid, double scale) {
    Bill b;
    b.add(cost_guid, scale);
    return can_pay_bill(n, b);
}

int64_t pay(NodeElection& n, int64_t cost_guid, double scale) {
    Bill b;
    b.add(cost_guid, scale);
    return pay_bill(n, b);
}

std::map<int64_t, int32_t> granted_by(const PlotState* live, int64_t building_guid) {
    std::map<int64_t, int32_t> out;
    if (const Building* bd = building(building_guid)) {
        for (size_t i = bd->mods_first; i < bd->mods_first + bd->mods_count && i < kBuildingMods_count; ++i) add_points_of_mod(kBuildingMods[i], out, +1);
    }
    if (live) {
        for (const int64_t r : live->ranks) {
            const TreeNode* t = rank_node(r);
            const Expansion* ex = t ? expansion(t->expansion) : nullptr;
            if (!ex) continue;
            for (size_t i = ex->mods_first; i < ex->mods_first + ex->mods_count && i < kExpansionMods_count; ++i) add_points_of_mod(kExpansionMods[i], out, +1);
        }
    }
    return out;
}

std::map<int64_t, int32_t> skill_points(const NodeBuildings& b, const std::vector<PlotInfo>& plots) {
    std::map<int64_t, int32_t> out;
    for (const int64_t t : node_point_types()) out[t] = 0;
    for (const PlotInfo& p : plots) {
        const PlotNow n = now_of(b, p);
        if (n.state != kPlotOperational) continue;
        for (const auto& [type, q] : granted_by(n.live, n.building)) out[type] += q;
    }
    for (const auto& [plot, s] : b.plots) {
        for (const int64_t r : s.ranks) add_rank_points(r, out, -1);
        if (s.exp_type == kExpTypePurchase && s.exp_state != kExpNone) add_rank_points(s.exp_rank, out, -1);
    }
    return out;
}

std::vector<int64_t> running_mods(const NodeBuildings& b, const std::vector<PlotInfo>& plots) {
    std::vector<int64_t> out;
    for (const PlotInfo& p : plots) {
        const PlotNow n = now_of(b, p);
        if (n.state != kPlotOperational) continue;
        if (const Building* bd = building(n.building)) {
            for (size_t i = bd->mods_first; i < bd->mods_first + bd->mods_count && i < kBuildingMods_count; ++i) out.push_back(kBuildingMods[i]);
        }
        if (!n.live) continue;
        for (const int64_t r : n.live->ranks) {
            const TreeNode* t = rank_node(r);
            const Expansion* ex = t ? expansion(t->expansion) : nullptr;
            if (!ex) continue;
            for (size_t i = ex->mods_first; i < ex->mods_first + ex->mods_count && i < kExpansionMods_count; ++i) out.push_back(kExpansionMods[i]);
        }
    }
    return out;
}

const char* result_name(Result r) {
    switch (r) {
        case Result::Ok: return "ok";
        case Result::NoPlot: return "no such plot in the node at its level";
        case Result::NotEmpty: return "the plot is not empty";
        case Result::NotCandidate: return "that building cannot stand on the plot";
        case Result::NotForSale: return "that building is not sold in the Building Manager";
        case Result::NoMoney: return "the node cannot pay for it (treasury or stores)";
        case Result::NotOperational: return "the building is not operational";
        case Result::Busy: return "the plot has a project running";
        case Result::NoTree: return "the building has no expansions";
        case Result::NoRank: return "no such expansion for this building";
        case Result::LevelTooLow: return "the node's level is too low";
        case Result::NeedsPoints: return "the building needs more of its expansions first";
        case Result::NeedsPrerequisite: return "an expansion it needs is not built";
        case Result::Excluded: return "an expansion it excludes is built";
        case Result::AlreadyBought: return "the expansion is built already";
        case Result::NoSkillPoints: return "the node lacks the skill points";
        case Result::NotBought: return "the expansion is not built";
        case Result::NeededByAnother: return "another expansion needs it";
        case Result::NoProject: return "no such project is running";
        case Result::NotVoting: return "there is no vote on the plot";
        case Result::AlreadyVoted: return "this account voted already";
        case Result::PointsInUse: return "its skill points are spent on expansions";
        case Result::CannotDemolish: return "that building cannot be demolished";
        case Result::NotDemolishing: return "the building is not being demolished";
        case Result::NotUnderConstruction: return "nothing is being built there";
        case Result::NothingNeeded: return "it needs none of that";
        case Result::BadState: return "no such building state";
    }
    return "?";
}

const char* event_name(Event::Kind k) {
    switch (k) {
        case Event::Built: return "built";
        case Event::TimedOut: return "ran out of time";
        case Event::ExpansionStarted: return "expansion voted in";
        case Event::ExpansionBuilt: return "expansion built";
        case Event::ExpansionFailed: return "expansion ended unbuilt";
        case Event::ExpansionRemoved: return "expansion removed";
        case Event::ExpansionKept: return "expansion kept (the vote failed)";
        case Event::Demolished: return "demolished";
        case Event::Disarray: return "in disarray (upkeep unpaid)";
        case Event::Repairing: return "being repaired";
        case Event::Repaired: return "repaired";
        case Event::Upkeep: return "upkeep paid";
    }
    return "?";
}

Result purchase(NodeElection& n, const PlotInfo& p, int64_t building_guid, int64_t now_ms, bool free, bool skip_players) {
    NodeBuildings& b = n.buildings;
    if (now_of(b, p).state != kPlotEmpty) return Result::NotEmpty;
    if (std::find(p.candidates.begin(), p.candidates.end(), building_guid) == p.candidates.end()) return Result::NotCandidate;
    const Building* bd = building(building_guid);
    if (!bd || !buyable(*bd)) return Result::NotForSale;
    if (!free && !can_pay(n, bd->node_cost)) return Result::NoMoney;
    if (!free) pay(n, bd->node_cost);
    PlotState s;
    s.building = building_guid;
    s.state = kPlotUnderConstruction;
    s.phase_started_ms = now_ms;
    s.phase_ends_ms = bd->can_time_out && bd->construction_ms > 0 ? now_ms + bd->construction_ms : 0;
    if (skip_players || cost_items(bd->player_cost).empty()) {
        for (const auto& [item, q] : cost_items(bd->player_cost)) s.given[item] = q;
        s.complete_ms = now_ms + interpolation_ms(bd->phases);
    }
    b.plots[p.plot] = std::move(s);
    return Result::Ok;
}

namespace {

Target target_of(NodeBuildings& b, const PlotInfo& p, bool create) {
    Target t;
    const PlotNow now = now_of(b, p);
    if (now.state == kPlotUnderConstruction && (!now.live || now.live->complete_ms == 0)) {
        const Building* bd = building(now.building);
        if (!bd || bd->player_cost == 0) return t;
        t.cost = bd->player_cost;
        if (create) t.given = &live_of(b, p).given;
        return t;
    }
    if (now.live && now.live->exp_state == kExpPlayerContribution) {
        const TreeNode* tn = rank_node(now.live->exp_rank);
        const Expansion* ex = tn ? expansion(tn->expansion) : nullptr;
        if (!ex || ex->player_cost == 0) return t;
        t.cost = ex->player_cost;
        t.expansion = true;
        if (create) t.given = &b.plots[p.plot].exp_given;
    }
    return t;
}

}  // namespace

std::vector<NeedRow> progress(const NodeBuildings& b, const PlotInfo& p) {
    std::vector<NeedRow> out;
    const Target t = target_of(const_cast<NodeBuildings&>(b), p, false);
    if (t.cost == 0) return out;
    const PlotNow now = now_of(b, p);
    const std::map<int64_t, int32_t>* given = nullptr;
    if (now.live) given = t.expansion ? &now.live->exp_given : &now.live->given;
    for (const Bill::Line& l : entries_of(t.cost)) {
        NeedRow r;
        r.item = l.item;
        r.asked = l.quantity;
        if (given) {
            const auto it = given->find(l.item);
            if (it != given->end()) r.given = std::min(it->second, l.quantity);
        }
        out.push_back(r);
    }
    return out;
}

CostList needs(const NodeBuildings& b, const PlotInfo& p) {
    CostList out;
    for (const NeedRow& r : progress(b, p)) {
        if (r.asked > r.given) out.emplace_back(r.item, r.asked - r.given);
    }
    return out;
}

CostList contribute(NodeElection& n, const PlotInfo& p, const CostList& offer, int64_t now_ms) {
    CostList taken;
    NodeBuildings& b = n.buildings;
    if (needs(b, p).empty()) return taken;
    const bool fresh = b.plots.find(p.plot) == b.plots.end();
    Target t = target_of(b, p, true);
    if (t.cost == 0 || !t.given) return taken;
    PlotState& s = b.plots[p.plot];
    if (fresh && !t.expansion) {   // a NodeXP project the data starts under construction: its players' phase starts with the first gift
        const Building* bd = building(s.building);
        s.phase_started_ms = now_ms;
        s.phase_ends_ms = bd && bd->can_time_out && bd->construction_ms > 0 ? now_ms + bd->construction_ms : 0;
    }
    std::map<int64_t, int32_t> left;
    for (const auto& [item, q] : offer) {
        if (q > 0) left[item] += q;
    }
    bool complete = true;
    for (const Bill::Line& l : entries_of(t.cost)) {
        int32_t& given = (*t.given)[l.item];
        for (const int64_t item : {l.item, l.alt}) {
            if (item == 0 || given >= l.quantity) continue;
            const auto it = left.find(item);
            if (it == left.end() || it->second <= 0) continue;
            const int32_t take = std::min(it->second, l.quantity - given);
            it->second -= take;
            given += take;
            taken.emplace_back(item, take);
        }
        if (given < l.quantity) complete = false;
    }
    for (auto it = t.given->begin(); it != t.given->end();) it = it->second <= 0 ? t.given->erase(it) : std::next(it);
    if (complete) {
        if (t.expansion) {
            const TreeNode* tn = rank_node(s.exp_rank);
            const Expansion* ex = tn ? expansion(tn->expansion) : nullptr;
            s.exp_state = kExpConstruction;
            s.exp_started_ms = now_ms;
            s.exp_ends_ms = now_ms + (ex ? std::max<int64_t>(0, ex->construction_ms) : 0);
        } else {
            const Building* bd = building(s.building);
            s.complete_ms = now_ms + (bd ? interpolation_ms(bd->phases) : 0);
            if (s.complete_ms == 0) s.complete_ms = 1;
        }
    }
    return taken;
}

Result purchase_expansion(NodeElection& n, const std::vector<PlotInfo>& plots, const PlotInfo& p, int64_t rank, int level, int64_t now_ms, bool free,
                          bool skip_players) {
    NodeBuildings& b = n.buildings;
    const PlotNow now = now_of(b, p);
    if (now.state != kPlotOperational) return Result::NotOperational;
    const Building* bd = building(now.building);
    if (!bd || bd->tree == 0) return Result::NoTree;
    const TreeNode* tn = rank_node(rank);
    if (!tn || tn->tree != bd->tree) return Result::NoRank;
    static const PlotState kNone;
    const PlotState& cur = now.live ? *now.live : kNone;
    if (cur.exp_state != kExpNone) return Result::Busy;
    if (std::find(cur.ranks.begin(), cur.ranks.end(), rank) != cur.ranks.end()) return Result::AlreadyBought;
    if (level < tn->level_requirement) return Result::LevelTooLow;
    int32_t in_tree = 0;
    for (const int64_t r : cur.ranks) {
        const TreeNode* o = rank_node(r);
        if (o && o->tree == tn->tree) ++in_tree;
    }
    if (in_tree < tn->point_requirement) return Result::NeedsPoints;
    if (tn->req_count > 0) {
        size_t met = 0;
        for (size_t i = tn->req_first; i < tn->req_first + tn->req_count && i < kRequires_count; ++i) {
            if (bought(cur, kRequires[i].node)) ++met;
        }
        if (tn->all_required ? met < tn->req_count : met == 0) return Result::NeedsPrerequisite;
    }
    for (size_t i = tn->ex_first; i < tn->ex_first + tn->ex_count && i < kExclusive_count; ++i) {
        for (const int64_t r : cur.ranks) {
            const TreeNode* o = rank_node(r);
            if (o && o->skill == kExclusive[i]) return Result::Excluded;
        }
    }
    const std::map<int64_t, int32_t> points = skill_points(b, plots);
    for (size_t i = tn->points_first; i < tn->points_first + tn->points_count && i < kRankPoints_count; ++i) {
        const auto it = points.find(kRankPoints[i].type);
        if ((it == points.end() ? 0 : it->second) < kRankPoints[i].quantity) return Result::NoSkillPoints;
    }
    if (!free) {
        CostList items;
        for (size_t i = tn->items_first; i < tn->items_first + tn->items_count && i < kRankItems_count; ++i) items.emplace_back(kRankItems[i].item, kRankItems[i].quantity);
        Bill bill;
        bill.add_items(items, tn->copper);
        if (!can_pay_bill(n, bill)) return Result::NoMoney;
        pay_bill(n, bill);
    }
    PlotState& s = live_of(b, p);
    if (s.building == 0) s.building = now.building;
    const Expansion* ex = expansion(tn->expansion);
    s.exp_type = kExpTypePurchase;
    s.exp_rank = rank;
    s.exp_started_ms = now_ms;
    s.exp_given.clear();
    s.votes.clear();
    if (ex && ex->vote_ms > 0 && !skip_players) {   // (a game master's bSkipPlayerPhase skips the citizens' vote too)
        s.exp_state = kExpVote;
        s.exp_ends_ms = now_ms + ex->vote_ms;
    } else {
        start_players_phase(s, ex, now_ms, skip_players);
    }
    return Result::Ok;
}

Result cancel_expansion_purchase(NodeElection& n, const PlotInfo& p) {
    const auto it = n.buildings.plots.find(p.plot);
    if (it == n.buildings.plots.end()) return Result::NoProject;
    PlotState& s = it->second;
    if (s.exp_type != kExpTypePurchase || (s.exp_state != kExpVote && s.exp_state != kExpVoteDelay && s.exp_state != kExpPreInitiation &&
                                           s.exp_state != kExpPlayerContribution && s.exp_state != kExpPostPlayerContribution)) {
        return Result::NoProject;
    }
    clear_project(s);
    return Result::Ok;
}

Result remove_expansion(NodeElection& n, const PlotInfo& p, int64_t rank, int64_t now_ms) {
    const auto it = n.buildings.plots.find(p.plot);
    if (it == n.buildings.plots.end()) return Result::NotBought;
    PlotState& s = it->second;
    if (std::find(s.ranks.begin(), s.ranks.end(), rank) == s.ranks.end()) return Result::NotBought;
    if (s.exp_state != kExpNone) return Result::Busy;
    const TreeNode* tn = rank_node(rank);
    if (!tn) return Result::NoRank;
    for (const int64_t other : s.ranks) {
        const TreeNode* o = rank_node(other);
        if (!o || other == rank) continue;
        for (size_t i = o->req_first; i < o->req_first + o->req_count && i < kRequires_count; ++i) {
            if (kRequires[i].node == tn->node) return Result::NeededByAnother;
        }
    }
    const Expansion* ex = expansion(tn->expansion);
    if (ex && ex->removal_vote_ms > 0) {
        s.exp_type = kExpTypeRemoval;
        s.exp_state = kExpRemovalVote;
        s.exp_rank = rank;
        s.exp_started_ms = now_ms;
        s.exp_ends_ms = now_ms + ex->removal_vote_ms;
        s.votes.clear();
        return Result::Ok;
    }
    s.ranks.erase(std::remove(s.ranks.begin(), s.ranks.end(), rank), s.ranks.end());
    return Result::Ok;
}

Result cancel_expansion_removal(NodeElection& n, const PlotInfo& p) {
    const auto it = n.buildings.plots.find(p.plot);
    if (it == n.buildings.plots.end() || it->second.exp_type != kExpTypeRemoval || it->second.exp_state == kExpNone) return Result::NoProject;
    clear_project(it->second);
    return Result::Ok;
}

Result vote(NodeElection& n, const PlotInfo& p, int64_t account, bool yes) {
    const auto it = n.buildings.plots.find(p.plot);
    if (it == n.buildings.plots.end() || (it->second.exp_state != kExpVote && it->second.exp_state != kExpRemovalVote)) return Result::NotVoting;
    if (it->second.votes.count(account)) return Result::AlreadyVoted;
    it->second.votes[account] = yes;
    return Result::Ok;
}

Result demolish(NodeElection& n, const std::vector<PlotInfo>& plots, const PlotInfo& p, int64_t now_ms) {
    NodeBuildings& b = n.buildings;
    const PlotNow now = now_of(b, p);
    if (now.state != kPlotOperational && now.state != kPlotDisarray) return Result::NotOperational;
    const Building* bd = building(now.building);
    if (!bd || !(buyable(*bd) || p.seeded)) return Result::CannotDemolish;
    if (now.live && now.live->exp_state != kExpNone) return Result::Busy;
    if (now.state == kPlotOperational) {
        const std::map<int64_t, int32_t> points = skill_points(b, plots);
        for (const auto& [type, q] : granted_by(now.live, now.building)) {
            const auto it = points.find(type);
            if ((it == points.end() ? 0 : it->second) - q < 0) return Result::PointsInUse;
        }
    }
    PlotState& s = live_of(b, p);
    if (s.building == 0) s.building = now.building;
    s.state = kPlotDemolition;
    s.phase_started_ms = now_ms;
    s.phase_ends_ms = now_ms + std::max<int64_t>(0, bd->demolition_ms);
    return Result::Ok;
}

Result cancel_demolition(NodeElection& n, const PlotInfo& p) {
    const auto it = n.buildings.plots.find(p.plot);
    if (it == n.buildings.plots.end() || it->second.state != kPlotDemolition) return Result::NotDemolishing;
    it->second.state = kPlotOperational;
    it->second.phase_started_ms = it->second.phase_ends_ms = 0;
    return Result::Ok;
}

Result set_state(NodeElection& n, const PlotInfo& p, int64_t building_guid, uint8_t state, int64_t now_ms) {
    if (state > kPlotDemolition) return Result::BadState;
    const Building* bd = building(building_guid);
    if (building_guid != 0 && !bd) return Result::NotCandidate;
    if (state == kPlotEmpty && p.state == kPlotEmpty) {
        n.buildings.plots.erase(p.plot);
        return Result::Ok;
    }
    PlotState s;
    s.building = state == kPlotEmpty ? 0 : (building_guid != 0 ? building_guid : now_of(n.buildings, p).building);
    s.state = state;
    s.phase_started_ms = now_ms;
    const Building* on = building(s.building);
    if (state == kPlotUnderConstruction && on && on->can_time_out && on->construction_ms > 0) s.phase_ends_ms = now_ms + on->construction_ms;
    if (state == kPlotDemolition) s.phase_ends_ms = now_ms + (on ? std::max<int64_t>(0, on->demolition_ms) : 0);
    if (state == kPlotRepairing) s.phase_ends_ms = now_ms + (on ? std::max<int64_t>(0, on->repair_ms) : 0);
    if (state == kPlotOperational) s.built_ms = now_ms;
    if (const auto it = n.buildings.plots.find(p.plot); it != n.buildings.plots.end() && it->second.building == s.building) s.ranks = it->second.ranks;
    n.buildings.plots[p.plot] = std::move(s);
    return Result::Ok;
}

Result set_phase_end(NodeElection& n, const PlotInfo& p, int64_t now_ms, int64_t seconds) {
    NodeBuildings& b = n.buildings;
    const PlotNow now = now_of(b, p);
    const int64_t at = now_ms + std::max<int64_t>(0, seconds) * 1000;
    if (now.live && now.live->exp_state != kExpNone) {
        b.plots[p.plot].exp_ends_ms = std::max<int64_t>(at, 1);
        return Result::Ok;
    }
    if (now.state == kPlotUnderConstruction) {
        PlotState& s = live_of(b, p);
        if (s.complete_ms != 0) s.complete_ms = std::max<int64_t>(at, 1);
        else s.phase_ends_ms = std::max<int64_t>(at, 1);
        return Result::Ok;
    }
    if (now.state == kPlotDemolition || now.state == kPlotRepairing) {
        b.plots[p.plot].phase_ends_ms = std::max<int64_t>(at, 1);
        return Result::Ok;
    }
    return Result::NoProject;
}

std::vector<Event> advance(NodeElection& n, const std::vector<PlotInfo>& plots, int64_t now_ms, bool force_upkeep, double upkeep_scale) {
    std::vector<Event> out;
    NodeBuildings& b = n.buildings;
    for (auto it = b.plots.begin(); it != b.plots.end();) {
        const int64_t plot = it->first;
        PlotState& s = it->second;
        const Building* bd = building(s.building);
        if (s.state == kPlotUnderConstruction) {
            if (s.complete_ms != 0 && now_ms >= s.complete_ms) {
                Event e{Event::Built, plot, s.building, 0, bd ? bd->completion_xp : 0.0, 0};
                s.state = kPlotOperational;
                s.built_ms = s.complete_ms;
                s.phase_started_ms = s.complete_ms;
                s.phase_ends_ms = 0;
                s.complete_ms = 0;
                s.given.clear();
                out.push_back(e);
            } else if (s.complete_ms == 0 && s.phase_ends_ms != 0 && now_ms >= s.phase_ends_ms) {
                out.push_back(Event{Event::TimedOut, plot, s.building, 0, 0.0, 0});
                s = PlotState{};
            }
        } else if (s.state == kPlotDemolition && now_ms >= s.phase_ends_ms) {
            out.push_back(Event{Event::Demolished, plot, s.building, 0, 0.0, 0});
            s = PlotState{};
        } else if (s.state == kPlotRepairing && now_ms >= s.phase_ends_ms) {
            s.state = kPlotOperational;
            s.phase_started_ms = s.phase_ends_ms;
            s.phase_ends_ms = 0;
            out.push_back(Event{Event::Repaired, plot, s.building, 0, 0.0, 0});
        }
        // the expansion project
        const TreeNode* tn = rank_node(s.exp_rank);
        const Expansion* ex = tn ? expansion(tn->expansion) : nullptr;
        const int64_t exg = tn ? tn->expansion : 0;
        size_t yes = 0, no = 0;
        for (const auto& [a, v] : s.votes) (v ? yes : no) += 1;
        if (s.exp_state == kExpVote && now_ms >= s.exp_ends_ms) {
            if (yes >= no) {
                start_players_phase(s, ex, now_ms, false);
                out.push_back(Event{Event::ExpansionStarted, plot, s.building, exg, 0.0, 0});
            } else {
                out.push_back(Event{Event::ExpansionFailed, plot, s.building, exg, 0.0, 0});
                clear_project(s);
            }
        } else if (s.exp_state == kExpPlayerContribution && s.exp_ends_ms != 0 && now_ms >= s.exp_ends_ms) {
            out.push_back(Event{Event::ExpansionFailed, plot, s.building, exg, 0.0, 0});
            clear_project(s);
        } else if (s.exp_state == kExpConstruction && now_ms >= s.exp_ends_ms) {
            if (s.exp_rank != 0 && std::find(s.ranks.begin(), s.ranks.end(), s.exp_rank) == s.ranks.end()) s.ranks.push_back(s.exp_rank);
            out.push_back(Event{Event::ExpansionBuilt, plot, s.building, exg, 0.0, 0});
            clear_project(s);
        } else if (s.exp_state == kExpRemovalVote && now_ms >= s.exp_ends_ms) {
            if (yes >= no) {
                s.ranks.erase(std::remove(s.ranks.begin(), s.ranks.end(), s.exp_rank), s.ranks.end());
                out.push_back(Event{Event::ExpansionRemoved, plot, s.building, exg, 0.0, 0});
            } else {
                out.push_back(Event{Event::ExpansionKept, plot, s.building, exg, 0.0, 0});
            }
            clear_project(s);
        }
        // an Empty plot whose row is Empty too needs no entry
        const PlotInfo* info = info_of(plots, plot);
        if (s.state == kPlotEmpty && s.exp_state == kExpNone && info && info->state == kPlotEmpty) {
            it = b.plots.erase(it);
        } else {
            ++it;
        }
    }
    // the weekly upkeep
    if (b.next_upkeep_ms == 0) b.next_upkeep_ms = now_ms + kUpkeepPeriodMs;
    if (force_upkeep || now_ms >= b.next_upkeep_ms) {
        int64_t copper = 0;
        bool charged = false;
        for (const PlotInfo& p : plots) {
            const PlotNow now = now_of(b, p);
            if (now.state != kPlotOperational && now.state != kPlotDisarray) continue;
            const Building* bd = building(now.building);
            if (!bd) continue;
            Bill bill;
            // the building's own upkeep: one a mayor built (the plot's row was Empty, or held another building); the world's own (the data's and the seed's) pay none
            const bool built_here = p.state == kPlotEmpty || p.building != now.building;
            if (built_here) bill.add(bd->maintenance_cost, upkeep_scale);
            if (now.live) {
                for (const int64_t r : now.live->ranks) {
                    const TreeNode* t = rank_node(r);
                    const Expansion* ex = t ? expansion(t->expansion) : nullptr;
                    if (ex) bill.add(ex->added_maintenance, upkeep_scale);
                }
            }
            if (now.state == kPlotDisarray) bill.add(bd->disarray_repair_cost, 1.0);
            if (bill.copper == 0 && bill.lines.empty() && now.state == kPlotOperational) continue;
            if (can_pay_bill(n, bill)) {
                copper += pay_bill(n, bill);
                charged = true;
                if (now.state == kPlotDisarray) {
                    PlotState& s = live_of(b, p);
                    s.state = kPlotRepairing;
                    s.phase_started_ms = now_ms;
                    s.phase_ends_ms = now_ms + std::max<int64_t>(0, bd->repair_ms);
                    out.push_back(Event{Event::Repairing, p.plot, now.building, 0, 0.0, 0});
                }
            } else if (now.state == kPlotOperational) {
                PlotState& s = live_of(b, p);
                if (s.building == 0) s.building = now.building;
                s.state = kPlotDisarray;
                s.phase_started_ms = now_ms;
                s.phase_ends_ms = 0;
                out.push_back(Event{Event::Disarray, p.plot, now.building, 0, 0.0, 0});
            }
        }
        if (charged) out.push_back(Event{Event::Upkeep, 0, 0, 0, 0.0, copper});
        b.next_upkeep_ms = now_ms + kUpkeepPeriodMs;
    }
    return out;
}

int income_due(NodeBuildings& b, int64_t now_ms, int cap) {
    if (b.next_income_ms == 0) {
        b.next_income_ms = now_ms + kIncomePeriodMs;
        return 0;
    }
    int due = 0;
    while (now_ms >= b.next_income_ms) {
        b.next_income_ms += kIncomePeriodMs;
        ++due;
        if (due >= cap) {
            b.next_income_ms = now_ms + kIncomePeriodMs;
            break;
        }
    }
    return due;
}

// ---- elections.json

namespace {

nlohmann::json pairs_json(const std::map<int64_t, int32_t>& m) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& [k, v] : m) a.push_back({k, v});
    return a;
}

std::map<int64_t, int32_t> pairs_of(const nlohmann::json& j) {
    std::map<int64_t, int32_t> m;
    if (!j.is_array()) return m;
    for (const auto& e : j) {
        if (e.is_array() && e.size() == 2 && e[0].is_number_integer() && e[1].is_number_integer()) m[e[0].get<int64_t>()] = e[1].get<int32_t>();
    }
    return m;
}

}  // namespace

nlohmann::json to_json(const NodeBuildings& b) {
    nlohmann::json j;
    j["upkeep"] = b.next_upkeep_ms;
    j["income"] = b.next_income_ms;
    j["plots"] = nlohmann::json::array();
    for (const auto& [plot, s] : b.plots) {
        nlohmann::json p = {{"plot", plot}, {"building", s.building}, {"state", s.state}, {"start", s.phase_started_ms}, {"end", s.phase_ends_ms},
                            {"complete", s.complete_ms}, {"built", s.built_ms}};
        if (!s.given.empty()) p["given"] = pairs_json(s.given);
        if (!s.ranks.empty()) p["ranks"] = s.ranks;
        if (s.exp_state != kExpNone) {
            p["exp"] = {{"state", s.exp_state}, {"type", s.exp_type}, {"rank", s.exp_rank}, {"start", s.exp_started_ms}, {"end", s.exp_ends_ms}};
            if (!s.exp_given.empty()) p["exp"]["given"] = pairs_json(s.exp_given);
        }
        if (!s.votes.empty()) {
            p["votes"] = nlohmann::json::array();
            for (const auto& [a, yes] : s.votes) p["votes"].push_back({a, yes});
        }
        j["plots"].push_back(std::move(p));
    }
    return j;
}

NodeBuildings from_json(const nlohmann::json& j) {
    NodeBuildings b;
    if (!j.is_object()) return b;
    b.next_upkeep_ms = j.value("upkeep", int64_t{0});
    b.next_income_ms = j.value("income", int64_t{0});
    for (const auto& p : j.value("plots", nlohmann::json::array())) {
        if (!p.is_object()) continue;
        const int64_t plot = p.value("plot", int64_t{0});
        if (plot == 0) continue;
        PlotState s;
        s.building = p.value("building", int64_t{0});
        s.state = static_cast<uint8_t>(std::min<int>(p.value("state", 0), kPlotDemolition));
        s.phase_started_ms = p.value("start", int64_t{0});
        s.phase_ends_ms = p.value("end", int64_t{0});
        s.complete_ms = p.value("complete", int64_t{0});
        s.built_ms = p.value("built", int64_t{0});
        if (p.contains("given")) s.given = pairs_of(p["given"]);
        for (const auto& r : p.value("ranks", nlohmann::json::array())) {
            if (r.is_number_integer()) s.ranks.push_back(r.get<int64_t>());
        }
        if (p.contains("exp") && p["exp"].is_object()) {
            const auto& e = p["exp"];
            s.exp_state = static_cast<uint8_t>(std::min<int>(e.value("state", 0), kExpFinished));
            s.exp_type = static_cast<uint8_t>(std::min<int>(e.value("type", 0), kExpTypeRemoval));
            s.exp_rank = e.value("rank", int64_t{0});
            s.exp_started_ms = e.value("start", int64_t{0});
            s.exp_ends_ms = e.value("end", int64_t{0});
            if (e.contains("given")) s.exp_given = pairs_of(e["given"]);
        }
        for (const auto& v : p.value("votes", nlohmann::json::array())) {
            if (v.is_array() && v.size() == 2 && v[0].is_number_integer() && v[1].is_boolean()) s.votes[v[0].get<int64_t>()] = v[1].get<bool>();
        }
        b.plots[plot] = std::move(s);
    }
    return b;
}

}  // namespace node_buildings
}  // namespace aoc::worldd::world

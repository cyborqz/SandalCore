// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  gather/gathering.cpp (see gathering.h)
// ============================================================================
#include "gather/gathering.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

namespace aoc::worldd::gather {

namespace {

/// The host of the formulas that are not reward tables (the channel's length, the tool's wear): the harvest's reads and the random numbers.
class FormulaHost final : public stats::ExprHost {
public:
    FormulaHost(const content::GatherDefRow& def, stats::CharacterStats::Evaluation& player, stats::SplitMix& rng, const std::map<int64_t, double>& ratings)
        : def_(def), player_(player), rng_(rng), ratings_(ratings) {}
    double rand_double() override { return rng_.rand_double(); }
    double host_fn(int fn, const std::string& name, const double* args, int n) override {
        switch (fn) {
            case stats::kHostRandIntRange: {
                if (n < 2) return 0;
                const double lo = std::floor(args[0]), hi = std::floor(args[1]);
                if (hi < lo) return lo;
                return std::min(hi, lo + std::floor(rng_.rand_double() * (hi - lo + 1.0)));
            }
            case stats::kHostArtisanStat: {
                if (name.empty() || (n >= 1 && static_cast<int>(args[0]) != stats::kRoleCaster)) return 0;
                char* end = nullptr;
                const long long guid = std::strtoll(name.c_str(), &end, 10);
                if (end != name.c_str() + name.size()) return 0.0;
                const auto it = ratings_.find(static_cast<int64_t>(guid));
                return it != ratings_.end() ? it->second : player_.stat(static_cast<int64_t>(guid));
            }
            case stats::kHostSourceCertTier: return def_.cert_tier;
            case stats::kHostGatherBaseSecs: return def_.base_duration;
            default: return 0;
        }
    }
// CoA x C Y B

private:
    const content::GatherDefRow&          def_;
    stats::CharacterStats::Evaluation&    player_;
    stats::SplitMix&                      rng_;
    const std::map<int64_t, double>&      ratings_;
};

constexpr std::array<int, stats::kRarities> kCommonFloors = {1, 6, 16, 31, 51, 71, 101};

/// An item record the design data retired: its name says so.
bool retired_name(const std::string& name) { return name.rfind("RETIRED_", 0) == 0 || name.rfind("NOTUSED_", 0) == 0; }

/// Whether rolling `table` can give an item a player can use: one in the table or a sub table that is a record and not a retired one.
bool gives_an_item(const combat::RewardBook& book, const content::ContentDb& db, int64_t table, int depth, std::set<int64_t>& seen) {
    if (depth > 8 || !seen.insert(table).second) return false;
    const content::LootTable* t = book.table(table);
    if (!t) return false;
    for (const content::LootContainer& c : t->containers) {
        for (const content::LootReward& r : c.rewards) {
            for (const content::LootItem& it : r.items) {
                std::string err;
                const auto rec = it.item != 0 ? db.item(it.item, err) : std::nullopt;
                if (rec && !retired_name(rec->name)) return true;
            }
        }
    }
    for (size_t i = 0; i < t->subs.size(); ++i) {
        if (t->algo == "PerElementWeightedRandom" && i < t->w.size() && !(t->w[i] > 0)) continue;   // a weight of 0 is never selected (the Wyrmling's only subtable)
        if (gives_an_item(book, db, t->subs[i], depth + 1, seen)) return true;
    }
    return false;
}

}  // namespace

std::set<int64_t> hunting_defs(const content::FoliageData& data) {
    std::set<int64_t> out;
    for (const auto& [guid, d] : data.interaction_defs) {
        const auto it = data.interactions.find(d.gather_guid);
        if (it != data.interactions.end() && it->second.profession_guid == content::kProfessionHunting) out.insert(guid);
    }
    return out;
}

std::map<int64_t, std::string> hunting_names(const content::FoliageData& data) {
    std::map<int64_t, std::string> out;
    for (const int64_t def : hunting_defs(data)) {
        std::string name = data.interaction_defs.at(def).name;
        while (name.rfind("Hunting_", 0) == 0) name.erase(0, 8);
        std::string shown;
        for (size_t i = 0; i < name.size(); ++i) {
            const char c = name[i];
            if (c == '_') {
                if (!shown.empty() && shown.back() != ' ') shown += ' ';
                continue;
            }
            if (i > 0 && std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(name[i - 1])) && !shown.empty() && shown.back() != ' ') shown += ' ';
            shown += c;
        }
        if (!shown.empty()) out.emplace(def, std::move(shown));
    }
    return out;
}

std::unique_ptr<Gathering> Gathering::make(const content::ContentDb& db, world::ActorRegistry& registry, const abilities::Catalog& catalog, Config config, std::string& error) {
    error.clear();
    std::string e;
    auto data = db.gather_data(e);
    if (!data) {
        error = e.empty() ? "content.db has no gather tables (rebuild it with contentc 13 or later)" : e;
        return nullptr;
    }
    if (data->defs.empty()) {
        error = "content.db's gather_def is empty";
        return nullptr;
    }
    std::set<int64_t> roots;
    for (const auto& [guid, d] : data->defs) {
        if (d.reward_guid != 0) roots.insert(d.reward_guid);
    }
    // Gathering stage 3: the foliage's and the Hunting corpses' interactions have reward tables of their own (a file of an older contentc has no foliage tables: no foliage)
    auto foliage = db.foliage_data(e);
    if (!foliage && !e.empty()) {
        error = e;
        return nullptr;
    }
    if (foliage) {
        for (const auto& [guid, i] : foliage->interactions) {
            if (i.reward_guid != 0) roots.insert(i.reward_guid);
        }
    }
    auto tables = db.loot_tables(std::vector<int64_t>(roots.begin(), roots.end()), e);
    if (!tables || tables->empty()) {
        error = "the reward tables of the nodes do not read: " + (e.empty() ? std::string("none found") : e);
        return nullptr;
    }
    // Gathering stage 3, "Hunting": a Hunting_<kind> table picks its carcass OR its trainable by the weights (80 % / 20 %), and the five captured hunts gave one item and one experience line each;
    // the data's NumberOfSubtablesToSelect of those roots is 2 (the default it has everywhere), which would pick both: the loader reads it as 1
    size_t one_of = 0;
    if (foliage) {
        for (const int64_t def : hunting_defs(*foliage)) {
            const auto row = foliage->interactions.find(foliage->interaction_defs.at(def).gather_guid);
            if (row == foliage->interactions.end()) continue;
            const auto t = tables->find(row->second.reward_guid);
            if (t != tables->end() && t->second.algo == "PerElementWeightedRandom" && t->second.n > 1 && t->second.subs.size() > 1) {
                t->second.n = 1;
                ++one_of;
            }
        }
    }
    std::unique_ptr<Gathering> g(new Gathering(config.board, registry, config));
    g->db_ = &db;
    g->catalog_ = &catalog;
    g->data_ = std::move(*data);
    if (foliage) {
        for (const auto& [guid, i] : foliage->interactions) {
            content::GatherDefRow r;
            r.guid = i.guid;
            r.name = i.name;
            r.interaction_guid = i.interaction_guid;
            r.interact_radius = i.interact_radius;
            r.interact_tolerance = i.interact_tolerance;
            r.allow_in_combat = i.allow_in_combat;
            r.profession_guid = i.profession_guid;
            r.tool_slot = i.tool_slot;
            r.cert_guid = i.cert_guid;
            r.cert_tier = i.cert_tier;
            r.base_duration = i.base_duration;
            r.duration_expr = i.duration_expr;
            r.durability_expr = i.durability_expr;
            r.reward_guid = i.reward_guid;
            g->interactions_.emplace(i.guid, std::move(r));
        }
        g->foliage_ = std::move(foliage);
    }
    g->rewards_.emplace(catalog, std::move(*tables), /*decay_expr=*/0);
    // Gathering stage 3, "Hunting": the kinds of huntable creature, each with its interaction and whether its table gives an item
    if (g->foliage_) {
        for (const int64_t def : hunting_defs(*g->foliage_)) {
            const content::GatherInteractionDefRow& d = g->foliage_->interaction_defs.at(def);
            const auto row = g->interactions_.find(d.gather_guid);
            if (row == g->interactions_.end()) continue;
            HuntKind k;
            k.def = def;
            k.name = d.name;
            k.row = &row->second;
            std::set<int64_t> seen;
            k.gives_nothing = !gives_an_item(*g->rewards_, db, row->second.reward_guid, 0, seen);
            g->hunting_.emplace(def, std::move(k));
        }
    }
    g->hunting_one_of_ = one_of;
    g->rng_.state = config.seed;
    return g;
}
// C:Y:B - C O A

const content::GatherDefRow* Gathering::def(int64_t actor_def) const {
    const auto it = data_.defs.find(actor_def);
    return it == data_.defs.end() ? nullptr : &it->second;
}

const content::GatherToolRow* Gathering::tool(int64_t item) const {
    const auto it = data_.tools.find(item);
    return it == data_.tools.end() ? nullptr : &it->second;
}

const content::GatherDefRow* Gathering::interaction(int64_t interaction_guid) const {
    const auto it = interactions_.find(interaction_guid);
    return it == interactions_.end() ? nullptr : &it->second;
}

size_t Gathering::classify_hunting(stats::CharacterStats::Evaluation& probe) {
    size_t none = 0;
    for (auto& [def, k] : hunting_) {
        if (!k.gives_nothing) {
            // a dry run with a private draw: some tables never select anything (a weight of 0, a predicate that reads the creature's wounds: the roller answers it with "no")
            stats::SplitMix rng;
            rng.state = 0x48554E54ull ^ static_cast<uint64_t>(def);
            bool any = false;
            std::string why;
            for (int i = 0; i < 60 && !any; ++i) {
                const auto h = harvest(*k.row, probe, 25, world::NodeSite{}, nullptr, rng, why);
                for (const combat::RolledItem& it : h ? h->items : std::vector<combat::RolledItem>{}) {
                    std::string err;
                    const auto rec = db_ ? db_->item(it.item, err) : std::nullopt;
                    any = any || (rec && !retired_name(rec->name));
                }
            }
            k.gives_nothing = !any;
        }
        none += k.gives_nothing ? 1 : 0;
    }
    return none;
}

const Gathering::HuntKind* Gathering::hunt_kind(int64_t interaction_def) const {
    const auto it = hunting_.find(interaction_def);
    return it == hunting_.end() ? nullptr : &it->second;
}

const content::GatherDefRow* Gathering::def_of(const world::WorldActor& node) const {
    if (node.kind != world::ActorKind::Gatherable) return nullptr;
    const repl::ObjectState& actor = node.actor.obj;
    if (!actor.schema || !actor.schema->command("GatherableActorDef.Guid")) return nullptr;
    const repl::Value* v = actor.state.get(*actor.schema, "GatherableActorDef.Guid");
    if (!v) return nullptr;
    if (const int64_t* i = v->get_if<int64_t>()) return def(*i);
    if (const uint64_t* u = v->get_if<uint64_t>()) return def(static_cast<int64_t>(*u));
    return nullptr;
}

int Gathering::rarity_of(int64_t item, int quality) const {
    auto it = floors_.find(item);
    if (it == floors_.end()) {
        std::array<int, stats::kRarities> floors = kCommonFloors;
        if (db_) {
            std::string err;
            if (const auto gear = db_->item_gear(item, err); gear && gear->has_curve) {
                bool ok = true;
                for (int r = 0; r < stats::kRarities; ++r) {
                    floors[static_cast<size_t>(r)] = static_cast<int>(std::lround(gear->bands[static_cast<size_t>(r)].x));
                    if (r > 0 && floors[static_cast<size_t>(r)] <= floors[static_cast<size_t>(r - 1)]) ok = false;
                }
                if (!ok) floors = kCommonFloors;
            }
        }
        it = floors_.emplace(item, floors).first;
    }
    int rarity = 0;
    for (int r = 0; r < stats::kRarities; ++r) {
        if (quality >= it->second[static_cast<size_t>(r)]) rarity = r;
    }
    return rarity;
}
// C.O.A with Cyb

std::optional<double> Gathering::channel_seconds(const content::GatherDefRow& def, stats::CharacterStats::Evaluation& player, std::string& why) {
    if (def.duration_expr == 0) return def.base_duration;
    const int program = catalog_->program(def.duration_expr);
    if (program < 0) {
        why = "the duration formula " + std::to_string(def.duration_expr) + " of " + def.name + " has no program";
        return std::nullopt;
    }
    FormulaHost host(def, player, rng_, ratings_);
    double out = 0;
    std::string err;
    if (!player.eval(program, host, out, err) || !std::isfinite(out)) {
        why = "the duration formula of " + def.name + " does not run" + (err.empty() ? std::string() : ": " + err);
        return std::nullopt;
    }
    return std::max(0.0, out);
}

std::optional<Harvest> Gathering::harvest(const content::GatherDefRow& def, stats::CharacterStats::Evaluation& player, double level, const world::NodeSite& site,
                                          const world::CityNodes* nodes, stats::SplitMix& rng, std::string& why) {
    if (!rewards_ || !rewards_->table(def.reward_guid)) {
        why = "the reward table " + std::to_string(def.reward_guid) + " of " + def.name + " is not loaded";
        return std::nullopt;
    }
    combat::GatherRoll g;
    g.context.source_tier = def.cert_tier;
    g.context.base_duration = def.base_duration;
    g.context.ratings = ratings_;
    g.quality_expr = &data_.quality_expr;
    g.rarity_of = [this](int64_t item, int quality) { return rarity_of(item, quality); };
    g.quality_literal = cfg_.quality_literal;
    combat::RollContext ctx;
    ctx.npc = &player;
    ctx.gather = &g;
    ctx.player_level = level;
    ctx.loot_max_level = level;
    ctx.rng = &rng;
    ctx.nodes = nodes;
    ctx.node = site.node_rank;
    ctx.biome = site.biome;
    const combat::Rolled rolled = rewards_->roll({def.reward_guid}, ctx);
    Harvest h;
    h.items = rolled.loot;
    h.items.insert(h.items.end(), rolled.direct.begin(), rolled.direct.end());
    for (const combat::RolledXp& x : rolled.xp) {
        if (x.type == "Profession") h.profession_xp += x.amount;
        else if (x.type == "Adventuring") h.adventuring_xp += x.amount;
    }
    h.primary_quality = g.primary_quality;
    h.primary_rarity = g.primary_rarity;
    h.tables = rolled.tables;
    h.gated = rolled.gated;
    h.quality_failures = g.quality_failures;
    if (def.durability_expr != 0) {
        const int program = catalog_->program(def.durability_expr);
        if (program >= 0) {
            FormulaHost host(def, player, rng, ratings_);
            double out = 0;
            std::string err;
            if (player.eval(program, host, out, err) && std::isfinite(out) && out >= 0) h.durability_loss = out;
        }
    }
    return h;
}

}  // namespace aoc::worldd::gather

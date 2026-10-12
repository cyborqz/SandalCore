// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/rewards.cpp (see rewards.h)
// ============================================================================
#include "combat/rewards.h"

#include "combat/hit_context.h"
#include "stats/stat_program.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <cmath>

namespace aoc::worldd::combat {

namespace {

constexpr int kMaxDepth = 8;
constexpr size_t kMaxRolled = 64;   // the most stacks one death rolls (a table cannot make a corpse of thousands)

/// The host of a reward expression.
class RewardHost final : public stats::ExprHost {
public:
    explicit RewardHost(RollContext& c) : c_(c) {}
    double rand_double() override { return c_.rng ? c_.rng->rand_double() : 0.0; }
    double host_fn(int fn, const std::string& name, const double* args, int n) override {
        switch (fn) {
            case stats::kHostRandIntRange: {
                if (n < 2) return 0;
                const double lo = std::ceil(std::min(args[0], args[1])), hi = std::floor(std::max(args[0], args[1]));
                if (hi < lo) return lo;
                return std::min(hi, lo + std::floor(rand_double() * (hi - lo + 1.0)));
            }
            case stats::kHostLootMaxLevel: return c_.loot_max_level;
            case stats::kHostLootAvgLuck: return c_.avg_luck;
            case stats::kHostPlayerLevel: return c_.player_level;
            case stats::kHostNpcTag: {
                if (!c_.npc_tags) return 0;
                for (const std::string& t : *c_.npc_tags) {
                    if (tag_matches(t, name)) return 1.0;
                }
                return 0;
            }
            case stats::kHostIsClient: return 0;
            default: {
                if (fn >= stats::kHostNodeBiome && fn <= stats::kHostNodeLast) {
                    // the player is read as being where the creature died
                    const world::NodeContext where{c_.node, c_.biome, c_.node, c_.node};
                    double out = 0;
                    if (world::node_host_fn(c_.nodes, where, fn, args, n, out)) return out;
                }
                return 0;
            }
        }
    }
// ~ CoA & C_Y_B ~

private:
    RollContext& c_;
};

class Roller {
public:
    Roller(const RewardBook& book, const abilities::Catalog& catalog, const std::map<int64_t, content::LootTable>&, RollContext& ctx, Rolled& out,
           const std::function<const content::LootTable*(int64_t)>& lookup)
        : book_(book), catalog_(catalog), ctx_(ctx), out_(out), host_(ctx), lookup_(lookup) {}

    void table(int64_t guid, int depth) {
        if (depth > kMaxDepth) return;
        const content::LootTable* t = lookup_(guid);
        if (!t) return;
        if (t->pred != 0 && !truth(t->pred)) return;
        ++out_.tables;
        for (const content::LootContainer& c : t->containers) container(c);
        for (const size_t i : select(t->algo, t->n, t->subs.size(), t->w, t->pct, t->xw, /*empty_is_all=*/false)) table(t->subs[i], depth + 1);
    }

private:
    /// An expression's value; nullopt for none (id 0) and for one that does not run (counted).
    std::optional<double> value(int64_t expr) {
        if (expr == 0) return std::nullopt;
        const int program = catalog_.program(expr);
        if (program < 0 || !ctx_.npc) {
            ++out_.gated;
            return std::nullopt;
        }
        double v = 0;
        std::string err;
        if (!ctx_.npc->eval(program, host_, v, err) || !std::isfinite(v)) {
            ++out_.gated;
            return std::nullopt;
        }
        return v;
    }

    /// A predicate: none holds, one that does not run does not.
    bool truth(int64_t expr) {
        if (expr == 0) return true;
        const std::optional<double> v = value(expr);
        return v && *v != 0.0;
    }

    double weight_at(const std::vector<double>& w, size_t i) { return i < w.size() ? std::max(0.0, w[i]) : 1.0; }

    std::vector<size_t> select(const std::string& algo, int64_t n, size_t count, const std::vector<double>& w, const std::vector<int64_t>& pct,
                               const std::vector<int64_t>& xw, bool empty_is_all) {
        std::vector<size_t> out;
        if (count == 0) return out;
        const std::string a = algo.empty() || algo == "None" ? (empty_is_all ? "All" : "None") : algo;
        const size_t pick = static_cast<size_t>(std::max<int64_t>(1, std::min<int64_t>(n, static_cast<int64_t>(count))));
        if (a == "None") return out;
        if (a == "All") {
            for (size_t i = 0; i < count; ++i) out.push_back(i);
            return out;
        }
        if (a == "RollEachByPercent") {
            for (size_t i = 0; i < count; ++i) {
                if (i >= pct.size()) continue;
                const std::optional<double> p = value(pct[i]);
                if (p && ctx_.rng && ctx_.rng->rand_double() < std::min(1.0, *p * ctx_.chance_mult)) out.push_back(i);
            }
            return out;
        }
        std::vector<double> weights(count, 1.0);
        if (a == "PerElementWeightedRandom") {
            for (size_t i = 0; i < count; ++i) weights[i] = weight_at(w, i);
        } else if (a == "PerElementWeightedExpressionRandom") {
            for (size_t i = 0; i < count; ++i) {
                const std::optional<double> v = i < xw.size() ? value(xw[i]) : std::nullopt;
                weights[i] = v ? std::max(0.0, *v) : 0.0;
            }
        } else if (a != "PureRandom") {
            for (size_t i = 0; i < count; ++i) out.push_back(i);   // an algorithm this reader does not know: everything
            return out;
        }
        for (size_t k = 0; k < pick; ++k) {
            double total = 0;
            for (const double x : weights) total += x;
            if (!(total > 0) || !ctx_.rng) break;
            double r = ctx_.rng->rand_double() * total;
            size_t chosen = count;
            for (size_t i = 0; i < count; ++i) {
                if (weights[i] <= 0) continue;
                chosen = i;
                r -= weights[i];
                if (r < 0) break;
            }
            if (chosen == count) break;
            out.push_back(chosen);
            weights[chosen] = 0;
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    void container(const content::LootContainer& c) {
        if (c.pred != 0 && !truth(c.pred)) return;
        for (const size_t i : select(c.algo, c.n, c.rewards.size(), c.w, c.pct, c.xw, /*empty_is_all=*/true)) {
            const content::LootReward& r = c.rewards[i];
            for (const content::LootXp& x : r.xp) {
                const std::optional<double> v = value(x.expr);
                if (v && *v > 0) out_.xp.push_back({x.type, static_cast<int64_t>(std::llround(*v))});
            }
            for (const content::LootItem& it : r.items) {
                const std::optional<double> q = value(it.qty);
                const int64_t qty = q ? static_cast<int64_t>(std::floor(*q + 1e-9)) : 0;
                if (qty <= 0 || it.item == 0) continue;
                add(c.grant == "Auto" ? out_.direct : out_.loot, {it.item, it.variation, qty});
            }
            out_.currency += r.currency.size();
        }
    }

    void add(std::vector<RolledItem>& list, const RolledItem& item) {
        for (RolledItem& x : list) {
            if (x.item == item.item && x.variation == item.variation) {
                x.quantity += item.quantity;
                return;
            }
        }
        if (list.size() < kMaxRolled) list.push_back(item);
    }

    const RewardBook&                                           book_;
    const abilities::Catalog&                                   catalog_;
    RollContext&                                                ctx_;
    Rolled&                                                     out_;
    RewardHost                                                  host_;
    const std::function<const content::LootTable*(int64_t)>&    lookup_;
};

}  // namespace

Rolled RewardBook::roll(const std::vector<int64_t>& roots, RollContext& ctx) const {
    Rolled out;
    const std::function<const content::LootTable*(int64_t)> lookup = [this](int64_t g) { return table(g); };
    Roller r(*this, catalog_, tables_, ctx, out, lookup);
    std::vector<int64_t> done;
    for (const int64_t root : roots) {
        if (std::find(done.begin(), done.end(), root) != done.end()) continue;   // a table named twice rolls once
        done.push_back(root);
        r.table(root, 0);
    }
    // the same kind of experience from two tables adds up
    std::vector<RolledXp> merged;
    for (const RolledXp& x : out.xp) {
        const auto it = std::find_if(merged.begin(), merged.end(), [&](const RolledXp& m) { return m.type == x.type; });
        if (it == merged.end()) merged.push_back(x);
        else it->amount += x.amount;
    }
    out.xp = std::move(merged);
    return out;
}

int64_t RewardBook::xp_for(int64_t amount, RollContext& ctx) const {
    double mult = 1.0;
    if (decay_expr_ != 0 && ctx.npc) {
        const int program = catalog_.program(decay_expr_);
        if (program >= 0) {
            RewardHost host(ctx);
            double v = 0;
            std::string err;
            if (ctx.npc->eval(program, host, v, err) && std::isfinite(v) && v >= 0) mult = v;
        }
    }
    return static_cast<int64_t>(std::llround(static_cast<double>(amount) * mult));
}

}  // namespace aoc::worldd::combat

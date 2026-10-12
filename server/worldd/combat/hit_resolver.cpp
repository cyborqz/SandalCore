// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/hit_resolver.cpp (see hit_resolver.h)
// ============================================================================
#include "combat/hit_resolver.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::combat {

namespace {

double clamp01(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; }

// The host of a resolver run on its own (an NPC's hit): a random number from the resolver's source and nothing of a cast.
class ResolverHost final : public ContextHost {
public:
    explicit ResolverHost(stats::SplitMix& rng) : rng_(rng) {}
    double rand_double() override { return rng_.rand_double(); }

protected:
    double other_fn(int fn, const std::string&, const double* args, int) override {
        if (fn == stats::kHostRandIntRange) {
            const double lo = std::floor(args[0]), hi = std::floor(args[1]);
            return hi < lo ? lo : lo + std::floor(rng_.rand_double() * (hi - lo + 1.0));
        }
        return 0;
    }

private:
    stats::SplitMix& rng_;
};

}  // namespace

HitResolver::Rates HitResolver::default_rates(const stats::StatView& attacker, const stats::StatView& target, bool physical) const {
    const CombatStatIds& id = data_.ids();
    Rates r;
    const int64_t accuracy = physical ? id.physical_accuracy : id.magical_accuracy;
    const int64_t evasion = physical ? id.physical_evasion : id.magical_evasion;
    const int64_t crit = physical ? id.physical_crit_chance : id.magical_crit_chance;
    const int64_t avoid = physical ? id.physical_crit_avoidance : id.magical_crit_avoidance;
    const int64_t block = physical ? id.physical_block_chance : id.magical_block_chance;
    r.miss = clamp01((evasion ? target.stat_value(evasion) : 0.0) - (accuracy ? attacker.stat_value(accuracy) : 0.0));
    r.block = clamp01(block ? target.stat_value(block) : 0.0);
    r.crit = clamp01((crit ? attacker.stat_value(crit) : 0.0) - (avoid ? target.stat_value(avoid) : 0.0));
    return r;
}

bool HitResolver::resolve(Fighter& attacker, Fighter& target, const abilities::HitDef& hit, const HitMeta& meta, Resolved& out, std::string& why) {
    std::string err;
    auto a = attacker.stats().evaluation(attacker.worn(), err, true);
    if (!a) {
        why = "the attacker's stats do not run: " + err;
        return false;
    }
    auto t = target.stats().evaluation(target.worn(), err, true);
    if (!t) {
        why = "the target's stats do not run: " + err;
        return false;
    }
    ResolverHost host(rng_);
    const double dist = distance_cm(attacker.position(), target.position());
    const Relation rel = data_.relation(attacker.alignments(), target.alignments());
    return resolve_with(*a, host, t.get(), dist, rel == Relation::Hostile, rel == Relation::Neutral, hit, meta, out, why);
}
// C_Y_B / CoA

bool HitResolver::resolve_with(stats::CharacterStats::Evaluation& attacker, ContextHost& host, const stats::CharacterStats::Evaluation* target, double distance_cm,
                               bool enemy, bool neutral, const abilities::HitDef& hit, const HitMeta& meta, Resolved& out, std::string& why) {
    out = Resolved{};
    const EvalView attacker_view(attacker);
    const EvalView target_view(target ? *target : attacker);
    // the target's stats are the actor of the role GetTarget() (a hit on the caster itself: nobody is set, the role reads as the caster)
    if (target && target != &attacker) {
        if (!attacker.set_actor(stats::kRoleTarget, *target, why)) return false;
    } else {
        attacker.clear_actors();
    }

    // whatever way this returns, the evaluation and the host forget the hit and its actors
    struct Forget {
        stats::CharacterStats::Evaluation& e;
        ContextHost&                       h;
        ~Forget() {
            h.set_context(nullptr);
            e.clear_actors();
        }
    } forget{attacker, host};

    HitContext ctx;
    ctx.profile = &data_.profile();
    ctx.tags = hit.tags;
    ctx.tags.insert(ctx.tags.end(), meta.ability_tags.begin(), meta.ability_tags.end());
    ctx.source_type = meta.source_type;
    ctx.instigator = &attacker_view;
    ctx.target = target && target != &attacker ? &target_view : nullptr;
    ctx.targets = meta.targets;
    ctx.distance_cm = distance_cm;
    ctx.enemy = enemy;
    ctx.neutral = neutral;
    const HitAspect aspect = aspect_of(ctx.tags);
    ctx.hit.tag = aspect.element;
    ctx.hit.physical = aspect.physical;

    // ---- the rolls
    bool damaging = false;
    for (const content::ModRow& m : hit.mods) damaging = damaging || (m.stat_guid == data_.ids().health && !m.shield_absorb);
    const bool on_other = ctx.target != nullptr;
    if (damaging && on_other && enemy) {
        Rates rates = default_rates(attacker_view, target_view, aspect.physical);
        double v = 0;
        std::string e;
        host.set_context(&ctx);
        const auto override_rate = [&](int64_t expr, double& rate) {
            const content::ExprRow* row = catalog_.expr(expr);
            if (row && row->program >= 0 && attacker.eval(row->program, host, v, e) && std::isfinite(v)) rate = clamp01(v);
        };
        if (hit.miss_rate_expr) override_rate(hit.miss_rate_expr, rates.miss);
        if (hit.block_rate_expr) override_rate(hit.block_rate_expr, rates.block);
        if (hit.crit_rate_expr) override_rate(hit.crit_rate_expr, rates.crit);
        if (hit.force_cant_miss) rates.miss = 0;
        if (hit.force_cant_block) rates.block = 0;
        if (hit.force_cant_crit) rates.crit = 0;
        if (hit.force_roll_miss) rates.miss = 1;
        if (hit.force_roll_block) rates.block = 1;
        if (hit.force_roll_crit) rates.crit = 1;
        // one draw per roll, always in the same order, so a seeded source gives the same fight whatever the chances
        const double r_miss = rng_.rand_double(), r_block = rng_.rand_double(), r_crit = rng_.rand_double();
        uint8_t flags = 0;
        if (r_miss < rates.miss) flags |= stats::kHitMiss;
        else {
            if (r_block < rates.block) flags |= stats::kHitBlock;
            if (r_crit < rates.crit) flags |= stats::kHitCrit;
        }
        ctx.hit.flags = flags;
    }
    out.flags = ctx.hit.flags;
    if (out.miss()) return true;   // a hit that misses does nothing

    // ---- the mods
    host.set_context(&ctx);
    bool ok = true;
    for (const content::ModRow& m : hit.mods) {
        if (m.shield_absorb || m.value_expr == 0) continue;
        const content::ExprRow* row = catalog_.expr(m.value_expr);
        if (!row || row->program < 0) {
            why = "the formula of " + hit.row.name + " does not compile: " + (row ? row->error : "no expression " + std::to_string(m.value_expr));
            ok = false;
            break;
        }
        double v = 0;
        std::string e;
        if (!attacker.eval(row->program, host, v, e) || !std::isfinite(v)) {
            why = "the formula of " + hit.row.name + " does not run: " + e;
            ok = false;
            break;
        }
        out.mods.push_back({m.stat_mod_guid, m.stat_guid, v});
        if (m.stat_guid == data_.ids().health && v < 0) {
            const double tg = data_.ids().threat_generation ? attacker_view.stat_value(data_.ids().threat_generation) : 0.0;
            out.threat += -v * (tg > 0 ? tg : 1.0);
        }
    }
    if (ok && hit.row.threat_expr != 0) {
        const content::ExprRow* row = catalog_.expr(hit.row.threat_expr);
        double v = 0;
        std::string e;
        if (row && row->program >= 0 && attacker.eval(row->program, host, v, e) && std::isfinite(v)) out.threat = std::max(0.0, v);
    }
    return ok;
}

}  // namespace aoc::worldd::combat

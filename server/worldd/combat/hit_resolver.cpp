// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/hit_resolver.cpp (see hit_resolver.h)
// ============================================================================
#include "combat/hit_resolver.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace aoc::worldd::combat {

namespace {

double clamp01(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; }

// The guids of the effects a compiled expression text asks the length of: CalcEffectDuration($#<record type>:<guid>$).
std::vector<int64_t> effect_duration_refs(const std::string& text) {
    std::vector<int64_t> out;
    const std::string call = "CalcEffectDuration(";
    for (size_t p = text.find(call); p != std::string::npos; p = text.find(call, p + 1)) {
        size_t q = p + call.size();
        while (q < text.size() && text[q] == ' ') ++q;
        if (text.compare(q, 2, "$#") != 0) continue;
        q += 2;
        while (q < text.size() && std::isdigit(static_cast<unsigned char>(text[q]))) ++q;
        if (q >= text.size() || text[q] != ':') continue;
        ++q;
        int64_t guid = 0;
        bool any = false;
        while (q < text.size() && std::isdigit(static_cast<unsigned char>(text[q])) && guid < 922337203685477580LL) {
            guid = guid * 10 + (text[q] - '0');
            any = true;
            ++q;
        }
        if (any && q < text.size() && text[q] == '$') out.push_back(guid);
    }
    return out;
}

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
    // the tags the damage rules read: the held weapon's first when the hit asks for them, then the record's own and the ability's
    if (hit.use_item_tags) ctx.tags = meta.item_tags;
    ctx.tags.insert(ctx.tags.end(), hit.tags.begin(), hit.tags.end());
    ctx.tags.insert(ctx.tags.end(), meta.ability_tags.begin(), meta.ability_tags.end());
    ctx.source_type = meta.source_type;
    ctx.instigator = &attacker_view;
    ctx.target = target && target != &attacker ? &target_view : nullptr;
    ctx.targets = meta.targets;
    ctx.distance_cm = distance_cm;
    ctx.enemy = enemy;
    ctx.neutral = neutral;
    ctx.active_is_target = meta.target_is_active;
    ctx.active_in_hit = meta.active_in_hit;
    ctx.target_is_closest = meta.target_is_closest;
    ctx.target_effects = meta.target_effects;
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
    // the velocity change the hit gives (contentc version 42): its magnitude and the angle it is lifted by, worked out for the two like the mods are (Get Off The Stage's reads whether the target stands
    // under Heroic Stand: the data's way to leave it where it is)
    if (ok && hit.impulse && hit.impulse->applies) {
        const auto number = [&](int64_t expr, double& v) {
            if (expr == 0) {
                v = 0;
                return true;
            }
            const content::ExprRow* row = catalog_.expr(expr);
            std::string e;
            return row && row->program >= 0 && attacker.eval(row->program, host, v, e) && std::isfinite(v);
        };
        double magnitude = 0, angle = 0;
        const bool ran = number(hit.impulse->magnitude_expr, magnitude) && number(hit.impulse->angle_expr, angle);
        if (!ran) why = "the impulse of " + hit.row.name + " does not run";   // (the hit's other numbers stand)
        else if (magnitude > 0) out.impulse = {true, magnitude, angle};
    }
    return ok;
}

// ---- what a hit puts on its target, and whether it may run at all ----------------------------------------------------------------------

namespace {

// The context the expressions of a hit run in, once its stat mods are applied (the fields resolve_with fills; no rolls: the flags are the hit's).
void fill_context(HitContext& ctx, const CombatData& data, const abilities::HitDef& hit, const HitMeta& meta, const EvalView& attacker_view, const EvalView* target_view,
                  double distance_cm, bool enemy, bool neutral) {
    ctx.profile = &data.profile();
    if (hit.use_item_tags) ctx.tags = meta.item_tags;
    ctx.tags.insert(ctx.tags.end(), hit.tags.begin(), hit.tags.end());
    ctx.tags.insert(ctx.tags.end(), meta.ability_tags.begin(), meta.ability_tags.end());
    ctx.source_type = meta.source_type;
    ctx.instigator = &attacker_view;
    ctx.target = target_view;
    ctx.targets = meta.targets;
    ctx.distance_cm = distance_cm;
    ctx.enemy = enemy;
    ctx.neutral = neutral;
    ctx.active_is_target = meta.target_is_active;
    ctx.active_in_hit = meta.active_in_hit;
    ctx.target_is_closest = meta.target_is_closest;
    ctx.target_effects = meta.target_effects;
    const HitAspect aspect = aspect_of(ctx.tags);
    ctx.hit.tag = aspect.element;
    ctx.hit.physical = aspect.physical;
}

}  // namespace

bool HitResolver::resolve_effects(Fighter& attacker, Fighter& target, const abilities::HitDef& hit, const HitMeta& meta, Resolved& out, std::string& why) {
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
    return resolve_effects_with(*a, host, t.get(), dist, rel == Relation::Hostile, rel == Relation::Neutral, hit, meta, out, why);
}

bool HitResolver::resolve_effects_with(stats::CharacterStats::Evaluation& attacker, ContextHost& host, const stats::CharacterStats::Evaluation* target, double distance_cm,
                                       bool enemy, bool neutral, const abilities::HitDef& hit, const HitMeta& meta, Resolved& out, std::string& why) {
    bool any = false;
    for (const content::HitEffectRow& row : hit.effects) any = any || row.kind != "self";
    if (!any) return true;
    const EvalView attacker_view(attacker);
    const EvalView target_view(target ? *target : attacker);
    if (target && target != &attacker) {
        if (!attacker.set_actor(stats::kRoleTarget, *target, why)) return false;
    } else {
        attacker.clear_actors();
    }
    struct Forget {   // whatever way this returns, the evaluation and the host forget the hit and its actors
        stats::CharacterStats::Evaluation& e;
        ContextHost&                       h;
        ~Forget() {
            h.set_context(nullptr);
            e.clear_actors();
        }
    } forget{attacker, host};
    HitContext ctx;
    fill_context(ctx, data_, hit, meta, attacker_view, target && target != &attacker ? &target_view : nullptr, distance_cm, enemy, neutral);
    host.set_context(&ctx);
    const auto number = [&](int64_t expr, double& v) {
        const content::ExprRow* e = catalog_.expr(expr);
        std::string err;
        return e && e->program >= 0 && attacker.eval(e->program, host, v, err) && std::isfinite(v);
    };
    // The effect whose numbers are being worked out is the context's effect in progress (EffectHasTag(GetEffect(), tag)), and CalcEffectDuration(<effect>) reads the lengths
    // of the effects the expression names, worked out ahead of it: the engine cannot run a program from inside another one.
    std::vector<std::string> effect_tags;
    std::map<int64_t, double> durations;
    ctx.effect_tags = &effect_tags;
    ctx.effect_durations = &durations;
    std::set<int64_t> measuring;
    const std::function<void(int64_t)> measure = [&](int64_t expr) {
        const content::ExprRow* e = expr != 0 ? catalog_.expr(expr) : nullptr;
        if (!e) return;
        for (const int64_t guid : effect_duration_refs(e->text)) {
            if (durations.count(guid) != 0 || !measuring.insert(guid).second) continue;
            const abilities::EffectDef* d = catalog_.effect(guid);
            double secs = 0;
            if (d && !d->row.infinite_duration && d->row.duration_expr != 0) {
                measure(d->row.duration_expr);
                double v = 0;
                if (number(d->row.duration_expr, v)) secs = std::max(0.0, v);
            }
            durations[guid] = secs;
            measuring.erase(guid);
        }
    };
    const auto run = [&](int64_t expr, double& v) {
        measure(expr);
        return number(expr, v);
    };
    for (const content::HitEffectRow& row : hit.effects) {
        if (row.kind == "self") continue;
        const abilities::EffectDef* def = catalog_.effect(row.effect_guid);
        if (!def) continue;
        effect_tags = def->row.tags;
        effect_tags.insert(effect_tags.end(), def->row.effect_tags.begin(), def->row.effect_tags.end());
        ctx.effect_guid = def->row.guid;
        double v = 0;
        // the row's own requirement, then its stack count: no expression is one stack, and a count that works out to nothing applies nothing
        if (row.requirement_expr != 0 && (!run(row.requirement_expr, v) || v == 0)) continue;
        double count = 1;
        if (row.stack_count_expr != 0 && !run(row.stack_count_expr, count)) {
            why += (why.empty() ? "" : "; ") + std::string("the stack count of ") + def->row.name + " does not run";
            continue;
        }
        const int stacks = static_cast<int>(std::clamp(std::floor(count), 0.0, 4096.0));
        if (stacks < 1) continue;
        // the effect's own requirement (it reads the target: the burn is applied to a target that has burn to give)
        if (def->row.requirement_expr != 0 && (!run(def->row.requirement_expr, v) || v == 0)) continue;
        effects::Applied a;
        a.def = def;
        a.stacks = stacks;
        // the duration: the row's, else the record's (a number of seconds), else none
        if (row.duration_expr != 0 && run(row.duration_expr, v)) {
            a.duration = std::max(0.0, v);
        } else if (!def->row.infinite_duration && def->row.duration_expr != 0) {
            if (!run(def->row.duration_expr, v)) {
                why += (why.empty() ? "" : "; ") + std::string("the duration of ") + def->row.name + " does not run";
                continue;
            }
            a.duration = std::max(0.0, v);
        }
        a.in_place_limit = std::max(1, def->row.global_stack_limit);
        if (def->row.in_place_stack_limit_expr != 0 && run(def->row.in_place_stack_limit_expr, v) && v >= 1) a.in_place_limit = static_cast<int>(std::min(v, 4096.0));
        // ExtendDuration: the seconds one more application adds and the longest it may be extended to; an amount that does not run (the burn aura's does not compile) leaves the
        // duration refreshed in full
        a.extended_max = def->row.extended_max;
        if (def->row.extension_expr != 0 && run(def->row.extension_expr, v)) a.extension = std::max(0.0, v);
        for (const content::ModRow& m : def->mods) {
            if (m.shield_absorb || m.value_expr == 0 || m.stat_guid == content::kGuidTempHealth) continue;
            if (run(m.value_expr, v)) a.mods.push_back({m.stat_guid, v});
        }
        out.effects.push_back(std::move(a));
    }
    return true;
}

double HitResolver::effect_shield(Fighter& attacker, Fighter& holder, const abilities::EffectDef& effect) {
    std::string err;
    auto a = attacker.stats().evaluation(attacker.worn(), err, true);
    auto h = holder.stats().evaluation(holder.worn(), err, true);
    if (!a || !h || !a->set_actor(stats::kRoleTarget, *h, err)) return 0;
    ResolverHost host(rng_);
    double total = 0;
    for (const content::ModRow& m : effect.mods) {
        if (m.stat_guid != content::kGuidTempHealth || m.value_expr == 0) continue;
        const content::ExprRow* e = catalog_.expr(m.value_expr);
        if (!e || e->program < 0) continue;
        const bool holders = e->text.find("GetOwner(") != std::string::npos;
        double v = 0;
        if ((holders ? h->eval(e->program, host, v, err) : a->eval(e->program, host, v, err)) && std::isfinite(v)) total += std::max(0.0, v);
    }
    a->clear_actors();
    return total;
}

bool HitResolver::holds(Fighter& fighter, int64_t expr, bool none) {
    const content::ExprRow* row = expr ? catalog_.expr(expr) : nullptr;
    if (!row || row->program < 0) return none;
    std::string err;
    auto a = fighter.stats().evaluation(fighter.worn(), err, true);
    if (!a) return none;
    ResolverHost host(rng_);
    double v = 0;
    return a->eval(row->program, host, v, err) ? v != 0.0 : none;
}

bool HitResolver::requirement_holds(Fighter& attacker, Fighter& target, const abilities::HitDef& hit, const HitMeta& meta) {
    if (hit.row.requirement_expr == 0) return true;
    std::string err;
    auto a = attacker.stats().evaluation(attacker.worn(), err, true);
    auto t = target.stats().evaluation(target.worn(), err, true);
    const content::ExprRow* e = catalog_.expr(hit.row.requirement_expr);
    if (!a || !t || !e || e->program < 0) return false;
    if (!a->set_actor(stats::kRoleTarget, *t, err)) return false;
    const EvalView attacker_view(*a), target_view(*t);
    const Relation rel = data_.relation(attacker.alignments(), target.alignments());
    HitContext ctx;
    fill_context(ctx, data_, hit, meta, attacker_view, &target_view, distance_cm(attacker.position(), target.position()), rel == Relation::Hostile, rel == Relation::Neutral);
    ResolverHost host(rng_);
    host.set_context(&ctx);
    double v = 0;
    const bool ran = a->eval(e->program, host, v, err) && std::isfinite(v);
    host.set_context(nullptr);
    a->clear_actors();
    return ran && v != 0;
}

bool HitResolver::eval_between(stats::CharacterStats::Evaluation& attacker, const stats::CharacterStats::Evaluation* target, int64_t expr, double distance_cm, double& out, std::string& why) {
    const content::ExprRow* e = catalog_.expr(expr);
    if (!e || e->program < 0) {
        why = e ? "the expression does not compile: " + e->error : "there is no expression " + std::to_string(expr);
        return false;
    }
    const bool other = target && target != &attacker;
    if (other) {
        if (!attacker.set_actor(stats::kRoleTarget, *target, why)) return false;
    } else {
        attacker.clear_actors();
    }
    const EvalView attacker_view(attacker), target_view(target ? *target : attacker);
    HitContext ctx;
    ctx.profile = &data_.profile();
    ctx.instigator = &attacker_view;
    ctx.target = other ? &target_view : nullptr;
    ctx.distance_cm = distance_cm;
    ResolverHost host(rng_);
    host.set_context(&ctx);
    double v = 0;
    const bool ran = attacker.eval(e->program, host, v, why) && std::isfinite(v);
    host.set_context(nullptr);
    attacker.clear_actors();
    if (ran) out = v;
    return ran;
}

}  // namespace aoc::worldd::combat

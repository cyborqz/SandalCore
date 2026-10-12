// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/hit_resolver.h  (plan E1, stage 3, part 1)
//
//  What a hit does, worked out: the flags the server rolls for it (Miss, Block, Crit), then the value of each of its stat mods, evaluated by the
//  stat engine as the instigator with the target's stats in the role of GetTarget() (the expression programs the contentc compiler makes of the
//  DamageMod equation and the heal formulas: the seven native damage functions answered by combat/hit_context.h).  Nothing is applied here: the result
//  is what the world applies to an NPC target and what a session applies to its player (shields first), and what the confirmation carries.
//
//  The rolls [modelled: no capture shows a rate; the captured player's hits crit 8.9 % of the time, block 0.2 %]:
//    miss   the target's Evasion over the instigator's Accuracy, of the hit's aspect (a hit that cannot miss: bForceCantMiss)
//    block  the target's BlockChance of the aspect (physical hits and magical ones alike)
//    crit   the instigator's CriticalChance less the target's CriticalAvoidance, of the aspect
//  a hit's own CritRate / BlockRate / MissRate expression replaces the chance of the stats, and the hit's force flags decide what can happen at all.
//
//  A hit that misses has no stat mods.  A heal (a hit on an ally) rolls only the crit.
// ============================================================================
#pragma once

#include "combat/combat_data.h"
#include "combat/fighter.h"
#include "combat/hit_context.h"
#include "session/ability_catalog.h"
#include "stats/stat_engine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aoc::worldd::combat {

/// The StatView of an evaluation's run (its settled stat values).
class EvalView final : public stats::StatView {
public:
    explicit EvalView(const stats::CharacterStats::Evaluation& e) : e_(e) {}
    double stat_value(int64_t guid) const override { return e_.stat(guid); }

private:
    const stats::CharacterStats::Evaluation& e_;
};

/// The ExprHost of a hit's expressions: it answers what the HitContext knows and leaves the rest to `other_fn` (an ability's charges, a random number ...).
class ContextHost : public stats::ExprHost {
public:
    void set_context(const HitContext* ctx) { ctx_ = ctx; }
    const HitContext* context() const { return ctx_; }

    bool   hit_has_tag(const std::string& tag) override { return ctx_ && ctx_->has_tag(tag); }
    bool   hit_has_flag(const std::string& flag) override { return ctx_ && ctx_->has_flag(flag); }
    double hit_targets() override { return ctx_ ? ctx_->targets : 0; }
    double host_fn(int fn, const std::string& name, const double* args, int n) override {
        double v = 0;
        if (ctx_ && ctx_->host_fn(fn, name, args, n, v)) return v;
        return other_fn(fn, name, args, n);
    }

protected:
    virtual double other_fn(int /*fn*/, const std::string& /*name*/, const double* /*args*/, int /*n*/) { return 0; }

    const HitContext* ctx_ = nullptr;
};

/// What the caller says of the hit besides the record.
struct HitMeta {
    int64_t                  source_type = 0;       // content::kSourceType...
    int64_t                  source_guid = 0;
    std::vector<std::string> ability_tags;          // the ability's own tags (ability.Melee ...)
    double                   targets = 1;           // how many targets the hit has
};
// (C-O-A ~ C.Y.B)

struct ResolvedMod {
    int64_t stat_mod_guid = 0;
    int64_t stat_guid = 0;
    double  value = 0;      // the formula's value: a loss is negative
};

struct Resolved {
    uint8_t                  flags = 0;     // stats::kHitMiss | kHitBlock | kHitCrit (Lethal is the applier's)
    std::vector<ResolvedMod> mods;
    double                   threat = 0;    // what a damaging hit adds to the target's threat table
    bool miss() const { return (flags & stats::kHitMiss) != 0; }
};

class HitResolver {
public:
    HitResolver(const abilities::Catalog& catalog, const CombatData& data, uint64_t seed = 1) : catalog_(catalog), data_(data) { rng_.state = seed; }

    /// Rolls the flags and evaluates the mods of `hit` for `attacker` on `target`, each fighter's stats run fresh (a player's with the items it wears).
    /// False, with `why`, when a formula does not run (the hit then does nothing).
    bool resolve(Fighter& attacker, Fighter& target, const abilities::HitDef& hit, const HitMeta& meta, Resolved& out, std::string& why);

    /// The same with the caller's own evaluation of the attacker and ContextHost (the ability runtime's: its host knows the cast), and the target's.
    /// `target` may be the attacker's own evaluation (a hit on the caster itself); `host` must derive from ContextHost.
    bool resolve_with(stats::CharacterStats::Evaluation& attacker, ContextHost& host, const stats::CharacterStats::Evaluation* target, double distance_cm, bool enemy,
                      bool neutral, const abilities::HitDef& hit, const HitMeta& meta, Resolved& out, std::string& why);

    /// The chances of the three rolls for a hit of the aspect between two stat views (the stats' own, before a hit's overrides).
    struct Rates {
        double miss = 0, block = 0, crit = 0;
    };
    Rates default_rates(const stats::StatView& attacker, const stats::StatView& target, bool physical) const;

    double random() { return rng_.rand_double(); }
    const CombatData& data() const { return data_; }
    const abilities::Catalog& catalog() const { return catalog_; }

private:
    const abilities::Catalog& catalog_;
    const CombatData&         data_;
    stats::SplitMix           rng_;
};

}  // namespace aoc::worldd::combat

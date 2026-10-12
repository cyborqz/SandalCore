// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/ability_catalog.h  (plan D2)
//
//  Every ability, hit, effect and expression of content.db's ab_* tables, read once at startup and held in memory (the session thread never
//  touches SQLite): what the generic ability runtime (session/ability_pipeline.h) looks things up in. It is a resolved view of
//  content::AbilityContent's rows, nothing more: the expression columns are ab_expr ids whose `program` is an index into the stat bundle's
//  expression table (stats::ExprEvaluator runs them).
//
//  Not thread safe to build, safe to read from any number of sessions once built (every accessor is const).
// ============================================================================
#pragma once

#include "content/ability_content.h"
#include "stats/stat_program.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc::worldd::abilities {

struct EffectDef {
    content::EffectRow                          row;
    std::vector<content::ModRow>                mods;   // the effect's stat mods
    std::vector<std::pair<std::string, int64_t>> hits;   // (phase, hit guid): PreApply, Apply, Tick, Consume, Remove, Expire, End
};
// [C Y B x CoA]

struct HitDef {
    content::HitRow                    row;
    std::vector<content::HitEffectRow> effects;   // what it applies to its targets ("apply") and to its caster ("self")
    std::vector<content::ModRow>       mods;      // the hit's own stat mods (a heal: Health +amount)
    std::vector<int64_t>               remove_effects;   // plan D2, stage 2: the effects it takes off its targets (EffectsToRemoveIds)
    int                                max_remove = 0;   // MaxNumEffectsToRemove (0: no limit)
    std::vector<int64_t>               zones;            // the lingering zones the hit spawns (LingeringZonesIds)
    std::vector<int64_t>               fires;            // E1 stage 3 fix 2: the projectiles the hit fires when it lands (ab_hit_projectile "fire": the book's Split hits)
    // plan E1, stage 3: what the damage rules read of the hit (extra's EventTags ...)
    std::vector<std::string>           tags;             // EventTags: the element (Element.Physical.Slash), ability.Melee, ComboStep.End ...
    int64_t                            crit_rate_expr = 0, block_rate_expr = 0, miss_rate_expr = 0;   // ab_expr ids that override the stats' chances (0: the default)
    bool                               force_cant_crit = false, force_cant_block = false, force_cant_miss = false;
    bool                               force_roll_crit = false, force_roll_block = false, force_roll_miss = false;
};

struct AbilityDef {
    content::AbilityRow                     row;
    std::vector<content::CostRow>           costs;
    std::vector<content::EffectCostRow>     effect_costs;
    std::vector<content::MontageRow>        montages;
    std::vector<content::AbilityHitRow>     hits;
    std::vector<content::TargetingRow>      targetings;   // plan D2, stage 2
    std::vector<std::pair<int64_t, int64_t>> effects;   // (key, effect guid): the effects the ability lists
    /// The lingering zones the ability itself spawns when it is cast (LingeringEffectsIds, in key order), without the ones one of its hits spawns (those
    /// come with the hit): a Bard's dance opens its zone's actor with the cast. A zone may have no hit of its own (Mesmerizing's).
    std::vector<int64_t> zones;
};

class Catalog {
public:
    /// Reads every row of the ability tables of the content.db at `path`; `bundle` is the stat program of that same file (the programs in
    /// ab_expr index its expression table: a bundle of another build is refused). nullopt, with `error`, when a table cannot be read or a
    /// row names an expression the bundle does not have.
    static std::optional<Catalog> load(const std::string& path, const stats::StatBundle& bundle, std::string& error);

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    Catalog(Catalog&&) noexcept = default;
    Catalog& operator=(Catalog&&) noexcept = default;

    const AbilityDef* ability(int64_t guid) const;
    const AbilityDef* ability_named(std::string_view name) const;
    const EffectDef*  effect(int64_t guid) const;
    const EffectDef*  effect_named(std::string_view name) const;
    const HitDef*     hit(int64_t guid) const;
    /// A hit by its record name (the tooltip's `hit:Name` tokens name hits so; plan E1, stage 3 fix, Part C).
    const HitDef*     hit_named(std::string_view name) const;
    /// A lingering zone (ab_zone: its lifetime and tick rate; the hits are the ones the abilities list).
    const content::ZoneRow* zone(int64_t guid) const;
    /// The expression with the ab_expr id; null for 0 (none) and for an id the tables do not have.
    const content::ExprRow* expr(int64_t id) const;
    /// The bundle program of an expression id: -1 when there is none (id 0, or one that does not compile: expr()->error says why).
    int program(int64_t id) const { const content::ExprRow* e = expr(id); return e ? e->program : -1; }
    const stats::StatBundle& bundle() const { return *bundle_; }

    size_t abilities() const { return abilities_.size(); }
    size_t effects() const { return effects_.size(); }
    size_t hits() const { return hits_.size(); }
    size_t exprs() const { return exprs_.size(); }

private:
    Catalog() = default;
    const stats::StatBundle*              bundle_ = nullptr;
    std::map<int64_t, AbilityDef>         abilities_;
    std::map<std::string, int64_t, std::less<>> ability_names_, effect_names_, hit_names_;
    std::map<int64_t, EffectDef>          effects_;
    std::map<int64_t, HitDef>             hits_;
    std::map<int64_t, content::ZoneRow>   zones_;
    std::map<int64_t, content::ExprRow>   exprs_;
};

}  // namespace aoc::worldd::abilities

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/ability_catalog.cpp  (see ability_catalog.h)
// ============================================================================
#include "session/ability_catalog.h"

#include <nlohmann/json.hpp>

#include <set>

namespace aoc::worldd::abilities {

std::optional<Catalog> Catalog::load(const std::string& path, const stats::StatBundle& bundle, std::string& error) {
    auto db = content::AbilityContent::open(path, error);
    if (!db) return std::nullopt;
    // the programs of ab_expr index the expression table of the file's own stat program: any other bundle would run the wrong code
    const auto data = db->stat_program_data(error);
    if (!data) return std::nullopt;
    stats::StatBundle own;
    if (!stats::parse_bundle(*data, own, error)) {
        error = "the stat program of " + path + " does not verify: " + error;
        return std::nullopt;
    }
    if (own.crc32 != bundle.crc32 || own.exprs.size() != bundle.exprs.size()) {
        error = "the stat bundle the ability tables compile to (crc32 " + std::to_string(own.crc32) + ") is not the one worldd runs (crc32 " +
                std::to_string(bundle.crc32) + ")";
        return std::nullopt;
    }
    Catalog c;
    c.bundle_ = &bundle;
    if (!db->all_exprs(c.exprs_, error)) return std::nullopt;
    for (const auto& [id, e] : c.exprs_) {
        if (e.program >= 0 && !bundle.is_expr(e.program)) {
            error = "expression " + std::to_string(id) + " names program " + std::to_string(e.program) + ", which is no expression program of the bundle";
            return std::nullopt;
        }
    }
// ~ C Y B :: C-O-A ~

    std::vector<int64_t> guids;
    if (!db->guids(content::AbilityContent::Table::Effect, guids, error)) return std::nullopt;
    for (const int64_t g : guids) {
        EffectDef d;
        auto row = db->effect(g, error);
        if (!row) return std::nullopt;
        d.row = std::move(*row);
        if (!db->mods("effect", g, d.mods, error)) return std::nullopt;
        std::vector<content::EffectHitRow> hits;
        if (!db->effect_hits(g, hits, error)) return std::nullopt;
        for (const content::EffectHitRow& h : hits) d.hits.emplace_back(h.phase, h.hit_guid);
        c.effect_names_[d.row.name] = g;
        c.effects_.emplace(g, std::move(d));
    }
    if (!db->guids(content::AbilityContent::Table::Hit, guids, error)) return std::nullopt;
    for (const int64_t g : guids) {
        HitDef d;
        auto row = db->hit(g, error);
        if (!row) return std::nullopt;
        d.row = std::move(*row);
        if (!db->hit_effects(g, d.effects, error) || !db->mods("hit", g, d.mods, error) || !db->hit_projectiles(g, d.fires, error)) return std::nullopt;
        // what the hit takes off its targets: extra's EffectsToRemoveIds (the effects' guids) and MaxNumEffectsToRemove
        const nlohmann::json extra = nlohmann::json::parse(d.row.extra, nullptr, false);
        if (extra.is_object()) {
            const auto rm = extra.find("EffectsToRemoveIds");
            if (rm != extra.end() && rm->is_array()) {
                for (const auto& v : *rm) {
                    if (v.is_number_integer()) d.remove_effects.push_back(v.get<int64_t>());
                }
            }
            const auto mx = extra.find("MaxNumEffectsToRemove");
            if (mx != extra.end() && mx->is_number_integer()) d.max_remove = mx->get<int>();
            const auto zs = extra.find("LingeringZonesIds");
            if (zs != extra.end() && zs->is_array()) {
                for (const auto& v : *zs) {
                    if (v.is_number_integer() && v.get<int64_t>() != 0) d.zones.push_back(v.get<int64_t>());
                }
            }
            // plan E1, stage 3 (contentc version 9)
            const auto tags = extra.find("EventTags");
            if (tags != extra.end() && tags->is_array()) {
                for (const auto& v : *tags) {
                    if (v.is_string()) d.tags.push_back(v.get<std::string>());
                }
            }
            const auto id_of = [&](const char* key) -> int64_t {
                const auto it = extra.find(key);
                return it != extra.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
            };
            const auto flag = [&](const char* key) {
                const auto it = extra.find(key);
                return it != extra.end() && it->is_boolean() && it->get<bool>();
            };
            d.crit_rate_expr = id_of("CritRateExpr");
            d.block_rate_expr = id_of("BlockRateExpr");
            d.miss_rate_expr = id_of("MissRateExpr");
            d.force_cant_crit = flag("bForceCantCrit");
            d.force_cant_block = flag("bForceCantBlock");
            d.force_cant_miss = flag("bForceCantMiss");
            d.force_roll_crit = flag("bForceRollCrit");
            d.force_roll_block = flag("bForceRollBlock");
            d.force_roll_miss = flag("bForceRollMiss");
        }
        c.hit_names_[d.row.name] = g;
        c.hits_.emplace(g, std::move(d));
    }
    if (!db->guids(content::AbilityContent::Table::Ability, guids, error)) return std::nullopt;
    for (const int64_t g : guids) {
        AbilityDef d;
        auto row = db->ability(g, error);
        if (!row) return std::nullopt;
        d.row = std::move(*row);
        if (!db->costs(g, d.costs, error) || !db->effect_costs(g, d.effect_costs, error) || !db->montages(g, d.montages, error) ||
            !db->ability_hits(g, d.hits, error) || !db->ability_effects(g, d.effects, error) || !db->targetings(g, d.targetings, error))
            return std::nullopt;
        // the zones the ability itself spawns: extra's LingeringEffectsIds, minus the ones a hit of it spawns (they come with the hit)
        const nlohmann::json extra = nlohmann::json::parse(d.row.extra, nullptr, false);
        if (extra.is_object()) {
            const auto zs = extra.find("LingeringEffectsIds");
            if (zs != extra.end() && zs->is_array()) {
                std::set<int64_t> by_hit;
                for (const content::AbilityHitRow& h : d.hits) {
                    const auto hit = c.hits_.find(h.hit_guid);
                    if (hit != c.hits_.end()) by_hit.insert(hit->second.zones.begin(), hit->second.zones.end());
                }
                for (const auto& v : *zs) {
                    if (v.is_number_integer() && v.get<int64_t>() != 0 && !by_hit.count(v.get<int64_t>())) d.zones.push_back(v.get<int64_t>());
                }
            }
        }
        c.ability_names_[d.row.name] = g;
        c.abilities_.emplace(g, std::move(d));
    }
    if (!db->guids(content::AbilityContent::Table::Zone, guids, error)) return std::nullopt;
    for (const int64_t g : guids) {
        auto row = db->zone(g, error);
        if (!row) return std::nullopt;
        c.zones_.emplace(g, std::move(*row));
    }
    // every row a row names is in the catalog: a hit of an effect, an effect of a hit, a hit of an ability (a dangling guid would be found at a cast)
    for (const auto& [g, e] : c.effects_) {
        for (const auto& [phase, hit] : e.hits) {
            if (!c.hits_.count(hit)) {
                error = "effect " + e.row.name + " names hit " + std::to_string(hit) + " (" + phase + "), which the tables do not have";
                return std::nullopt;
            }
        }
    }
    for (const auto& [g, h] : c.hits_) {
        for (const content::HitEffectRow& x : h.effects) {
            if (!c.effects_.count(x.effect_guid)) {
                error = "hit " + h.row.name + " applies effect " + std::to_string(x.effect_guid) + ", which the tables do not have";
                return std::nullopt;
            }
        }
    }
    for (const auto& [g, a] : c.abilities_) {
        for (const content::AbilityHitRow& h : a.hits) {
            if (!c.hits_.count(h.hit_guid)) {
                error = "ability " + a.row.name + " fires hit " + std::to_string(h.hit_guid) + ", which the tables do not have";
                return std::nullopt;
            }
        }
    }
    return c;
}

const AbilityDef* Catalog::ability(int64_t guid) const {
    const auto it = abilities_.find(guid);
    return it == abilities_.end() ? nullptr : &it->second;
}

const AbilityDef* Catalog::ability_named(std::string_view name) const {
    const auto it = ability_names_.find(name);
    return it == ability_names_.end() ? nullptr : ability(it->second);
}

const EffectDef* Catalog::effect(int64_t guid) const {
    const auto it = effects_.find(guid);
    return it == effects_.end() ? nullptr : &it->second;
}

const EffectDef* Catalog::effect_named(std::string_view name) const {
    const auto it = effect_names_.find(name);
    return it == effect_names_.end() ? nullptr : effect(it->second);
}

const HitDef* Catalog::hit(int64_t guid) const {
    const auto it = hits_.find(guid);
    return it == hits_.end() ? nullptr : &it->second;
}
// C Y B + CoA

const HitDef* Catalog::hit_named(std::string_view name) const {
    const auto it = hit_names_.find(name);
    return it == hit_names_.end() ? nullptr : hit(it->second);
}

const content::ZoneRow* Catalog::zone(int64_t guid) const {
    const auto it = zones_.find(guid);
    return it == zones_.end() ? nullptr : &it->second;
}

const content::ExprRow* Catalog::expr(int64_t id) const {
    if (id == 0) return nullptr;
    const auto it = exprs_.find(id);
    return it == exprs_.end() ? nullptr : &it->second;
}

}  // namespace aoc::worldd::abilities

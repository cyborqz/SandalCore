// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  content/ability_content.h  (plan D2, contentc version 6)
//
//  A read-only reader of the ability tables of content.db: every ability of every class and what it
//  reaches, as plain data and as expressions (services/internal/content abilities.go has the DDL and
//  the Go readers; cmd/contentc abilities.go writes them). It is a connection of its own, so the
//  ContentDb of the A5-B3 plans is untouched; the session layer that runs abilities will own both.
//
//    (ranks: plan D1's skill_tree, skill_node and skill_rank, the one table of ranks; the ab_* tables point at their abilities and effects)
//    ab_ability, ab_targeting, ab_cost, ab_effect_cost, ab_montage
//                    an ability: targeting, usability, cooldown group, what it spends, its montages by AnimationIndex
//    ab_ability_effect, ab_ability_hit, ab_hit, ab_hit_effect, ab_hit_projectile, ab_hit_zone,
//    ab_projectile, ab_projectile_hit, ab_zone, ab_zone_hit, ab_effect, ab_effect_hit, ab_mod
//                    the closure: hits, projectiles, lingering zones, effects and their stat mods
//    ab_expr         every expression once: the text and the program of the stat bundle (stat_program) it compiles to
//    stat_program    the bundle itself (stats::parse_bundle reads it)
//
//  A field that ends in `expr` is an ab_expr id, 0 for none. The reader opens the file SQLITE_OPEN_READONLY in
//  serialized mode and prepares a statement per call: one AbilityContent may be shared by threads. Errors are
//  returned, not thrown: a failing call returns false or nullopt and sets `error`.
// ============================================================================
#pragma once

#include "stats/stat_engine.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace aoc::worldd::content {

/// The oldest contentc whose file has the ability tables (cmd/contentc Version): 6.
inline constexpr int kMinAbilityContentcVersion = 6;

// (the record types GetSkillPurchased's and GetEffectCount's references name are stats::kRefTypeSkillTree, kRefTypeSkill and kRefTypeEffect)

/// ab_expr: an expression as written, and where it compiled to.
struct ExprRow {
    int64_t                         id = 0;
    std::string                     text;
    std::map<int64_t, std::string>  terms;     // a StatMod's input terms by term guid
    int                             program = -1;   // the program of the stat bundle (listed in its exprs), -1 when there is none
    bool                            uses_target = false;   // it calls GetTarget(): compiled as the caster
    std::string                     error;     // why it does not compile
};
// C-Y-B : CoA

/// ab_ability. The fields that end in `expr` are ab_expr ids.
struct AbilityRow {
    int64_t     guid = 0;
    std::string name, display_name;
    std::vector<std::string> tags;
    std::string held_behavior, activation_input;
    std::string target_requirement;   // ...::Always: a target is needed
    std::string fallback_target;
    int         applicable_targets = 0;   // EAffectedTarget mask: Self 1, Enemy 2, Ally 4, Party 8, Raid 16
    int         applicable_types = 0;
    std::string weapon_requirement, shared_cooldown_tag;
    bool        disable_during_cooldown = false;
    double      max_hold_time = 0;
    bool        hold_animation = false, usable_in_air = false, usable_while_swimming = false, usable_while_walking = false,
                usable_while_mounted = false, usable_from_hotbar = false;
    double      area_range = 0, area_radius = 0, networking_max_speed = 0;
    int64_t     cooldown_expr = 0;       // seconds
    int64_t     charges_expr = 0;        // the charges (none: one)
    int64_t     cast_expr = 0;           // seconds (none: the montage's)
    int64_t     requirement_expr = 0, can_break_expr = 0, has_proc_expr = 0;
    std::string extra;                   // JSON: ModifyCooldownsIds, ReactionsIds, TeleportRecordsIds
};

/// ab_cost: one stat cost of a cost group (stat_ord -1 for a group that has none).
struct CostRow {
    int         cost_ord = 0, stat_ord = 0;
    int64_t     enabled_expr = 0;
    bool        triggers_cooldown = false;
    int64_t     charge_cost_expr = 0;
    int64_t     stat_guid = 0;
    std::string stat_name;
    int64_t     amount_expr = 0;
};

/// ab_effect_cost: an effect an ability consumes (effect_guid 0 for a filter of another shape).
struct EffectCostRow {
    int         cost_ord = 0, ord = 0;
    int64_t     effect_guid = 0;
    std::string filter;
    int64_t     num_expr = 0;
};

/// ab_montage: the montage of an AnimationIndex.
struct MontageRow {
    int                      anim_index = 0;
    std::string              soft_path;
    std::string              direction;
    int64_t                  select_expr = 0;
    std::string              start_section;
    int64_t                  targeting_key = 0;
    std::optional<uint64_t>  net_guid;   // empty: no retail capture names the montage
};

/// ab_targeting: one targeting record of an ability by the TargetingRecordsIds map's key (a hit of the ability with the same key is aimed by it;
/// a hit whose key has none is aimed at the caster: Default_Self).
struct TargetingRow {
    int         key = 0;
    int64_t     guid = 0;
    std::string name;
};

/// ab_ability_hit: a hit an ability fires, directly (source "hit"), through a projectile ("projectile") or a lingering zone ("zone").
struct AbilityHitRow {
    std::string source;
    int64_t     key = 0, owner_guid = 0;
    std::string role;
    int         ord = 0;
    int64_t     hit_guid = 0;
};

/// ab_hit.
struct HitRow {
    int64_t     guid = 0;
    std::string name, net_type;      // NET_ServerVerify: the client sends RequestHitVerification_Server
    int         applicable_targets = 0, applicable_types = 0;
    std::string fallback_target;
    int64_t     requirement_expr = 0, delay_expr = 0, threat_expr = 0, max_targets_expr = 0;
    std::string extra;
    bool        has_fx = false;   // OnHitFXId or OnActivateFXId is set (extra's HitFX): the server confirms such a hit to the client (it plays the effect)
};

/// ab_hit_effect: what a hit applies to its targets (kind "apply") or to its caster ("self").
struct HitEffectRow {
    std::string kind;
    int         ord = 0;
    int64_t     effect_guid = 0, requirement_expr = 0, stack_count_expr = 0, duration_expr = 0;
    std::string on_hit_behavior;
    int         max_applications = 0;
};
// built by C | O | A with C.Y.B

/// ab_effect.
struct EffectRow {
    int64_t     guid = 0;
    std::string name, display_name, category;
    std::vector<std::string> tags, effect_tags;
    bool        infinite_duration = false;
    int64_t     duration_expr = 0;       // seconds
    double      tick_timer = 0;
    bool        ignore_initial_tick = false;
    std::string stack_type;
    int         global_stack_limit = 0;
    int64_t     in_place_stack_limit_expr = 0;
    std::string stack_duration_refresh;
    bool        persists_through_death = false, persists_relog = false, dispellable = false, significant = false;
    int64_t     requirement_expr = 0;
    int64_t     crowd_control_guid = 0;
    std::string extra;
};

/// ab_effect_hit: a hit an effect fires in a phase (PreApply, Apply, Tick, Consume, Remove, Expire, End).
struct EffectHitRow {
    std::string phase;
    int         ord = 0;
    int64_t     hit_guid = 0;
};

/// ab_mod: a stat mod of an effect or a hit, with the ab_expr id of its value.
struct ModRow {
    int         ord = 0;
    int64_t     stat_mod_guid = 0, stat_guid = 0;
    std::string stat_name;
    int64_t     value_expr = 0;
    bool        shield_absorb = false;
};

/// ab_projectile with the hits it fires on impact.
struct ProjectileRow {
    int64_t     guid = 0;
    std::string name, projectile_type;
    double      speed = 0, range = 0, life_time = 0;
    int         max_hits = 0, chain_hit_count = 0;
    double      chain_radius = 0, sphere_radius = 0;
    std::vector<int64_t> hits;
};

/// ab_zone (a lingering zone) with its hits by role (InitialHitsIds, LingeringHitsIds, OnBeginOverlapHitsIds, OnEndOverlapHitsIds, EndHitsIds).
struct ZoneRow {
    int64_t     guid = 0;
    std::string name;
    double      life_time = 0, tick_rate = 0;
    bool        use_global_tick_rate = false;
    int         charges = 0;
    double      delay_time = 0;
    std::string delay_type;
    double      speed = 0;
    int64_t     expire_after_hits_expr = 0, expire_if_expr = 0, targeting_guid = 0;
    std::map<std::string, std::vector<int64_t>> hits;
};

class AbilityContent {
public:
    /// Opens `path` read-only. nullopt, with `error`, when the file is missing, is no content.db of this schema version, was built
    /// by a contentc older than kMinAbilityContentcVersion (the message says to rebuild it) or lacks a table or column used here.
    static std::optional<AbilityContent> open(const std::string& path, std::string& error);

    AbilityContent(const AbilityContent&) = delete;
    AbilityContent& operator=(const AbilityContent&) = delete;
    AbilityContent(AbilityContent&& other) noexcept;
    AbilityContent& operator=(AbilityContent&& other) noexcept;
    ~AbilityContent();

    const std::string& path() const { return path_; }
    int contentc_version() const { return contentc_version_; }

    /// The stat bundle (stat_program.data) and its header's crc32; nullopt with `error` when the table has no row.
    std::optional<std::vector<uint8_t>> stat_program_data(std::string& error) const;

    /// The expression with the id; nullopt, with `error` set, when there is none or a query failed.
    std::optional<ExprRow> expr(int64_t id, std::string& error) const;

    /// The abilities the ranks of a class's tree grant (the class record's guid; plan D1's tables: the slot options' too), in guid order.
    bool class_ability_guids(int64_t class_guid, std::vector<int64_t>& out, std::string& error) const;
    /// The skills a character of `level` holds by itself, as the engine's inputs (GetSkillPurchased reads them): every auto-granted
    /// node of any tree (plan D1's skill_node) whose level requirement is reached.
    bool auto_ranks_at(int level, std::vector<stats::SkillRank>& out, std::string& error) const;

    std::optional<AbilityRow> ability(int64_t guid, std::string& error) const;
    std::optional<AbilityRow> ability_by_name(const std::string& name, std::string& error) const;
    bool costs(int64_t ability, std::vector<CostRow>& out, std::string& error) const;
    bool effect_costs(int64_t ability, std::vector<EffectCostRow>& out, std::string& error) const;
    bool montages(int64_t ability, std::vector<MontageRow>& out, std::string& error) const;
    bool targetings(int64_t ability, std::vector<TargetingRow>& out, std::string& error) const;
    bool ability_hits(int64_t ability, std::vector<AbilityHitRow>& out, std::string& error) const;
    /// The effects an ability applies by itself (EffectsIds), by map key.
    bool ability_effects(int64_t ability, std::vector<std::pair<int64_t, int64_t>>& key_effect, std::string& error) const;

    std::optional<HitRow> hit(int64_t guid, std::string& error) const;
    bool hit_effects(int64_t hit, std::vector<HitEffectRow>& out, std::string& error) const;
    /// ab_hit_projectile: the projectiles a hit fires when it lands (kind "fire": the book's Split hits fire the split projectile), in order.
    bool hit_projectiles(int64_t hit, std::vector<int64_t>& out, std::string& error) const;
    std::optional<EffectRow> effect(int64_t guid, std::string& error) const;
    std::optional<EffectRow> effect_by_name(const std::string& name, std::string& error) const;
    bool effect_hits(int64_t effect, std::vector<EffectHitRow>& out, std::string& error) const;
    /// The stat mods of an effect (kind "effect") or a hit ("hit"), in order.
    bool mods(const std::string& kind, int64_t owner, std::vector<ModRow>& out, std::string& error) const;
    std::optional<ProjectileRow> projectile(int64_t guid, std::string& error) const;
    std::optional<ZoneRow> zone(int64_t guid, std::string& error) const;

    /// The guids of every ability, hit, effect, projectile or zone of the tables, in guid order (what a catalog that holds them all loads).
    enum class Table { Ability, Hit, Effect, Projectile, Zone };
    bool guids(Table table, std::vector<int64_t>& out, std::string& error) const;
    /// Every expression of ab_expr by id.
    bool all_exprs(std::map<int64_t, ExprRow>& out, std::string& error) const;

private:
    AbilityContent() = default;

    sqlite3*    db_ = nullptr;
    std::string path_;
    int         contentc_version_ = 0;
};

}  // namespace aoc::worldd::content

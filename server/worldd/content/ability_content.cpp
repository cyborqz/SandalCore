// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  content/ability_content.cpp  (plan D2)
// ============================================================================
#include "content/ability_content.h"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <system_error>
#include <utility>

namespace aoc::worldd::content {

namespace {

/// A prepared statement, finalized on every path out.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr); }
    ~Stmt() { if (stmt_) sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    bool ok() const { return rc_ == SQLITE_OK && stmt_ != nullptr; }
    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
    int           rc_ = SQLITE_OK;
};

std::string col_text(sqlite3_stmt* s, int col) {
    const unsigned char* t = sqlite3_column_text(s, col);
    return t ? std::string(reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(s, col))) : std::string();
}
int64_t col_i64(sqlite3_stmt* s, int col) { return sqlite3_column_int64(s, col); }
int col_int(sqlite3_stmt* s, int col) { return static_cast<int>(sqlite3_column_int64(s, col)); }
bool col_bool(sqlite3_stmt* s, int col) { return sqlite3_column_int64(s, col) != 0; }
double col_f64(sqlite3_stmt* s, int col) { return sqlite3_column_double(s, col); }

std::string db_message(sqlite3* db) {
    const char* m = sqlite3_errmsg(db);
    return m ? m : "unknown SQLite error";
}

/// run executes `sql` with `bind`, calls `row` for each row, and reports a failure in `error`.
bool run(sqlite3* db, const char* sql, const std::function<void(sqlite3_stmt*)>& bind, const std::function<bool(sqlite3_stmt*)>& row, std::string& error) {
    Stmt q(db, sql);
    if (!q.ok()) {
        error = std::string("content.db: ") + db_message(db);
        return false;
    }
    if (bind) bind(q.get());
    int step;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        if (!row(q.get())) return false;
    }
    if (step != SQLITE_DONE) {
        error = std::string("content.db: ") + db_message(db);
        return false;
    }
    return true;
}
// C.Y.B - CoA

bool parse_strings(const std::string& text, std::vector<std::string>& out, const char* what, std::string& error) {
    out.clear();
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) {
        error = std::string("content.db: ") + what + " is not a JSON array";
        return false;
    }
    for (const auto& e : j) {
        if (!e.is_string()) {
            error = std::string("content.db: ") + what + " holds a value that is no string";
            return false;
        }
        out.push_back(e.get<std::string>());
    }
    return true;
}

// The statements of the reader. open() prepares each once, so a missing table or column fails at the open.
constexpr const char* kMetaSql = "SELECT key, value FROM meta";
constexpr const char* kStatProgramSql = "SELECT data FROM stat_program WHERE id = 1";
constexpr const char* kExprSql = "SELECT id, text, terms, program, uses_target, error FROM ab_expr WHERE id = ?1";
// The ranks are plan D1's tables (skill_tree, skill_node, skill_rank): the one source of ranks.
constexpr const char* kClassAbilitiesSql = "SELECT DISTINCT r.ability_guid FROM skill_tree t JOIN skill_node n ON n.tree_guid = t.guid "
                                           "JOIN skill_rank r ON r.skill_guid = n.skill_guid WHERE t.class_guid = ?1 AND r.ability_guid <> 0 "
                                           "ORDER BY r.ability_guid";
constexpr const char* kAutoRanksSql = "SELECT DISTINCT tree_guid, skill_guid, level_requirement FROM skill_node WHERE auto_granted = 1 "
                                      "ORDER BY tree_guid, skill_guid";
constexpr const char* kAbilityByGuidSql =
    "SELECT guid, name, display_name, tags, held_behavior, activation_input, target_requirement, fallback_target, applicable_targets, applicable_types, "
    "weapon_requirement, shared_cooldown_tag, disable_during_cooldown, max_hold_time, hold_animation, usable_in_air, usable_while_swimming, "
    "usable_while_walking, usable_while_mounted, usable_from_hotbar, area_range, area_radius, networking_max_speed, cooldown_expr, charges_expr, cast_expr, "
    "requirement_expr, can_break_expr, has_proc_expr, extra FROM ab_ability WHERE guid = ?1";
constexpr const char* kAbilityByNameSql =
    "SELECT guid, name, display_name, tags, held_behavior, activation_input, target_requirement, fallback_target, applicable_targets, applicable_types, "
    "weapon_requirement, shared_cooldown_tag, disable_during_cooldown, max_hold_time, hold_animation, usable_in_air, usable_while_swimming, "
    "usable_while_walking, usable_while_mounted, usable_from_hotbar, area_range, area_radius, networking_max_speed, cooldown_expr, charges_expr, cast_expr, "
    "requirement_expr, can_break_expr, has_proc_expr, extra FROM ab_ability WHERE name = ?1";
constexpr const char* kCostsSql = "SELECT cost_ord, stat_ord, enabled_expr, triggers_cooldown, charge_cost_expr, stat_guid, stat_name, amount_expr "
                                  "FROM ab_cost WHERE ability_guid = ?1 ORDER BY cost_ord, stat_ord";
constexpr const char* kEffectCostsSql = "SELECT cost_ord, ord, effect_guid, filter, num_expr FROM ab_effect_cost WHERE ability_guid = ?1 ORDER BY cost_ord, ord";
constexpr const char* kMontagesSql = "SELECT anim_index, soft_path, direction, select_expr, start_section, targeting_key, net_guid FROM ab_montage "
                                     "WHERE ability_guid = ?1 ORDER BY anim_index";
constexpr const char* kTargetingsSql = "SELECT key, targeting_guid, targeting_name FROM ab_targeting WHERE ability_guid = ?1 ORDER BY key";
constexpr const char* kAbilityHitsSql = "SELECT source, key, owner_guid, role, ord, hit_guid FROM ab_ability_hit WHERE ability_guid = ?1 "
                                        "ORDER BY source, key, owner_guid, role, ord";
constexpr const char* kAbilityEffectsSql = "SELECT key, effect_guid FROM ab_ability_effect WHERE ability_guid = ?1 ORDER BY key";
constexpr const char* kHitSql = "SELECT guid, name, net_type, applicable_targets, applicable_types, fallback_target, requirement_expr, delay_expr, "
                                "threat_expr, max_targets_expr, extra FROM ab_hit WHERE guid = ?1";
constexpr const char* kHitEffectsSql = "SELECT kind, ord, effect_guid, requirement_expr, stack_count_expr, duration_expr, on_hit_behavior, max_applications "
                                       "FROM ab_hit_effect WHERE hit_guid = ?1 ORDER BY kind, ord";
constexpr const char* kHitProjectilesSql = "SELECT projectile_guid FROM ab_hit_projectile WHERE hit_guid = ?1 AND kind = 'fire' ORDER BY ord";
constexpr const char* kEffectByGuidSql =
    "SELECT guid, name, display_name, category, tags, effect_tags, infinite_duration, duration_expr, tick_timer, ignore_initial_tick, stack_type, "
    "global_stack_limit, in_place_stack_limit_expr, stack_duration_refresh, persists_through_death, persists_relog, dispellable, significant, "
    "requirement_expr, crowd_control_guid, extra FROM ab_effect WHERE guid = ?1";
constexpr const char* kEffectByNameSql =
    "SELECT guid, name, display_name, category, tags, effect_tags, infinite_duration, duration_expr, tick_timer, ignore_initial_tick, stack_type, "
    "global_stack_limit, in_place_stack_limit_expr, stack_duration_refresh, persists_through_death, persists_relog, dispellable, significant, "
    "requirement_expr, crowd_control_guid, extra FROM ab_effect WHERE name = ?1";
constexpr const char* kEffectHitsSql = "SELECT phase, ord, hit_guid FROM ab_effect_hit WHERE effect_guid = ?1 ORDER BY phase, ord";
constexpr const char* kModsSql = "SELECT ord, stat_mod_guid, stat_guid, stat_name, value_expr, shield_absorb FROM ab_mod WHERE owner_kind = ?1 AND owner_guid = ?2 ORDER BY ord";
constexpr const char* kProjectileSql = "SELECT guid, name, projectile_type, speed, range, life_time, max_hits, chain_hit_count, chain_radius, sphere_radius "
                                       "FROM ab_projectile WHERE guid = ?1";
constexpr const char* kProjectileHitsSql = "SELECT hit_guid FROM ab_projectile_hit WHERE projectile_guid = ?1 ORDER BY ord";
constexpr const char* kZoneSql = "SELECT guid, name, life_time, tick_rate, use_global_tick_rate, charges, delay_time, delay_type, speed, "
                                 "expire_after_hits_expr, expire_if_expr, targeting_guid FROM ab_zone WHERE guid = ?1";
constexpr const char* kZoneHitsSql = "SELECT role, hit_guid FROM ab_zone_hit WHERE zone_guid = ?1 ORDER BY role, ord";
constexpr const char* kAllExprsSql = "SELECT id, text, terms, program, uses_target, error FROM ab_expr ORDER BY id";
constexpr const char* kGuidListSql[] = {"SELECT guid FROM ab_ability ORDER BY guid", "SELECT guid FROM ab_hit ORDER BY guid",
                                        "SELECT guid FROM ab_effect ORDER BY guid", "SELECT guid FROM ab_projectile ORDER BY guid",
                                        "SELECT guid FROM ab_zone ORDER BY guid"};

}  // namespace

AbilityContent::AbilityContent(AbilityContent&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), path_(std::move(other.path_)), contentc_version_(other.contentc_version_) {}

AbilityContent& AbilityContent::operator=(AbilityContent&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = std::exchange(other.db_, nullptr);
        path_ = std::move(other.path_);
        contentc_version_ = other.contentc_version_;
    }
    return *this;
}

AbilityContent::~AbilityContent() {
    if (db_) sqlite3_close(db_);
}
// CoA and C:Y:B

std::optional<AbilityContent> AbilityContent::open(const std::string& path, std::string& error) {
    std::error_code ec;
    const std::filesystem::path fs_path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    if (!std::filesystem::is_regular_file(fs_path, ec)) {
        error = "content.db not found at " + path + " (build it with: cd services && go run ./cmd/contentc)";
        return std::nullopt;
    }
    AbilityContent c;
    c.path_ = path;
    const int rc = sqlite3_open_v2(path.c_str(), &c.db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        error = "cannot open " + path + ": " + (c.db_ ? db_message(c.db_) : std::string("out of memory"));
        return std::nullopt;
    }
    std::map<std::string, std::string> meta;
    std::string e;
    if (!run(c.db_, kMetaSql, nullptr, [&](sqlite3_stmt* s) {
            meta[col_text(s, 0)] = col_text(s, 1);
            return true;
        }, e)) {
        error = path + " is not a content.db: " + e;
        return std::nullopt;
    }
    const auto schema = meta.find("schema_version");
    if (schema == meta.end() || schema->second != "1") {
        error = path + " has schema version \"" + (schema == meta.end() ? "" : schema->second) + "\", this reader wants 1";
        return std::nullopt;
    }
    const auto version = meta.find("contentc.version");
    c.contentc_version_ = version == meta.end() ? 0 : std::atoi(version->second.c_str());
    if (c.contentc_version_ < kMinAbilityContentcVersion) {
        error = path + " was built by contentc version " + (version == meta.end() ? "?" : version->second) + ", and the ability tables need version " +
                std::to_string(kMinAbilityContentcVersion) + " or later (rebuild it with: cd services && go run ./cmd/contentc)";
        return std::nullopt;
    }
    // every statement prepares: a missing table or column fails here
    for (const char* sql : {kStatProgramSql, kExprSql, kClassAbilitiesSql, kAutoRanksSql, kAbilityByGuidSql, kAbilityByNameSql, kCostsSql, kEffectCostsSql,
                            kMontagesSql, kAbilityHitsSql, kAbilityEffectsSql, kHitSql, kHitEffectsSql, kEffectByGuidSql, kEffectByNameSql, kEffectHitsSql,
                            kModsSql, kProjectileSql, kProjectileHitsSql, kZoneSql, kZoneHitsSql, kAllExprsSql, kGuidListSql[0], kGuidListSql[1], kGuidListSql[2],
                            kGuidListSql[3], kGuidListSql[4]}) {
        Stmt q(c.db_, sql);
        if (!q.ok()) {
            error = path + " lacks a table or column the ability reader uses: " + db_message(c.db_);
            return std::nullopt;
        }
    }
    return c;
}

std::optional<std::vector<uint8_t>> AbilityContent::stat_program_data(std::string& error) const {
    std::vector<uint8_t> data;
    bool found = false;
    if (!run(db_, kStatProgramSql, nullptr, [&](sqlite3_stmt* s) {
            const void* p = sqlite3_column_blob(s, 0);
            const int n = sqlite3_column_bytes(s, 0);
            data.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
            found = true;
            return true;
        }, error))
        return std::nullopt;
    if (!found) {
        error = "content.db has no stat_program row";
        return std::nullopt;
    }
    return data;
}

std::optional<ExprRow> AbilityContent::expr(int64_t id, std::string& error) const {
    std::optional<ExprRow> out;
    bool ok = run(db_, kExprSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, id); }, [&](sqlite3_stmt* s) {
        ExprRow e;
        e.id = col_i64(s, 0);
        e.text = col_text(s, 1);
        const nlohmann::json terms = nlohmann::json::parse(col_text(s, 2), nullptr, false);
        if (!terms.is_object()) {
            error = "content.db: expression " + std::to_string(id) + ": terms are no JSON object";
            return false;
        }
        for (auto it = terms.begin(); it != terms.end(); ++it) {
            char* end = nullptr;
            const long long g = std::strtoll(it.key().c_str(), &end, 10);
            if (!end || *end != 0 || !it.value().is_string()) {
                error = "content.db: expression " + std::to_string(id) + ": a bad term";
                return false;
            }
            e.terms[g] = it.value().get<std::string>();
        }
        e.program = col_int(s, 3);
        e.uses_target = col_bool(s, 4);
        e.error = col_text(s, 5);
        out = std::move(e);
        return true;
    }, error);
    if (!ok) return std::nullopt;
    if (!out) error = "content.db has no expression " + std::to_string(id);
    return out;
}

bool AbilityContent::class_ability_guids(int64_t class_guid, std::vector<int64_t>& out, std::string& error) const {
    out.clear();
    return run(db_, kClassAbilitiesSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, class_guid); }, [&](sqlite3_stmt* s) {
        out.push_back(col_i64(s, 0));
        return true;
    }, error);
}

bool AbilityContent::auto_ranks_at(int level, std::vector<stats::SkillRank>& out, std::string& error) const {
    out.clear();
    return run(db_, kAutoRanksSql, nullptr, [&](sqlite3_stmt* s) {
        if (col_int(s, 2) <= level) {
            stats::SkillRank k;
            k.cls = {stats::kRefTypeSkillTree, col_i64(s, 0)};
            k.skill = {stats::kRefTypeSkill, col_i64(s, 1)};
            out.push_back(k);
        }
        return true;
    }, error);
}

namespace {

bool read_ability(sqlite3_stmt* s, AbilityRow& a, std::string& error) {
    a.guid = col_i64(s, 0);
    a.name = col_text(s, 1);
    a.display_name = col_text(s, 2);
    if (!parse_strings(col_text(s, 3), a.tags, "ability tags", error)) return false;
    a.held_behavior = col_text(s, 4);
    a.activation_input = col_text(s, 5);
    a.target_requirement = col_text(s, 6);
    a.fallback_target = col_text(s, 7);
    a.applicable_targets = col_int(s, 8);
    a.applicable_types = col_int(s, 9);
    a.weapon_requirement = col_text(s, 10);
    a.shared_cooldown_tag = col_text(s, 11);
    a.disable_during_cooldown = col_bool(s, 12);
    a.max_hold_time = col_f64(s, 13);
    a.hold_animation = col_bool(s, 14);
    a.usable_in_air = col_bool(s, 15);
    a.usable_while_swimming = col_bool(s, 16);
    a.usable_while_walking = col_bool(s, 17);
    a.usable_while_mounted = col_bool(s, 18);
    a.usable_from_hotbar = col_bool(s, 19);
    a.area_range = col_f64(s, 20);
    a.area_radius = col_f64(s, 21);
    a.networking_max_speed = col_f64(s, 22);
    a.cooldown_expr = col_i64(s, 23);
    a.charges_expr = col_i64(s, 24);
    a.cast_expr = col_i64(s, 25);
    a.requirement_expr = col_i64(s, 26);
    a.can_break_expr = col_i64(s, 27);
    a.has_proc_expr = col_i64(s, 28);
    a.extra = col_text(s, 29);
    return true;
}

bool read_effect(sqlite3_stmt* s, EffectRow& e, std::string& error) {
    e.guid = col_i64(s, 0);
    e.name = col_text(s, 1);
    e.display_name = col_text(s, 2);
    e.category = col_text(s, 3);
    if (!parse_strings(col_text(s, 4), e.tags, "effect tags", error) || !parse_strings(col_text(s, 5), e.effect_tags, "effect effect_tags", error)) return false;
    e.infinite_duration = col_bool(s, 6);
    e.duration_expr = col_i64(s, 7);
    e.tick_timer = col_f64(s, 8);
    e.ignore_initial_tick = col_bool(s, 9);
    e.stack_type = col_text(s, 10);
    e.global_stack_limit = col_int(s, 11);
    e.in_place_stack_limit_expr = col_i64(s, 12);
    e.stack_duration_refresh = col_text(s, 13);
    e.persists_through_death = col_bool(s, 14);
    e.persists_relog = col_bool(s, 15);
    e.dispellable = col_bool(s, 16);
    e.significant = col_bool(s, 17);
    e.requirement_expr = col_i64(s, 18);
    e.crowd_control_guid = col_i64(s, 19);
    e.extra = col_text(s, 20);
    return true;
}
// Cyb with C | O | A

}  // namespace

std::optional<AbilityRow> AbilityContent::ability(int64_t guid, std::string& error) const {
    std::optional<AbilityRow> out;
    if (!run(db_, kAbilityByGuidSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            AbilityRow a;
            if (!read_ability(s, a, error)) return false;
            out = std::move(a);
            return true;
        }, error))
        return std::nullopt;
    if (!out) error = "content.db has no ability " + std::to_string(guid);
    return out;
}

std::optional<AbilityRow> AbilityContent::ability_by_name(const std::string& name, std::string& error) const {
    std::optional<AbilityRow> out;
    if (!run(db_, kAbilityByNameSql, [&](sqlite3_stmt* s) { sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT); }, [&](sqlite3_stmt* s) {
            AbilityRow a;
            if (!read_ability(s, a, error)) return false;
            out = std::move(a);
            return true;
        }, error))
        return std::nullopt;
    if (!out) error = "content.db has no ability called " + name;
    return out;
}

bool AbilityContent::costs(int64_t ability, std::vector<CostRow>& out, std::string& error) const {
    out.clear();
    return run(db_, kCostsSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, ability); }, [&](sqlite3_stmt* s) {
        CostRow c;
        c.cost_ord = col_int(s, 0);
        c.stat_ord = col_int(s, 1);
        c.enabled_expr = col_i64(s, 2);
        c.triggers_cooldown = col_bool(s, 3);
        c.charge_cost_expr = col_i64(s, 4);
        c.stat_guid = col_i64(s, 5);
        c.stat_name = col_text(s, 6);
        c.amount_expr = col_i64(s, 7);
        out.push_back(std::move(c));
        return true;
    }, error);
}

bool AbilityContent::effect_costs(int64_t ability, std::vector<EffectCostRow>& out, std::string& error) const {
    out.clear();
    return run(db_, kEffectCostsSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, ability); }, [&](sqlite3_stmt* s) {
        EffectCostRow c;
        c.cost_ord = col_int(s, 0);
        c.ord = col_int(s, 1);
        c.effect_guid = col_i64(s, 2);
        c.filter = col_text(s, 3);
        c.num_expr = col_i64(s, 4);
        out.push_back(std::move(c));
        return true;
    }, error);
}
// (C | Y | B | C-O-A)

bool AbilityContent::targetings(int64_t ability, std::vector<TargetingRow>& out, std::string& error) const {
    out.clear();
    return run(db_, kTargetingsSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, ability); }, [&](sqlite3_stmt* s) {
        TargetingRow t;
        t.key = col_int(s, 0);
        t.guid = col_i64(s, 1);
        t.name = col_text(s, 2);
        out.push_back(std::move(t));
        return true;
    }, error);
}

bool AbilityContent::montages(int64_t ability, std::vector<MontageRow>& out, std::string& error) const {
    out.clear();
    return run(db_, kMontagesSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, ability); }, [&](sqlite3_stmt* s) {
        MontageRow m;
        m.anim_index = col_int(s, 0);
        m.soft_path = col_text(s, 1);
        m.direction = col_text(s, 2);
        m.select_expr = col_i64(s, 3);
        m.start_section = col_text(s, 4);
        m.targeting_key = col_i64(s, 5);
        if (sqlite3_column_type(s, 6) != SQLITE_NULL) m.net_guid = static_cast<uint64_t>(col_i64(s, 6));
        out.push_back(std::move(m));
        return true;
    }, error);
}

bool AbilityContent::ability_hits(int64_t ability, std::vector<AbilityHitRow>& out, std::string& error) const {
    out.clear();
    return run(db_, kAbilityHitsSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, ability); }, [&](sqlite3_stmt* s) {
        AbilityHitRow h;
        h.source = col_text(s, 0);
        h.key = col_i64(s, 1);
        h.owner_guid = col_i64(s, 2);
        h.role = col_text(s, 3);
        h.ord = col_int(s, 4);
        h.hit_guid = col_i64(s, 5);
        out.push_back(std::move(h));
        return true;
    }, error);
}

bool AbilityContent::ability_effects(int64_t ability, std::vector<std::pair<int64_t, int64_t>>& key_effect, std::string& error) const {
    key_effect.clear();
    return run(db_, kAbilityEffectsSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, ability); }, [&](sqlite3_stmt* s) {
        key_effect.push_back({col_i64(s, 0), col_i64(s, 1)});
        return true;
    }, error);
}

std::optional<HitRow> AbilityContent::hit(int64_t guid, std::string& error) const {
    std::optional<HitRow> out;
    if (!run(db_, kHitSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            HitRow h;
            h.guid = col_i64(s, 0);
            h.name = col_text(s, 1);
            h.net_type = col_text(s, 2);
            h.applicable_targets = col_int(s, 3);
            h.applicable_types = col_int(s, 4);
            h.fallback_target = col_text(s, 5);
            h.requirement_expr = col_i64(s, 6);
            h.delay_expr = col_i64(s, 7);
            h.threat_expr = col_i64(s, 8);
            h.max_targets_expr = col_i64(s, 9);
            h.extra = col_text(s, 10);
            h.has_fx = h.extra.find("\"HitFX\":true") != std::string::npos;
            out = std::move(h);
            return true;
        }, error))
        return std::nullopt;
    if (!out) error = "content.db has no hit " + std::to_string(guid);
    return out;
}

bool AbilityContent::hit_effects(int64_t hit, std::vector<HitEffectRow>& out, std::string& error) const {
    out.clear();
    return run(db_, kHitEffectsSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, hit); }, [&](sqlite3_stmt* s) {
        HitEffectRow h;
        h.kind = col_text(s, 0);
        h.ord = col_int(s, 1);
        h.effect_guid = col_i64(s, 2);
        h.requirement_expr = col_i64(s, 3);
        h.stack_count_expr = col_i64(s, 4);
        h.duration_expr = col_i64(s, 5);
        h.on_hit_behavior = col_text(s, 6);
        h.max_applications = col_int(s, 7);
        out.push_back(std::move(h));
        return true;
    }, error);
}

bool AbilityContent::hit_projectiles(int64_t hit, std::vector<int64_t>& out, std::string& error) const {
    out.clear();
    return run(db_, kHitProjectilesSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, hit); }, [&](sqlite3_stmt* s) {
        out.push_back(col_i64(s, 0));
        return true;
    }, error);
}

std::optional<EffectRow> AbilityContent::effect(int64_t guid, std::string& error) const {
    std::optional<EffectRow> out;
    if (!run(db_, kEffectByGuidSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            EffectRow e;
            if (!read_effect(s, e, error)) return false;
            out = std::move(e);
            return true;
        }, error))
        return std::nullopt;
    if (!out) error = "content.db has no effect " + std::to_string(guid);
    return out;
}

std::optional<EffectRow> AbilityContent::effect_by_name(const std::string& name, std::string& error) const {
    std::optional<EffectRow> out;
    if (!run(db_, kEffectByNameSql, [&](sqlite3_stmt* s) { sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT); }, [&](sqlite3_stmt* s) {
            EffectRow e;
            if (!read_effect(s, e, error)) return false;
            out = std::move(e);
            return true;
        }, error))
        return std::nullopt;
    if (!out) error = "content.db has no effect called " + name;
    return out;
}
// C-O-A captures, C-Y-B code

bool AbilityContent::effect_hits(int64_t effect, std::vector<EffectHitRow>& out, std::string& error) const {
    out.clear();
    return run(db_, kEffectHitsSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, effect); }, [&](sqlite3_stmt* s) {
        EffectHitRow h;
        h.phase = col_text(s, 0);
        h.ord = col_int(s, 1);
        h.hit_guid = col_i64(s, 2);
        out.push_back(std::move(h));
        return true;
    }, error);
}

bool AbilityContent::mods(const std::string& kind, int64_t owner, std::vector<ModRow>& out, std::string& error) const {
    out.clear();
    return run(db_, kModsSql, [&](sqlite3_stmt* s) {
        sqlite3_bind_text(s, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, owner);
    }, [&](sqlite3_stmt* s) {
        ModRow m;
        m.ord = col_int(s, 0);
        m.stat_mod_guid = col_i64(s, 1);
        m.stat_guid = col_i64(s, 2);
        m.stat_name = col_text(s, 3);
        m.value_expr = col_i64(s, 4);
        m.shield_absorb = col_bool(s, 5);
        out.push_back(std::move(m));
        return true;
    }, error);
}

std::optional<ProjectileRow> AbilityContent::projectile(int64_t guid, std::string& error) const {
    std::optional<ProjectileRow> out;
    if (!run(db_, kProjectileSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            ProjectileRow p;
            p.guid = col_i64(s, 0);
            p.name = col_text(s, 1);
            p.projectile_type = col_text(s, 2);
            p.speed = col_f64(s, 3);
            p.range = col_f64(s, 4);
            p.life_time = col_f64(s, 5);
            p.max_hits = col_int(s, 6);
            p.chain_hit_count = col_int(s, 7);
            p.chain_radius = col_f64(s, 8);
            p.sphere_radius = col_f64(s, 9);
            out = std::move(p);
            return true;
        }, error))
        return std::nullopt;
    if (!out) {
        error = "content.db has no projectile " + std::to_string(guid);
        return std::nullopt;
    }
    if (!run(db_, kProjectileHitsSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            out->hits.push_back(col_i64(s, 0));
            return true;
        }, error))
        return std::nullopt;
    return out;
}
// ~ C.Y.B - C.O.A ~

std::optional<ZoneRow> AbilityContent::zone(int64_t guid, std::string& error) const {
    std::optional<ZoneRow> out;
    if (!run(db_, kZoneSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            ZoneRow z;
            z.guid = col_i64(s, 0);
            z.name = col_text(s, 1);
            z.life_time = col_f64(s, 2);
            z.tick_rate = col_f64(s, 3);
            z.use_global_tick_rate = col_bool(s, 4);
            z.charges = col_int(s, 5);
            z.delay_time = col_f64(s, 6);
            z.delay_type = col_text(s, 7);
            z.speed = col_f64(s, 8);
            z.expire_after_hits_expr = col_i64(s, 9);
            z.expire_if_expr = col_i64(s, 10);
            z.targeting_guid = col_i64(s, 11);
            out = std::move(z);
            return true;
        }, error))
        return std::nullopt;
    if (!out) {
        error = "content.db has no lingering zone " + std::to_string(guid);
        return std::nullopt;
    }
    if (!run(db_, kZoneHitsSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            out->hits[col_text(s, 0)].push_back(col_i64(s, 1));
            return true;
        }, error))
        return std::nullopt;
    return out;
}

bool AbilityContent::guids(Table table, std::vector<int64_t>& out, std::string& error) const {
    out.clear();
    return run(db_, kGuidListSql[static_cast<size_t>(table)], nullptr, [&](sqlite3_stmt* s) {
        out.push_back(col_i64(s, 0));
        return true;
    }, error);
}

bool AbilityContent::all_exprs(std::map<int64_t, ExprRow>& out, std::string& error) const {
    out.clear();
    return run(db_, kAllExprsSql, nullptr, [&](sqlite3_stmt* s) {
        ExprRow e;
        e.id = col_i64(s, 0);
        e.text = col_text(s, 1);
        const nlohmann::json terms = nlohmann::json::parse(col_text(s, 2), nullptr, false);
        if (!terms.is_object()) {
            error = "content.db: expression " + std::to_string(e.id) + ": terms are no JSON object";
            return false;
        }
        for (auto it = terms.begin(); it != terms.end(); ++it) {
            char* end = nullptr;
            const long long g = std::strtoll(it.key().c_str(), &end, 10);
            if (!end || *end != 0 || !it.value().is_string()) {
                error = "content.db: expression " + std::to_string(e.id) + ": a bad term";
                return false;
            }
            e.terms[g] = it.value().get<std::string>();
        }
        e.program = col_int(s, 3);
        e.uses_target = col_bool(s, 4);
        e.error = col_text(s, 5);
        out[e.id] = std::move(e);
        return true;
    }, error);
}

}  // namespace aoc::worldd::content

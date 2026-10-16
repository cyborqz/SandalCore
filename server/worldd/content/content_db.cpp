// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  content/content_db.cpp
// ============================================================================
#include "content/content_db.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <tuple>
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

    bool        ok() const { return rc_ == SQLITE_OK && stmt_ != nullptr; }
    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
    int           rc_ = SQLITE_OK;
};

std::string column_text(sqlite3_stmt* s, int col) {
    const unsigned char* t = sqlite3_column_text(s, col);
    return t ? std::string(reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(s, col))) : std::string();
}

std::string db_message(sqlite3* db) {
    const char* m = sqlite3_errmsg(db);
    return m ? m : "unknown SQLite error";
}
// (C:Y:B) (CoA)

/// Each statement of the reader, so open() can prepare them once and fail on a missing table or column.
constexpr const char* kMetaSql = "SELECT key, value FROM meta";
constexpr const char* kClassSql = "SELECT guid, name, archetype_id FROM class WHERE archetype_id = ?1";
constexpr const char* kClassByNameSql = "SELECT guid, name, archetype_id FROM class WHERE name = ?1";
constexpr const char* kAttributesSql =
    "SELECT stat_guid, stat_name, value, rounding_ambiguous FROM class_stat_base "
    "WHERE class_guid = ?1 AND level = ?2 ORDER BY stat_guid";
// plan A7 (contentc version 2): the columns of schema.go's ability_runtime, effect_runtime, effect_stat_runtime, stamina_rule
constexpr const char* kAbilityRuntimeSql =
    "SELECT ability_guid, name, held_behavior, requires_out_of_combat, min_stamina, cost_stat_guid, cost_amount, "
    "cost_requires_moving, charge_interval, cooldown, triggers_cooldown, disable_during_cooldown, targets_self, effect_guid, "
    "montage_path, cancel_section FROM ability_runtime WHERE ability_guid = ?1";
constexpr const char* kEffectRuntimeSql =
    "SELECT effect_guid, name, infinite, duration, tick_timer, stops_regen_standing, stops_regen_moving, end_effect_guid, "
    "end_effect_duration FROM effect_runtime";
constexpr const char* kEffectStatSql =
    "SELECT effect_guid, stat_guid, source_stat_guid, factor, addend FROM effect_stat_runtime ORDER BY effect_guid, ord";
constexpr const char* kStaminaRuleSql = "SELECT value FROM stamina_rule WHERE key = ?1";
// plan B2 (contentc version 3): schema.go's item and item_equip_slot
constexpr const char* kItemSql = "SELECT guid, name, can_equip, occupies_all_equip_slots, default_quality, max_stack_size, max_durability, inv_w, inv_h, resource FROM item WHERE guid = ?1";
// Gathering stage 3 (contentc version 14): schema.go's resource_bag and resource_bag_cell
constexpr const char* kResourceBagSql = "SELECT def_guid, def_name, columns, rows, custom FROM resource_bag WHERE item_guid = ?1";
constexpr const char* kResourceBagCellSql = "SELECT col, row, value FROM resource_bag_cell WHERE def_guid = ?1 ORDER BY col, row";
// Gathering stage 3, "the pick-up popup" (contentc version 16): schema.go's item.display_name.  Not among the statements every file must answer: a file of version 15 has no such column
constexpr const char* kItemNameSql = "SELECT display_name FROM item WHERE guid = ?1";
constexpr const char* kItemSlotsSql = "SELECT slot FROM item_equip_slot WHERE item_guid = ?1 ORDER BY ord";
// plan B3 (contentc version 4): schema.go's stat_program, item (stat_block_guid, quality_curve_guid), quality_curve, stat_block_stat, item_set,
// item_tag, set_bonus and equip_slot
constexpr const char* kStatProgramSql = "SELECT version, mode, stats, classes, crc32, data FROM stat_program WHERE id = 1";
constexpr const char* kItemGearSql = "SELECT stat_block_guid, quality_curve_guid FROM item WHERE guid = ?1";
constexpr const char* kQualityCurveSql = "SELECT ord, x, y FROM quality_curve WHERE guid = ?1 ORDER BY ord";
constexpr const char* kStatBlockSql =
    "SELECT stat_guid, common_min, common_max, uncommon_min, uncommon_max, rare_min, rare_max, heroic_min, heroic_max, epic_min, epic_max, "
    "legendary_min, legendary_max, artifact_min, artifact_max FROM stat_block_stat WHERE block_guid = ?1 ORDER BY stat_guid";
constexpr const char* kItemSetSql = "SELECT set_guid FROM item_set WHERE item_guid = ?1 ORDER BY ord";
constexpr const char* kItemTagSql = "SELECT tag FROM item_tag WHERE item_guid = ?1 ORDER BY ord";
constexpr const char* kSetBonusSql =
    "SELECT tier_ord, pieces, stat_guid, common, uncommon, rare, heroic, epic, legendary, artifact FROM set_bonus WHERE set_guid = ?1 "
    "ORDER BY tier_ord, bonus_ord";
constexpr const char* kEquipSlotSql = "SELECT slot, name FROM equip_slot";
// plan D1 (contentc version 5): schema.go's item_use, item_action, action_response, action_reward, reward_table, reward_item, reward_xp,
// leveling_type, leveling_curve, skill_auto_grant and skill_rank_stat_mod
constexpr const char* kItemUseSql = "SELECT item_guid, activation, consume_on_use, cooldown, disabled_in_combat FROM item_use WHERE item_guid = ?1";
constexpr const char* kItemActionSql = "SELECT action_guid FROM item_action WHERE item_guid = ?1 ORDER BY ord";
constexpr const char* kActionSql = "SELECT guid, name, response_type, class_guid, class_name, remove_up_to_quantity FROM action_response WHERE guid = ?1";
constexpr const char* kActionRewardSql = "SELECT reward_table_guid FROM action_reward WHERE action_guid = ?1 ORDER BY ord";
constexpr const char* kRewardTableSql = "SELECT guid, name, predicate, level_below, modelled FROM reward_table WHERE guid = ?1";
constexpr const char* kRewardItemSql = "SELECT item_guid, variation_guid, quantity FROM reward_item WHERE table_guid = ?1 ORDER BY ord";
constexpr const char* kRewardXpSql = "SELECT experience_type, target_level, margin, curve_guid, mult_stat_guid, expr, expression FROM reward_xp WHERE table_guid = ?1 ORDER BY ord";
// Gathering stage 2 (contentc version 13): schema.go's profession, certification_level and action_certification (internal/content/gather.go)
constexpr const char* kProfessionTableSql = "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN ('profession', 'certification_level', 'action_certification')";
constexpr const char* kProfessionSql =
    "SELECT p.guid, p.name, p.category, p.leveling_type_guid, p.skill_tree_guid, l.name FROM profession p LEFT JOIN leveling_type l ON l.guid = p.leveling_type_guid ORDER BY p.guid";
constexpr const char* kCertificationSql =
    "SELECT guid, name, tier, min_level, max_level, has_max_allowable, max_allowable FROM certification_level ORDER BY tier";
constexpr const char* kCertificationActionSql = "SELECT action_guid, name, profession_guid, action FROM action_certification ORDER BY action_guid";
// Gathering stage 2, "Harvesting": schema.go's gather_def, gather_tool and item_variation (internal/content/gather.go)
constexpr const char* kGatherTableSql = "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN ('gather_def', 'gather_tool', 'item_variation')";
constexpr const char* kGatherDefSql =
    "SELECT guid, name, empty_duration, override_respawn, interaction_guid, interact_radius, interact_tolerance, allow_in_combat, profession_guid, tool_slot, cert_guid, "
    "cert_tier, base_duration, duration_expr, durability_expr, reward_guid FROM gather_def ORDER BY guid";
constexpr const char* kGatherToolSql =
    "SELECT item_guid, item_name, profession_guid, cert_guid, cert_tier, tool_slot, required_profession, required_cert FROM gather_tool ORDER BY item_guid";
constexpr const char* kItemVariationSql = "SELECT guid, quality_expr, durability_expr FROM item_variation ORDER BY guid";
// Gathering stage 3, "Foliage gathering": schema.go's foliage tables (internal/content/foliage.go)
constexpr const char* kFoliageTableSql = "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN ('foliage_def', 'foliage_state', 'gather_interaction', 'gather_interaction_def')";
constexpr const char* kFoliageDefSql =
    "SELECT guid, name, empty_duration, dynamic_guid, paint_state, gatherable_state, min_state, max_state, state0_guid, state1_guid, state2_guid FROM foliage_def ORDER BY guid";
constexpr const char* kFoliageStateSql =
    "SELECT guid, name, lifecycle_duration, interaction_guid, override_distance, override_angle, collision_radius FROM foliage_state ORDER BY guid";
constexpr const char* kGatherInteractionSql =
    "SELECT guid, name, target_state, profession_guid, cert_guid, cert_tier, tool_slot, base_duration, duration_expr, durability_expr, reward_guid, interaction_guid, interact_radius, "
    "interact_tolerance, allow_in_combat FROM gather_interaction ORDER BY guid";
constexpr const char* kGatherInteractionDefSql =
    "SELECT interaction_guid, name, gather_guid, interact_radius, interact_tolerance, discovery_distance, allow_in_combat FROM gather_interaction_def ORDER BY interaction_guid";
constexpr const char* kLevelingTypeSql = "SELECT guid, name, experience_type, level_cap, uses_debt, xp_mult_stat_guid FROM leveling_type WHERE name = ?1";
constexpr const char* kLevelingCurveSql = "SELECT level, xp FROM leveling_curve WHERE type_guid = ?1 ORDER BY level";
constexpr const char* kAutoGrantSql =
    "SELECT tree_guid, tree_name, leveling_type_guid, node_guid, node_name, level_requirement, skill_guid, skill_name, rank_guid, effect_guid "
    "FROM skill_auto_grant WHERE leveling_type_guid = ?1 ORDER BY level_requirement, node_guid";
constexpr const char* kRankModSql = "SELECT stat_guid, stat_name, value FROM skill_rank_stat_mod WHERE rank_guid = ?1 ORDER BY ord";
// plan D1, stage 2: the talents
constexpr const char* kTreeSql = "SELECT guid, name, kind, leveling_type_guid, class_guid, weapon_line_guid FROM skill_tree ORDER BY guid";
constexpr const char* kNodeSql =
    "SELECT tree_guid, node_guid, ord, name, skill_guid, skill_name, level_requirement, point_requirement, auto_granted, auto_hotbar, all_required, opt, slot_guid "
    "FROM skill_node ORDER BY tree_guid, ord, opt";
constexpr const char* kRequireSql = "SELECT tree_guid, node_guid, ord, skill_guid, slot_node_guid FROM skill_node_require ORDER BY tree_guid, node_guid, ord";
constexpr const char* kExclusiveSql = "SELECT tree_guid, node_guid, ord, skill_guid FROM skill_node_exclusive ORDER BY tree_guid, node_guid, ord";
constexpr const char* kSkillRankSql =
    "SELECT skill_guid, ord, rank_guid, name, ability_guid, ability_name, effect_guid, effect_name, effect_infinite, effect_duration "
    "FROM skill_rank ORDER BY skill_guid, ord";
constexpr const char* kRankCostSql = "SELECT rank_guid, ord, point_type_guid, quantity FROM skill_rank_cost ORDER BY rank_guid, ord";
constexpr const char* kAllRankModSql = "SELECT rank_guid, stat_guid, stat_name, value FROM skill_rank_stat_mod ORDER BY rank_guid, ord";
constexpr const char* kPointTypeSql = "SELECT guid, name FROM skill_point_type";
constexpr const char* kLevelingPointSql = "SELECT type_guid, level, point_type_guid, quantity FROM leveling_point";
constexpr const char* kItemWeaponSql = "SELECT attack_ability_guid, skill_line_guid, skill_line_name FROM item_weapon WHERE item_guid = ?1";

/// Plan B3 review fix (b): the Go readers refuse a value of another storage class than the column's (a REAL where an INTEGER
/// belongs, a TEXT in a REAL column); the C++ readers coerced. These read a column only when SQLite holds it as that class.
bool column_is_int(sqlite3_stmt* s, int col) { return sqlite3_column_type(s, col) == SQLITE_INTEGER; }
bool column_is_real(sqlite3_stmt* s, int col) {
    const int t = sqlite3_column_type(s, col);
    return t == SQLITE_FLOAT || t == SQLITE_INTEGER;   // an INTEGER is a whole REAL: SQLite stores 1.0 as 1
}
bool column_is_text(sqlite3_stmt* s, int col) { return sqlite3_column_type(s, col) == SQLITE_TEXT; }
// C | Y | B captures, CoA code

}  // namespace

ContentDb::ContentDb(ContentDb&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), path_(std::move(other.path_)), meta_(std::move(other.meta_)) {}

ContentDb& ContentDb::operator=(ContentDb&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = std::exchange(other.db_, nullptr);
        path_ = std::move(other.path_);
        meta_ = std::move(other.meta_);
    }
    return *this;
}

ContentDb::~ContentDb() {
    if (db_) sqlite3_close(db_);
}

std::optional<ContentDb> ContentDb::open(const std::string& path, std::string& error) {
    std::error_code ec;
    // the path is UTF-8, as SQLite takes it
    const std::filesystem::path fs_path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    if (!std::filesystem::is_regular_file(fs_path, ec)) {
        // SQLite would also refuse (READONLY never creates), but its message ("unable to open database file") names nothing.
        error = "content.db not found at " + path + " (build it with: cd services && go run ./cmd/contentc)";
        return std::nullopt;
    }

    ContentDb c;
    c.path_ = path;
    // READONLY: never creates, never writes. FULLMUTEX: one connection may be used from several threads.
    const int rc = sqlite3_open_v2(path.c_str(), &c.db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        error = "cannot open " + path + ": " + (c.db_ ? db_message(c.db_) : std::string("out of memory"));
        return std::nullopt;     // ~ContentDb closes the handle
    }

    // meta: what the file is.
    std::map<std::string, std::string> meta;
    {
        Stmt q(c.db_, kMetaSql);
        if (!q.ok()) {
            error = path + " is not a content.db: " + db_message(c.db_);
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) meta[column_text(q.get(), 0)] = column_text(q.get(), 1);
        if (step != SQLITE_DONE) {
            error = path + " cannot be read: " + db_message(c.db_);
            return std::nullopt;
        }
    }
    const auto get = [&](const char* key) -> const std::string* {
        const auto it = meta.find(key);
        return it == meta.end() ? nullptr : &it->second;
    };
    const std::string* version = get("schema_version");
    if (!version || *version != std::to_string(kSchemaVersion)) {
        error = path + " has schema_version " + (version ? "'" + *version + "'" : std::string("(none)")) +
                ", this worldd reads " + std::to_string(kSchemaVersion);
        return std::nullopt;
    }
    const std::string* contentc = get("contentc.version");
    const std::string* sha = get("source.dbc.sha256");
    if (!contentc || !sha) {
        error = path + " has no contentc.version or source.dbc.sha256 in meta: it was not written by contentc";
        return std::nullopt;
    }
    // plan B3: the stat program and the item stat tables are contentc version 4's; an older file says so and how to get a newer
    {
        char* end = nullptr;
        const long v = std::strtol(contentc->c_str(), &end, 10);
        if (contentc->empty() || end != contentc->c_str() + contentc->size()) {
            error = path + " has contentc.version '" + *contentc + "', which is no number: it was not written by contentc";
            return std::nullopt;
        }
        if (v < kMinContentcVersion) {
            error = path + " was built by contentc version " + *contentc + ", this worldd reads version " + std::to_string(kMinContentcVersion) +
                    " or later (the city nodes, contentc version 12): rebuild it with this checkout's contentc: "
                    "cd services && go run ./cmd/contentc";
            return std::nullopt;
        }
    }
    c.meta_.schema_version = kSchemaVersion;
    c.meta_.contentc_version = *contentc;
    c.meta_.source_dbc_sha256 = *sha;
    if (const std::string* built = get("built_at")) c.meta_.built_at = *built;
// [C.Y.B + CoA]

    // The tables and columns the queries use: a file of another shape fails here, not at the first login.
    for (const char* sql : {kClassSql, kClassByNameSql, kAttributesSql, kAbilityRuntimeSql, kEffectRuntimeSql, kEffectStatSql, kStaminaRuleSql, kItemSql,
                            kItemSlotsSql, kStatProgramSql, kItemGearSql, kQualityCurveSql, kStatBlockSql, kItemSetSql, kItemTagSql, kSetBonusSql,
                            kEquipSlotSql, kItemUseSql, kItemActionSql, kActionSql, kActionRewardSql, kRewardTableSql, kRewardItemSql, kRewardXpSql,
                            kLevelingTypeSql, kLevelingCurveSql, kAutoGrantSql, kRankModSql, kTreeSql, kNodeSql, kRequireSql, kExclusiveSql, kSkillRankSql,
                            kRankCostSql, kAllRankModSql, kPointTypeSql, kLevelingPointSql, kItemWeaponSql, kResourceBagSql, kResourceBagCellSql}) {
        Stmt q(c.db_, sql);
        if (!q.ok()) {
            error = path + " does not have the tables this worldd reads: " + db_message(c.db_) +
                    " (rebuild it with this checkout's contentc: cd services && go run ./cmd/contentc)";
            return std::nullopt;
        }
    }

    spdlog::info("[content] {}: contentc version {}, built from the design database sha256 {}{}", path, c.meta_.contentc_version,
                 c.meta_.source_dbc_sha256, c.meta_.built_at.empty() ? "" : (", built " + c.meta_.built_at));
    return std::optional<ContentDb>(std::move(c));
}

std::optional<ClassRecord> ContentDb::class_for_archetype(int64_t archetype_id, std::string& error) const {
    Stmt q(db_, kClassSql);
    if (!q.ok()) {
        error = "content.db class query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_int64(q.get(), 1, archetype_id);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_ROW) {
        ClassRecord r;
        r.guid = sqlite3_column_int64(q.get(), 0);
        r.name = column_text(q.get(), 1);
        r.archetype_id = sqlite3_column_int64(q.get(), 2);
        return r;
    }
    if (step == SQLITE_DONE) {
        error = "no player class has archetype id " + std::to_string(archetype_id) + " in content.db";
    } else {
        error = "content.db class query failed: " + db_message(db_);
    }
    return std::nullopt;
}

std::optional<ClassRecord> ContentDb::class_for_name(const std::string& name, std::string& error) const {
    Stmt q(db_, kClassByNameSql);
    if (!q.ok()) {
        error = "content.db class query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_text(q.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_ROW) {
        ClassRecord r;
        r.guid = sqlite3_column_int64(q.get(), 0);
        r.name = column_text(q.get(), 1);
        r.archetype_id = sqlite3_column_int64(q.get(), 2);
        return r;
    }
    if (step == SQLITE_DONE) {
        error = "no player class named '" + name + "' in content.db";
    } else {
        error = "content.db class query failed: " + db_message(db_);
    }
    return std::nullopt;
}

std::optional<std::vector<PrimaryAttribute>> ContentDb::primary_attributes(int64_t class_guid, int level, std::string& error) const {
    Stmt q(db_, kAttributesSql);
    if (!q.ok()) {
        error = "content.db attribute query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_int64(q.get(), 1, class_guid);
    sqlite3_bind_int(q.get(), 2, level);
    std::vector<PrimaryAttribute> out;
    int step;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        PrimaryAttribute a;
        a.stat_guid = sqlite3_column_int64(q.get(), 0);
        a.stat_name = column_text(q.get(), 1);
        a.value = sqlite3_column_double(q.get(), 2);
        a.rounding_ambiguous = sqlite3_column_int(q.get(), 3) != 0;
        out.push_back(std::move(a));
    }
    if (step != SQLITE_DONE) {
        error = "content.db attribute query failed: " + db_message(db_);
        return std::nullopt;
    }
    if (out.empty()) {
        error = "content.db has no primary attributes for class " + std::to_string(class_guid) + " at level " + std::to_string(level) +
                " (it covers levels " + std::to_string(kMinLevel) + " to " + std::to_string(kMaxLevel) + ")";
        return std::nullopt;
    }
    return out;
}

namespace {
// C-O-A + C-Y-B

/// The shortest interval the runtime accepts for a charge or a regeneration tick: an interval of a few microseconds would make
/// one tick of the runtime's clock run millions of timers (a file built by hand, not contentc's: the real ones are 0.25 s and 1 s).
constexpr double kMinInterval = 0.01;

Held held_of(const std::string& s, bool* known) {
    *known = true;
    if (s == "Smart") return Held::Smart;
    if (s == "Toggle") return Held::Toggle;
    if (s == "Naive") return Held::Naive;
    *known = false;
    return Held::Naive;
}

}  // namespace

std::optional<AbilityRules> ContentDb::ability_rules(std::string& error) const {
    AbilityRules out;
    const auto fail = [&](std::string why) {
        error = "content.db's ability runtime rows are not what this worldd runs: " + std::move(why) +
                " (rebuild it with this checkout's contentc: cd services && go run ./cmd/contentc)";
        return std::nullopt;
    };
    for (AbilityRule* rule : {&out.sprint, &out.dodge, &out.rest}) {
        const int64_t guid = rule == &out.sprint ? kGuidSprint : rule == &out.dodge ? kGuidDodge : kGuidRest;
        Stmt q(db_, kAbilityRuntimeSql);
        if (!q.ok()) return fail(db_message(db_));
        sqlite3_bind_int64(q.get(), 1, guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) return fail("no ability_runtime row for ability " + std::to_string(guid));
        if (step != SQLITE_ROW) return fail(db_message(db_));
        sqlite3_stmt* s = q.get();
        rule->guid = sqlite3_column_int64(s, 0);
        rule->name = column_text(s, 1);
        bool known = false;
        rule->held = held_of(column_text(s, 2), &known);
        if (!known) return fail(rule->name + " has held_behavior " + column_text(s, 2));
        rule->requires_out_of_combat = sqlite3_column_int(s, 3) != 0;
        rule->min_stamina = sqlite3_column_double(s, 4);
        rule->cost_stat_guid = sqlite3_column_int64(s, 5);
        rule->cost_amount = sqlite3_column_double(s, 6);
        rule->cost_requires_moving = sqlite3_column_int(s, 7) != 0;
        rule->charge_interval = sqlite3_column_double(s, 8);
        rule->cooldown = sqlite3_column_double(s, 9);
        rule->triggers_cooldown = sqlite3_column_int(s, 10) != 0;
        rule->disable_during_cooldown = sqlite3_column_int(s, 11) != 0;
        rule->targets_self = sqlite3_column_int(s, 12) != 0;
        rule->effect_guid = sqlite3_column_int64(s, 13);
        rule->montage_path = column_text(s, 14);
        rule->cancel_section = column_text(s, 15);
    }
    {
        Stmt q(db_, kEffectRuntimeSql);
        if (!q.ok()) return fail(db_message(db_));
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            EffectRule e;
            e.guid = sqlite3_column_int64(s, 0);
            e.name = column_text(s, 1);
            e.infinite = sqlite3_column_int(s, 2) != 0;
            e.duration = sqlite3_column_double(s, 3);
            e.tick_timer = sqlite3_column_double(s, 4);
            e.stops_regen_standing = sqlite3_column_int(s, 5) != 0;
            e.stops_regen_moving = sqlite3_column_int(s, 6) != 0;
            e.end_effect_guid = sqlite3_column_int64(s, 7);
            e.end_effect_duration = sqlite3_column_double(s, 8);
            out.effects[e.guid] = std::move(e);
        }
        if (step != SQLITE_DONE) return fail(db_message(db_));
    }
    {
        Stmt q(db_, kEffectStatSql);
        if (!q.ok()) return fail(db_message(db_));
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            const auto it = out.effects.find(sqlite3_column_int64(s, 0));
            if (it == out.effects.end()) continue;   // the mods of an effect worldd does not run
            StatMod m;
            m.stat_guid = sqlite3_column_int64(s, 1);
            m.source_stat_guid = sqlite3_column_int64(s, 2);
            m.factor = sqlite3_column_double(s, 3);
            m.addend = sqlite3_column_double(s, 4);
            it->second.stat_mods.push_back(m);
        }
        if (step != SQLITE_DONE) return fail(db_message(db_));
    }
    const auto rule_number = [&](const char* key, double* into) -> bool {
        Stmt q(db_, kStaminaRuleSql);
        if (!q.ok()) return false;
        sqlite3_bind_text(q.get(), 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(q.get()) != SQLITE_ROW) return false;
        const std::string v = column_text(q.get(), 0);
        char* end = nullptr;
        const double d = std::strtod(v.c_str(), &end);
        if (v.empty() || end != v.c_str() + v.size()) return false;
        *into = d;
        return true;
    };
    if (!rule_number("stat_regen_time", &out.stat_regen_time)) return fail("no number for the stamina rule stat_regen_time");
    if (!rule_number("regen.rate_enabled", &out.regen_rate)) return fail("no number for the stamina rule regen.rate_enabled");
// Cyb - C | O | A

    // What the runtime relies on, so that a file it cannot run is refused at startup and not in the middle of a sprint.
    const AbilityRule& s = out.sprint;
    const AbilityRule& d = out.dodge;
    const AbilityRule& r = out.rest;
    if (s.held != Held::Smart || s.cost_stat_guid != kGuidStamina || !(s.cost_amount > 0) || !s.cost_requires_moving ||
        !(s.charge_interval >= kMinInterval) || !(s.min_stamina > 0) || s.effect_guid == 0) {
        return fail("the Sprint is not a held ability that charges Stamina while moving, on an interval (at least " +
                    std::to_string(kMinInterval) + " s), from a minimum");
    }
    if (d.held != Held::Naive || d.cost_stat_guid != kGuidStamina || !(d.cost_amount > 0) || d.cost_requires_moving ||
        d.charge_interval != 0 || !(d.cooldown > 0) || !d.triggers_cooldown || d.effect_guid == 0) {
        return fail("the Dodge is not a plain ability that charges Stamina once and starts a cooldown");
    }
    if (r.held != Held::Toggle || r.cost_stat_guid != 0 || r.effect_guid == 0 || !r.targets_self) {
        return fail("the Rest is not a free toggle with an effect, on the caster");
    }
    if (!(out.stat_regen_time >= kMinInterval) || !(out.regen_rate > 0)) {
        return fail("the Stamina regeneration is not a positive rate on an interval of at least " + std::to_string(kMinInterval) + " s");
    }
    for (const AbilityRule* a : {&s, &d, &r}) {
        const EffectRule* e = out.effect(a->effect_guid);
        if (!e) return fail(a->name + " applies effect " + std::to_string(a->effect_guid) + ", which has no effect_runtime row");
        if (e->end_effect_guid != 0 && (!out.effect(e->end_effect_guid) || !(e->end_effect_duration > 0))) {
            return fail(e->name + "'s End hit applies effect " + std::to_string(e->end_effect_guid) + " for no time or with no row");
        }
        if (!e->infinite && !(e->duration > 0)) return fail(e->name + " is finite with no duration");
    }
    return out;
}

std::optional<ItemRecord> ContentDb::item(int64_t guid, std::string& error) const {
    error.clear();
    ItemRecord rec;
    {
        Stmt q(db_, kItemSql);
        if (!q.ok()) {
            error = "content.db item query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) return std::nullopt;   // no such item: not an error of the file
        if (step != SQLITE_ROW) {
            error = "content.db item query failed: " + db_message(db_);
            return std::nullopt;
        }
        rec.guid = sqlite3_column_int64(q.get(), 0);
        rec.name = column_text(q.get(), 1);
        rec.can_equip = sqlite3_column_int(q.get(), 2) != 0;
        rec.occupies_all_equip_slots = sqlite3_column_int(q.get(), 3) != 0;
        // plan D1: what a fresh instance is; a column of another storage class is a file this contentc did not write
        if (!column_is_int(q.get(), 4) || !column_is_int(q.get(), 5) || !column_is_real(q.get(), 6)) {
            error = "content.db item " + std::to_string(guid) + " has a default_quality, max_stack_size or max_durability of the wrong storage class";
            return std::nullopt;
        }
        // (six resource reagents of the design data -- the Ashlar ones and the sugar cane seeds -- have a DefaultQuality of -3 or -2, a marker no instance can hold: a fresh instance of
        // them takes quality 1, which is what a harvest did for a record it could not read; without this they were unknown records and went to the Backpack, which the client refuses)
        const int64_t quality = std::max<int64_t>(1, sqlite3_column_int64(q.get(), 4)), stack = sqlite3_column_int64(q.get(), 5);
        if (quality > 200 || stack < 1 || stack > 1000000) {
            error = "content.db item " + std::to_string(guid) + " has default_quality " + std::to_string(quality) + " or max_stack_size " + std::to_string(stack) +
                    " outside the character service's ranges";
            return std::nullopt;
        }
        rec.default_quality = static_cast<int32_t>(quality);
        rec.max_stack_size = static_cast<int32_t>(stack);
        rec.max_durability = sqlite3_column_double(q.get(), 6);
        // Gathering stage 3: the dimension an item takes in a resource container and whether it is stored there
        if (!column_is_int(q.get(), 7) || !column_is_int(q.get(), 8) || !column_is_int(q.get(), 9)) {
            error = "content.db item " + std::to_string(guid) + " has an inv_w, inv_h or resource of the wrong storage class";
            return std::nullopt;
        }
        const int64_t w = sqlite3_column_int64(q.get(), 7), h = sqlite3_column_int64(q.get(), 8), resource = sqlite3_column_int64(q.get(), 9);
        if (w < 1 || w > 255 || h < 1 || h > 255 || (resource != 0 && resource != 1)) {
            error = "content.db item " + std::to_string(guid) + " has the dimension " + std::to_string(w) + " x " + std::to_string(h) + " or the resource flag " +
                    std::to_string(resource) + ", which contentc does not write";
            return std::nullopt;
        }
        rec.inv_w = static_cast<int32_t>(w);
        rec.inv_h = static_cast<int32_t>(h);
        rec.resource = resource == 1;
    }
    {   // the display name (contentc version 16); a file without the column keeps it empty
        Stmt q(db_, kItemNameSql);
        if (q.ok()) {
            sqlite3_bind_int64(q.get(), 1, guid);
            if (sqlite3_step(q.get()) == SQLITE_ROW) rec.display_name = column_text(q.get(), 0);
        }
    }
    Stmt q(db_, kItemSlotsSql);
    if (!q.ok()) {
        error = "content.db item slot query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_int64(q.get(), 1, guid);
    int step;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        const int slot = sqlite3_column_int(q.get(), 0);
        if (slot < 0 || slot > 255) {
            error = "content.db item " + std::to_string(guid) + " has the equip slot " + std::to_string(slot) + ", which is no EEquipSlot";
            return std::nullopt;
        }
        rec.equip_slots.push_back(static_cast<uint8_t>(slot));
    }
    if (step != SQLITE_DONE) {
        error = "content.db item slot query failed: " + db_message(db_);
        return std::nullopt;
    }
    return rec;
}
// (C | O | A ~ C.Y.B)

// ---- Gathering stage 3: the resource bags ------------------------------------------------------------------------------------

std::optional<ResourceBagRecord> ContentDb::resource_bag(int64_t item_guid, std::string& error) const {
    error.clear();
    ResourceBagRecord bag;
    bag.item = item_guid;
    {
        Stmt q(db_, kResourceBagSql);
        if (!q.ok()) {
            error = "content.db resource bag query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, item_guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) return std::nullopt;   // no such bag: not an error of the file
        if (step != SQLITE_ROW) {
            error = "content.db resource bag query failed: " + db_message(db_);
            return std::nullopt;
        }
        if (!column_is_int(q.get(), 0) || !column_is_text(q.get(), 1) || !column_is_int(q.get(), 2) || !column_is_int(q.get(), 3) || !column_is_int(q.get(), 4)) {
            error = "content.db resource bag " + std::to_string(item_guid) + " has a column of the wrong storage class";
            return std::nullopt;
        }
        const int64_t columns = sqlite3_column_int64(q.get(), 2), rows = sqlite3_column_int64(q.get(), 3);
        if (columns < 1 || rows < 1 || columns * rows > 255) {
            error = "content.db resource bag " + std::to_string(item_guid) + " has the grid " + std::to_string(columns) + " x " + std::to_string(rows) +
                    ", outside 1 to 255 cells";
            return std::nullopt;
        }
        bag.def = sqlite3_column_int64(q.get(), 0);
        bag.def_name = column_text(q.get(), 1);
        bag.columns = static_cast<int32_t>(columns);
        bag.rows = static_cast<int32_t>(rows);
        bag.custom = sqlite3_column_int64(q.get(), 4) != 0;
    }
    Stmt q(db_, kResourceBagCellSql);
    if (!q.ok()) {
        error = "content.db resource bag cell query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_int64(q.get(), 1, bag.def);
    int step;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        const int64_t col = sqlite3_column_int64(q.get(), 0), row = sqlite3_column_int64(q.get(), 1), value = sqlite3_column_int64(q.get(), 2);
        if (col < 0 || col > 255 || row < 0 || row > 255 || (value != -1 && value != -2)) {
            error = "content.db resource bag " + std::to_string(item_guid) + " has the cell (" + std::to_string(col) + ", " + std::to_string(row) + ") = " + std::to_string(value);
            return std::nullopt;
        }
        if (bag.matrix.size() <= static_cast<size_t>(col)) bag.matrix.resize(static_cast<size_t>(col) + 1);
        std::vector<int32_t>& column = bag.matrix[static_cast<size_t>(col)];
        if (column.size() <= static_cast<size_t>(row)) column.resize(static_cast<size_t>(row) + 1, -1);
        column[static_cast<size_t>(row)] = static_cast<int32_t>(value);
    }
    if (step != SQLITE_DONE) {
        error = "content.db resource bag cell query failed: " + db_message(db_);
        return std::nullopt;
    }
    return bag;
}

// ---- plan B3 -----------------------------------------------------------------------------------------------------------------
// [C Y B - CoA]

std::optional<StatProgramRow> ContentDb::stat_program(std::string& error) const {
    Stmt q(db_, kStatProgramSql);
    if (!q.ok()) {
        error = "content.db stat program query failed: " + db_message(db_);
        return std::nullopt;
    }
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_DONE) {
        error = "content.db has no stat program";   // the caller adds the way out (PlayerWorld)
        return std::nullopt;
    }
    if (step != SQLITE_ROW) {
        error = "content.db stat program query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_stmt* s = q.get();
    StatProgramRow r;
    // Plan B3 review fix (a): the row's numbers are read as the int64 SQLite holds and compared without narrowing: a crc32 of the
    // real value + 2^32 (or a version, stat or class count beyond an int) used to pass the provenance check through a cast.
    if (!column_is_int(s, 0) || !column_is_text(s, 1) || !column_is_int(s, 2) || !column_is_int(s, 3) || !column_is_int(s, 4)) {
        error = "content.db's stat program row has a column of the wrong storage class (version, stats, classes and crc32 are integers, mode text)";
        return std::nullopt;
    }
    const int64_t version = sqlite3_column_int64(s, 0), stats = sqlite3_column_int64(s, 2), classes = sqlite3_column_int64(s, 3), crc = sqlite3_column_int64(s, 4);
    if (version < 0 || version > INT32_MAX || stats < 0 || stats > INT32_MAX || classes < 0 || classes > INT32_MAX || crc < 0 || crc > 0xFFFFFFFFll) {
        error = "content.db's stat program row has a version, stat count, class count or crc32 outside its range (crc32 " + std::to_string(crc) + ")";
        return std::nullopt;
    }
    r.version = static_cast<int>(version);
    r.mode = column_text(s, 1);
    r.stats = static_cast<int>(stats);
    r.classes = static_cast<int>(classes);
    r.crc32 = static_cast<uint32_t>(crc);
    const void* blob = sqlite3_column_blob(s, 5);
    const int n = sqlite3_column_bytes(s, 5);
    if (!blob || n <= 0) {
        error = "content.db's stat program is empty";
        return std::nullopt;
    }
    r.data.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + n);
    return r;
}

std::optional<stats::ItemGear> ContentDb::item_gear(int64_t guid, std::string& error) const {
    error.clear();
    stats::ItemGear g;
    g.record = guid;
    int64_t block = 0, curve = 0;
    {
        Stmt q(db_, kItemGearSql);
        if (!q.ok()) {
            error = "content.db item gear query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) return std::nullopt;   // no such item: not an error of the file
        if (step != SQLITE_ROW) {
            error = "content.db item gear query failed: " + db_message(db_);
            return std::nullopt;
        }
        block = sqlite3_column_int64(q.get(), 0);
        curve = sqlite3_column_int64(q.get(), 1);
    }
    const auto fail = [&](const char* what) {
        error = std::string("content.db ") + what + " query failed for item " + std::to_string(guid) + ": " + db_message(db_);
        return std::nullopt;
    };
    if (curve != 0) {
        Stmt q(db_, kQualityCurveSql);
        if (!q.ok()) return fail("quality curve");
        sqlite3_bind_int64(q.get(), 1, curve);
        int n = 0, step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0) || sqlite3_column_int64(q.get(), 0) != n || n >= stats::kRarities) {
                error = "content.db's quality curve " + std::to_string(curve) + " has its rarities out of order";
                return std::nullopt;
            }
            if (!column_is_real(q.get(), 1) || !column_is_real(q.get(), 2)) {
                error = "content.db's quality curve " + std::to_string(curve) + " has a band that is not a number";
                return std::nullopt;
            }
            g.bands[static_cast<size_t>(n)] = {sqlite3_column_double(q.get(), 1), sqlite3_column_double(q.get(), 2)};
            ++n;
        }
        if (step != SQLITE_DONE) return fail("quality curve");
        if (n != stats::kRarities) {
            error = "content.db's quality curve " + std::to_string(curve) + " has " + std::to_string(n) + " rarities, not " + std::to_string(stats::kRarities);
            return std::nullopt;
        }
        g.has_curve = true;
    }
    if (block != 0) {
        Stmt q(db_, kStatBlockSql);
        if (!q.ok()) return fail("stat block");
        sqlite3_bind_int64(q.get(), 1, block);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            stats::BlockStat s;
            if (!column_is_int(q.get(), 0)) {
                error = "content.db's stat block " + std::to_string(block) + " names a stat that is not an integer";
                return std::nullopt;
            }
            s.stat = sqlite3_column_int64(q.get(), 0);
            for (int r = 0; r < stats::kRarities; ++r) {
                if (!column_is_real(q.get(), 1 + 2 * r) || !column_is_real(q.get(), 2 + 2 * r)) {
                    error = "content.db's stat block " + std::to_string(block) + " has a value of stat " + std::to_string(s.stat) + " that is not a number";
                    return std::nullopt;
                }
                s.min[static_cast<size_t>(r)] = sqlite3_column_double(q.get(), 1 + 2 * r);
                s.max[static_cast<size_t>(r)] = sqlite3_column_double(q.get(), 2 + 2 * r);
            }
            g.stats.push_back(s);
        }
        if (step != SQLITE_DONE) return fail("stat block");
    }
    {
        Stmt q(db_, kItemSetSql);
        if (!q.ok()) return fail("item set");
        sqlite3_bind_int64(q.get(), 1, guid);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) g.sets.push_back(sqlite3_column_int64(q.get(), 0));
        if (step != SQLITE_DONE) return fail("item set");
    }
    {
        Stmt q(db_, kItemTagSql);
        if (!q.ok()) return fail("item tag");
        sqlite3_bind_int64(q.get(), 1, guid);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) g.tags.push_back(column_text(q.get(), 0));
        if (step != SQLITE_DONE) return fail("item tag");
    }
    return g;
}
// [C.Y.B :: C O A]

std::optional<stats::SetBonus> ContentDb::set_bonus(int64_t guid, std::string& error) const {
    error.clear();
    Stmt q(db_, kSetBonusSql);
    if (!q.ok()) {
        error = "content.db set bonus query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_int64(q.get(), 1, guid);
    stats::SetBonus out;
    int64_t tier_ord = -1;
    int step;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 1) || !column_is_int(s, 2)) {
            error = "content.db's set " + std::to_string(guid) + " has a tier order, piece count or stat that is not an integer";
            return std::nullopt;
        }
        const int64_t ord = sqlite3_column_int64(s, 0);
        if (ord < 0) {
            error = "content.db's set " + std::to_string(guid) + " has a negative tier order";
            return std::nullopt;   // no bonus rather than a tier from a row that cannot be placed
        }
        if (out.tiers.empty() || ord != tier_ord) {
            // the pieces a tier needs: read as the int64 SQLite holds and range-checked (a set has at most the 83 equipment slots' worth)
            const int64_t pieces = sqlite3_column_int64(s, 1);
            if (pieces < 1 || pieces > 83) {
                error = "content.db's set " + std::to_string(guid) + " has a tier of " + std::to_string(pieces) + " pieces";
                return std::nullopt;
            }
            stats::SetTier t;
            t.pieces = static_cast<int>(pieces);
            out.tiers.push_back(std::move(t));
            tier_ord = ord;
        }
        stats::SetEntry e;
        e.stat = sqlite3_column_int64(s, 2);
        for (int r = 0; r < stats::kRarities; ++r) {
            const int type = sqlite3_column_type(s, 3 + r);
            if (type == SQLITE_NULL) continue;
            if (type != SQLITE_FLOAT && type != SQLITE_INTEGER) {
                error = "content.db's set " + std::to_string(guid) + " has a bonus of stat " + std::to_string(e.stat) + " that is not a number";
                return std::nullopt;
            }
            e.has[static_cast<size_t>(r)] = true;
            e.value[static_cast<size_t>(r)] = sqlite3_column_double(s, 3 + r);
        }
        out.tiers.back().entries.push_back(e);
    }
    if (step != SQLITE_DONE) {
        error = "content.db set bonus query failed: " + db_message(db_);
        return std::nullopt;
    }
    if (out.tiers.empty()) return std::nullopt;   // a set with no bonus rows is no set to the engine
    return out;
}

std::optional<std::map<int, std::string>> ContentDb::equip_slot_names(std::string& error) const {
    Stmt q(db_, kEquipSlotSql);
    if (!q.ok()) {
        error = "content.db equip slot query failed: " + db_message(db_);
        return std::nullopt;
    }
    std::map<int, std::string> out;
    int step;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) out[sqlite3_column_int(q.get(), 0)] = column_text(q.get(), 1);
    if (step != SQLITE_DONE) {
        error = "content.db equip slot query failed: " + db_message(db_);
        return std::nullopt;
    }
    return out;
}

// ---- plan D1 -----------------------------------------------------------------------------------------------------------------

namespace {

/// The guids of a one-column query of `parent`, in the query's order; false (error set) when the query fails.
bool guid_list(sqlite3* db, const char* sql, int64_t parent, std::vector<int64_t>& out, std::string& error, const char* what) {
    Stmt q(db, sql);
    if (!q.ok()) {
        error = std::string("content.db ") + what + " query failed: " + db_message(db);
        return false;
    }
    sqlite3_bind_int64(q.get(), 1, parent);
    int step;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) out.push_back(sqlite3_column_int64(q.get(), 0));
    if (step != SQLITE_DONE) {
        error = std::string("content.db ") + what + " query failed: " + db_message(db);
        return false;
    }
    return true;
}

}  // namespace

std::optional<ItemUse> ContentDb::item_use(int64_t item_guid, std::string& error) const {
    error.clear();
    ItemUse u;
    {
        Stmt q(db_, kItemUseSql);
        if (!q.ok()) {
            error = "content.db item use query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, item_guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) return std::nullopt;   // an item that is not used
        if (step != SQLITE_ROW) {
            error = "content.db item use query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_stmt* s = q.get();
        if (!column_is_text(s, 1) || !column_is_int(s, 2) || !column_is_real(s, 3) || !column_is_int(s, 4)) {
            error = "content.db item_use " + std::to_string(item_guid) + " has a column of the wrong storage class";
            return std::nullopt;
        }
        u.item = sqlite3_column_int64(s, 0);
        u.activation = column_text(s, 1);
        u.consume_on_use = sqlite3_column_int64(s, 2) != 0;
        u.cooldown = sqlite3_column_double(s, 3);
        u.disabled_in_combat = sqlite3_column_int64(s, 4) != 0;
    }
    if (!guid_list(db_, kItemActionSql, item_guid, u.actions, error, "item action")) return std::nullopt;
    return u;
}
// -- CoA . C-Y-B --

std::optional<ActionResponse> ContentDb::action_response(int64_t guid, std::string& error) const {
    error.clear();
    ActionResponse a;
    {
        Stmt q(db_, kActionSql);
        if (!q.ok()) {
            error = "content.db action response query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) return std::nullopt;
        if (step != SQLITE_ROW) {
            error = "content.db action response query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_stmt* s = q.get();
        if (!column_is_text(s, 1) || !column_is_text(s, 2) || !column_is_int(s, 3) || !column_is_int(s, 5)) {
            error = "content.db action_response " + std::to_string(guid) + " has a column of the wrong storage class";
            return std::nullopt;
        }
        a.guid = sqlite3_column_int64(s, 0);
        a.name = column_text(s, 1);
        a.response_type = column_text(s, 2);
        a.class_guid = sqlite3_column_int64(s, 3);
        a.class_name = column_text(s, 4);
        a.remove_up_to_quantity = sqlite3_column_int64(s, 5) != 0;
    }
    if (!guid_list(db_, kActionRewardSql, guid, a.reward_tables, error, "action reward")) return std::nullopt;
    return a;
}

std::optional<RewardTable> ContentDb::reward_table(int64_t guid, std::string& error) const {
    error.clear();
    RewardTable t;
    {
        Stmt q(db_, kRewardTableSql);
        if (!q.ok()) {
            error = "content.db reward table query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) return std::nullopt;
        if (step != SQLITE_ROW) {
            error = "content.db reward table query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_stmt* s = q.get();
        if (!column_is_text(s, 1) || !column_is_text(s, 2) || !column_is_int(s, 3) || !column_is_int(s, 4)) {
            error = "content.db reward_table " + std::to_string(guid) + " has a column of the wrong storage class";
            return std::nullopt;
        }
        const int64_t below = sqlite3_column_int64(s, 3);
        if (below < -1 || below > 255) {
            error = "content.db reward_table " + std::to_string(guid) + " has level_below " + std::to_string(below);
            return std::nullopt;
        }
        t.guid = sqlite3_column_int64(s, 0);
        t.name = column_text(s, 1);
        t.predicate = column_text(s, 2);
        t.level_below = static_cast<int>(below);
        t.modelled = sqlite3_column_int64(s, 4) != 0;
    }
    {
        Stmt q(db_, kRewardItemSql);
        if (!q.ok()) {
            error = "content.db reward item query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, guid);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 1) || !column_is_int(s, 2)) {
                error = "content.db reward_item of table " + std::to_string(guid) + " has a column of the wrong storage class";
                return std::nullopt;
            }
            const int64_t quantity = sqlite3_column_int64(s, 2);
            if (quantity < 1 || quantity > 1000000) {
                error = "content.db reward_item of table " + std::to_string(guid) + " grants " + std::to_string(quantity) + " of an item";
                return std::nullopt;
            }
            t.items.push_back({sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1), static_cast<int32_t>(quantity)});
        }
        if (step != SQLITE_DONE) {
            error = "content.db reward item query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    {
        Stmt q(db_, kRewardXpSql);
        if (!q.ok()) {
            error = "content.db reward xp query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, guid);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_text(s, 0) || !column_is_int(s, 1) || !column_is_real(s, 2) || !column_is_int(s, 3) || !column_is_int(s, 4) || !column_is_int(s, 5) ||
                !column_is_text(s, 6)) {
                error = "content.db reward_xp of table " + std::to_string(guid) + " has a column of the wrong storage class";
                return std::nullopt;
            }
            const int64_t target = sqlite3_column_int64(s, 1);
            if (target < 0 || target > 255) {
                error = "content.db reward_xp of table " + std::to_string(guid) + " has target_level " + std::to_string(target);
                return std::nullopt;
            }
            RewardXp x;
            x.experience_type = column_text(s, 0);
            x.target_level = static_cast<int>(target);
            x.margin = sqlite3_column_double(s, 2);
            x.curve_guid = sqlite3_column_int64(s, 3);
            x.mult_stat_guid = sqlite3_column_int64(s, 4);
            x.expr = sqlite3_column_int64(s, 5);
            x.expression = column_text(s, 6);
            t.experience.push_back(std::move(x));
        }
        if (step != SQLITE_DONE) {
            error = "content.db reward xp query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    return t;
}
// (CoA | C-Y-B)

std::optional<LevelingType> ContentDb::leveling_type_named(const std::string& name, std::string& error) const {
    LevelingType t;
    {
        Stmt q(db_, kLevelingTypeSql);
        if (!q.ok()) {
            error = "content.db leveling type query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_text(q.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) {
            error = "content.db has no leveling type named '" + name + "'";
            return std::nullopt;
        }
        if (step != SQLITE_ROW) {
            error = "content.db leveling type query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_text(s, 1) || !column_is_text(s, 2) || !column_is_int(s, 3) || !column_is_int(s, 4) || !column_is_int(s, 5)) {
            error = "content.db leveling_type '" + name + "' has a column of the wrong storage class";
            return std::nullopt;
        }
        const int64_t cap = sqlite3_column_int64(s, 3);
        if (cap < 0 || cap > 255) {
            error = "content.db leveling_type '" + name + "' has level_cap " + std::to_string(cap);
            return std::nullopt;
        }
        t.guid = sqlite3_column_int64(s, 0);
        t.name = column_text(s, 1);
        t.experience_type = column_text(s, 2);
        t.level_cap = static_cast<int>(cap);
        t.uses_debt = sqlite3_column_int64(s, 4) != 0;
        t.xp_mult_stat_guid = sqlite3_column_int64(s, 5);
    }
    Stmt q(db_, kLevelingCurveSql);
    if (!q.ok()) {
        error = "content.db leveling curve query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_int64(q.get(), 1, t.guid);
    int step;
    int64_t last_level = 0, last_xp = -1;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 1)) {
            error = "content.db leveling_curve of '" + name + "' has a key that is not a whole level at a whole XP total";
            return std::nullopt;
        }
        const int64_t level = sqlite3_column_int64(s, 0), xp = sqlite3_column_int64(s, 1);
        if (level < 1 || level > 255 || level <= last_level || xp < 0 || xp < last_xp) {
            error = "content.db leveling_curve of '" + name + "' is not ascending whole levels from 1 at non-decreasing XP (level " + std::to_string(level) +
                    " at " + std::to_string(xp) + ")";
            return std::nullopt;
        }
        last_level = level;
        last_xp = xp;
        t.curve.emplace_back(static_cast<int>(level), xp);
    }
    if (step != SQLITE_DONE) {
        error = "content.db leveling curve query failed: " + db_message(db_);
        return std::nullopt;
    }
    if (t.curve.empty() || t.curve.front().first != 1) {
        error = "content.db leveling_curve of '" + name + "' does not start at level 1";
        return std::nullopt;
    }
    return t;
}
// [CoA - C | Y | B]

std::optional<FoliageData> ContentDb::foliage_data(std::string& error) const {
    error.clear();
    {
        Stmt q(db_, kFoliageTableSql);
        if (!q.ok()) {
            error = "content.db table query failed: " + db_message(db_);
            return std::nullopt;
        }
        int found = 0, step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) ++found;
        if (step != SQLITE_DONE) {
            error = "content.db table query failed: " + db_message(db_);
            return std::nullopt;
        }
        if (found == 0) return std::nullopt;
        if (found != 4) {
            error = "content.db has some of the foliage tables (foliage_def, foliage_state, gather_interaction, gather_interaction_def) and not all: rebuild it with this checkout's contentc";
            return std::nullopt;
        }
    }
    FoliageData out;
    const auto bad = [&](const char* table, const std::string& what) {
        error = std::string("content.db ") + table + ": " + what;
        return std::optional<FoliageData>{};
    };
    // each query: the columns' storage classes are checked (a column of another class is not what contentc writes), then the row is read by `read`
    const auto each = [&](const char* sql, const char* table, const std::vector<char>& kinds, const auto& read) -> bool {
        Stmt q(db_, sql);
        if (!q.ok()) {
            error = std::string("content.db ") + table + " query failed: " + db_message(db_);
            return false;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (size_t c = 0; c < kinds.size(); ++c) {
                const int col = static_cast<int>(c);
                const bool ok = kinds[c] == 'i' ? column_is_int(s, col) : kinds[c] == 'r' ? column_is_real(s, col) : column_is_text(s, col);
                if (!ok) {
                    error = std::string("content.db ") + table + " has a column of the wrong storage class";
                    return false;
                }
            }
            if (!read(s)) return false;
        }
        if (step != SQLITE_DONE) {
            error = std::string("content.db ") + table + " query failed: " + db_message(db_);
            return false;
        }
        return true;
    };
    const auto state_ok = [](int64_t v) { return v >= kFoliageInfancy && v <= kFoliageEmpty; };
    if (!each(kFoliageDefSql, "foliage_def", {'i', 't', 'r', 'i', 'i', 'i', 'i', 'i', 'i', 'i', 'i'}, [&](sqlite3_stmt* s) {
            FoliageDefRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.empty_duration = sqlite3_column_double(s, 2);
            r.dynamic_guid = sqlite3_column_int64(s, 3);
            const int64_t paint = sqlite3_column_int64(s, 4), gatherable = sqlite3_column_int64(s, 5), lo = sqlite3_column_int64(s, 6), hi = sqlite3_column_int64(s, 7);
            if (!state_ok(paint) || !state_ok(gatherable) || !state_ok(lo) || !state_ok(hi) || !(r.empty_duration >= 0)) {
                error = "content.db foliage_def " + r.name + " has a state outside Infancy..Empty or a negative empty duration";
                return false;
            }
            r.paint_state = static_cast<int>(paint);
            r.gatherable_state = static_cast<int>(gatherable);
            r.min_state = static_cast<int>(lo);
            r.max_state = static_cast<int>(hi);
            for (int i = 0; i < 3; ++i) r.state_guids[i] = sqlite3_column_int64(s, 8 + i);
            out.defs_by_name.emplace(r.name, r.guid);
            out.defs[r.guid] = std::move(r);
            return true;
        })) {
        return std::nullopt;
    }
    if (!each(kFoliageStateSql, "foliage_state", {'i', 't', 'r', 'i', 'r', 'r', 'r'}, [&](sqlite3_stmt* s) {
            FoliageStateRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.lifecycle_duration = sqlite3_column_double(s, 2);
            r.interaction_guid = sqlite3_column_int64(s, 3);
            r.override_distance = sqlite3_column_double(s, 4);
            r.override_angle = sqlite3_column_double(s, 5);
            r.collision_radius = sqlite3_column_double(s, 6);
            if (!(r.lifecycle_duration >= 0) || !(r.collision_radius >= 0)) {
                error = "content.db foliage_state " + r.name + " has a negative lifecycle duration or collision radius";
                return false;
            }
            out.states[r.guid] = std::move(r);
            return true;
        })) {
        return std::nullopt;
    }
    if (!each(kGatherInteractionSql, "gather_interaction", {'i', 't', 'i', 'i', 'i', 'i', 'i', 'r', 'i', 'i', 'i', 'i', 'r', 'r', 'i'}, [&](sqlite3_stmt* s) {
            GatherInteractionRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            const int64_t target = sqlite3_column_int64(s, 2), tier = sqlite3_column_int64(s, 5), slot = sqlite3_column_int64(s, 6);
            if (!state_ok(target) || tier < 0 || tier > 4 || slot < -1 || slot > 255) {
                error = "content.db gather_interaction " + r.name + " has target state " + std::to_string(target) + ", certification tier " + std::to_string(tier) + " and tool slot " +
                        std::to_string(slot);
                return false;
            }
            r.target_state = static_cast<int>(target);
            r.profession_guid = sqlite3_column_int64(s, 3);
            r.cert_guid = sqlite3_column_int64(s, 4);
            r.cert_tier = static_cast<int>(tier);
            r.tool_slot = static_cast<int>(slot);
            r.base_duration = sqlite3_column_double(s, 7);
            r.duration_expr = sqlite3_column_int64(s, 8);
            r.durability_expr = sqlite3_column_int64(s, 9);
            r.reward_guid = sqlite3_column_int64(s, 10);
            r.interaction_guid = sqlite3_column_int64(s, 11);
            r.interact_radius = sqlite3_column_double(s, 12);
            r.interact_tolerance = sqlite3_column_double(s, 13);
            r.allow_in_combat = sqlite3_column_int64(s, 14) != 0;
            if (!(r.base_duration >= 0) || !(r.interact_radius >= 0) || !(r.interact_tolerance >= 0)) {
                error = "content.db gather_interaction " + r.name + " has a negative or no-number duration or range";
                return false;
            }
            out.interactions[r.guid] = std::move(r);
            return true;
        })) {
        return std::nullopt;
    }
    if (!each(kGatherInteractionDefSql, "gather_interaction_def", {'i', 't', 'i', 'r', 'r', 'r', 'i'}, [&](sqlite3_stmt* s) {
            GatherInteractionDefRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.gather_guid = sqlite3_column_int64(s, 2);
            r.interact_radius = sqlite3_column_double(s, 3);
            r.interact_tolerance = sqlite3_column_double(s, 4);
            r.discovery_distance = sqlite3_column_double(s, 5);
            r.allow_in_combat = sqlite3_column_int64(s, 6) != 0;
            out.interaction_defs[r.guid] = std::move(r);
            return true;
        })) {
        return std::nullopt;
    }
    if (out.defs.empty() || out.states.empty() || out.interactions.empty()) return bad("foliage tables", "a table is empty");
    return out;
}
// -- C.O.A . C_Y_B --

std::optional<GatherData> ContentDb::gather_data(std::string& error) const {
    error.clear();
    {
        // a file without the tables is an older one (the tests' small fixtures): no gathering, and no error
        Stmt q(db_, kGatherTableSql);
        if (!q.ok()) {
            error = "content.db table query failed: " + db_message(db_);
            return std::nullopt;
        }
        int found = 0, step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) ++found;
        if (step != SQLITE_DONE) {
            error = "content.db table query failed: " + db_message(db_);
            return std::nullopt;
        }
        if (found == 0) return std::nullopt;
        if (found != 3) {
            error = "content.db has some of the gathering tables (gather_def, gather_tool, item_variation) and not all: rebuild it with this checkout's contentc";
            return std::nullopt;
        }
    }
    GatherData out;
    const auto wrong = [&](const char* table) {
        error = std::string("content.db ") + table + " has a column of the wrong storage class";
        return std::optional<GatherData>{};
    };
    {
        Stmt q(db_, kGatherDefSql);
        if (!q.ok()) {
            error = "content.db gather_def query failed: " + db_message(db_);
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int c : {0, 4, 7, 8, 9, 10, 11, 13, 14, 15}) {
                if (!column_is_int(s, c)) return wrong("gather_def");
            }
            if (!column_is_text(s, 1)) return wrong("gather_def");
            for (const int c : {2, 3, 5, 6, 12}) {
                if (!column_is_real(s, c)) return wrong("gather_def");
            }
            GatherDefRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.empty_duration = sqlite3_column_double(s, 2);
            r.override_respawn = sqlite3_column_double(s, 3);
            r.interaction_guid = sqlite3_column_int64(s, 4);
            r.interact_radius = sqlite3_column_double(s, 5);
            r.interact_tolerance = sqlite3_column_double(s, 6);
            r.allow_in_combat = sqlite3_column_int64(s, 7) != 0;
            r.profession_guid = sqlite3_column_int64(s, 8);
            const int64_t slot = sqlite3_column_int64(s, 9), tier = sqlite3_column_int64(s, 11);
            if (slot < 0 || slot > 255 || tier < 0 || tier > 4) {
                error = "content.db gather_def " + r.name + " has tool slot " + std::to_string(slot) + " and certification tier " + std::to_string(tier);
                return std::nullopt;
            }
            r.tool_slot = static_cast<int>(slot);
            r.cert_guid = sqlite3_column_int64(s, 10);
            r.cert_tier = static_cast<int>(tier);
            r.base_duration = sqlite3_column_double(s, 12);
            r.duration_expr = sqlite3_column_int64(s, 13);
            r.durability_expr = sqlite3_column_int64(s, 14);
            r.reward_guid = sqlite3_column_int64(s, 15);
            if (!(r.interact_radius >= 0) || !(r.interact_tolerance >= 0) || !(r.base_duration >= 0) || !(r.empty_duration >= 0) || !(r.override_respawn >= 0)) {
                error = "content.db gather_def " + r.name + " has a negative or no-number range, duration or respawn";
                return std::nullopt;
            }
            out.defs[r.guid] = std::move(r);
        }
        if (step != SQLITE_DONE) {
            error = "content.db gather_def query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    {
        Stmt q(db_, kGatherToolSql);
        if (!q.ok()) {
            error = "content.db gather_tool query failed: " + db_message(db_);
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int c : {0, 2, 3, 4, 5, 6, 7}) {
                if (!column_is_int(s, c)) return wrong("gather_tool");
            }
            if (!column_is_text(s, 1)) return wrong("gather_tool");
            const int64_t tier = sqlite3_column_int64(s, 4), slot = sqlite3_column_int64(s, 5);
            if (tier < 0 || tier > 4 || slot < 0 || slot > 255) {
                error = "content.db gather_tool " + column_text(s, 1) + " has certification tier " + std::to_string(tier) + " and slot " + std::to_string(slot);
                return std::nullopt;
            }
            GatherToolRow r;
            r.item_guid = sqlite3_column_int64(s, 0);
            r.item_name = column_text(s, 1);
            r.profession_guid = sqlite3_column_int64(s, 2);
            r.cert_guid = sqlite3_column_int64(s, 3);
            r.cert_tier = static_cast<int>(tier);
            r.tool_slot = static_cast<int>(slot);
            r.required_profession = sqlite3_column_int64(s, 6);
            r.required_cert = sqlite3_column_int64(s, 7);
            out.tools[r.item_guid] = std::move(r);
        }
        if (step != SQLITE_DONE) {
            error = "content.db gather_tool query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    {
        Stmt q(db_, kItemVariationSql);
        if (!q.ok()) {
            error = "content.db item_variation query failed: " + db_message(db_);
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int c : {0, 1, 2}) {
                if (!column_is_int(s, c)) return wrong("item_variation");
            }
            const int64_t guid = sqlite3_column_int64(s, 0);
            out.quality_expr[guid] = sqlite3_column_int64(s, 1);
            out.durability_expr[guid] = sqlite3_column_int64(s, 2);
        }
        if (step != SQLITE_DONE) {
            error = "content.db item_variation query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    return out;
}
// ~ Cyb - C O A ~

std::optional<ProfessionData> ContentDb::profession_data(std::string& error) const {
    error.clear();
    {
        // a file without the tables is an older one (the tests' small fixtures): no professions, and no error
        Stmt q(db_, kProfessionTableSql);
        if (!q.ok()) {
            error = "content.db table query failed: " + db_message(db_);
            return std::nullopt;
        }
        int found = 0, step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) ++found;
        if (step != SQLITE_DONE) {
            error = "content.db table query failed: " + db_message(db_);
            return std::nullopt;
        }
        if (found == 0) return std::nullopt;
        if (found != 3) {
            error = "content.db has some of the profession tables (profession, certification_level, action_certification) and not all: rebuild it with this checkout's contentc";
            return std::nullopt;
        }
    }
    ProfessionData out;
    const auto wrong = [&](const char* table) {
        error = std::string("content.db ") + table + " has a column of the wrong storage class";
        return std::optional<ProfessionData>{};
    };
    std::vector<std::string> leveling_names;
    {
        Stmt q(db_, kProfessionSql);
        if (!q.ok()) {
            error = "content.db profession query failed: " + db_message(db_);
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_text(s, 1) || !column_is_text(s, 2) || !column_is_int(s, 3) || !column_is_int(s, 4)) return wrong("profession");
            ProfessionRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.category = column_text(s, 2);
            r.leveling_type_guid = sqlite3_column_int64(s, 3);
            r.skill_tree_guid = sqlite3_column_int64(s, 4);
            if (!column_is_text(s, 5)) {
                error = "content.db profession " + r.name + " names the leveling type " + std::to_string(r.leveling_type_guid) + ", which content.db does not have";
                return std::nullopt;
            }
            leveling_names.push_back(column_text(s, 5));
            out.professions.push_back(std::move(r));
        }
        if (step != SQLITE_DONE) {
            error = "content.db profession query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    for (size_t i = 0; i < out.professions.size(); ++i) {
        std::string why;
        auto type = leveling_type_named(leveling_names[i], why);
        if (!type) {
            error = "the leveling type of profession " + out.professions[i].name + ": " + why;
            return std::nullopt;
        }
        out.professions[i].leveling = std::move(*type);
    }
    {
        Stmt q(db_, kCertificationSql);
        if (!q.ok()) {
            error = "content.db certification level query failed: " + db_message(db_);
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int c : {0, 2, 3, 4, 5, 6}) {
                if (!column_is_int(s, c)) return wrong("certification_level");
            }
            if (!column_is_text(s, 1)) return wrong("certification_level");
            const int64_t tier = sqlite3_column_int64(s, 2), lo = sqlite3_column_int64(s, 3), hi = sqlite3_column_int64(s, 4), allow = sqlite3_column_int64(s, 6);
            if (tier < 0 || tier > 255 || lo < 1 || lo > 255 || hi < lo || hi > 255 || allow < 0 || allow > 1000) {
                error = "content.db certification_level " + column_text(s, 1) + " has tier " + std::to_string(tier) + ", levels " + std::to_string(lo) + " to " +
                        std::to_string(hi) + " and a limit of " + std::to_string(allow);
                return std::nullopt;
            }
            CertificationRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.tier = static_cast<int>(tier);
            r.min_level = static_cast<int>(lo);
            r.max_level = static_cast<int>(hi);
            r.limited = sqlite3_column_int64(s, 5) != 0;
            r.max_allowable = static_cast<int>(allow);
            out.certifications.push_back(std::move(r));
        }
        if (step != SQLITE_DONE) {
            error = "content.db certification level query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    {
        Stmt q(db_, kCertificationActionSql);
        if (!q.ok()) {
            error = "content.db certification action query failed: " + db_message(db_);
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_text(s, 1) || !column_is_int(s, 2) || !column_is_text(s, 3)) return wrong("action_certification");
            CertificationActionRow r;
            r.action_guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.profession_guid = sqlite3_column_int64(s, 2);
            const std::string what = column_text(s, 3);
            if (what != "Promote" && what != "Demote") {
                error = "content.db action_certification " + r.name + " is a '" + what + "' (Promote or Demote)";
                return std::nullopt;
            }
            r.promote = what == "Promote";
            out.actions[r.action_guid] = std::move(r);
        }
        if (step != SQLITE_DONE) {
            error = "content.db certification action query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    return out;
}
// -- C.O.A & Cyb --

std::optional<std::vector<AutoGrant>> ContentDb::auto_grants(int64_t leveling_type_guid, std::string& error) const {
    std::vector<AutoGrant> out;
    {
        Stmt q(db_, kAutoGrantSql);
        if (!q.ok()) {
            error = "content.db auto grant query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, leveling_type_guid);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int col : {0, 2, 3, 5, 6, 8, 9}) {
                if (!column_is_int(s, col)) {
                    error = "content.db skill_auto_grant has a column of the wrong storage class";
                    return std::nullopt;
                }
            }
            const int64_t level = sqlite3_column_int64(s, 5);
            if (level < 1 || level > 255) {
                error = "content.db skill_auto_grant has level_requirement " + std::to_string(level);
                return std::nullopt;
            }
            AutoGrant g;
            g.tree_guid = sqlite3_column_int64(s, 0);
            g.tree_name = column_text(s, 1);
            g.leveling_type_guid = sqlite3_column_int64(s, 2);
            g.node_guid = sqlite3_column_int64(s, 3);
            g.node_name = column_text(s, 4);
            g.level_requirement = static_cast<int>(level);
            g.skill_guid = sqlite3_column_int64(s, 6);
            g.skill_name = column_text(s, 7);
            g.rank_guid = sqlite3_column_int64(s, 8);
            g.effect_guid = sqlite3_column_int64(s, 9);
            out.push_back(std::move(g));
        }
        if (step != SQLITE_DONE) {
            error = "content.db auto grant query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    for (AutoGrant& g : out) {
        if (g.rank_guid == 0) continue;
        Stmt q(db_, kRankModSql);
        if (!q.ok()) {
            error = "content.db rank mod query failed: " + db_message(db_);
            return std::nullopt;
        }
        sqlite3_bind_int64(q.get(), 1, g.rank_guid);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_real(s, 2)) {
                error = "content.db skill_rank_stat_mod of rank " + std::to_string(g.rank_guid) + " has a column of the wrong storage class";
                return std::nullopt;
            }
            g.mods.push_back({sqlite3_column_int64(s, 0), column_text(s, 1), sqlite3_column_double(s, 2)});
        }
        if (step != SQLITE_DONE) {
            error = "content.db rank mod query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    return out;
}
// ~ C Y B - C | O | A ~

std::optional<TalentData> ContentDb::talent_data(std::string& error) const {
    TalentData out;
    const auto bad = [&](const char* table) {
        error = std::string("content.db ") + table + " has a column of the wrong storage class";
        return std::nullopt;
    };
    const auto failed = [&](const char* what) {
        error = std::string("content.db ") + what + " query failed: " + db_message(db_);
        return std::nullopt;
    };
    // the point types
    {
        Stmt q(db_, kPointTypeSql);
        if (!q.ok()) return failed("skill_point_type");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0) || !column_is_text(q.get(), 1)) return bad("skill_point_type");
            out.point_types[column_text(q.get(), 1)] = sqlite3_column_int64(q.get(), 0);
        }
        if (step != SQLITE_DONE) return failed("skill_point_type");
    }
    {
        Stmt q(db_, kLevelingPointSql);
        if (!q.ok()) return failed("leveling_point");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int col : {0, 1, 2, 3}) {
                if (!column_is_int(s, col)) return bad("leveling_point");
            }
            const int64_t level = sqlite3_column_int64(s, 1), qty = sqlite3_column_int64(s, 3);
            if (level < 1 || level > 255 || qty < 0 || qty > 1000) {
                error = "content.db leveling_point has a level or quantity out of range";
                return std::nullopt;
            }
            out.points[sqlite3_column_int64(s, 0)][static_cast<int>(level)][sqlite3_column_int64(s, 2)] = qty;
        }
        if (step != SQLITE_DONE) return failed("leveling_point");
    }
    // the ranks by skill, with their costs and mods
    std::map<int64_t, std::vector<SkillCost>> costs;
    {
        Stmt q(db_, kRankCostSql);
        if (!q.ok()) return failed("skill_rank_cost");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int col : {0, 1, 2, 3}) {
                if (!column_is_int(s, col)) return bad("skill_rank_cost");
            }
            const int64_t qty = sqlite3_column_int64(s, 3);
            if (qty < 1 || qty > 1000) {
                error = "content.db skill_rank_cost has a quantity out of range";
                return std::nullopt;
            }
            costs[sqlite3_column_int64(s, 0)].push_back({sqlite3_column_int64(s, 2), qty});
        }
        if (step != SQLITE_DONE) return failed("skill_rank_cost");
    }
    std::map<int64_t, std::vector<RankStatMod>> mods;
    {
        Stmt q(db_, kAllRankModSql);
        if (!q.ok()) return failed("skill_rank_stat_mod");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 1) || !column_is_real(s, 3)) return bad("skill_rank_stat_mod");
            mods[sqlite3_column_int64(s, 0)].push_back({sqlite3_column_int64(s, 1), column_text(s, 2), sqlite3_column_double(s, 3)});
        }
        if (step != SQLITE_DONE) return failed("skill_rank_stat_mod");
    }
    std::map<int64_t, std::vector<SkillRankRow>> ranks_of_skill;
    {
        Stmt q(db_, kSkillRankSql);
        if (!q.ok()) return failed("skill_rank");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int col : {0, 1, 2, 4, 6, 8}) {
                if (!column_is_int(s, col)) return bad("skill_rank");
            }
            if (!column_is_real(s, 9)) return bad("skill_rank");
            SkillRankRow r;
            r.ord = static_cast<int>(sqlite3_column_int64(s, 1));
            r.guid = sqlite3_column_int64(s, 2);
            r.name = column_text(s, 3);
            r.ability = sqlite3_column_int64(s, 4);
            r.ability_name = column_text(s, 5);
            r.effect = sqlite3_column_int64(s, 6);
            r.effect_name = column_text(s, 7);
            r.effect_infinite = sqlite3_column_int64(s, 8) != 0;
            r.effect_duration = sqlite3_column_double(s, 9);
            if (!(r.effect_duration >= 0) || r.effect_duration > 1e7) {
                error = "content.db skill_rank has an effect duration out of range";
                return std::nullopt;
            }
            if (const auto c = costs.find(r.guid); c != costs.end()) r.costs = c->second;
            if (r.effect != 0 && r.effect_infinite) {
                if (const auto m = mods.find(r.guid); m != mods.end()) r.mods = m->second;
            }
            ranks_of_skill[sqlite3_column_int64(s, 0)].push_back(std::move(r));
        }
        if (step != SQLITE_DONE) return failed("skill_rank");
    }
    // the gates: (tree, node) -> skills (a requirement that names a slot node: that node)
    std::map<std::pair<int64_t, int64_t>, std::vector<int64_t>> requires_of, requires_nodes_of, exclusive_of;
    {
        const struct { const char* sql; std::map<std::pair<int64_t, int64_t>, std::vector<int64_t>>* into; const char* table; bool slots; } gates[] = {
            {kRequireSql, &requires_of, "skill_node_require", true}, {kExclusiveSql, &exclusive_of, "skill_node_exclusive", false}};
        for (const auto& g : gates) {
            Stmt q(db_, g.sql);
            if (!q.ok()) return failed(g.table);
            int step;
            while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
                sqlite3_stmt* s = q.get();
                for (const int col : {0, 1, 2, 3}) {
                    if (!column_is_int(s, col)) return bad(g.table);
                }
                const std::pair<int64_t, int64_t> key{sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1)};
                if (g.slots) {
                    if (!column_is_int(s, 4)) return bad(g.table);
                    if (const int64_t slot_node = sqlite3_column_int64(s, 4); slot_node != 0) {
                        requires_nodes_of[key].push_back(slot_node);
                        continue;
                    }
                }
                (*g.into)[key].push_back(sqlite3_column_int64(s, 3));
            }
            if (step != SQLITE_DONE) return failed(g.table);
        }
    }
    // the trees and their nodes
    std::map<int64_t, size_t> tree_index;
    {
        Stmt q(db_, kTreeSql);
        if (!q.ok()) return failed("skill_tree");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int col : {0, 3, 4, 5}) {
                if (!column_is_int(s, col)) return bad("skill_tree");
            }
            SkillTreeRow t;
            t.guid = sqlite3_column_int64(s, 0);
            t.name = column_text(s, 1);
            t.kind = column_text(s, 2);
            t.leveling_type_guid = sqlite3_column_int64(s, 3);
            t.class_guid = sqlite3_column_int64(s, 4);
            t.weapon_line_guid = sqlite3_column_int64(s, 5);
            tree_index[t.guid] = out.trees.size();
            out.trees.push_back(std::move(t));
        }
        if (step != SQLITE_DONE) return failed("skill_tree");
    }
    {
        Stmt q(db_, kNodeSql);
        if (!q.ok()) return failed("skill_node");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int col : {0, 1, 2, 4, 6, 7, 8, 9, 10, 11, 12}) {
                if (!column_is_int(s, col)) return bad("skill_node");
            }
            const auto tree = tree_index.find(sqlite3_column_int64(s, 0));
            if (tree == tree_index.end()) {
                error = "content.db skill_node names a tree that is none";
                return std::nullopt;
            }
            const int64_t level = sqlite3_column_int64(s, 6), points = sqlite3_column_int64(s, 7);
            if (level < 0 || level > 255 || points < 0 || points > 1000) {
                error = "content.db skill_node has a level or point requirement out of range";
                return std::nullopt;
            }
            SkillNodeRow n;
            n.node = sqlite3_column_int64(s, 1);
            n.ord = static_cast<int>(sqlite3_column_int64(s, 2));
            n.name = column_text(s, 3);
            n.skill = sqlite3_column_int64(s, 4);
            n.skill_name = column_text(s, 5);
            n.level_requirement = static_cast<int>(level);
            n.point_requirement = static_cast<int>(points);
            n.auto_granted = sqlite3_column_int64(s, 8) != 0;
            n.auto_hotbar = sqlite3_column_int64(s, 9) != 0;
            n.all_required = sqlite3_column_int64(s, 10) != 0;
            n.opt = static_cast<int>(sqlite3_column_int64(s, 11));
            n.slot = sqlite3_column_int64(s, 12);
            const std::pair<int64_t, int64_t> key{sqlite3_column_int64(s, 0), n.node};
            if (const auto r = requires_of.find(key); r != requires_of.end()) n.requires_skills = r->second;
            if (const auto r = requires_nodes_of.find(key); r != requires_nodes_of.end()) n.requires_nodes = r->second;
            if (const auto e = exclusive_of.find(key); e != exclusive_of.end()) n.exclusive_skills = e->second;
            if (const auto r = ranks_of_skill.find(n.skill); r != ranks_of_skill.end()) n.ranks = r->second;
            out.trees[tree->second].nodes.push_back(std::move(n));
        }
        if (step != SQLITE_DONE) return failed("skill_node");
    }
    return out;
}

std::optional<ItemWeaponRow> ContentDb::item_weapon(int64_t item_guid, std::string& error) const {
    Stmt q(db_, kItemWeaponSql);
    if (!q.ok()) {
        error = "content.db item_weapon query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_int64(q.get(), 1, item_guid);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) {
        error = "content.db item_weapon query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_stmt* s = q.get();
    if (!column_is_int(s, 0) || !column_is_int(s, 1)) {
        error = "content.db item_weapon has a column of the wrong storage class";
        return std::nullopt;
    }
    ItemWeaponRow w;
    w.attack_ability = sqlite3_column_int64(s, 0);
    w.skill_line = sqlite3_column_int64(s, 1);
    w.skill_line_name = column_text(s, 2);
    return w;
}

// ---- plan E1: the NPC population chain and the vendors ---------------------------------------------------------------------------

namespace {

constexpr const char* kNpcAssetSql =
    "SELECT guid, name, asset_ref, archetype_path, faction_guid, aggro_radius, leash_radius, override_aggro, override_leash, scale_min, scale_max, "
    "dialogue_root_guid, interact_component_guid, interaction_def_guid, appearance_guid, dev_only, filter, tags FROM npc_asset ORDER BY guid";
constexpr const char* kNpcAssetLootSql = "SELECT asset_guid, table_guid FROM npc_asset_loot ORDER BY asset_guid, ord";
constexpr const char* kNpcSetMemberAssetSql = "SELECT set_guid, asset_guid, weight FROM npc_asset_set_member ORDER BY set_guid, ord";
constexpr const char* kNpcAssetSetSql = "SELECT guid, tags FROM npc_asset_set";
constexpr const char* kNpcInstanceSql =
    "SELECT guid, name, level_min, level_max, dynamic_leveling, respawn_time, respawn_variance, respawn_rule, patrol_route_guid, ambient_behavior, "
    "min_radius, max_radius, arrival_distance, wander_speed_mult, wander_delay, wander_delay_deviation, speed_expr, snapping_rule, orientation_type, "
    "aggro_radius, override_aggro, area_weight, vendor_guid, interact_component_guid, interact_response_guid, dialogue_guid, group_id, spawn_location_type, "
    "activation_range, blacklisted, num_waypoints, max_angle, follow_distance, formation, idle_anim, align_to_slope, dev_only, filter_override_guid, spawner_priority, tags "
    "FROM npc_instance ORDER BY guid";
constexpr const char* kNpcInstanceSetSql = "SELECT instance_guid, set_guid FROM npc_instance_asset_set ORDER BY instance_guid, ord";
constexpr const char* kNpcInstanceLootSql = "SELECT instance_guid, table_guid FROM npc_instance_loot ORDER BY instance_guid, ord";
constexpr const char* kNpcSetSql = "SELECT guid, name, area_guid, activation_range, blacklisted, dev_only, filter_override_guid, tags FROM npc_set ORDER BY guid";
constexpr const char* kNpcWorldSpawnSql = "SELECT guid, name, culture, map_name, dev_only, filter_override_guid FROM npc_world_spawn ORDER BY guid";
constexpr const char* kNpcFilterSql = "SELECT guid, filter FROM npc_filter ORDER BY guid";
constexpr const char* kNpcGateSql = "SELECT set_guid, source FROM npc_gate ORDER BY set_guid, source";
constexpr const char* kNpcSetMemberSql = "SELECT set_guid, key, instance_guid, x, y, z, yaw, random_rotation FROM npc_set_member ORDER BY set_guid, key";
constexpr const char* kNpcPlacementSql = "SELECT spawn_guid, key, set_guid, x, y, z, yaw, min_level, max_level FROM npc_placement ORDER BY spawn_guid, key";
constexpr const char* kNpcAreaSql = "SELECT guid, name, min_spawns, max_spawns FROM npc_area ORDER BY guid";
constexpr const char* kNpcRouteSql =
    "SELECT guid, name, looping, start_closest, relative_city_node, relative_encounter, tx, ty, tz, qx, qy, qz, qw, sx, sy, sz, points, waypoints, pauses, pathing_set "
    "FROM npc_route ORDER BY guid";
constexpr const char* kNpcAreaCircleSql = "SELECT area_guid, x, y, z, extent, weight, max_spawns FROM npc_area_circle ORDER BY area_guid, ord";
constexpr const char* kVendorByGuidSql = "SELECT guid, name FROM vendor_def WHERE guid = ?1";
constexpr const char* kVendorByNameSql = "SELECT guid, name FROM vendor_def WHERE name = ?1";
constexpr const char* kVendorNamesWithPrefixSql = "SELECT name FROM vendor_def WHERE substr(name, 1, length(?1)) = ?1 ORDER BY name";
constexpr const char* kVendorInventoriesSql =
    "SELECT d.inventory_guid, i.name FROM vendor_def_inventory d LEFT JOIN vendor_inventory i ON i.guid = d.inventory_guid WHERE d.vendor_guid = ?1 ORDER BY d.ord";
constexpr const char* kVendorListingSql = "SELECT item_guid, offers, hidden FROM vendor_listing WHERE inventory_guid = ?1 ORDER BY ord";
constexpr const char* kGmItemSql = "SELECT guid, name, max_stack_size FROM item WHERE name LIKE 'GM%' OR name LIKE '%PTR%' ORDER BY guid";
constexpr const char* kMountSql =
    "SELECT item_guid, item_name, kind, blueprint_path, blueprint, activation, cast_ability_guid, level, ground_pct, flying_pct, water_pct, health_pct, mule, "
    "crate_capacity, tradeable, description FROM mount ORDER BY item_name";
constexpr const char* kMountStatSql = "SELECT level, stat_guid, stat_name, value FROM mount_stat ORDER BY level, stat_guid";
constexpr const char* kMountAbilitySql = "SELECT ability_guid, name, role, usable_while_mounted, auto_dismount FROM mount_ability ORDER BY name";
constexpr const char* kMountEffectSql =
    "SELECT effect_guid, name, kind, tier, stat_guid, value, relative, infinite, duration, force_dismount FROM mount_effect ORDER BY name";

/// A JSON array of numbers ("[1,2.5]") and one of [x, y, z] triples, as contentc writes a route.
bool parse_number_array(const std::string& text, std::vector<double>& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) return false;
    for (const auto& v : j) {
        if (!v.is_number()) return false;
        out.push_back(v.get<double>());
    }
    return true;
}
// -- CoA x C Y B --

bool parse_vec3_array(const std::string& text, std::vector<std::array<double, 3>>& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) return false;
    for (const auto& v : j) {
        if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) return false;
        out.push_back({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
    }
    return true;
}

/// A JSON array of strings ("[\"a\",\"b\"]"), as contentc writes tags.
bool parse_string_array(const std::string& text, std::vector<std::string>& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) return false;
    for (const auto& v : j) {
        if (!v.is_string()) return false;
        out.push_back(v.get<std::string>());
    }
    return true;
}

/// A PopulationFilter as contentc version 9 writes it (npc_asset.filter, npc_filter.filter).
bool parse_pop_filter(const std::string& text, PopFilter& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    const auto strings = [&](const char* key, std::vector<std::string>& v) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_array()) return false;
        for (const auto& e : *it) {
            if (!e.is_string()) return false;
            v.push_back(e.get<std::string>());
        }
        return true;
    };
    const auto ints = [&](const char* key, std::vector<int64_t>& v) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_array()) return false;
        for (const auto& e : *it) {
            if (!e.is_number_integer()) return false;
            v.push_back(e.get<int64_t>());
        }
        return true;
    };
    const auto number = [&](const char* key, double& v) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_number()) return false;
        v = it->get<double>();
        return true;
    };
    const auto text_of = [&](const char* key, std::string& v) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_string()) return false;
        v = it->get<std::string>();
        return true;
    };
    return strings("biomes", out.biomes) && strings("cultures", out.cultures) && ints("node_ids", out.node_ids) && text_of("node_level_min", out.node_level_min) &&
           text_of("node_level_max", out.node_level_max) && strings("node_types", out.node_types) && strings("required_tags", out.required_tags) &&
           strings("seasons", out.seasons) && number("time_of_day_start", out.time_of_day_start) && number("time_of_day_end", out.time_of_day_end) &&
           strings("weather", out.weather) && ints("zones_of_influence", out.zones);
}

/// A JSON array of integers ("[1,2]"), as contentc writes the offers.
bool parse_int_array(const std::string& text, std::vector<int64_t>& out) {
    size_t i = 0;
    const auto skip = [&] { while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i; };
    skip();
    if (i >= text.size() || text[i] != '[') return false;
    ++i;
    skip();
    if (i < text.size() && text[i] == ']') return i + 1 == text.size();
    while (i < text.size()) {
        skip();
        const size_t start = i;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (i == start) return false;
        out.push_back(std::strtoll(text.substr(start, i - start).c_str(), nullptr, 10));
        skip();
        if (i < text.size() && text[i] == ',') {
            ++i;
            continue;
        }
        if (i < text.size() && text[i] == ']') return i + 1 == text.size();
        return false;
    }
    return false;
}

}  // namespace

std::optional<PopulationData> ContentDb::population_data(std::string& error) const {
    PopulationData out;
    const auto bad = [&](const char* table) {
        error = std::string("content.db ") + table + " has a column of the wrong storage class";
        return std::nullopt;
    };
    const auto failed = [&](const char* what) {
        error = std::string("content.db ") + what + " query failed: " + db_message(db_) +
                " (the NPC population tables are contentc version 9's: rebuild the file with this checkout's contentc)";
        return std::nullopt;
    };
    {   // assets
        Stmt q(db_, kNpcAssetSql);
        if (!q.ok()) return failed("npc_asset");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int col : {0, 4, 7, 8, 11, 12, 13, 14, 15}) {
                if (!column_is_int(s, col)) return bad("npc_asset");
            }
            for (const int col : {5, 6, 9, 10}) {
                if (!column_is_real(s, col)) return bad("npc_asset");
            }
            NpcAssetRow a;
            a.guid = sqlite3_column_int64(s, 0);
            a.name = column_text(s, 1);
            a.asset_ref = column_text(s, 2);
            a.archetype_path = column_text(s, 3);
            a.faction = sqlite3_column_int64(s, 4);
            a.aggro_radius = sqlite3_column_double(s, 5);
            a.leash_radius = sqlite3_column_double(s, 6);
            a.override_aggro = sqlite3_column_int64(s, 7) != 0;
            a.override_leash = sqlite3_column_int64(s, 8) != 0;
            a.scale_min = sqlite3_column_double(s, 9);
            a.scale_max = sqlite3_column_double(s, 10);
            a.dialogue_root = sqlite3_column_int64(s, 11);
            a.interact_component = sqlite3_column_int64(s, 12);
            a.interaction_def = sqlite3_column_int64(s, 13);
            a.appearance = sqlite3_column_int64(s, 14);
            a.dev_only = sqlite3_column_int64(s, 15) != 0;
            if (!parse_pop_filter(column_text(s, 16), a.filter) || !parse_string_array(column_text(s, 17), a.tags)) return bad("npc_asset");
            out.assets[a.guid] = std::move(a);
        }
        if (step != SQLITE_DONE) return failed("npc_asset");
    }
    {
        Stmt q(db_, kNpcAssetLootSql);
        if (!q.ok()) return failed("npc_asset_loot");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0) || !column_is_int(q.get(), 1)) return bad("npc_asset_loot");
            if (const auto it = out.assets.find(sqlite3_column_int64(q.get(), 0)); it != out.assets.end()) it->second.loot_tables.push_back(sqlite3_column_int64(q.get(), 1));
        }
        if (step != SQLITE_DONE) return failed("npc_asset_loot");
    }
    {   // asset sets (a set with no member is a set all the same)
        Stmt q(db_, kNpcAssetSetSql);
        if (!q.ok()) return failed("npc_asset_set");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0)) return bad("npc_asset_set");
            const int64_t guid = sqlite3_column_int64(q.get(), 0);
            out.asset_sets[guid];
            std::vector<std::string> tags;
            if (!parse_string_array(column_text(q.get(), 1), tags)) return bad("npc_asset_set");
            if (!tags.empty()) out.asset_set_tags[guid] = std::move(tags);
        }
        if (step != SQLITE_DONE) return failed("npc_asset_set");
    }
    {
        Stmt q(db_, kNpcSetMemberAssetSql);
        if (!q.ok()) return failed("npc_asset_set_member");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0) || !column_is_int(q.get(), 1) || !column_is_real(q.get(), 2)) return bad("npc_asset_set_member");
            out.asset_sets[sqlite3_column_int64(q.get(), 0)].push_back({sqlite3_column_int64(q.get(), 1), sqlite3_column_double(q.get(), 2)});
        }
        if (step != SQLITE_DONE) return failed("npc_asset_set_member");
    }
    {   // instances
        Stmt q(db_, kNpcInstanceSql);
        if (!q.ok()) return failed("npc_instance");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int col : {0, 2, 3, 4, 8, 20, 22, 23, 24, 25, 26, 29, 30, 35, 36, 37}) {
                if (!column_is_int(s, col)) return bad("npc_instance");
            }
            for (const int col : {5, 6, 10, 11, 12, 13, 14, 15, 19, 21, 28, 31, 32}) {
                if (!column_is_real(s, col)) return bad("npc_instance");
            }
            NpcInstanceRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.level_min = static_cast<int>(sqlite3_column_int64(s, 2));
            r.level_max = static_cast<int>(sqlite3_column_int64(s, 3));
            r.dynamic_leveling = sqlite3_column_int64(s, 4) != 0;
            r.respawn_time = sqlite3_column_double(s, 5);
            r.respawn_variance = sqlite3_column_double(s, 6);
            r.respawn_rule = column_text(s, 7);
            r.patrol_route = sqlite3_column_int64(s, 8);
            r.ambient_behavior = column_text(s, 9);
            r.min_radius = sqlite3_column_double(s, 10);
            r.max_radius = sqlite3_column_double(s, 11);
            r.arrival_distance = sqlite3_column_double(s, 12);
            r.wander_speed_mult = sqlite3_column_double(s, 13);
            r.wander_delay = sqlite3_column_double(s, 14);
            r.wander_delay_deviation = sqlite3_column_double(s, 15);
            r.speed_expr = column_text(s, 16);
            r.snapping_rule = column_text(s, 17);
            r.orientation_type = column_text(s, 18);
            r.aggro_radius = sqlite3_column_double(s, 19);
            r.override_aggro = sqlite3_column_int64(s, 20) != 0;
            r.area_weight = sqlite3_column_double(s, 21);
            r.vendor = sqlite3_column_int64(s, 22);
            r.interact_component = sqlite3_column_int64(s, 23);
            r.interact_response = sqlite3_column_int64(s, 24);
            r.dialogue = sqlite3_column_int64(s, 25);
            r.group_id = sqlite3_column_int64(s, 26);
            r.spawn_location_type = column_text(s, 27);
            r.activation_range = sqlite3_column_double(s, 28);
            r.blacklisted = sqlite3_column_int64(s, 29) != 0;
            r.num_waypoints = static_cast<int>(sqlite3_column_int64(s, 30));
            r.max_angle = sqlite3_column_double(s, 31);
            r.follow_distance = sqlite3_column_double(s, 32);
            r.formation = column_text(s, 33);
            r.idle_anim = column_text(s, 34);
            r.align_to_slope = sqlite3_column_int64(s, 35) != 0;
            r.dev_only = sqlite3_column_int64(s, 36) != 0;
            r.filter_override = sqlite3_column_int64(s, 37);
            r.spawner_priority = column_text(s, 38);
            if (!parse_string_array(column_text(s, 39), r.tags)) return bad("npc_instance");
            out.instances[r.guid] = std::move(r);
        }
        if (step != SQLITE_DONE) return failed("npc_instance");
    }
    for (const auto& [sql, table, loot] : {std::tuple{kNpcInstanceSetSql, "npc_instance_asset_set", false}, std::tuple{kNpcInstanceLootSql, "npc_instance_loot", true}}) {
        Stmt q(db_, sql);
        if (!q.ok()) return failed(table);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0) || !column_is_int(q.get(), 1)) return bad(table);
            const auto it = out.instances.find(sqlite3_column_int64(q.get(), 0));
            if (it == out.instances.end()) continue;
            (loot ? it->second.loot_tables : it->second.asset_sets).push_back(sqlite3_column_int64(q.get(), 1));
        }
        if (step != SQLITE_DONE) return failed(table);
    }
    {   // sets and their members
        Stmt q(db_, kNpcSetSql);
        if (!q.ok()) return failed("npc_set");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 2) || !column_is_real(s, 3) || !column_is_int(s, 4) || !column_is_int(s, 5) || !column_is_int(s, 6)) return bad("npc_set");
            NpcSetRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.area = sqlite3_column_int64(s, 2);
            r.activation_range = sqlite3_column_double(s, 3);
            r.blacklisted = sqlite3_column_int64(s, 4) != 0;
            r.dev_only = sqlite3_column_int64(s, 5) != 0;
            r.filter_override = sqlite3_column_int64(s, 6);
            if (!parse_string_array(column_text(s, 7), r.tags)) return bad("npc_set");
            out.sets[r.guid] = std::move(r);
        }
        if (step != SQLITE_DONE) return failed("npc_set");
    }
    {   // world spawns, the filters and the gates (contentc version 9)
        Stmt q(db_, kNpcWorldSpawnSql);
        if (!q.ok()) return failed("npc_world_spawn");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 4) || !column_is_int(s, 5)) return bad("npc_world_spawn");
            NpcWorldSpawn w;
            w.guid = sqlite3_column_int64(s, 0);
            w.name = column_text(s, 1);
            w.culture = column_text(s, 2);
            w.map_name = column_text(s, 3);
            w.dev_only = sqlite3_column_int64(s, 4) != 0;
            w.filter_override = sqlite3_column_int64(s, 5);
            out.world_spawns[w.guid] = std::move(w);
        }
        if (step != SQLITE_DONE) return failed("npc_world_spawn");
    }
    {
        Stmt q(db_, kNpcFilterSql);
        if (!q.ok()) return failed("npc_filter");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0)) return bad("npc_filter");
            PopFilter f;
            if (!parse_pop_filter(column_text(q.get(), 1), f)) return bad("npc_filter");
            out.filters[sqlite3_column_int64(q.get(), 0)] = std::move(f);
        }
        if (step != SQLITE_DONE) return failed("npc_filter");
    }
    {
        Stmt q(db_, kNpcGateSql);
        if (!q.ok()) return failed("npc_gate");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0)) return bad("npc_gate");
            out.gates[sqlite3_column_int64(q.get(), 0)].push_back(column_text(q.get(), 1));
        }
        if (step != SQLITE_DONE) return failed("npc_gate");
    }
    {
        Stmt q(db_, kNpcSetMemberSql);
        if (!q.ok()) return failed("npc_set_member");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 1) || !column_is_int(s, 2) || !column_is_real(s, 3) || !column_is_real(s, 4) || !column_is_real(s, 5) ||
                !column_is_real(s, 6) || !column_is_int(s, 7)) {
                return bad("npc_set_member");
            }
            const auto it = out.sets.find(sqlite3_column_int64(s, 0));
            if (it == out.sets.end()) continue;
            NpcSetMember m;
            m.key = sqlite3_column_int64(s, 1);
            m.instance = sqlite3_column_int64(s, 2);
            m.x = sqlite3_column_double(s, 3);
            m.y = sqlite3_column_double(s, 4);
            m.z = sqlite3_column_double(s, 5);
            m.yaw = sqlite3_column_double(s, 6);
            m.random_rotation = sqlite3_column_int64(s, 7) != 0;
            it->second.members.push_back(m);
        }
        if (step != SQLITE_DONE) return failed("npc_set_member");
    }
    {   // placements
        Stmt q(db_, kNpcPlacementSql);
        if (!q.ok()) return failed("npc_placement");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 1) || !column_is_int(s, 2) || !column_is_real(s, 3) || !column_is_real(s, 4) || !column_is_real(s, 5) ||
                !column_is_real(s, 6) || !column_is_int(s, 7) || !column_is_int(s, 8)) {
                return bad("npc_placement");
            }
            NpcPlacement p;
            p.spawn = sqlite3_column_int64(s, 0);
            p.key = sqlite3_column_int64(s, 1);
            p.set = sqlite3_column_int64(s, 2);
            p.x = sqlite3_column_double(s, 3);
            p.y = sqlite3_column_double(s, 4);
            p.z = sqlite3_column_double(s, 5);
            p.yaw = sqlite3_column_double(s, 6);
            p.min_level = static_cast<int>(sqlite3_column_int64(s, 7));
            p.max_level = static_cast<int>(sqlite3_column_int64(s, 8));
            out.placements.push_back(p);
        }
        if (step != SQLITE_DONE) return failed("npc_placement");
    }
    {   // patrol routes (contentc version 8)
        Stmt q(db_, kNpcRouteSql);
        if (!q.ok()) return failed("npc_route");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            for (const int col : {0, 2, 3, 4, 5}) {
                if (!column_is_int(s, col)) return bad("npc_route");
            }
            for (int col = 6; col <= 15; ++col) {
                if (!column_is_real(s, col)) return bad("npc_route");
            }
            NpcRouteRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.looping = sqlite3_column_int64(s, 2) != 0;
            r.start_closest = sqlite3_column_int64(s, 3) != 0;
            r.relative_city_node = sqlite3_column_int64(s, 4) != 0;
            r.relative_encounter = sqlite3_column_int64(s, 5) != 0;
            double* v[10] = {&r.tx, &r.ty, &r.tz, &r.qx, &r.qy, &r.qz, &r.qw, &r.sx, &r.sy, &r.sz};
            for (int i = 0; i < 10; ++i) *v[i] = sqlite3_column_double(s, 6 + i);
            if (!parse_vec3_array(column_text(s, 16), r.points) || !parse_vec3_array(column_text(s, 17), r.waypoints) || !parse_number_array(column_text(s, 18), r.pauses)) return bad("npc_route");
            r.pathing_set = column_text(s, 19);
            out.routes[r.guid] = std::move(r);
        }
        if (step != SQLITE_DONE) return failed("npc_route");
    }
    {   // areas and their circles
        Stmt q(db_, kNpcAreaSql);
        if (!q.ok()) return failed("npc_area");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 2) || !column_is_int(s, 3)) return bad("npc_area");
            NpcAreaRow a;
            a.guid = sqlite3_column_int64(s, 0);
            a.name = column_text(s, 1);
            a.min_spawns = static_cast<int>(sqlite3_column_int64(s, 2));
            a.max_spawns = static_cast<int>(sqlite3_column_int64(s, 3));
            out.areas[a.guid] = std::move(a);
        }
        if (step != SQLITE_DONE) return failed("npc_area");
    }
    {
        Stmt q(db_, kNpcAreaCircleSql);
        if (!q.ok()) return failed("npc_area_circle");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_real(s, 1) || !column_is_real(s, 2) || !column_is_real(s, 3) || !column_is_real(s, 4) || !column_is_real(s, 5) ||
                !column_is_int(s, 6)) {
                return bad("npc_area_circle");
            }
            const auto it = out.areas.find(sqlite3_column_int64(s, 0));
            if (it == out.areas.end()) continue;
            NpcAreaCircle c;
            c.x = sqlite3_column_double(s, 1);
            c.y = sqlite3_column_double(s, 2);
            c.z = sqlite3_column_double(s, 3);
            c.extent = sqlite3_column_double(s, 4);
            c.weight = sqlite3_column_double(s, 5);
            c.max_spawns = sqlite3_column_int64(s, 6);
            it->second.circles.push_back(c);
        }
        if (step != SQLITE_DONE) return failed("npc_area_circle");
    }
    return out;
}
// C:Y:B + C-O-A

namespace {

std::optional<VendorDefRow> read_vendor(sqlite3* db, const char* sql, const std::string* name, int64_t guid, std::string& error) {
    const auto failed = [&](const char* what) {
        error = std::string("content.db ") + what + " query failed: " + db_message(db) +
                " (the vendor tables are contentc version 8's: rebuild the file with this checkout's contentc)";
        return std::nullopt;
    };
    VendorDefRow out;
    {
        Stmt q(db, sql);
        if (!q.ok()) return failed("vendor_def");
        if (name) sqlite3_bind_text(q.get(), 1, name->c_str(), static_cast<int>(name->size()), SQLITE_TRANSIENT);
        else sqlite3_bind_int64(q.get(), 1, guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) return std::nullopt;
        if (step != SQLITE_ROW) return failed("vendor_def");
        if (!column_is_int(q.get(), 0)) {
            error = "content.db vendor_def has a column of the wrong storage class";
            return std::nullopt;
        }
        out.guid = sqlite3_column_int64(q.get(), 0);
        out.name = column_text(q.get(), 1);
    }
    {
        Stmt q(db, kVendorInventoriesSql);
        if (!q.ok()) return failed("vendor_def_inventory");
        sqlite3_bind_int64(q.get(), 1, out.guid);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0)) {
                error = "content.db vendor_def_inventory has a column of the wrong storage class";
                return std::nullopt;
            }
            VendorInventoryRow inv;
            inv.guid = sqlite3_column_int64(q.get(), 0);
            inv.name = column_text(q.get(), 1);
            out.inventories.push_back(std::move(inv));
        }
        if (step != SQLITE_DONE) return failed("vendor_def_inventory");
    }
    for (VendorInventoryRow& inv : out.inventories) {
        Stmt q(db, kVendorListingSql);
        if (!q.ok()) return failed("vendor_listing");
        sqlite3_bind_int64(q.get(), 1, inv.guid);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0) || !column_is_text(q.get(), 1) || !column_is_int(q.get(), 2)) {
                error = "content.db vendor_listing has a column of the wrong storage class";
                return std::nullopt;
            }
            VendorListing l;
            l.item = sqlite3_column_int64(q.get(), 0);
            if (!parse_int_array(column_text(q.get(), 1), l.offers)) {
                error = "content.db vendor_listing has offers that are no JSON array of integers";
                return std::nullopt;
            }
            l.hidden = sqlite3_column_int64(q.get(), 2) != 0;
            inv.listings.push_back(std::move(l));
        }
        if (step != SQLITE_DONE) return failed("vendor_listing");
    }
    return out;
}

}  // namespace

std::optional<VendorDefRow> ContentDb::vendor_def(int64_t guid, std::string& error) const { return read_vendor(db_, kVendorByGuidSql, nullptr, guid, error); }

std::optional<CombatRows> ContentDb::combat_rows(std::string& error) const {
    CombatRows out;
    const auto failed = [&](const char* what) {
        error = std::string("content.db ") + what + " query failed: " + db_message(db_) +
                " (the combat tables are contentc version 9's: rebuild the file with this checkout's contentc)";
        return std::nullopt;
    };
    {
        Stmt q(db_, "SELECT key, stat_guid FROM combat_profile");
        if (!q.ok()) return failed("combat_profile");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) out.profile[column_text(q.get(), 0)] = sqlite3_column_int64(q.get(), 1);
        if (step != SQLITE_DONE) return failed("combat_profile");
    }
    {
        Stmt q(db_, "SELECT tag, damage, damage_mult, damage_add_mult, mitigation FROM combat_element ORDER BY tag");
        if (!q.ok()) return failed("combat_element");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            CombatRows::Element e;
            e.tag = column_text(q.get(), 0);
            e.damage = sqlite3_column_int64(q.get(), 1);
            e.damage_mult = sqlite3_column_int64(q.get(), 2);
            e.damage_add_mult = sqlite3_column_int64(q.get(), 3);
            e.mitigation = sqlite3_column_int64(q.get(), 4);
            out.elements.push_back(std::move(e));
        }
        if (step != SQLITE_DONE) return failed("combat_element");
    }
    {
        Stmt q(db_, "SELECT key, value FROM combat_config");
        if (!q.ok()) return failed("combat_config");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) out.config[column_text(q.get(), 0)] = sqlite3_column_double(q.get(), 1);
        if (step != SQLITE_DONE) return failed("combat_config");
    }
    {
        Stmt q(db_, "SELECT guid, permissions FROM relationship");
        if (!q.ok()) return failed("relationship");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) out.permissions[sqlite3_column_int64(q.get(), 0)] = static_cast<int>(sqlite3_column_int64(q.get(), 1));
        if (step != SQLITE_DONE) return failed("relationship");
    }
    {
        Stmt q(db_, "SELECT alignment, other, relationship FROM alignment_relation");
        if (!q.ok()) return failed("alignment_relation");
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            out.relation[sqlite3_column_int64(q.get(), 0)][sqlite3_column_int64(q.get(), 1)] = sqlite3_column_int64(q.get(), 2);
        }
        if (step != SQLITE_DONE) return failed("alignment_relation");
    }
    if (out.profile.empty() || out.elements.empty()) {
        error = "content.db has no combat profile (the combat tables are empty)";
        return std::nullopt;
    }
    return out;
}

namespace {
// CoA : C:Y:B

std::vector<int64_t> int_list(const nlohmann::json& j, const char* key) {
    std::vector<int64_t> out;
    const auto it = j.find(key);
    if (it != j.end() && it->is_array()) {
        for (const auto& v : *it) out.push_back(v.is_number() ? v.get<int64_t>() : 0);
    }
    return out;
}

std::vector<double> number_list(const nlohmann::json& j, const char* key) {
    std::vector<double> out;
    const auto it = j.find(key);
    if (it != j.end() && it->is_array()) {
        for (const auto& v : *it) out.push_back(v.is_number() ? v.get<double>() : 0.0);
    }
    return out;
}

int64_t int_of(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int64_t>() : 0;
}

std::string str_of(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

std::optional<std::map<int64_t, LootTable>> ContentDb::loot_tables(const std::vector<int64_t>& roots, std::string& error) const {
    Stmt q(db_, "SELECT name, data FROM loot_table WHERE guid = ?1");
    if (!q.ok()) {
        error = "content.db loot_table query failed: " + db_message(db_) +
                " (the reward tables are contentc version 10's: rebuild the file with this checkout's contentc)";
        return std::nullopt;
    }
    std::map<int64_t, LootTable> out;
    std::vector<int64_t> todo(roots.begin(), roots.end());
    while (!todo.empty()) {
        const int64_t guid = todo.back();
        todo.pop_back();
        if (guid == 0 || out.count(guid)) continue;
        sqlite3_reset(q.get());
        sqlite3_bind_int64(q.get(), 1, guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) continue;
        if (step != SQLITE_ROW) {
            error = "content.db loot_table query failed: " + db_message(db_);
            return std::nullopt;
        }
        LootTable t;
        t.guid = guid;
        t.name = column_text(q.get(), 0);
        const nlohmann::json j = nlohmann::json::parse(column_text(q.get(), 1), nullptr, false);
        if (!j.is_object()) {
            error = "content.db loot_table " + t.name + " (" + std::to_string(guid) + ") holds no JSON object";
            return std::nullopt;
        }
        t.pred = int_of(j, "pred");
        t.algo = str_of(j, "algo");
        t.n = int_of(j, "n");
        t.subs = int_list(j, "subs");
        t.w = number_list(j, "w");
        t.pct = int_list(j, "pct");
        t.xw = int_list(j, "xw");
        if (const auto cs = j.find("c"); cs != j.end() && cs->is_array()) {
            for (const auto& cj : *cs) {
                LootContainer c;
                c.grant = str_of(cj, "grant");
                c.algo = str_of(cj, "algo");
                c.pred = int_of(cj, "pred");
                c.n = int_of(cj, "n");
                c.w = number_list(cj, "w");
                c.pct = int_list(cj, "pct");
                c.xw = int_list(cj, "xw");
                if (const auto rs = cj.find("r"); rs != cj.end() && rs->is_array()) {
                    for (const auto& rj : *rs) {
                        LootReward r;
                        if (const auto xs = rj.find("xp"); xs != rj.end() && xs->is_array()) {
                            for (const auto& xj : *xs) r.xp.push_back({str_of(xj, "type"), int_of(xj, "expr")});
                        }
                        if (const auto is = rj.find("items"); is != rj.end() && is->is_array()) {
                            for (const auto& ij : *is) r.items.push_back({int_of(ij, "item"), int_of(ij, "variation"), int_of(ij, "qty")});
                        }
                        if (const auto cu = rj.find("currency"); cu != rj.end() && cu->is_array()) {
                            for (const auto& uj : *cu) r.currency.push_back({int_of(uj, "currency"), int_of(uj, "amount")});
                        }
                        c.rewards.push_back(std::move(r));
                    }
                }
                t.containers.push_back(std::move(c));
            }
        }
        for (const int64_t sub : t.subs) todo.push_back(sub);
        out.emplace(guid, std::move(t));
    }
    return out;
}

std::optional<int64_t> ContentDb::xp_rule(const std::string& key, std::string& error) const {
    Stmt q(db_, "SELECT expr FROM xp_rule WHERE key = ?1");
    if (!q.ok()) {
        error = "content.db xp_rule query failed: " + db_message(db_) + " (the reward tables are contentc version 10's: rebuild the file with this checkout's contentc)";
        return std::nullopt;
    }
    sqlite3_bind_text(q.get(), 1, key.c_str(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_ROW) return sqlite3_column_int64(q.get(), 0);
    if (step == SQLITE_DONE) return int64_t{0};
    error = "content.db xp_rule query failed: " + db_message(db_);
    return std::nullopt;
}

std::optional<VendorDefRow> ContentDb::vendor_def_named(const std::string& name, std::string& error) const {
    return read_vendor(db_, kVendorByNameSql, &name, 0, error);
}
// (CoA + C Y B)

std::optional<std::vector<VendorDefRow>> ContentDb::vendor_defs_with_prefix(const std::string& prefix, std::string& error) const {
    std::vector<std::string> names;
    {
        Stmt q(db_, kVendorNamesWithPrefixSql);
        if (!q.ok()) {
            error = "content.db vendor_def query failed: " + db_message(db_) + " (the vendor tables are contentc version 8's: rebuild the file with this checkout's contentc)";
            return std::nullopt;
        }
        sqlite3_bind_text(q.get(), 1, prefix.c_str(), static_cast<int>(prefix.size()), SQLITE_TRANSIENT);
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) names.push_back(column_text(q.get(), 0));
        if (step != SQLITE_DONE) {
            error = "content.db vendor_def query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    std::vector<VendorDefRow> out;
    for (const std::string& n : names) {
        auto row = read_vendor(db_, kVendorByNameSql, &n, 0, error);
        if (!row) {
            if (error.empty()) continue;
            return std::nullopt;
        }
        out.push_back(std::move(*row));
    }
    return out;
}

std::optional<std::vector<GmItemRow>> ContentDb::gm_items(std::string& error) const {
    Stmt q(db_, kGmItemSql);
    if (!q.ok()) {
        error = "content.db item query failed: " + db_message(db_);
        return std::nullopt;
    }
    std::vector<GmItemRow> out;
    int step;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        if (!column_is_int(q.get(), 0) || !column_is_int(q.get(), 2)) {
            error = "content.db item has a column of the wrong storage class";
            return std::nullopt;
        }
        GmItemRow r;
        r.guid = sqlite3_column_int64(q.get(), 0);
        r.name = column_text(q.get(), 1);
        r.max_stack = static_cast<int>(sqlite3_column_int64(q.get(), 2));
        out.push_back(std::move(r));
    }
    if (step != SQLITE_DONE) {
        error = "content.db item query failed: " + db_message(db_);
        return std::nullopt;
    }
    return out;
}

const MountRow* MountData::find(int64_t item) const {
    for (const MountRow& m : mounts)
        if (m.item == item) return &m;
    return nullptr;
}

double MountData::stat_at(int level, int64_t stat, double fallback) const {
    for (const MountStatRow& r : stats)
        if (r.level == level && r.stat == stat) return r.value;
    return fallback;
}

const MountAbilityRow* MountData::ability(int64_t guid) const {
    for (const MountAbilityRow& a : abilities)
        if (a.ability == guid) return &a;
    return nullptr;
}

const MountAbilityRow* MountData::ability_with_role(const std::string& role) const {
    for (const MountAbilityRow& a : abilities)
        if (a.role == role) return &a;
    return nullptr;
}

const MountEffectRow* MountData::road(int tier) const {
    for (const MountEffectRow& e : effects)
        if (e.kind == "road" && e.tier == tier) return &e;
    return nullptr;
}

const MountEffectRow* MountData::dash() const {
    for (const MountEffectRow& e : effects)
        if (e.kind == "dash") return &e;
    return nullptr;
}

std::optional<MountData> ContentDb::mount_data(std::string& error) const {
    MountData out;
    const std::string rebuild = " (the mount tables are contentc version 11's: rebuild the file with this checkout's contentc)";
    const auto wrong = [&](const char* table) {
        error = std::string("content.db ") + table + " has a column of the wrong storage class";
        return std::optional<MountData>{};
    };
    {
        Stmt q(db_, kMountSql);
        if (!q.ok()) {
            error = "content.db mount query failed: " + db_message(db_) + rebuild;
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            for (const int c : {0, 6, 7, 8, 9, 10, 11, 12, 13, 14})
                if (!column_is_int(q.get(), c)) return wrong("mount");
            MountRow r;
            r.item = sqlite3_column_int64(q.get(), 0);
            r.item_name = column_text(q.get(), 1);
            r.kind = column_text(q.get(), 2);
            r.blueprint_path = column_text(q.get(), 3);
            r.blueprint = column_text(q.get(), 4);
            r.activation = column_text(q.get(), 5);
            r.cast_ability = sqlite3_column_int64(q.get(), 6);
            r.level = static_cast<int>(sqlite3_column_int64(q.get(), 7));
            r.ground_pct = static_cast<int>(sqlite3_column_int64(q.get(), 8));
            r.flying_pct = static_cast<int>(sqlite3_column_int64(q.get(), 9));
            r.water_pct = static_cast<int>(sqlite3_column_int64(q.get(), 10));
            r.health_pct = static_cast<int>(sqlite3_column_int64(q.get(), 11));
            r.mule = sqlite3_column_int64(q.get(), 12) != 0;
            r.crate_capacity = static_cast<int>(sqlite3_column_int64(q.get(), 13));
            r.tradeable = sqlite3_column_int64(q.get(), 14) != 0;
            r.description = column_text(q.get(), 15);
            out.mounts.push_back(std::move(r));
        }
        if (step != SQLITE_DONE) {
            error = "content.db mount query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    {
        Stmt q(db_, kMountStatSql);
        if (!q.ok()) {
            error = "content.db mount_stat query failed: " + db_message(db_) + rebuild;
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0) || !column_is_int(q.get(), 1)) return wrong("mount_stat");
            MountStatRow r;
            r.level = static_cast<int>(sqlite3_column_int64(q.get(), 0));
            r.stat = sqlite3_column_int64(q.get(), 1);
            r.stat_name = column_text(q.get(), 2);
            r.value = sqlite3_column_double(q.get(), 3);
            out.stats.push_back(std::move(r));
        }
        if (step != SQLITE_DONE) {
            error = "content.db mount_stat query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    {
        Stmt q(db_, kMountAbilitySql);
        if (!q.ok()) {
            error = "content.db mount_ability query failed: " + db_message(db_) + rebuild;
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            if (!column_is_int(q.get(), 0) || !column_is_int(q.get(), 3) || !column_is_int(q.get(), 4)) return wrong("mount_ability");
            MountAbilityRow r;
            r.ability = sqlite3_column_int64(q.get(), 0);
            r.name = column_text(q.get(), 1);
            r.role = column_text(q.get(), 2);
            r.usable_while_mounted = sqlite3_column_int64(q.get(), 3) != 0;
            r.auto_dismount = sqlite3_column_int64(q.get(), 4) != 0;
            out.abilities.push_back(std::move(r));
        }
        if (step != SQLITE_DONE) {
            error = "content.db mount_ability query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    {
        Stmt q(db_, kMountEffectSql);
        if (!q.ok()) {
            error = "content.db mount_effect query failed: " + db_message(db_) + rebuild;
            return std::nullopt;
        }
        int step;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            for (const int c : {0, 3, 4, 6, 7, 9})
                if (!column_is_int(q.get(), c)) return wrong("mount_effect");
            MountEffectRow r;
            r.effect = sqlite3_column_int64(q.get(), 0);
            r.name = column_text(q.get(), 1);
            r.kind = column_text(q.get(), 2);
            r.tier = static_cast<int>(sqlite3_column_int64(q.get(), 3));
            r.stat = sqlite3_column_int64(q.get(), 4);
            r.value = sqlite3_column_double(q.get(), 5);
            r.relative = sqlite3_column_int64(q.get(), 6) != 0;
            r.infinite = sqlite3_column_int64(q.get(), 7) != 0;
            r.duration = sqlite3_column_double(q.get(), 8);
            r.force_dismount = sqlite3_column_int64(q.get(), 9) != 0;
            out.effects.push_back(std::move(r));
        }
        if (step != SQLITE_DONE) {
            error = "content.db mount_effect query failed: " + db_message(db_);
            return std::nullopt;
        }
    }
    return out;
}


ContentGearCatalog::ContentGearCatalog(const ContentDb& db) : db_(db) {
    if (auto names = db.equip_slot_names(error_)) slots_ = std::move(*names);
}

const stats::ItemGear* ContentGearCatalog::item(int64_t record) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = items_.find(record); it != items_.end()) return it->second.get();
    std::string error;
    auto g = db_.item_gear(record, error);
    if (!g) {
        if (!error.empty()) spdlog::error("[content] {}", error);
        return nullptr;   // not kept: a record that is none is asked about rarely, and a query that failed may work next time
    }
    auto& slot = items_[record];
    slot = std::make_unique<stats::ItemGear>(std::move(*g));
    return slot.get();
}

const stats::SetBonus* ContentGearCatalog::set(int64_t guid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = sets_.find(guid); it != sets_.end()) return it->second.get();
    std::string error;
    auto s = db_.set_bonus(guid, error);
    if (!s) {
        if (!error.empty()) spdlog::error("[content] {}", error);
        return nullptr;
    }
    auto& slot = sets_[guid];
    slot = std::make_unique<stats::SetBonus>(std::move(*s));
    return slot.get();
}

std::string ContentGearCatalog::slot_name(int slot) const {
    const auto it = slots_.find(slot);
    return it != slots_.end() ? it->second : "Slot" + std::to_string(slot);
}

}  // namespace aoc::worldd::content

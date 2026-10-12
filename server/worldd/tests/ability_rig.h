// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_rig.h  (plan D2)
//
//  What the tests of the generic ability runtime share: a Bard (or any class) of a level with the ranks the game grants and the ones a test
//  adds, on the REAL content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB: the stat program, the gear rules, the ability
//  tables and A7's rules all come from it), A7's AbilityRuntime and the pipeline on a simulated clock, and a `Pawn` that takes the events the way
//  PlayerBootstrap::apply_ability_change does (the same key checks), so that a test reads what the client holds: stats, effect items, the ability
//  item and the Cooldowns items.
// ============================================================================
#pragma once

#include "bootstrap/ability_replication.h"
#include "bootstrap/character_setup.h"
#include "content/ability_content.h"
#include "content/content_db.h"
#include "session/abilities.h"
#include "session/ability_catalog.h"
#include "session/ability_pipeline.h"
#include "session/talents.h"
#include "stats/character_stats.h"
#include "world_stats.h"

#include <sqlite3.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ability_rig {

using namespace aoc::worldd;

#ifndef ABILITY_CONTENT_DB_PATH
#define ABILITY_CONTENT_DB_PATH "dist/content/content.db"
#endif

inline std::string real_db_path() {
    std::string path = ABILITY_CONTENT_DB_PATH;
    if (const char* env = std::getenv("AOC_CONTENT_DB")) path = env;
    return path;
}

inline bool real_db_present() {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(real_db_path()), ec);
}

// the stats the tests look at
inline constexpr int64_t kMoveSpeedMult = 109228544644729ll;
inline constexpr int64_t kResonanceStat = 6064632920367763592ll;
inline constexpr int64_t kMitigation = 6064628617716498440ll;
inline constexpr int64_t kHealingMult = 6064630634099769360ll;
inline constexpr int64_t kEvasion = 6064628617716498442ll;
// [C.O.A + C.Y.B]

/// The record guid of a row of a design table by schema and name (0 when there is none), read straight from content.db's `record` table.
inline int64_t record_guid(const std::string& path, const char* schema, const char* name) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return 0;
    sqlite3_stmt* st = nullptr;
    int64_t guid = 0;
    if (sqlite3_prepare_v2(db, "SELECT guid FROM record WHERE schema = ?1 AND name = ?2", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, schema, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) guid = sqlite3_column_int64(st, 0);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return guid;
}

/// The client's arrays as the events leave them (PlayerBootstrap::apply_ability_change's key rules): a change it would refuse is `broken`.
struct Pawn {
    std::map<int64_t, float>                         stats;       // record -> value
    std::map<int64_t, bootstrap::EffectItem>         effects;     // EffectId -> item
    std::map<int64_t, bootstrap::ActiveAbilityItem>  abilities;   // ServerInstanceId -> item
    std::map<int64_t, bootstrap::CooldownItem>       cooldowns;   // ability guid -> item
    std::vector<int32_t>                             ends;        // ClientEndAbility ids, in order
    std::vector<bootstrap::ShieldElement>            shields;     // the TempHealth array's item (plan D2, stage 2)
    std::vector<bootstrap::HitConfirm>               confirmations;   // every hit confirmation the client was sent, in order
    std::string                                      broken;
    size_t                                           stat_changes = 0;

    bool apply(const bootstrap::AbilityChange& c) {
        if (c.shields) shields = *c.shields;
        for (const auto& s : c.stats) {
            stats[s.record] = s.value;
            ++stat_changes;
        }
        for (const int64_t k : c.effects_removed)
            if (!effects.erase(k)) broken = "an effect removed that is not there";
        for (const auto& e : c.effects_added)
            if (!effects.emplace(e.effect_id, e).second) broken = "an effect added twice";
        for (const auto& e : c.effects_changed) {
            if (!effects.count(e.effect_id)) broken = "an effect changed that is not there";
            effects[e.effect_id] = e;
        }
        for (const int64_t k : c.abilities_removed)
            if (!abilities.erase(k)) broken = "an ability item removed that is not there";
        for (const auto& a : c.abilities_added)
            if (!abilities.emplace(a.server_instance_id, a).second) broken = "an ability item added twice";
        for (const auto& a : c.abilities_changed) {
            if (!abilities.count(a.server_instance_id)) broken = "an ability item changed that is not there";
            abilities[a.server_instance_id] = a;
        }
        for (const int64_t k : c.cooldowns_removed)
            if (!cooldowns.erase(k)) broken = "a cooldown removed that is not there";
        for (const auto& cd : c.cooldowns_set) cooldowns[cd.ability_guid] = cd;
        return broken.empty();
    }
    float stat(int64_t record) const {
        const auto it = stats.find(record);
        return it == stats.end() ? -12345.0f : it->second;
    }
    const bootstrap::EffectItem* effect(int64_t guid) const {
        for (const auto& [k, e] : effects)
            if (e.effect_guid == guid) return &e;
        return nullptr;
    }
    const bootstrap::ActiveAbilityItem* ability_item() const { return abilities.empty() ? nullptr : &abilities.begin()->second; }
    const bootstrap::CooldownItem* cooldown(int64_t ability) const {
        const auto it = cooldowns.find(ability);
        return it == cooldowns.end() ? nullptr : &it->second;
    }
};

/// One event as a test wants to read it.
struct Seen {
    double                    at = 0;
    bootstrap::AbilityChange  change;
    std::optional<int32_t>    end;
    std::string               note;
    bool                      refusal = false;
    std::vector<bootstrap::HitConfirm> confirmations;
    std::vector<bootstrap::ZoneOp>     zones;   // the lingering zones' actors this step opens, fades or closes
};

struct Rig {
    std::string                                 path;
    std::optional<content::ContentDb>           db;
    const bootstrap::StatContext*               ctx = nullptr;
    std::optional<abilities::Catalog>           catalog;
    content::AbilityRules                       rules;
    std::optional<stats::CharacterStats>        cs;
    std::optional<talents::Catalog>             talent_catalog;   // the production path's: the ranks the character holds come from a talents::Book
    std::optional<talents::Book>                book;
    std::vector<stats::SkillRank>               base_ranks;   // what the Book holds (held_ranks()) and the named skills it has no rank for
    std::optional<AbilityRuntime>               a7;
    std::optional<AbilityPipeline>              pipe;
    Pawn                                        pawn;
    std::vector<Seen>                           log;           // every event of every call, with the time it happened
    double                                      clock = 0;
    int32_t                                     next_client_id = 1;
    std::string                                 problem;

    static constexpr double kWorldTime = 1000.0;

    /// A character of `class_enum` at `level`, holding the ranks a character of that level has and `extra_skills` (SkillRecordBase names the
    /// character bought: Bard_Passive_Merriment ...); the stats start at their maximum. The ranks are the join's own (plan D2, review): a talents::Book
    /// of the level with the skills bought, whose held_ranks() are what GetSkillPurchased reads and whose passives() are the stat mods of the ranks
    /// (Endurance Training's MaxStamina +10), as build_player_setup and the session hand them to the stat engine. A skill that is in no tree the
    /// character can buy from is a skill-typed rank the test names alone (a talent inferred from a capture).
    Rig(int class_enum, int level, const std::vector<std::string>& extra_skills = {}, bool with_catalog = true, uint64_t seed = 7) {
        path = real_db_path();
        std::string error;
        db = content::ContentDb::open(path, error);
        if (!db) {
            problem = "content.db: " + error;
            return;
        }
        ctx = &world_stats::of(*db);
        if (with_catalog) {
            catalog = abilities::Catalog::load(path, *ctx->bundle, error);
            if (!catalog) {
                problem = "the ability tables: " + error;
                return;
            }
        }
        const auto r = db->ability_rules(error);
        if (!r) {
            problem = "ability rules: " + error;
            return;
        }
        rules = *r;
        auto talent_rows = db->talent_data(error);
        if (talent_rows) talent_catalog = talents::Catalog::from(std::move(*talent_rows), error);
        if (!talent_catalog) {
            problem = "the talents: " + error;
            return;
        }
        book.emplace(*talent_catalog, class_record_guid(class_enum), level);
        std::vector<stats::SkillRank> loose;   // skills no tree of the character has a rank for
        std::map<int64_t, std::vector<int64_t>> bought;
        for (const std::string& s : extra_skills) {
            const int64_t guid = record_guid(path, "SkillRecordBase", s.c_str());
            if (guid == 0) {
                problem = "no skill " + s;
                return;
            }
            // the first rank of the skill in the character's class tree, else the Universal tree: bought, as the character would have
            bool placed = false;
            for (const talents::Tree* tree : {book->class_tree(), talent_catalog->universal()}) {
                if (!tree || placed) continue;
                for (const talents::Node& node : tree->nodes) {
                    if (node.skill != guid || node.ranks.empty() || node.auto_granted) continue;
                    bought[tree->guid].push_back(node.ranks.front().guid);
                    placed = true;
                    break;
                }
            }
            // (the tree a skill belongs to: GetSkillPurchased's first reference; the formulas of the Bard's talents name the class tree)
            if (!placed) loose.push_back({{stats::kRefTypeSkillTree, class_tree_guid(class_enum)}, {stats::kRefTypeSkill, guid}});
        }
        for (const auto& [tree, ranks] : bought) book->load(tree, ranks);
        std::vector<stats::SkillRank> ranks = book->held_ranks();
        ranks.insert(ranks.end(), loose.begin(), loose.end());
        cs.emplace(*ctx->bundle, *ctx->gear, class_enum, level, ctx->rounding);
        base_ranks = ranks;
        cs->set_passives(book->passives(0));
        cs->set_ranks(ranks);
        if (!cs->start({}, error)) {
            problem = "the stats: " + error;
            return;
        }
        std::map<int64_t, double> stats_map;
        for (const stats::StatValue& v : cs->values()) stats_map[v.guid] = v.value;
        a7.emplace(rules, stats_map, kWorldTime);
        if (!a7->ok()) {
            problem = "A7: " + a7->error();
            return;
        }
        pipe.emplace(catalog ? &*catalog : nullptr, *cs, *a7, [] { return std::vector<stats::WornItem>(); }, kWorldTime, seed);
        pipe->set_data_timings(false);   // (a test of the runtime's mechanics runs on the default 300 ms; the retail tests turn the captures' times on)
        // the pawn as the join leaves it
        for (const stats::StatValue& v : cs->values()) {
            if (v.float_array() || (v.replication != 1 && v.replication != 5 && v.replication != 7)) continue;
            pawn.stats[v.guid] = v.value;
        }
        pipe->sync_a7_values(0);
        take();
    }
// built by C.O.A with Cyb

    bool ok() const { return problem.empty(); }

    /// The class record's guid (the Book's class).
    int64_t class_record_guid(int class_enum) const {
        sqlite3* d = nullptr;
        int64_t guid = 0;
        if (sqlite3_open_v2(path.c_str(), &d, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return 0;
        sqlite3_stmt* st = nullptr;
        static const char* kNames[] = {"", "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"};
        if (class_enum >= 1 && class_enum <= 8 && sqlite3_prepare_v2(d, "SELECT guid FROM class WHERE name = ?1", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, kNames[class_enum], -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) guid = sqlite3_column_int64(st, 0);
        }
        if (st) sqlite3_finalize(st);
        sqlite3_close(d);
        return guid;
    }

    int64_t class_tree_guid(int class_enum) const {
        // the class record's skill tree: the class table's row of this enum (the Bard's tree is what its talents' formulas name)
        sqlite3* d = nullptr;
        int64_t guid = 0;
        if (sqlite3_open_v2(path.c_str(), &d, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return 0;
        sqlite3_stmt* st = nullptr;
        static const char* kNames[] = {"", "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"};
        if (class_enum >= 1 && class_enum <= 8 && sqlite3_prepare_v2(d, "SELECT skill_tree_guid FROM class WHERE name = ?1", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, kNames[class_enum], -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) guid = sqlite3_column_int64(st, 0);
        }
        if (st) sqlite3_finalize(st);
        sqlite3_close(d);
        return guid;
    }

    int64_t ability(const char* name) const {
        const abilities::AbilityDef* d = catalog ? catalog->ability_named(name) : nullptr;
        return d ? d->row.guid : 0;
    }
    int64_t effect(const char* name) const {
        const abilities::EffectDef* d = catalog ? catalog->effect_named(name) : nullptr;
        return d ? d->row.guid : 0;
    }

    /// The character also holds these ranks (talents a test infers from a capture): the base ranks and these; the effects the runtime holds are re-run.
    void use_extra_ranks(const std::vector<stats::SkillRank>& extra) {
        std::vector<stats::SkillRank> all = base_ranks;
        all.insert(all.end(), extra.begin(), extra.end());
        cs->set_ranks(std::move(all));
        pipe->refresh(clock, "ranks", true);
        take();
    }

    /// The character knows these abilities (and nothing else).
    void know(const std::vector<const char*>& names) {
        std::vector<int64_t> g;
        for (const char* n : names) g.push_back(ability(n));
        pipe->set_known(g);
    }

    /// Takes the events of both runtimes, applies them to the pawn (the test fails the moment the pawn would refuse one) and returns them.
    std::vector<Seen> take() {
        std::vector<AbilityEvent> events = a7->take_events();
        for (AbilityEvent& e : pipe->take_events()) events.push_back(std::move(e));
        std::vector<Seen> out;
        for (AbilityEvent& e : events) {
            Seen s;
            s.at = e.at;
            s.change = e.change;
            s.end = e.end_ability;
            s.note = e.note;
            s.refusal = e.refusal;
            s.confirmations = e.confirmations;
            s.zones = e.zones;
            for (const auto& h : e.confirmations) pawn.confirmations.push_back(h);
            pawn.apply(e.change);
            if (e.end_ability) pawn.ends.push_back(*e.end_ability);
            out.push_back(s);
            log.push_back(s);
        }
        return out;
    }

    /// Advances both runtimes to `t` (seconds) and returns what happened.
    std::vector<Seen> run_to(double t) {
        clock = t;
        a7->tick(t);
        pipe->tick(t);
        return take();
    }

    bool asks = true;   // the client asks for every hit of the cast it can (RequestHitVerification_Server, 20 ms after the trigger), as the retail client does

    /// ServerTriggerAbility at `t`: the events up to then and the trigger's own. `target`: the call names a target (the caster).
    Refusal trigger(const char* name, double t, bool target = false, int32_t anim = 0, std::vector<Seen>* seen = nullptr) {
        clock = t;
        a7->tick(t);
        TriggerInfo info;
        info.animation_index = anim;
        info.target_count = target ? 1 : 0;
        const int64_t guid = ability(name);
        const Refusal r = pipe->trigger(guid, next_client_id++, t, info);
        if (r == Refusal::None && asks && catalog) {
            if (const abilities::AbilityDef* def = catalog->ability(guid)) {
                std::set<int64_t> asked;
                for (const auto& h : def->hits) {
                    const abilities::HitDef* hd = catalog->hit(h.hit_guid);
                    if (h.source != "hit" || !hd || hd->row.net_type.find("ServerVerify") == std::string::npos || !asked.insert(h.hit_guid).second) continue;
                    int64_t targeting = content::kTargetingDefaultSelf;
                    for (const auto& tg : def->targetings)
                        if (tg.key == static_cast<int>(h.key)) targeting = tg.guid;
                    // (a hit's own requirement is the client's to judge: the ones that do not hold are asked for all the same and run to nothing)
                    pipe->hit_request(guid, targeting, h.hit_guid, 1, 1, t + 0.02 + 0.0001 * static_cast<double>(asked.size()));
                }
            }
        }
        std::vector<Seen> s = take();
        if (seen) *seen = std::move(s);
        return r;
    }
// (C Y B ~ CoA)

    /// RequestHitVerification_Server at `t` for a hit of an ability: the targeting is the one the ability's data gives the hit's key. `own`: the client
    /// names the caster's own component. Returns the verdict; the events up to then are taken (and `seen` gets them).
    AbilityPipeline::HitVerdict hit_request(const char* ability_name, const char* hit_name, double t, bool own = true, std::vector<Seen>* seen = nullptr) {
        clock = t;
        a7->tick(t);
        const abilities::AbilityDef* def = catalog->ability_named(ability_name);
        int64_t targeting = content::kTargetingDefaultSelf, hit_guid = 0;
        for (const auto& h : def->hits) {
            const abilities::HitDef* hd = catalog->hit(h.hit_guid);
            if (hd && hd->row.name == hit_name) {
                hit_guid = h.hit_guid;
                if (h.source == "hit") {
                    for (const auto& tg : def->targetings)
                        if (tg.key == static_cast<int>(h.key)) targeting = tg.guid;
                }
            }
        }
        const auto v = pipe->hit_request(def->row.guid, targeting, hit_guid, own ? 1 : 0, own ? 1 : 0, t);
        std::vector<Seen> s = take();
        if (seen) *seen = std::move(s);
        return v;
    }

    /// A held stat (Health, Mana) is `value` now, on the client and in the engine.
    void set_current(int64_t stat, double value) {
        cs->set_current(stat, static_cast<float>(value));
        pawn.stats[stat] = static_cast<float>(value);
    }

    double mana() const { return pawn.stat(content::kGuidMana); }
    double health() const { return pawn.stat(content::kGuidHealth); }
    double max_health() const { return pawn.stat(content::kGuidMaxHealth); }
};

}  // namespace ability_rig

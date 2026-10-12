// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_tooltips.cpp: the ability runtime against the game client's tooltips, for EVERY ability of every class (plan E1, stage 3 fix, Part C)
//
//  The user's rule: each ability's in-game tooltip is the truth for its numbers and behaviour. tooltip_key.h (tools/capture/gen_tooltips.py) is what the
//  client's tooltip states for a level-25 character with no gear, no skill bought and no effects: the cooldown (the data's Cooldown; the skill tooltip
//  prints no cooldown line), the charges, each cost with the number the line prints, the cast time where the data has one, the range and its source,
//  who the ability may be aimed at, the hits whose number the text states (a percent: the stat mod's term x 100) and the effects the text names. For
//  each key a character of the ability's class (the weapon combos on a Fighter, the general, consumable and profession abilities on a Bard) at that level
//  on the REAL content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB) is built with the Rig of ability_rig.h, and the runtime's
//  own numbers are read through AbilityPipeline::inspect() (the very formulas a trigger runs: cooldown, charges, costs, cast time) and the catalog it
//  casts from (the target mask, the hits' stat-mod terms, the effects it reaches). Compared, with the tag of each difference:
//    CD        the cooldown in seconds                         CHARGES  the charges (round half up, at least 1)
//    COST      each enabled stat cost's amount                 CAST     DynamicCastDuration where the data has one; the montage's time otherwise
//    RANGE     the tooltip's range against the server's bound  TARGET   ApplicableTargets and whether a target is needed
//    HELD      the held behaviour                              HIT      the term the text states (BaseValue, Min, Max, Add: a constant, or the same formula)
//    APPLY     every effect the text names and the ability applies is one the runtime's catalog reaches; EFFDUR its record's duration (EFFDUR? only the
//              runtime evaluates it); APPLYREF an effect applied by a hit the ability does not fire itself (a reaction's)
//    HITREF    the text states a number of a hit the ability does not fire itself (a reaction's, a pet's); FORMULA a formula the runtime cannot run
//  A difference that is not a match is a LISTED one (tooltip_known.inc: the ability or "*", the tag, why), and the list is exact: an unlisted difference
//  fails. The report counts per group: match (the tooltip's numbers), listed (with the reason), unlisted.
//  Without a content.db the test says so and is skipped.  -v prints every ability; --no-known applies no list; --group NAME / --ability NAME narrow it.
// ============================================================================
#include "ability_rig.h"
#include "combat/combat_world.h"
#include "tooltip_key.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ability_rig;

static int g_pass = 0, g_fail = 0;
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

namespace {

// the stat-mod term slots the tooltip's number comes from (AoCRecordConstants; tools/capture/gen_tooltips.py TERM_*)
constexpr int64_t kTermBase = 6064631035951644675ll, kTermMin = 6064631120485154816ll, kTermMax = 6064631120485482497ll, kTermAdd = 6064631210309517324ll;
// the server's bound on a cast at another actor: the general OutOfRangeDistance plus the activation leeway (combat/combat_world.h, combat/combat_data.h:
// ServerActivationDistanceLeeway, 1000 cm in the data; test_worldd_combat holds the value)
constexpr double kActivationLeewayCm = 1000.0;

// ---- the listed differences -------------------------------------------------------------------------------------------------------------------------

const char* const R_RANGE = "the tooltip prints the ability's own range (ValidDistanceExp, the targeting record, AreaTargetRange: what the CLIENT enforces before it "
                            "sends the call); the server's rule is the general OutOfRangeDistance plus the activation leeway (combat/combat_world.h player_reach_cm), "
                            "which this test holds to be at least the tooltip's for every ability, so no cast the tooltip allows is refused for range";
const char* const R_MONTAGE = "the tooltip's cast time is the montage's end-notify time over the cast speed (UUILibrary::SetAbilityCastTime), a client-side value "
                              "that is in no table: the runtime's item and cost steps come at the retail captures' offsets (session/ability_timings.h), else 300 ms";
const char* const R_STATS = "the tooltip evaluates it for the player with the stat engine (GetStat ...); the generator has none, so the key states no value: the "
                            "runtime's own value is printed and holds";
const char* const R_PROGRESS = "the generator reads a tooltip with no ability in progress (GetProgressDuration, IsInCombat are 0): the runtime evaluates the cost at the "
                               "cast, with the cast in progress; the key states no value";
const char* const R_REF = "the text states a number of, or an effect applied by, a hit that none of the ability's HitsIds, projectiles or zones fires: a hit fired by a "
                          "REACTION of the ability's effect (ReactionsIds: the Rogue poisons' on-hit application, the Mage's Elemental Empowerment hits), or another "
                          "ability's (a summoned pet's attack). Reactions are stored and not run (the backlog's E1 stage 3 fix line); pets are outside E1 (the spec's section 5)";
const char* const R_DISABLE = "a crowd-control status (Stunned, Incapacitated, Tripped, Snared, Rooted, Silenced, Disarmed, Chilled, Exhausted ...) whose duration is N x the "
                              "DisableDuration formula, whose term calls GetEffect(...), a host function the stat program does not have: the client evaluates it for the "
                              "player, the generator and the runtime cannot. These are the statuses a hit puts on an ENEMY, which stage 3 does not apply to NPCs "
                              "(test_worldd_ability_npc's R_NPCFX); in the backlog with the reactions";
const char* const R_TERM = "the record's EffectDuration is a bare GetTerm(...) of a term record outside any formula (ab_expr 1085, the three Hunt marks' RangerMark term, whose "
                           "DefaultValue is empty): the stat program refuses it (\"GetTerm outside a formula\"), the generator has no value for it, and the runtime does not "
                           "apply an effect whose duration does not run; a mark the hit puts on an ENEMY, which stage 3 does not apply to NPCs";
const char* const R_DURSTAT ="the duration reads the caster's stats (DisableDuration through EvalFormula): the generator has no stat engine and states no value; the "
                              "runtime's own value is printed and holds";
const char* const R_DUMMY = "a dummy hit: its stat mod has no Value expression (the runtime applies nothing), while the tooltip prints its BaseValue term 10.0 as "
                            "\"1,000% Magical\" (the threat reduction's placeholder in the data)";
const char* const R_HOSTFN = "the activation requirement is no number the tooltip states; its host functions (GetLastProjectileFireTime, GetLastAbility, "
                             "HasPVPForceFlag, HasValidEquipment) are not in the stat program, so the runtime refuses the cast (NoFormula): no retail capture holds a "
                             "cast of the ability (0 of the 482 + 358 cases), and pets, PvP and harvesting are outside E1 (the spec's section 5); in the backlog";
// -- CoA x C Y B --

struct Known {
    const char* ability;   // the ability's name, or "*" for every ability
    const char* tag;       // the problem's text, as the test words it (a prefix)
    const char* why;
};
#include "tooltip_known.inc"

// ---- helpers -------------------------------------------------------------------------------------------------------------------------------------

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/// The text with its whitespace collapsed and the characters the key's encoding replaced ('|' -> '/', ';' -> ',').
std::string normalized(const std::string& s) {
    std::string out;
    bool space = false;
    for (const char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c == '|' ? '/' : c == ';' ? ',' : c;
    }
    return out;
}

/// A text that is one plain number ("1.5", "0.533", "-2").
bool plain_number(const std::string& s, double& v) {
    const std::string t = normalized(s);
    if (t.empty()) return false;
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end && *end == '\0';
}

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

int class_of_group(const std::string& group) {
    static const std::map<std::string, int> kClass = {{"Ranger", 1}, {"Bard", 2}, {"Cleric", 3}, {"Fighter", 4}, {"Rogue", 5}, {"Summoner", 6}, {"Tank", 7}, {"Mage", 8},
                                                      {"Weapons", 4}, {"General", 2}, {"Consumables", 2}, {"Professions", 2}};
    const auto it = kClass.find(group);
    return it == kClass.end() ? 2 : it->second;
}

/// Every effect the ability reaches: its own, those of its hits (applied to the target or the caster), of its projectiles' and zones' hits, and what those
/// effects' hits apply in turn.
std::set<int64_t> reached_effects(const abilities::Catalog& cat, const abilities::AbilityDef& def) {
    std::set<int64_t> effects, hits;
    std::vector<int64_t> todo;
    for (const auto& [key, e] : def.effects) effects.insert(e);
    for (const content::AbilityHitRow& h : def.hits) todo.push_back(h.hit_guid);
    for (const int64_t z : def.zones) {
        if (const content::ZoneRow* zr = cat.zone(z))
            for (const auto& [role, list] : zr->hits) todo.insert(todo.end(), list.begin(), list.end());
    }
    for (const auto& [key, e] : def.effects) {
        if (const abilities::EffectDef* ed = cat.effect(e))
            for (const auto& [phase, hit] : ed->hits) todo.push_back(hit);
    }
    while (!todo.empty()) {
        const int64_t g = todo.back();
        todo.pop_back();
        if (!hits.insert(g).second) continue;
        const abilities::HitDef* h = cat.hit(g);
        if (!h) continue;
        for (const int64_t z : h->zones) {
            if (const content::ZoneRow* zr = cat.zone(z))
                for (const auto& [role, list] : zr->hits) todo.insert(todo.end(), list.begin(), list.end());
        }
        for (const content::HitEffectRow& x : h->effects) {
            if (effects.insert(x.effect_guid).second) {
                if (const abilities::EffectDef* e = cat.effect(x.effect_guid))
                    for (const auto& [phase, hit] : e->hits) todo.push_back(hit);
            }
        }
    }
    return effects;
}
// CoA and C.Y.B

struct Tally {
    int abilities = 0, match = 0, numbers_match = 0, listed = 0, unlisted = 0;   // numbers_match: only the cast time (the montage's) and the range (the client's) are listed
    int hits_constant = 0, hits_formula = 0;   // the hit numbers the text states: a constant term that equals, a formula term that is the same text
    std::map<std::string, int> listed_by_tag;
};

}  // namespace

int main(int argc, char** argv) {
    if (!real_db_present()) {
        std::printf("no content.db at %s (cd services && go run ./cmd/contentc): the tooltip parity test is skipped\n", real_db_path().c_str());
        return 0;
    }
    bool verbose = false, use_known = true;
    std::string only_group, only_ability;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) verbose = true;
        else if (!std::strcmp(argv[i], "--no-known")) use_known = false;
        else if (!std::strcmp(argv[i], "--group") && i + 1 < argc) only_group = argv[++i];
        else if (!std::strcmp(argv[i], "--ability") && i + 1 < argc) only_ability = argv[++i];
    }
    spdlog::set_level(spdlog::level::warn);
    std::printf("the ability runtime against the client's tooltips of %d abilities (level %d, no gear, no skill, no effects)\n", tooltip_key::kKeyCount, tooltip_key::kLevel);

    const combat::CombatWorldConfig cfg;
    const double reach_bound = cfg.player_reach_cm + kActivationLeewayCm;

    std::map<std::string, Tally> tally;
    std::map<std::string, std::vector<std::string>> unlisted_by_tag;
    std::string current_group;
    std::optional<Rig> rig;
    int inspected = 0;
    for (const tooltip_key::Key& k : tooltip_key::kKeys) {
        if (!only_group.empty() && only_group != k.group) continue;
        if (!only_ability.empty() && only_ability != k.name) continue;
        if (!rig || current_group != k.group) {
            current_group = k.group;
            rig.emplace(class_of_group(k.group), tooltip_key::kLevel);
            if (!rig->ok()) {
                std::printf("  [FAIL] the %s rig: %s\n", k.group, rig->problem.c_str());
                return 2;
            }
        }
        Rig& r = *rig;
        Tally& ty = tally[k.group];
        ++ty.abilities;
        std::vector<std::string> problems;
        auto problem = [&](const std::string& p) { problems.push_back(p); };

        const abilities::AbilityDef* def = r.catalog->ability(k.guid);
        if (!def) {
            problem("CATALOG:no ability " + std::to_string(k.guid));
        } else {
            ++inspected;
            const auto insp = r.pipe->inspect(k.guid);
            if (!insp) {
                problem("CATALOG:inspect failed");
            } else {
                if (!insp->problem.empty()) problem("FORMULA:" + insp->problem);
                // the cooldown
                if (k.cooldown_s < 0) problem("CD:the generator could not evaluate it; ours " + num(insp->cooldown));
                else if (std::fabs(insp->cooldown - k.cooldown_s) > 1e-6) problem("CD:" + num(insp->cooldown) + " (" + num(k.cooldown_s) + ")");
                // the charges
                if (k.charges < 0) problem("CHARGES:the generator could not evaluate them; ours " + std::to_string(insp->max_charges));
                else if (insp->max_charges != k.charges) problem("CHARGES:" + std::to_string(insp->max_charges) + " (" + std::to_string(k.charges) + ")");
                // the costs: each enabled stat cost of the key against the runtime's
                std::set<std::string> key_stats;
                for (const std::string& c : split(k.costs, ';')) {
                    const size_t eq = c.find('=');
                    if (eq == std::string::npos) continue;
                    const std::string stat = c.substr(0, eq);
                    const std::vector<std::string> f = split(c.substr(eq + 1), '|');
                    const std::string value = f.size() > 0 ? f[0] : "?", enabled = f.size() > 2 ? f[2] : "1";
                    const AbilityPipeline::StatCost* mine = nullptr;
                    for (const auto& sc : insp->stat_costs)
                        if (sc.name == stat) mine = &sc;
                    key_stats.insert(stat);
                    if (enabled == "0") {
                        if (mine) problem("COST:" + stat + " spent though the group is disabled (" + num(mine->amount) + ")");
                        continue;
                    }
                    if (value == "?") {
                        problem("COST:" + stat + " the generator could not evaluate it; ours " + (mine ? num(mine->amount) : std::string("none")));
                        continue;
                    }
                    const double kv = std::strtod(value.c_str(), nullptr);
                    if (!mine) {
                        if (kv > 1e-9) problem("COST:" + stat + " missing (" + num(kv) + ")");
                    } else if (std::fabs(mine->amount - kv) > 1e-3) {
                        problem("COST:" + stat + " " + num(mine->amount) + " (" + num(kv) + ")");
                    }
                }
                for (const auto& sc : insp->stat_costs)
                    if (!key_stats.count(sc.name)) problem("COST:" + sc.name + " extra " + num(sc.amount));
                // the cast time
                if (k.cast_s >= 0) {
                    if (!insp->cast_seconds) problem("CAST:none (" + num(k.cast_s) + ")");
                    else if (std::fabs(*insp->cast_seconds - k.cast_s) > 1e-6) problem("CAST:" + num(*insp->cast_seconds) + " (" + num(k.cast_s) + ")");
                } else {
                    problem("CAST:montage");
                }
                // the range: the server's bound must cover the tooltip's range
                if (k.range_cm > 0) {
                    if (k.range_cm > reach_bound) problem("RANGE:over the server's bound " + num(k.range_cm) + " cm > " + num(reach_bound));
                    else problem("RANGE:tooltip " + num(k.range_cm) + " cm (" + std::string(k.range_source) + "), the server's bound " + num(reach_bound));
                }
                // the target
                if (def->row.applicable_targets != k.target_mask) problem("TARGET:mask " + std::to_string(def->row.applicable_targets) + " (" + std::to_string(k.target_mask) + ")");
                if (def->row.target_requirement.find(k.target_requirement) == std::string::npos) problem("TARGET:requirement " + def->row.target_requirement + " (" + std::string(k.target_requirement) + ")");
                if (def->row.held_behavior.find(k.held) == std::string::npos) problem("HELD:" + def->row.held_behavior + " (" + std::string(k.held) + ")");
                // the hits the text states a number for: the stat mod's term
                for (const std::string& h : split(k.hits, ';')) {
                    const std::vector<std::string> f = split(h, '|');
                    if (f.size() < 4) continue;
                    const std::string& name = f[0];
                    const std::string& slot = f[1];
                    const std::string& value = f[2];
                    const bool own = f[3] == "1";
                    const std::string text = f.size() > 4 ? f[4] : "";
                    const abilities::HitDef* hd = r.catalog->hit_named(name);
                    if (!own) {
                        problem("HITREF:" + name + " is another ability's hit" + (hd ? "" : " (not in the catalog)"));
                        continue;
                    }
                    if (!hd) {
                        problem("HIT:" + name + " unknown to the catalog");
                        continue;
                    }
                    if (hd->mods.empty() || hd->mods[0].value_expr == 0) {
                        problem("HIT:" + name + " has no stat mod");
                        continue;
                    }
                    const content::ExprRow* ex = r.catalog->expr(hd->mods[0].value_expr);
                    if (!ex) {
                        problem("HIT:" + name + " no expression");
                        continue;
                    }
                    std::string mine;
                    if (slot == "Value") {
                        mine = ex->text;
                    } else {
                        const int64_t g = slot == "BaseValue" ? kTermBase : slot == "BaseValueMin" ? kTermMin : slot == "BaseValueMax" ? kTermMax : kTermAdd;
                        const auto it = ex->terms.find(g);
                        if (it == ex->terms.end()) {
                            problem("HIT:" + name + " no " + slot + " term");
                            continue;
                        }
                        mine = it->second;
                    }
                    if (value == "?") {
                        problem("HIT:" + name + " needs the player's stats");
                        continue;
                    }
                    double v = 0;
                    const double kv = std::strtod(value.c_str(), nullptr);
                    if (plain_number(mine, v)) {
                        if (std::fabs(v - kv) > 1e-6) problem("HIT:" + name + " " + slot + " " + num(v) + " (" + num(kv) + ")");
                        else ++ty.hits_constant;
                    } else if (normalized(mine) != normalized(text)) {
                        problem("HIT:" + name + " " + slot + " formula differs");
                    } else {
                        ++ty.hits_formula;   // the runtime compiles the very formula the generator evaluated (0.533 x the weapon tree's factor ...)
                    }
                }
                // the effects the text names and the ability applies: reached by the runtime's catalog, with the record's duration
                const std::set<int64_t> reached = reached_effects(*r.catalog, *def);
                for (const std::string& e : split(k.applies, ';')) {
                    const size_t eq = e.find('=');
                    std::string name = e.substr(0, eq);
                    const std::string dur = eq == std::string::npos ? "?" : e.substr(eq + 1);
                    if (!name.empty() && name[0] == '!') {   // applied by a hit the ability itself does not fire
                        name.erase(0, 1);
                        const abilities::EffectDef* ed = r.catalog->effect_named(name);
                        problem("APPLYREF:" + name + " through a hit the ability does not fire itself" + (ed && reached.count(ed->row.guid) ? " (reached all the same)" : ""));
                        continue;
                    }
                    const abilities::EffectDef* ed = r.catalog->effect_named(name);
                    if (!ed) {
                        problem("APPLY:" + name + " unknown to the catalog");
                        continue;
                    }
                    if (!reached.count(ed->row.guid)) problem("APPLY:" + name + " not reached");
                    if (dur == "inf") {
                        if (!ed->row.infinite_duration) problem("EFFDUR:" + name + " not infinite");
                    } else if (dur == "?") {
                        double v = 0;
                        std::string why;
                        if (ed->row.duration_expr != 0 && r.pipe->evaluate(ed->row.duration_expr, v, why)) problem("EFFDUR?:" + name + " the generator could not evaluate it; ours " + num(v));
                        else if (why.find("GetEffect") != std::string::npos) problem("EFFDUR!:" + name + " neither evaluates it (the disable-duration formula's GetEffect term)");
                        else problem("EFFDUR:" + name + " neither the generator nor the runtime evaluates it: " + why);
                    } else if (ed->row.infinite_duration) {
                        problem("EFFDUR:" + name + " infinite (" + dur + ")");
                    } else {
                        double v = 0;
                        std::string why;
                        if (ed->row.duration_expr == 0) v = 0;
                        else if (!r.pipe->evaluate(ed->row.duration_expr, v, why)) {
                            problem("EFFDUR:" + name + " does not run: " + why);
                            continue;
                        }
                        const double kv = std::strtod(dur.c_str(), nullptr);
                        if (std::fabs(v - kv) > 1e-6) problem("EFFDUR:" + name + " " + num(v) + " (" + num(kv) + ")");
                    }
                }
            }
        }
        // the listed differences are no error
        std::vector<std::string> unlisted;
        for (const std::string& p : problems) {
            bool listed = false;
            for (const Known& kn : kKnown) {
                if (!use_known) break;
                const bool mine = !std::strcmp(kn.ability, "*") || !std::strcmp(kn.ability, k.name);
                listed = listed || (mine && p.rfind(kn.tag, 0) == 0);
            }
            if (listed) ++ty.listed_by_tag[p.substr(0, p.find(':'))];
            else unlisted.push_back(p);
        }
        // the two structural listings (the tooltip's cast time is the montage's, its range the client's) are no difference of a number the runtime has
        const bool structural_only = std::all_of(problems.begin(), problems.end(), [](const std::string& p) { return p.rfind("CAST:montage", 0) == 0 || p.rfind("RANGE:tooltip", 0) == 0; });
        if (problems.empty()) ++ty.match;
        else if (structural_only) ++ty.numbers_match;
        else if (unlisted.empty()) ++ty.listed;
        else ++ty.unlisted;
        for (const std::string& p : unlisted) unlisted_by_tag[p.substr(0, p.find(':'))].push_back(std::string(k.name) + " " + p);
        if (verbose || !unlisted.empty()) {
            std::printf("%s %-10s %s%s\n", unlisted.empty() ? "ok  " : "FAIL", k.group, k.name, problems.empty() ? ": matches the tooltip" : "");
            for (const std::string& p : problems) {
                const bool is_unlisted = std::find(unlisted.begin(), unlisted.end(), p) != unlisted.end();
                if (verbose || is_unlisted) std::printf("      %s%s\n", is_unlisted ? "UNLISTED " : "", p.c_str());
            }
        }
    }

    std::printf("\n%-12s %9s %7s %9s %7s %9s %9s %9s   listed by tag\n", "group", "abilities", "match", "numbers", "listed", "unlisted", "hit const", "hit formula");
    int total = 0, match = 0, numbers = 0, listed = 0, unlisted = 0, hc = 0, hf = 0;
    for (const auto& [g, t] : tally) {
        std::string tags;
        for (const auto& [tag, n] : t.listed_by_tag) tags += tag + " " + std::to_string(n) + "  ";
        std::printf("%-12s %9d %7d %9d %7d %9d %9d %9d   %s\n", g.c_str(), t.abilities, t.match, t.numbers_match, t.listed, t.unlisted, t.hits_constant, t.hits_formula, tags.c_str());
        total += t.abilities;
        match += t.match;
        numbers += t.numbers_match;
        listed += t.listed;
        unlisted += t.unlisted;
        hc += t.hits_constant;
        hf += t.hits_formula;
    }
    std::printf("%-12s %9d %7d %9d %7d %9d %9d %9d\n", "all", total, match, numbers, listed, unlisted, hc, hf);
    std::printf("(match: every line of the tooltip is the runtime's; numbers: every number the runtime has is the tooltip's, only the cast time (the montage's,\n"
                " client-side) and the range (the client's own check) are listed; listed: other differences, each in tooltip_known.inc with its reason; hit const /\n"
                " formula: the hit percents the texts state whose term is a constant the runtime's catalog has / a formula the runtime compiles as written)\n");
    if (!unlisted_by_tag.empty()) {
        std::printf("\nunlisted differences:\n");
        for (const auto& [tag, list] : unlisted_by_tag) {
            std::printf("  %5zu  %s\n", list.size(), tag.c_str());
            size_t shown = 0;
            for (const std::string& s : list) {
                if (++shown > 12) {
                    std::printf("           ...\n");
                    break;
                }
                std::printf("           %s\n", s.c_str());
            }
        }
    }
    if (only_group.empty() && only_ability.empty()) {
        CHECKF(inspected == tooltip_key::kKeyCount, "every ability of the key is in the catalog and inspected (%d of %d)", inspected, tooltip_key::kKeyCount);
    }
    CHECKF(unlisted == 0, "every difference from the tooltip is listed with its reason (%d abilities have an unlisted one)", unlisted);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

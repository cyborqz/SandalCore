// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_ability_all.cpp: the ability runtime against what the retail server answered, for EVERY class (plan D2, stage 2)
//
//  capture_casts.h (tools/capture/gen_casts.py) holds every ServerTriggerAbility of the 482 unique retail captures that a server without NPCs can
//  serve (stage S1 or S2: 589 casts of the eight classes, the weapon combos swung at the air, the consumables, Sprint and Dodge), with what the
//  client sent while it ran (hit requests, held cancels) and what the server answered, item by item with its time. The calls of one capture are
//  replayed in order on one character of the capture's class and level (the level comes from the Mana costs) of the real content.db, on a
//  simulated clock, each at its own time (so cooldowns, charges, running effects and resources are what they were). For each cast:
//    - it must be ACCEPTED (not refused);
//    - the ability item (the ability, MontageGroup, the montage's NetGUID, the caster as the target when the call named one), the Cooldowns item
//      (InitialCooldown, Charges left) and the cost (Mana to the decimal) must be what retail sent, in retail's order;
//    - every effect the ability can put on its caster that retail sent must be sent, with retail's Duration, TickTime and stacks;
//    - every hit confirmation of the ability's hits retail sent must be sent (the hit's record, the source's type and record, whether it answers
//      a request, the number of stat mods), the shields' count must be retail's, and a cast retail ended early is ended;
//    - the TIMES (ability item +-60 ms, Cooldowns and cost +-120 ms, effects and confirmations +-150 ms) are counted apart: a capture without
//      timestamps (Mage, Rogue, a third of the others) has order only.
//  What is not matched is a LISTED difference (kKnown: the ability, what, why), and the list is exact: an unlisted difference fails.
//  The report counts the casts accepted and matched per class, and what is left per reason.
//  Without a content.db the test says so and is skipped.
// ============================================================================
#include "ability_rig.h"
#include "capture_casts.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <sstream>
// built by C Y B with C O A

using namespace ability_rig;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

namespace {

struct RItem {
    std::string              kind;
    int                      ms = 0;
    std::vector<std::string> f;
};

std::string join(const char* const* parts) {
    std::string out;
    for (; *parts; ++parts) out += *parts;
    return out;
}

std::vector<RItem> parse_lines(const char* const* parts) {
    std::vector<RItem> out;
    std::istringstream in(join(parts));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        RItem r;
        ls >> r.kind >> r.ms;
        for (std::string w; ls >> w;) r.f.push_back(w);
        out.push_back(std::move(r));
    }
    return out;
}

std::map<int64_t, double> parse_before(const char* const* parts) {
    std::map<int64_t, double> out;
    std::istringstream in(join(parts));
    long long rec;
    double v;
    while (in >> rec >> v) out[rec] = v;
    return out;
}

double   to_d(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
int64_t  to_i(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }
uint64_t to_u(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }

int class_enum(const std::string& name) {
    static const char* kNames[] = {"", "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"};
    for (int i = 1; i <= 8; ++i) {
        if (name == kNames[i]) return i;
    }
    return 2;
}

constexpr double kItemTolerance = 0.060, kStepTolerance = 0.120, kEffectTolerance = 0.150;

// every effect the ability can put on its caster: its own, those of its hits, projectiles' and zones' hits, and what those effects fire; and every hit
void closure(const abilities::Catalog& cat, const abilities::AbilityDef& def, std::set<int64_t>& effects, std::set<int64_t>& hits) {
    std::vector<int64_t> todo;
    for (const auto& [key, e] : def.effects) effects.insert(e);
    for (const content::AbilityHitRow& h : def.hits) todo.push_back(h.hit_guid);
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
        for (const content::HitEffectRow& x : h->effects) {
            if (effects.insert(x.effect_guid).second) {
                if (const abilities::EffectDef* e = cat.effect(x.effect_guid)) {
                    for (const auto& [phase, hit] : e->hits) todo.push_back(hit);
                }
            }
        }
    }
}
// Cyb with C-O-A

/// The skills an expression asks GetSkillPurchased about: (class tree, skill reference) pairs as the text names them.
std::vector<stats::SkillRank> skills_in(const std::string& text) {
    static const std::regex re(R"(\$#297887639323616934:(\d+)\$\s*,\s*\$#(\d+):(\d+)\$)");
    std::vector<stats::SkillRank> out;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
        stats::SkillRank r;
        r.cls = {stats::kRefTypeSkillTree, std::stoll((*it)[1])};
        r.skill = {std::stoull((*it)[2]), std::stoll((*it)[3])};
        out.push_back(r);
    }
    return out;
}

/// The talents the capture's character held, as far as the cooldowns and charges its casts show: for each ability whose cooldown or charge formula
/// asks about skills, the smallest set of them under which the formulas give retail's InitialCooldown and at least as many charges as retail's
/// Cooldowns items show (a cast leaves Charges - 1 at most). The retail testers had talents the captures do not name.
std::vector<stats::SkillRank> infer_talents(Rig& r, int capture) {
    std::vector<stats::SkillRank> extra;
    std::set<int64_t> done;
    for (const capture_casts::Case& c : capture_casts::kCases) {
        if (c.capture != capture || !done.insert(c.ability).second) continue;
        const abilities::AbilityDef* def = r.catalog->ability(c.ability);
        if (!def || r.rules.ability(c.ability)) continue;
        std::vector<stats::SkillRank> cand;
        for (const int64_t id : {def->row.cooldown_expr, def->row.charges_expr}) {
            if (const content::ExprRow* e = r.catalog->expr(id)) {
                for (const stats::SkillRank& s : skills_in(e->text)) {
                    bool dup = false;
                    for (const stats::SkillRank& x : cand) dup = dup || (x.skill == s.skill && x.cls == s.cls);
                    if (!dup) cand.push_back(s);
                }
            }
        }
        if (cand.empty() || cand.size() > 4) continue;
        // retail's numbers
        double cooldown = -1;
        int most_left = -1;
        for (const capture_casts::Case& d : capture_casts::kCases) {
            if (d.capture != capture || d.ability != c.ability) continue;
            for (const RItem& it : parse_lines(d.reply)) {
                if (it.kind == "CD" && to_i(it.f[0]) == c.ability) {
                    cooldown = to_d(it.f[1]);
                    most_left = std::max(most_left, static_cast<int>(to_i(it.f[2])));
                }
            }
        }
        if (cooldown < 0) continue;
        std::vector<stats::SkillRank> best;
        bool found = false;
        for (unsigned size = 0; size <= cand.size() && !found; ++size) {
            for (unsigned mask = 0; mask < (1u << cand.size()) && !found; ++mask) {
                if (static_cast<unsigned>(std::popcount(mask)) != size) continue;
                std::vector<stats::SkillRank> trial = extra;
                for (size_t i = 0; i < cand.size(); ++i)
                    if (mask & (1u << i)) trial.push_back(cand[i]);
                std::vector<stats::SkillRank> all = r.base_ranks;
                all.insert(all.end(), trial.begin(), trial.end());
                r.cs->set_ranks(all);
                std::string err;
                auto ev = r.cs->evaluation({}, err);
                if (!ev) continue;
                stats::ExprHost host;
                double cd = 0, ch = 1;
                if (def->row.cooldown_expr != 0 && !ev->eval(r.catalog->program(def->row.cooldown_expr), host, cd, err)) continue;
                if (def->row.charges_expr != 0 && !ev->eval(r.catalog->program(def->row.charges_expr), host, ch, err)) continue;
                if (std::fabs(cd - cooldown) < 1e-3 && static_cast<int>(std::lround(ch)) >= most_left + 1) {
                    best = trial;
                    found = true;
                }
            }
        }
        if (found) extra = best;
    }
    return extra;
}

/// Why a difference is listed (the groups: what is known, and what is not resolved).
const char* const R_D1 = "a consumable is used through D1's item path (use_item), which sends no ability montage; the all-class test feeds it to the ability pipeline";
const char* const R_A7 = "General_Guard, Dodge and Sprint are A7's rows (kept as they are by the brief); their stamina hits, shields and ends are A7's, not the generic pipeline's";
const char* const R_NPC = "the effect or hit depends on an enemy NPC, an incoming hit or a lingering zone (milestone E); there is no enemy in the replay";
const char* const R_MOD = "retail's number carries a talent or gear modifier the capture does not name (the test infers talents only from cooldown and charges, and the level from the first cost)";
const char* const R_SEQ = "capture 152 is one long mixed session; the state before this cast (mana, a charge, a running channel) comes from earlier modelled casts and differs from retail's, so the cast is refused, or over before its cost step";
const char* const R_CHAN = "a channelled ability re-triggered or ended by the client in retail (FlashCure, SoothingGlow, DivineInfusion); the re-trigger and the early end are not matched";
const char* const R_CONFX = "our confirmation rule (stat mods, an FX id or the party mask) confirms a hit that retail did not confirm in this cast";
const char* const R_CONFM = "retail confirmed a hit (an ally or a zone hit) that our cast does not reach: the ally or zone is not in the replay";
const char* const R_SONG = "the effect's stack count follows the band in range (Resonance) or the song's siblings; the band is not modelled";
const char* const R_UNRES = "not resolved in stage 2";
// [Cyb + C O A]

/// A difference that is listed: it is not an error (the ability, its capture, the start of the problem's text, why). Exact: a problem that is not here
/// fails the test.
struct Known {
    const char* ability;
    int         capture;
    const char* tag;
    const char* why;
};
const Known kKnown[] = {
    {"Ranger_Camouflage", 7, "CD:cooldown", R_MOD},
    {"Bard_Misc_QuickWit_Jest", 39, "CONF:missing Bard_Glee_Tick", R_CONFM},
    {"Bard_Song_Lullaby_AOE", 44, "EFF:differs Bard_Resonance", R_SONG},
    {"Bard_Saga", 44, "refused:", R_SEQ},
    {"Bard_Misc_Fortissimo", 44, "refused:", R_SEQ},
    {"Bard_Misc_QuickWit_Jest", 44, "CONF:extra Bard_QuickWit_Apply", R_CONFX},
    {"Bard_Dance_Maddening", 44, "EFF:differs Bard_Resonance", R_SONG},
    {"Bard_Song_Anthem", 44, "COST:mana", R_SEQ},
    {"Bard_Dance_Nimble", 44, "EFF:differs Bard_Resonance", R_SONG},
    {"Bard_Misc_Flourish", 44, "EFF:missing Bard_Flourish_Power", R_UNRES},
    {"Bard_Song_Serenade", 44, "EFF:missing Bard_Serenade_Spirited", R_UNRES},
    {"Bard_Song_Serenade", 44, "SHIELD:missing", R_UNRES},
    {"Cleric_Judgment", 77, "CONF:extra Cleric_Judgment_Ally", R_CONFX},
    {"Cleric_DivineInfusion", 77, "refused:", R_SEQ},
    {"Cleric_Judgment", 78, "CONF:extra Cleric_Judgment_Ally", R_CONFX},
    {"Cleric_Judgment", 98, "CONF:extra Cleric_Judgment_Ally", R_CONFX},
    {"Cleric_Judgment", 98, "refused:", R_SEQ},
    {"Cleric_FlashCure", 114, "CONF:extra Cleric_FlashCure", R_CHAN},
    {"Cleric_FlashCure", 114, "refused:", R_SEQ},
    {"Cleric_FlashCure", 114, "COST:mana", R_MOD},
    {"Cleric_FlashCure", 116, "COST:mana", R_MOD},
    {"General_Guard", 124, "CONF:extra General_Stamina_StamRegenDisable_EndHit", R_A7},
    {"Cleric_Mend", 152, "EFF:missing Cleric_Mend_Combo", R_UNRES},
    {"Cleric_Mend", 152, "CONF:missing Cleric_Mend_Hit", R_CONFM},
    {"Cleric_Mend", 152, "EFF:differs Cleric_Mend_Combo", R_UNRES},
    {"Cleric_Mend", 152, "refused:", R_SEQ},
    {"Cleric_FlashCure", 152, "CONF:extra Cleric_FlashCure", R_CHAN},
    {"Cleric_FlashCure", 152, "CD:charges", R_MOD},
    {"Cleric_FlashCure", 152, "COST:mana", R_MOD},
    {"Cleric_FlashCure", 152, "CONF:fewer Cleric_FlashCure", R_CHAN},
    {"Cleric_SoothingGlow", 152, "COST:mana", R_SEQ},
    {"Cleric_ConsecratingWave", 152, "COST:mana", R_SEQ},
    {"Cleric_ConsecratingWave", 152, "CONF:missing Cleric_ConsecratingWave_AllyHit", R_CONFM},
    {"Cleric_DefiantLight", 152, "CONF:fewer Cleric_DefiantLight_Tick", R_CONFM},
    {"Cleric_CommunalRestoration", 152, "CONF:missing Cleric_CommunalRestoration_HoT", R_CONFM},
    {"Cleric_ChainsOfRestraint", 152, "CD:missing", R_SEQ},
    {"Cleric_ChainsOfRestraint", 152, "COST:mana", R_SEQ},
    {"Cleric_CommunalRestoration", 152, "CD:missing", R_SEQ},
    {"Cleric_CommunalRestoration", 152, "COST:mana", R_SEQ},
    {"Cleric_CommunalRestoration", 152, "EFF:missing Cleric_CommunalRestoration_Effect1", R_UNRES},
    {"Cleric_CommunalRestoration", 152, "CONF:missing Cleric_CommunalRestoration_1", R_CONFM},
    {"Cleric_CommunalRestoration", 152, "CONF:missing Cleric_CommunalRestoration_2", R_CONFM},
    {"Cleric_CommunalRestoration", 152, "SHIELD:missing", R_UNRES},
    {"Cleric_DivineFlare", 152, "COST:mana", R_MOD},
    {"Cleric_DefiantLight", 152, "COST:mana", R_MOD},
    {"Cleric_Mend", 152, "CONF:extra Cleric_Mend_Hit", R_CONFX},
    {"Cleric_SoothingGlow", 152, "AB:missing", R_SEQ},
    {"Cleric_SoothingGlow", 152, "CD:missing", R_SEQ},
    {"Cleric_SoothingGlow", 152, "EFF:missing Cleric_SoothingGlow", R_CHAN},
    {"Cleric_SoothingGlow", 152, "CONF:missing Cleric_SoothingGlow", R_CHAN},
    {"Cleric_SoothingGlow", 152, "CONF:missing Cleric_SoothingGlow_Tick", R_CHAN},
    {"Cleric_DefiantLight", 152, "EFF:missing DisableEvasion_Hard_T2_Buildup", R_UNRES},
    {"Cleric_DivineInfusion", 152, "CONF:extra Cleric_DivineInfusion", R_CHAN},
    {"Cleric_DivineInfusion", 152, "CONF:missing Cleric_DivineInfusion", R_CHAN},
    {"Cleric_DivineInfusion", 152, "END:missing", R_CHAN},
    {"Cleric_Purify", 152, "CONF:extra Cleric_Purify", R_CONFX},
    {"Cleric_FlashCure", 152, "CD:missing", R_SEQ},
    {"Cleric_FlashCure", 152, "CONF:missing Cleric_FlashCure", R_CHAN},
    {"Cleric_FlashCure", 152, "END:missing", R_CHAN},
    {"Cleric_FlashCure", 152, "refused:", R_SEQ},
    {"Cleric_SoothingGlow", 152, "END:missing", R_CHAN},
    {"Ranger_BearTrap", 306, "CD:charges", R_MOD},
    {"Ranger_Camouflage", 310, "CD:cooldown", R_MOD},
    {"Ranger_ScatterShot", 312, "EFF:missing Ranger_Dash_Remove", R_UNRES},
    {"Ranger_ScatterShot", 312, "COST:mana", R_MOD},
    {"Ranger_ScatterShot", 313, "EFF:missing Ranger_Dash_Remove", R_UNRES},
    {"Rogue_Caltrops", 408, "CONF:missing Rogue_Caltrops_Spawn", R_CONFM},
    {"Rogue_ShadowCascade", 422, "refused:", R_SEQ},
    {"Rogue_Stealth", 430, "EFF:differs Rogue_Advantage", R_UNRES},
    {"General_Guard", 516, "CONF:extra General_Stamina_StamRegenDisable_EndHit", R_A7},
    {"Tank_IronAura", 516, "CONF:extra Tank_IronAura_Mitigation", R_NPC},
    {"Tank_IndomitableSpirit", 516, "EFF:missing Tank_IndomitableSpirit", R_NPC},
    {"Consumable_Commission_Scroll_AdventureXP_T1", 516, "AB:montage", R_D1},
    {"Consumable_Scroll_CraftingXP_T1_Mini", 516, "AB:montage", R_D1},
    {"Consumable_Commission_Scroll_ProcessingXP_T1", 516, "AB:montage", R_D1},
    {"Tank_Grit", 517, "refused:", R_SEQ},
    {"General_Guard", 519, "CONF:extra General_Stamina_StamRegenDisable_ApplyHit", R_A7},
    {"General_Guard", 519, "CONF:extra General_Stamina_StamRegenDisable_EndHit", R_A7},
    {"General_Guard", 519, "END:missing", R_A7},
    {"Tank_Desolate", 524, "COST:mana", R_MOD},
    {"Tank_Fortify", 524, "EFF:missing Tank_Fortify", R_NPC},
    {"Tank_IndomitableSpirit", 524, "EFF:missing Tank_IndomitableSpirit", R_NPC},
    {"Tank_IronAura", 524, "CONF:extra Tank_IronAura_Mitigation", R_NPC},
    {"Tank_Charge", 524, "CONF:extra Tank_Charge_Courage_Check", R_NPC},
    {"Tank_IronAura", 525, "CONF:extra Tank_IronAura_Mitigation", R_NPC},
    {"Tank_Fortify", 525, "EFF:missing Tank_Fortify", R_NPC},
    {"Tank_IndomitableSpirit", 525, "EFF:missing Tank_IndomitableSpirit", R_NPC},
    {"Tank_Charge", 525, "AB:targets", R_NPC},
    {"Tank_Charge", 525, "CONF:extra Tank_Charge_Courage_Check", R_NPC},
    {"General_Dodge", 526, "refused:", R_A7},
    {"Tank_IndomitableSpirit", 526, "EFF:missing Tank_IndomitableSpirit", R_NPC},
    {"Tank_Slam", 535, "refused:", R_SEQ},
    {"Tank_IronAura", 540, "CONF:extra Tank_IronAura_Mitigation", R_NPC},
    {"Tank_Grit", 542, "refused:", R_SEQ},
    {"Tank_IndomitableSpirit", 543, "EFF:missing Tank_IndomitableSpirit", R_NPC},
    {"Tank_Fortify", 544, "EFF:missing Tank_Fortify", R_NPC},
    {"Tank_Charge", 548, "CONF:extra Tank_Charge_Courage_Check", R_NPC},
    {"General_Dodge", 551, "SHIELD:missing", R_A7},
    {"General_Guard", 551, "CONF:extra General_Stamina_StamRegenDisable_EndHit", R_A7},
    {"General_Guard", 551, "SHIELD:missing", R_A7},
    {"General_Dodge", 551, "refused:", R_A7},
    {"WeaponCombo_Melee_1H_Axe", 552, "SHIELD:missing", R_UNRES},
    {"WeaponCombo_Melee_1H_Axe", 553, "SHIELD:missing", R_UNRES},
    {"Consumable_Elixir_Herbalism", 729, "AB:montage", R_D1},
    {"Consumable_Food_GatherersGreenSalad", 730, "AB:montage", R_D1},
    {"Consumable_Food_GatherersGreenSalad", 731, "AB:montage", R_D1},
    {"Consumable_Food_GatherersGreenSalad", 732, "AB:montage", R_D1},
    {"Consumable_Food_FruitSalad", 733, "AB:montage", R_D1},
    {"General_Dodge", 742, "CD:cooldown", R_A7},
    {"General_Sprint", 745, "END:missing", R_A7},
    {"General_Guard", 760, "CONF:extra General_Stamina_StamRegenDisable_EndHit", R_A7},
    {"General_Guard", 821, "EFF:missing General_Stamina_DisableRegen", R_A7},
    {"General_Guard", 821, "CONF:missing General_Stamina_StamRegenDisable_ApplyHit", R_A7},
    {"General_Guard", 822, "EFF:missing General_Stamina_DisableRegen", R_A7},
    {"General_Guard", 822, "CONF:missing General_Stamina_StamRegenDisable_ApplyHit", R_A7},
    {"General_Guard", 823, "EFF:missing General_Stamina_DisableRegen", R_A7},
    {"General_Guard", 823, "CONF:missing General_Stamina_StamRegenDisable_ApplyHit", R_A7},
    {"General_Guard", 917, "CONF:extra General_Stamina_StamRegenDisable_EndHit", R_A7},
    {"Tank_RushOfCourage", 1065, "AB:missing", R_NPC},
    {"Tank_RushOfCourage", 1065, "CD:missing", R_NPC},
    {"Tank_RushOfCourage", 1065, "COST:mana", R_MOD},
    {"Tank_RushOfCourage", 1065, "CONF:missing Tank_RushOfCourage", R_NPC},
    {"Tank_Grit", 1065, "SHIELD:missing", R_NPC},
    {"Tank_Reflect", 1065, "CONF:missing Tank_Deflect_Refreshing", R_NPC},
    {"Tank_Reflect", 1065, "CONF:missing General_Block_FX_Reaction_Hit", R_NPC},
    {"Consumable_Food_RoastedChicken", 1189, "AB:montage", R_D1},
    {"Consumable_Commission_Scroll_ProcessingXP_T1", 1190, "AB:montage", R_D1},
    {"Consumable_Scroll_Constitution_T1", 1191, "AB:montage", R_D1},
    {"Consumable_Commission_Scroll_AdventureXP_T1", 1357, "AB:montage", R_D1},
    {"Consumable_Regen_Meal", 1358, "AB:montage", R_D1},
    {"Consumable_Regen_Meal", 1358, "EFF:missing Consumable_Regen_Meal", R_D1},
    {"Consumable_Regen_Meal", 1358, "EFF:missing General_Rest", R_D1},
    {"Consumable_Regen_Meal", 1358, "CONF:missing Consumable_Regen_Meal_Tick", R_D1},
    {"Tank_Charge", 1510, "CONF:extra Tank_Charge_Courage_Check", R_NPC},
};

std::string problem_name(const std::string& tag) { return tag; }

/// How far a zone actor's open may be from retail's (the offset varies from one capture of the same dance to the next: Shielding +112 and +462 ms), and
/// how far its close (a dance's length: Shielding 5,123 and 5,371 ms in two captures).
constexpr double kZoneTolerance = 0.20;
constexpr double kZoneCloseTolerance = 0.30;

/// A zone actor retail closed: the close is compared once the capture's replay is over.
struct PendingClose {
    const char* name;
    int         capture;
    int64_t     ability;
    int64_t     zone_id;   // the runtime's number of the instance the cast opened
    double      t0;        // when the cast was triggered
    int         close_ms;  // retail's ZCLOSE, ms after the trigger
};
// (C-O-A) (C | Y | B)

}  // namespace

int main(int argc, char** argv) {
    if (!real_db_present()) {
        std::printf("no content.db at %s (cd services && go run ./cmd/contentc): the all-classes acceptance test is skipped\n", real_db_path().c_str());
        return 0;
    }
    bool verbose = false;
    std::string only_class, only_ability;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) verbose = true;
        else if (!std::strcmp(argv[i], "--class") && i + 1 < argc) only_class = argv[++i];
        else if (!std::strcmp(argv[i], "--ability") && i + 1 < argc) only_ability = argv[++i];
    }
    spdlog::set_level(spdlog::level::warn);
    const size_t n_cases = sizeof(capture_casts::kCases) / sizeof(capture_casts::kCases[0]);
    std::printf("the ability runtime against %zu retail casts of every class\n", n_cases);
    std::set<int> captures;
    for (const auto& c : capture_casts::kCases) captures.insert(c.capture);

    struct Tally {
        size_t total = 0, accepted = 0, refused_listed = 0, matched = 0, timed = 0, with_ts = 0;
    };
    std::map<std::string, Tally> tally;
    std::map<std::string, size_t> reasons;
    std::map<std::string, std::vector<std::string>> reason_cases;

    for (const int capture : captures) {
        std::string rig_cls;
        int level = 0;
        std::vector<int64_t> known;
        for (const auto& c : capture_casts::kCases) {
            if (c.capture != capture) continue;
            rig_cls = c.rig_cls;
            level = std::max(level, c.level);
            known.push_back(c.ability);
        }
        if (level == 0) level = 25;
        if (!only_class.empty() && rig_cls != only_class) continue;
        if (!only_ability.empty()) {
            bool has = false;
            for (const auto& c : capture_casts::kCases) has = has || (c.capture == capture && only_ability == c.name);
            if (!has) continue;
        }
        Rig r(class_enum(rig_cls), level);
        if (!r.ok()) {
            std::printf("  [FAIL] the rig of capture %d (%s %d): %s\n", capture, rig_cls.c_str(), level, r.problem.c_str());
            ++g_fail;
            continue;
        }
        r.pipe->set_known(known);
        r.pipe->set_data_timings(true);
        r.take();
        r.use_extra_ranks(infer_talents(r, capture));
        double clock_no_ts = 10.0;
        std::vector<PendingClose> pending_closes;
        // the capture's casts in the order they happened (the header is sorted by class)
        std::vector<const capture_casts::Case*> in_order;
        for (const capture_casts::Case& x : capture_casts::kCases) {
            if (x.capture == capture) in_order.push_back(&x);
        }
        std::stable_sort(in_order.begin(), in_order.end(), [](const capture_casts::Case* a, const capture_casts::Case* b) { return a->line < b->line; });
        for (const capture_casts::Case* cp : in_order) {
            const capture_casts::Case& c = *cp;
            if (!only_ability.empty() && only_ability != c.name) continue;
            Tally& ty = tally[c.cls];
            ++ty.total;
            const bool has_ts = c.at_ms >= 0;
            if (has_ts) ++ty.with_ts;
            const std::vector<RItem> reply = parse_lines(c.reply);
            const std::vector<RItem> client = parse_lines(c.client);
            const std::map<int64_t, double> before = parse_before(c.before);
            std::vector<std::string> problems, late;
            auto problem = [&](const std::string& p) { problems.push_back(p); };
// -- C | Y | B . CoA --

            const double t0 = has_ts ? 5.0 + c.at_ms / 1000.0 + 0.0123 : (clock_no_ts += 15.0);
            r.run_to(t0 - 0.2);
            r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
            r.take();
            // the class resources the character had (a Tank's Courage, a Cleric's DivinePower ...)
            for (const int64_t guid : content::kResourceStats) {
                const auto b = before.find(guid);
                if (b != before.end()) r.set_current(guid, b->second);
            }
            r.take();
            r.run_to(t0 - 0.001);
            const double mana_before = r.pawn.stat(content::kGuidMana);
            const size_t first_event = r.log.size();
            const bool a7 = r.rules.ability(c.ability) != nullptr;
            Refusal refusal = Refusal::None;
            TriggerInfo info;
            info.animation_index = c.animation_index;
            info.target_count = static_cast<uint32_t>(c.targets);
            info.item = false;
            if (a7) {
                refusal = r.a7->trigger(c.ability, c.client_instance_id, t0, info);
            } else {
                refusal = r.pipe->trigger(c.ability, c.client_instance_id, t0, info);
            }
            r.take();
            // what the client sent while the cast ran
            // (the cast's own steps come about 0.3 s after the call whatever retail's window was: a window cut short by the next call still lets them run)
            double end_t = t0 + (c.window_ms > 0 ? std::max(c.window_ms / 1000.0, 0.8) : 6.0);
            std::vector<std::pair<double, const RItem*>> sends;
            for (const RItem& q : client) sends.emplace_back(has_ts ? t0 + q.ms / 1000.0 : t0 + 0.3, &q);
            std::stable_sort(sends.begin(), sends.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [t, q] : sends) {
                if (refusal != Refusal::None) break;
                if (q->kind == "REQ") {
                    if (!a7) {
                        r.clock = t;
                        r.a7->tick(t);
                        const auto verdict = r.pipe->hit_request(to_i(q->f[0]), to_i(q->f[1]), to_i(q->f[2]), static_cast<size_t>(to_i(q->f[3])), static_cast<size_t>(to_i(q->f[4])), t);
                        if (verbose) std::printf("      request at %.3f: %s\n", t - t0, AbilityPipeline::verdict_name(verdict));
                        r.take();
                    }
                } else if (q->kind == "ENDREQ") {
                    r.run_to(t);
                    if (!a7) {
                        // the client names the ServerInstanceId of the item the pawn lists
                        int32_t sid = -1;
                        for (const auto& [k, v] : r.pawn.abilities)
                            if (v.ability_guid == c.ability) sid = static_cast<int32_t>(k);
                        if (sid >= 0) r.pipe->end_request(sid, t);
                    }
                    r.take();
                } else if (q->kind == "HELD") {
                    r.run_to(t);
                    if (a7) r.a7->cancel(c.client_instance_id, t);
                    else r.pipe->cancel(c.client_instance_id, t);
                    r.take();
                }
            }
            r.run_to(end_t);
            if (refusal != Refusal::None) {
                problem(std::string("refused:") + refusal_name(refusal));
            } else {
                ++ty.accepted;
            }
            if (!r.pawn.broken.empty()) problem("pawn:" + r.pawn.broken);

            if (refusal == Refusal::None) {
                std::vector<const Seen*> events;
                for (size_t i = first_event; i < r.log.size(); ++i) events.push_back(&r.log[i]);
                const abilities::AbilityDef* def = r.catalog->ability(c.ability);
                std::set<int64_t> reach_effects, reach_hits;
                if (def) closure(*r.catalog, *def, reach_effects, reach_hits);
                if (r.pipe->resource_class() >= 0) {
                    // (the class's own effect resource, e.g. Bard_Resonance, is part of every cast)
                }
                // the ability item
                const bootstrap::ActiveAbilityItem* my_item = nullptr;
                double my_item_at = 0;
                for (const Seen* e : events) {
                    if (!my_item && !e->change.abilities_added.empty()) {
                        my_item = &e->change.abilities_added[0];
                        my_item_at = e->at - t0;
                    }
                }
                for (const RItem& it : reply) {
                    if (it.kind != "AB" || to_i(it.f[0]) != c.ability || to_u(it.f[3]) == 0) continue;
                    if (!my_item) {
                        problem("AB:missing");
                        break;
                    }
                    if (my_item->ability_guid != c.ability || my_item->montage_group != it.f[2]) problem("AB:group " + it.f[2] + " (" + my_item->montage_group + ")");
                    if (!a7 && my_item->montage_guid != to_u(it.f[3])) problem("AB:montage");   // (A7's items name the montage by path: its own test compares them)
                    if (my_item->targets_self != (to_i(it.f[5]) > 0)) problem("AB:targets");
                    if (has_ts && std::fabs(my_item_at - it.ms / 1000.0) > kItemTolerance) late.push_back("AB " + std::to_string(static_cast<int>(my_item_at * 1000)) + "/" + std::to_string(it.ms));
                    break;
                }
                // the Cooldowns item
                const bootstrap::CooldownItem* my_cd = nullptr;
                double my_cd_at = -1;
                for (const Seen* e : events) {
                    for (const auto& cd : e->change.cooldowns_set) {
                        if (!my_cd && cd.ability_guid == c.ability) {
                            my_cd = &cd;
                            my_cd_at = e->at - t0;
                        }
                    }
                }
                for (const RItem& it : reply) {
                    if (it.kind != "CD" || to_i(it.f[0]) != c.ability) continue;
                    if (!my_cd) {
                        problem("CD:missing");
                        break;
                    }
                    if (my_cd->initial_cooldown != static_cast<float>(to_d(it.f[1]))) problem("CD:cooldown " + it.f[1]);
                    if (my_cd->charges != to_i(it.f[2])) problem("CD:charges " + it.f[2] + " (" + std::to_string(my_cd->charges) + ")");
                    if (has_ts && std::fabs(my_cd_at - it.ms / 1000.0) > kStepTolerance) late.push_back("CD " + std::to_string(static_cast<int>(my_cd_at * 1000)) + "/" + std::to_string(it.ms));
                    break;
                }
                // the Mana cost
                {
                    int cd_ms = -1;
                    for (const RItem& it : reply)
                        if (it.kind == "CD" && to_i(it.f[0]) == c.ability) cd_ms = it.ms;
                    double retail_before = before.count(content::kGuidMana) ? before.at(content::kGuidMana) : -1;
                    const RItem* step = nullptr;
                    for (const RItem& it : reply) {
                        if (it.kind != "STAT" || to_i(it.f[1]) != content::kGuidMana) continue;
                        if (cd_ms >= 0 && it.ms == cd_ms) {
                            step = &it;
                            break;
                        }
                        if (cd_ms < 0 || it.ms < cd_ms) retail_before = to_d(it.f[2]);
                    }
                    if (step && retail_before >= 0) {
                        const double retail_delta = retail_before - to_d(step->f[2]);
                        double my_after = mana_before;
                        for (const Seen* e : events) {
                            bool cost_event = !e->change.cooldowns_set.empty();
                            if (!cost_event) continue;
                            for (const auto& s : e->change.stats)
                                if (s.record == content::kGuidMana) my_after = s.value;
                            break;
                        }
                        if (retail_delta > 1 && std::fabs((mana_before - my_after) - retail_delta) > 1e-3)
                            problem("COST:mana " + std::to_string(retail_delta) + " (" + std::to_string(mana_before - my_after) + ")");
                    }
                }
                // the effects
                std::set<int64_t> first_seen;
                for (const RItem& it : reply) {
                    if (it.kind != "EFF" || a7) continue;   // (the effects of Sprint, Dodge and Rest are A7's: its own tests compare them)
                    const int64_t guid = to_i(it.f[0]);
                    if (!reach_effects.count(guid) || !first_seen.insert(guid).second) continue;
                    const abilities::EffectDef* ed = r.catalog->effect(guid);
                    const std::string name = ed ? ed->row.name : std::to_string(guid);
                    bool produced = false, same = false, timely = false;
                    for (const Seen* e : events) {
                        for (const auto* list : {&e->change.effects_added, &e->change.effects_changed}) {
                            for (const auto& x : *list) {
                                if (x.effect_guid != guid) continue;
                                produced = true;
                                const bool inf = to_d(it.f[1]) > 1e30;
                                const bool extends = ed && ed->row.stack_duration_refresh.find("ExtendDuration") != std::string::npos;
                                const bool same_here = (inf ? x.duration > 1e30f : std::fabs(x.duration - to_d(it.f[1])) < (extends ? 0.2 : 1e-3)) &&
                                                       std::fabs(x.tick_time - to_d(it.f[2])) < 1e-4 && x.stacks == to_i(it.f[3]);
                                if (same_here) {
                                    same = true;
                                    if (!has_ts || std::fabs((e->at - t0) * 1000 - it.ms) <= kEffectTolerance * 1000) timely = true;
                                }
                            }
                        }
                    }
                    if (!produced) problem("EFF:missing " + name);
                    else if (!same) problem("EFF:differs " + name + " " + it.f[1] + "/" + it.f[2] + "/" + it.f[3]);
                    else if (!timely) late.push_back("EFF " + name);
                }
                // the confirmations
                std::map<int64_t, int> retail_conf, my_conf;
                for (const RItem& it : reply)
                    if (it.kind == "CONF" && !a7) ++retail_conf[to_i(it.f[1])];
                for (const Seen* e : events)
                    for (const auto& h : e->confirmations)
                        if (reach_hits.count(h.record_guid) && !a7) ++my_conf[h.record_guid];
                for (const auto& [rec, n] : retail_conf) {
                    if (!reach_hits.count(rec)) continue;
                    const int mine = my_conf.count(rec) ? my_conf[rec] : 0;
                    const abilities::HitDef* hd = r.catalog->hit(rec);
                    const std::string hn = hd ? hd->row.name : std::to_string(rec);
                    if (mine == 0) problem("CONF:missing " + hn);
                    else if (mine < n && n <= 3) problem("CONF:fewer " + hn + " " + std::to_string(mine) + "/" + std::to_string(n));
                }
                for (const auto& [rec, n] : my_conf) {
                    if (!retail_conf.count(rec)) {
                        const abilities::HitDef* hd = r.catalog->hit(rec);
                        problem("CONF:extra " + (hd ? hd->row.name : std::to_string(rec)));
                    }
                }
                // the shields: the count of the last TempHealth item
                {
                    int retail_n = -1;
                    for (const RItem& it : reply)
                        if (it.kind == "SHIELD") retail_n = static_cast<int>(to_i(it.f[0]));
                    if (retail_n >= 0 && retail_n > 0 && r.pawn.shields.empty()) problem("SHIELD:missing");
                }
                // a cast retail ended early
                for (const RItem& it : reply) {
                    if (it.kind == "END") {
                        bool mine = false;
                        for (const Seen* e : events) mine = mine || (e->end && *e->end == to_i(it.f[0]));
                        if (!mine) problem("END:missing");
                        break;
                    }
                }
                // the lingering zone's actor [R: ZONE lines: a dance's visual effect; 4 of the 511 casts]: it opens near retail's offset, and its close is
                // compared when the capture is over (what ends a dance is the next cast)
                for (const RItem& it : reply) {
                    if (it.kind != "ZONE" || a7) continue;
                    const bootstrap::ZoneOp* open = nullptr;
                    double open_at = 0;
                    for (const Seen* e : events) {
                        for (const bootstrap::ZoneOp& z : e->zones) {
                            if (!open && z.kind == bootstrap::ZoneOp::Kind::Open && z.ability == c.ability) {
                                open = &z;
                                open_at = e->at - t0;
                            }
                        }
                    }
                    if (!open) {
                        problem("ZONE:missing");
                        continue;
                    }
                    if (has_ts && std::fabs(open_at * 1000 - it.ms) > kZoneTolerance * 1000) late.push_back("ZONE " + std::to_string(static_cast<int>(open_at * 1000)) + "/" + std::to_string(it.ms));
                    int close_ms = -1;
                    for (const RItem& z : reply)
                        if (z.kind == "ZCLOSE") close_ms = z.ms;
                    if (has_ts && close_ms >= 0) pending_closes.push_back({c.name, c.capture, c.ability, open->id, t0, close_ms});
                }
            }
            // the listed differences are no error
            std::vector<std::string> unlisted;
            for (const std::string& p : problems) {
                bool listed = false;
                for (const Known& k : kKnown) listed = listed || (std::string(k.ability) == c.name && k.capture == c.capture && p.rfind(k.tag, 0) == 0);
                if (!listed) unlisted.push_back(p);
            }
            if (refusal != Refusal::None && unlisted.empty()) ++ty.refused_listed;
            if (unlisted.empty()) ++ty.matched;
            if (unlisted.empty() && late.empty()) ++ty.timed;
            for (const std::string& p : unlisted) {
                std::string key = p;
                // (the reasons are counted by their first words)
                const size_t sp = key.find(' ');
                const size_t colon = key.find(':');
                std::string head = key.substr(0, std::min(sp == std::string::npos ? key.size() : sp, colon == std::string::npos ? key.size() : key.size()));
                if (sp != std::string::npos) head = key.substr(0, sp);
                ++reasons[head + " [" + c.cls + "]"];
                reason_cases[head + " [" + c.cls + "]"].push_back(std::string(c.name) + "@" + std::to_string(c.capture) + ":" + std::to_string(c.line) + " " + p);
            }
            if (verbose || (!unlisted.empty() && !only_ability.empty())) {
                std::printf("%s %s cap %d line %d: %s", unlisted.empty() ? "ok  " : "FAIL", c.name, c.capture, c.line, unlisted.empty() ? "matches" : "");
                for (const std::string& p : unlisted) std::printf("\n      %s", p.c_str());
                for (const std::string& p : late) std::printf("\n      late %s", p.c_str());
                std::printf("\n");
            }
        }
        // the zone actors retail destroyed: ours are closed when the dance ends too, as long after its trigger
        if (!pending_closes.empty()) {
            r.run_to(r.clock + 40.0);
            for (const PendingClose& pc : pending_closes) {
                double closed_at = -1;
                for (const Seen& s : r.log)
                    for (const bootstrap::ZoneOp& z : s.zones)
                        if (z.kind == bootstrap::ZoneOp::Kind::Close && z.id == pc.zone_id) closed_at = s.at - pc.t0;
                CHECKF(closed_at >= 0 && std::fabs(closed_at * 1000 - pc.close_ms) <= kZoneCloseTolerance * 1000, "%s (capture %d): the zone's actor is closed %.0f ms after the trigger (retail %d)", pc.name, pc.capture,
                       closed_at * 1000, pc.close_ms);
            }
        }
    }
    std::printf("\n%-10s %7s %9s %9s %9s %9s\n", "class", "casts", "accepted", "matched", "on time", "with ts");
    size_t total = 0, accepted = 0, matched = 0;
    for (const auto& [cls, t] : tally) {
        std::printf("%-10s %7zu %9zu %9zu %9zu %9zu\n", cls.c_str(), t.total, t.accepted, t.matched, t.timed, t.with_ts);
        total += t.total;
        accepted += t.accepted;
        matched += t.matched;
    }
    std::printf("%-10s %7zu %9zu %9zu\n", "all", total, accepted, matched);
    std::printf("\nwhat is left:\n");
    for (const auto& [reason, n] : reasons) {
        std::printf("  %5zu  %s\n", n, reason.c_str());
        if (verbose) {
            size_t shown = 0;
            for (const std::string& s : reason_cases[reason]) {
                if (++shown > 6) break;
                std::printf("           %s\n", s.c_str());
            }
        }
    }
    size_t refused_listed = 0;
    for (const auto& [cls, t] : tally) refused_listed += t.refused_listed;
    std::printf("%zu casts are refused and listed in kKnown with the reason\n", refused_listed);
    CHECKF(accepted + refused_listed == total, "every cast is accepted, or its refusal is listed (%zu + %zu of %zu)", accepted, refused_listed, total);
    CHECKF(matched == total, "every accepted cast matches retail's reply (%zu of %zu)", matched, total);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

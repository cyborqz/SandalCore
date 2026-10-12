// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_ability_npc.cpp: the ability runtime against what the retail server answered to every class ability that needs another actor, for every class
//  (plan E1, stage 3, part 2)
//
//  capture_npc_casts.h (tools/capture/gen_npc_casts.py) holds every ServerTriggerAbility of the 482 unique retail captures that stage 2's all-classes test
//  could not serve (gen_casts.py's stage NPC, the class abilities: the call names another actor, a hit request or confirmation names one, a pet or summon,
//  an enemy-only ability), with what the client sent while it ran and what the server answered, item by item with its time.  The calls of one capture are
//  replayed in order on one character of the capture's class and level of the real content.db on a simulated clock, each at its own time, in a world with
//  ground and ONE creature 3 m in front of the caster, made new for every cast (a fresh Wolf, or the Blueprint the capture's target was when the capture
//  opened it, at the caster's level, that does not come for the caster before it is hit).  For each cast:
//    - it must be ACCEPTED: an enemy ability is cast at the creature, an ability that may help allies is cast as retail's call was aimed (at the caster, an
//      ally being no actor of this world), the hit requests name the creature's collision cylinder where retail's named another actor's;
//    - what the caster's own side holds is compared as the all-classes test does it: the ability item (the ability, MontageGroup, the montage's NetGUID,
//      whether it has a target), the Cooldowns item, the Mana cost, the effects on the caster, the shields, an early end;
//    - the hit confirmations of the ability's hits retail sent must be sent, and the creature must have been hit where retail's confirmation had a target
//      (the confirmation's number of actors hit and the stat mods' record guids, which say what the hit does: damage, a heal, a debuff);
//    - the AMOUNTS are compared by their ratio to retail's: the creature's stats and the caster's gear and talents are not in the captures, so the numbers
//      are a distribution (the median and the quartiles are reported per class), not a pass or fail;
//    - the TIMES are counted apart as in the all-classes test.
//  What is not matched is a LISTED difference (the ability, the capture, what, why) and the list is exact: an unlisted difference fails the test.
//  The report counts the casts accepted and matched per class and what is left per reason.  Without a content.db the test says so and is skipped.
// ============================================================================
#include "ability_rig.h"
#include "capture_npc_casts.h"
#include "combat/combat_data.h"
#include "combat/npc_stats.h"
#include "combat_rig.h"
#include "npc/motion_table.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif

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

using NpcCase = capture_npc_casts::Case;

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
// -- C:Y:B . C O A --

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
// ~ CoA :: C Y B ~

/// The talents the capture's character held, as far as the cooldowns and charges its casts show (the all-classes test's inference): for each ability whose
/// cooldown or charge formula asks about skills, the smallest set of them under which the formulas give retail's InitialCooldown and at least as many charges
/// as retail's Cooldowns items show.
std::vector<stats::SkillRank> infer_talents(Rig& r, int capture) {
    std::vector<stats::SkillRank> extra;
    std::set<int64_t> done;
    for (const NpcCase& c : capture_npc_casts::kCases) {
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
        double cooldown = -1;
        int most_left = -1;
        for (const NpcCase& d : capture_npc_casts::kCases) {
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

/// The state a character of a capture had from before the capture, as far as an ability's requirement names it: the effects it asks the caster (or its
/// owner) to have (a stealth, a stack, an enchant) and a summoned pet (the effect that carries the tag Pet.Summon: the pet itself, an NPC, is not made).
/// Granted to the caster before the cast whose requirement was false.
size_t grant_states(Rig& r, const abilities::AbilityDef& def, double t) {
    const content::ExprRow* e = r.catalog->expr(def.row.requirement_expr);
    if (!e) return 0;
    size_t granted = 0;
    static const std::regex re(R"(GetEffectCount\(Get(?:Instigator|Owner)\(\),\s*\$#\d+:(\d+)\$\))");
    for (auto it = std::sregex_iterator(e->text.begin(), e->text.end(), re); it != std::sregex_iterator(); ++it) {
        const int64_t guid = std::stoll((*it)[1]);
        if (r.catalog->effect(guid) && !r.pawn.effect(guid) && r.pipe->grant_effect(guid, 1, t)) ++granted;
    }
    // a class resource the requirement asks for (a Tank's Courage at 20): the capture's character had built it up in the fights before the capture
    static const std::regex stat_re(R"(GetStat\(Get(?:Instigator|Owner)\(\),\s*\$#\d+:(\d+)\$\)\)?\s*>=\s*([0-9.]+))");
    for (auto it = std::sregex_iterator(e->text.begin(), e->text.end(), stat_re); it != std::sregex_iterator(); ++it) {
        const int64_t stat = std::stoll((*it)[1]);
        const double need = std::stod((*it)[2]);
        const stats::StatValue* v = r.cs->find(stat);
        if (v && v->value < need) {
            r.set_current(stat, need);
            ++granted;
        }
    }
    std::string lower;
    for (const char ch : e->text) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.find("pet.summon") != std::string::npos) {
        const char* pet = def.row.name.find("Mystic") != std::string::npos ? "Summoner_Pet_Mystic" : (def.row.name.find("Guardian") != std::string::npos ? "Summoner_Pet_Guardian" : "Summoner_Pet_Hunter");
        const abilities::EffectDef* ed = r.catalog->effect_named(pet);
        if (ed && !r.pawn.effect(ed->row.guid) && r.pipe->grant_effect(ed->row.guid, 1, t)) ++granted;
    }
    return granted;
}

// ---- the listed differences -------------------------------------------------------------------------------------------------------------------------

const char* const R_STATE = "the ability's activation requirement reads a state the capture's character had from before the capture or that a summoned pet, a stealth, a stack or an enchant gives (GetEffectCount, GetEffectCountByTag on the caster or the target): the replay starts each capture's character fresh and stage 3 has no pets";
const char* const R_CHARGES = "the capture's character had talents or gear that give the ability more charges or a shorter cooldown than its formula's base (the test infers only the talents the formulas name): the replay's charges run out where retail's did not";
const char* const R_MOD = "retail's Mana cost carries a talent or gear modifier the capture does not name (the level comes from the first cost, the talents from the cooldowns)";
const char* const R_SHIELD = "retail's reply lists a TempHealth shield the replay's pawn does not have: a shield whose amount follows the damage the hit did or the caster's gear and talents; not resolved";
const char* const R_REACT = "retail applies the effect on the caster through a reaction (an effect's or a hit's ReactionsIds: stored, not followed) or through a state the replay's character lacks (Vengeance, a Ranger's dash, Advantage, a combo's step); not run";
const char* const R_NPCFX = "an effect the hit puts on the creature (a damage over time, a snare, a debuff: Burning, Bleeding, Hemorrhage, Conflagration ...) is not run: stage 3 puts only shields on an NPC, so that effect's periodic hits and what depends on its stacks are not made";
const char* const R_AREA = "an area hit at a ground point or around a target that the replay does not reach (the client's aim point is not in the call, retail had more creatures around): the hit is not made or finds nobody here";
const char* const R_ALLY = "the hit needs an ally, a party member or a second creature that retail's cast reached and the replay (one creature, no ally) does not have";
const char* const R_CONFX = "our confirmation rule confirms a hit that retail did not confirm in this cast (the caster counts as an ally of its own heal, a second step of a chain, another number of creatures hit)";
const char* const R_CHAN = "a channelled, toggled or chained ability that the client re-triggers or the server ends early in retail; the early end is not matched";
const char* const R_UNRES = "not resolved in stage 3";
const char* const R_KILL = "the replay's creature (a level-1 Wolf, 34.9 Health, where the capture's target is another actor of unknown level) dies of the ability's first hit since the E1 review's stat fix gave a level-1 creature retail's MaxHealth (a fifth less): the hits after the kill are not confirmed; retail's target survived";

struct Known {
    const char* ability;
    int         capture;
    const char* tag;
    const char* why;
};
#include "ability_npc_known.inc"

}  // namespace

int main(int argc, char** argv) {
    if (!real_db_present()) {
        std::printf("no content.db at %s (cd services && go run ./cmd/contentc): the NPC casts' acceptance test is skipped\n", real_db_path().c_str());
        return 0;
    }
    bool verbose = false, use_known = true;   // (--no-known: the listed differences are not applied: the run that writes ability_npc_known.inc)
    std::string only_class, only_ability;
    int only_capture = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) verbose = true;
        else if (!std::strcmp(argv[i], "--no-known")) use_known = false;
        else if (!std::strcmp(argv[i], "--class") && i + 1 < argc) only_class = argv[++i];
        else if (!std::strcmp(argv[i], "--ability") && i + 1 < argc) only_ability = argv[++i];
        else if (!std::strcmp(argv[i], "--capture") && i + 1 < argc) only_capture = std::atoi(argv[++i]);
    }
    spdlog::set_level(spdlog::level::warn);
    const size_t n_cases = sizeof(capture_npc_casts::kCases) / sizeof(capture_npc_casts::kCases[0]);
    std::printf("the ability runtime against %zu retail casts of every class that need another actor\n", n_cases);
    std::set<int> captures;
    for (const auto& c : capture_npc_casts::kCases) captures.insert(c.capture);

    // the data every capture's world shares
    std::string error;
    npc::MotionTable motion;
    if (!motion.load(NPC_MOTION_PATH, error)) {
        std::printf("  [FAIL] npc_motion.tsv: %s\n", error.c_str());
        return 2;
    }
    std::string wolf_path;
    for (const auto& [p, row] : motion.rows()) {
        if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = p;
    }

    struct Tally {
        size_t total = 0, accepted = 0, refused_listed = 0, matched = 0, exact = 0, timed = 0, with_ts = 0, hit_cases = 0, hit_landed = 0, mods_same = 0, mods_diff = 0;
        std::vector<double> ratios;   // our damage over retail's, a hit that landed with the same stat mods
    };
    std::map<std::string, Tally> tally;
    std::map<std::string, size_t> reasons;
    std::map<std::string, std::vector<std::string>> reason_cases;
    std::map<std::string, size_t> kinds;   // target_kind of the cases that were run
    size_t at_npc = 0, at_air = 0, at_self = 0, states_granted = 0;
// -- C:Y:B & CoA --

    for (const int capture : captures) {
        std::string rig_cls;
        int level = 0;
        std::vector<int64_t> known;
        for (const auto& c : capture_npc_casts::kCases) {
            if (c.capture != capture) continue;
            rig_cls = c.rig_cls;
            level = std::max(level, c.level);
            known.push_back(c.ability);
        }
        if (level == 0) level = 25;
        if (!only_class.empty() && rig_cls != only_class) continue;
        if (only_capture != 0 && only_capture != capture) continue;
        if (!only_ability.empty()) {
            bool has = false;
            for (const auto& c : capture_npc_casts::kCases) has = has || (c.capture == capture && only_ability == c.name);
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

        // the world: ground, the caster, one creature at a time
        const auto rows = r.db->combat_rows(error);
        std::optional<combat::CombatData> data;
        if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
        if (!data) {
            std::printf("  [FAIL] the combat data of capture %d: %s\n", capture, error.c_str());
            ++g_fail;
            continue;
        }
        combat_rig::World w;
        if (!w.problem.empty()) {
            std::printf("  [FAIL] the world of capture %d: %s\n", capture, w.problem.c_str());
            ++g_fail;
            continue;
        }
        combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
        combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
        combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
        cw.add_player(&player);
        combat::WorldHitPort port(cw, w.registry, player.fighter());
        r.pipe->set_combat(&port);

        double world_now = 0;
        const auto advance = [&](double t) {
            while (world_now < t - 1e-9) {
                world_now = std::min(t, world_now + 0.1);
                w.npcs->tick(world_now);
                cw.tick(world_now);
                r.run_to(world_now);
            }
            if (t < world_now) r.run_to(t);   // (a request's own time, inside a step: the runtime keeps its own clock)
        };

        double clock_no_ts = 10.0;
        // the capture's casts in the order they happened (the header is sorted by class)
        std::vector<const NpcCase*> in_order;
        for (const NpcCase& x : capture_npc_casts::kCases) {
            if (x.capture == capture) in_order.push_back(&x);
        }
        std::stable_sort(in_order.begin(), in_order.end(), [](const NpcCase* a, const NpcCase* b) { return a->line < b->line; });
        uint64_t creature = 0;
        for (const NpcCase* cp : in_order) {
            const NpcCase& c = *cp;
            if (!only_ability.empty() && only_ability != c.name) continue;
            Tally& ty = tally[c.cls];
            ++ty.total;
            ++kinds[c.target_kind];
            const bool has_ts = c.at_ms >= 0;
            if (has_ts) ++ty.with_ts;
            const std::vector<RItem> reply = parse_lines(c.reply);
            const std::vector<RItem> client = parse_lines(c.client);
            const std::map<int64_t, double> before = parse_before(c.before);
            std::vector<std::string> problems, late;
            auto problem = [&](const std::string& p) { problems.push_back(p); };

            const double t0 = has_ts ? 5.0 + c.at_ms / 1000.0 + 0.0123 : (clock_no_ts += 15.0);
            // a new creature for every cast, of the kind the capture's target was when the capture knew it
            if (creature) {
                w.registry.remove(creature);
                cw.forget(creature);
            }
            std::string path = wolf_path;
            if (std::strlen(c.archetype) > 0 && motion.rows().count(c.archetype)) path = c.archetype;
            const int npc_level = std::max(1, c.level);
            // 3 m in front of the caster, or as far as the ability's own requirement asks (a Rogue's Sleep Dart needs 3 m, its Death's Mark 7.5 m)
            double distance_cm = 300;
            if (const abilities::AbilityDef* d0 = r.catalog->ability(c.ability)) {
                if (const content::ExprRow* e = r.catalog->expr(d0->row.requirement_expr)) {
                    static const std::regex re(R"(GetDistanceBetween\(GetInstigator\(\), GetTarget\(\)\)\s*>=\s*([0-9.]+))");
                    for (auto it = std::sregex_iterator(e->text.begin(), e->text.end(), re); it != std::sregex_iterator(); ++it) distance_cm = std::max(distance_cm, std::stod((*it)[1]) + 150.0);
                    distance_cm = std::min(distance_cm, 1200.0);
                }
            }
            creature = w.spawn(path, distance_cm, 0, npc_level, 1.0);
            if (!creature) {
                std::printf("  [FAIL] no creature for %s (capture %d)\n", c.name, capture);
                ++g_fail;
                continue;
            }
            advance(t0 - 0.2);
            r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
            r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
            r.take();
            for (const int64_t guid : content::kResourceStats) {
                const auto b = before.find(guid);
                if (b != before.end()) r.set_current(guid, b->second);
            }
            r.take();
            advance(t0 - 0.001);
            const double mana_before = r.pawn.stat(content::kGuidMana);
            const size_t first_event = r.log.size();
            const abilities::AbilityDef* def = r.catalog->ability(c.ability);
            // who the cast is aimed at: an ability that may harm is cast at the creature when retail's call named a target (or any other actor), one that
            // only helps (no Enemy in its mask) at the caster, as retail's call at an ally is replayed
            const bool enemy_ok = def && (def->row.applicable_targets & 2) != 0;
            const bool ally_ok = def && (def->row.applicable_targets & (1 | 4 | 8 | 16)) != 0;
            const bool names_target = c.targets > 0;
            const bool aim_at_creature = names_target && enemy_ok && (std::strcmp(c.target_kind, "npc") == 0 || !ally_ok);
            TriggerInfo info;
            info.animation_index = c.animation_index;
            info.target_count = static_cast<uint32_t>(c.targets);
            info.item = false;
            if (aim_at_creature) info.target_actors = {creature};
            (aim_at_creature ? at_npc : (names_target ? at_self : at_air)) += 1;
            int32_t trigger_id = c.client_instance_id;
            Refusal refusal = r.pipe->trigger(c.ability, trigger_id, t0, info);
            r.take();
            if (refusal == Refusal::Requirement && def && grant_states(r, *def, t0) > 0) {
                // [modelled] the capture's character had the state the requirement names from before the capture: it is given and the call is made again
                r.pipe->refresh(t0, "the state before the capture", true);
                r.take();
                ++states_granted;
                trigger_id = c.client_instance_id + 500000;   // (the refused call's id is still the runtime's)
                refusal = r.pipe->trigger(c.ability, trigger_id, t0 + 0.0005, info);
                r.take();
            }
            // what the client sent while the cast ran
            double end_t = t0 + (c.window_ms > 0 ? std::max(c.window_ms / 1000.0, 0.8) : 6.0);
            std::vector<std::pair<double, const RItem*>> sends;
            for (const RItem& q : client) sends.emplace_back(has_ts ? t0 + q.ms / 1000.0 : t0 + 0.3, &q);
            std::stable_sort(sends.begin(), sends.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [t, q] : sends) {
                if (refusal != Refusal::None) break;
                if (q->kind == "REQ") {
                    advance(t);
                    r.clock = std::max(r.clock, t);
                    r.a7->tick(t);
                    const size_t components = static_cast<size_t>(to_i(q->f[3])), own = static_cast<size_t>(to_i(q->f[4]));
                    const abilities::HitDef* hd = r.catalog->hit(to_i(q->f[2]));
                    // a component that is not the caster's names another actor: the creature when the hit can harm, and the caster itself (an ally being no actor here) otherwise
                    const bool harms = hd && (hd->row.applicable_targets & 2) != 0;
                    std::vector<uint64_t> targets;
                    size_t own_used = own;
                    if (components > own) {
                        if (harms) targets = {creature};
                        else own_used = components;
                    }
                    const auto verdict = r.pipe->hit_request(to_i(q->f[0]), to_i(q->f[1]), to_i(q->f[2]), components, own_used, t, targets);
                    if (verbose) std::printf("      request at %.3f: %s\n", t - t0, AbilityPipeline::verdict_name(verdict));
                    r.take();
                } else if (q->kind == "ENDREQ") {
                    advance(t);
                    int32_t sid = -1;
                    for (const auto& [k, v] : r.pawn.abilities)
                        if (v.ability_guid == c.ability) sid = static_cast<int32_t>(k);
                    if (sid >= 0) r.pipe->end_request(sid, t);
                    r.take();
                } else if (q->kind == "HELD") {
                    advance(t);
                    r.pipe->cancel(trigger_id, t);
                    r.take();
                }
            }
            advance(end_t);
            if (refusal != Refusal::None) problem(std::string("refused:") + refusal_name(refusal));
            else ++ty.accepted;
            if (!r.pawn.broken.empty()) problem("pawn:" + r.pawn.broken);
// built with Cyb and C | O | A

            if (refusal == Refusal::None) {
                std::vector<const Seen*> events;
                for (size_t i = first_event; i < r.log.size(); ++i) events.push_back(&r.log[i]);
                std::set<int64_t> reach_effects, reach_hits;
                if (def) closure(*r.catalog, *def, reach_effects, reach_hits);
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
                    if (my_item->montage_guid != to_u(it.f[3])) problem("AB:montage");
                    const bool mine_has_target = my_item->targets_self || !my_item->target_objects.empty();
                    if (mine_has_target != (to_i(it.f[5]) > 0)) problem("AB:targets");
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
                            if (e->change.cooldowns_set.empty()) continue;
                            for (const auto& s : e->change.stats)
                                if (s.record == content::kGuidMana) my_after = s.value;
                            break;
                        }
                        if (retail_delta > 1 && std::fabs((mana_before - my_after) - retail_delta) > 1e-3)
                            problem("COST:mana " + std::to_string(retail_delta) + " (" + std::to_string(mana_before - my_after) + ")");
                    }
                }
                // the effects on the caster
                std::set<int64_t> first_seen;
                for (const RItem& it : reply) {
                    if (it.kind != "EFF") continue;
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
                // the confirmations: the records, then the creature's side of each (was it hit, with which stat mods, how much)
                std::map<int64_t, int> retail_conf, my_conf;
                std::map<int64_t, const RItem*> retail_first;
                for (const RItem& it : reply) {
                    if (it.kind != "CONF") continue;
                    ++retail_conf[to_i(it.f[1])];
                    if (!retail_first.count(to_i(it.f[1]))) retail_first[to_i(it.f[1])] = &it;
                    else if (to_i(it.f[6]) > 0 && to_i(retail_first[to_i(it.f[1])]->f[6]) == 0) retail_first[to_i(it.f[1])] = &it;
                }
                std::map<int64_t, const bootstrap::HitConfirm*> my_first;
                for (const Seen* e : events) {
                    for (const auto& h : e->confirmations) {
                        if (!reach_hits.count(h.record_guid)) continue;
                        ++my_conf[h.record_guid];
                        if (!my_first.count(h.record_guid) || (!h.targets.empty() && my_first[h.record_guid]->targets.empty())) my_first[h.record_guid] = &h;
                    }
                }
                for (const auto& [rec, n] : retail_conf) {
                    if (!reach_hits.count(rec)) continue;
                    const int mine = my_conf.count(rec) ? my_conf[rec] : 0;
                    const abilities::HitDef* hd = r.catalog->hit(rec);
                    const std::string hn = hd ? hd->row.name : std::to_string(rec);
                    if (mine == 0) problem("CONF:missing " + hn);
                    else if (mine < n && n <= 3) problem("CONF:fewer " + hn + " " + std::to_string(mine) + "/" + std::to_string(n));
                    // the creature's side
                    const RItem* rf = retail_first.at(rec);
                    const long retail_targets = static_cast<long>(to_i(rf->f[6]));
                    if (retail_targets > 0 && mine > 0) {
                        const bootstrap::HitConfirm* mf = my_first[rec];
                        ++ty.hit_cases;
                        if (mf->targets.empty()) {
                            // our confirmation lists no actor hit: a hit that needs somebody else (an ally, a second enemy) found nobody here
                            problem("HIT:nobody " + hn);
                            continue;
                        }
                        ++ty.hit_landed;
                        std::multiset<int64_t> retail_mods, my_mods;
                        double retail_sum = 0, my_sum = 0;
                        for (size_t k = 7; k < rf->f.size(); ++k) {
                            const std::string& m = rf->f[k];
                            const size_t a = m.find(':'), b = m.find(':', a + 1);
                            retail_mods.insert(std::stoll(m.substr(0, a)));
                            retail_sum += std::bit_cast<float>(static_cast<uint32_t>(std::stoul(m.substr(a + 1, b - a - 1), nullptr, 16)));
                        }
                        for (const auto& tg : mf->targets) {
                            for (const auto& m : tg.mods) {
                                my_mods.insert(m.stat_mod_guid);
                                my_sum += m.value;
                            }
                        }
                        // the stat mods of the hit that retail's confirmation lists once per actor hit: the set of records is what matters
                        const std::set<int64_t> rs(retail_mods.begin(), retail_mods.end()), ms(my_mods.begin(), my_mods.end());
                        if (rs.empty() || ms.empty()) {
                            // a miss or an immune target on either side: no amount to compare
                        } else if (rs == ms) {
                            ++ty.mods_same;
                            if (retail_sum != 0 && my_sum != 0 && retail_mods.size() == my_mods.size() && my_sum / retail_sum > 0) ty.ratios.push_back(my_sum / retail_sum);
                        } else {
                            ++ty.mods_diff;
                            problem("HIT:mods " + hn);
                        }
                    }
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
                    if (retail_n > 0 && r.pawn.shields.empty()) problem("SHIELD:missing");
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
            }
            // the listed differences are no error
            std::vector<std::string> unlisted;
            for (const std::string& p : problems) {
                bool listed = false;
                for (const Known& k : kKnown) listed = listed || (use_known && std::string(k.ability) == c.name && k.capture == c.capture && p.rfind(k.tag, 0) == 0);
                if (!listed) unlisted.push_back(p);
            }
            if (refusal != Refusal::None && unlisted.empty()) ++ty.refused_listed;
            if (unlisted.empty()) ++ty.matched;
            if (problems.empty()) ++ty.exact;
            if (unlisted.empty() && late.empty()) ++ty.timed;
            for (const std::string& p : unlisted) {
                const size_t sp = p.find(' ');
                const std::string head = sp == std::string::npos ? p : p.substr(0, sp);
                ++reasons[head + " [" + c.cls + "]"];
                reason_cases[head + " [" + c.cls + "]"].push_back(std::string(c.name) + "@" + std::to_string(c.capture) + ":" + std::to_string(c.line) + " " + p);
            }
            if (verbose || (!unlisted.empty() && !only_ability.empty())) {
                std::printf("%s %s cap %d line %d (%s): %s  [the creature: level %d, Health %.1f of %.1f after the cast]", unlisted.empty() ? "ok  " : "FAIL", c.name, c.capture, c.line,
                            c.target_kind, unlisted.empty() ? "matches" : "", npc_level, cw.health_of(creature), cw.max_health_of(creature));
                for (const std::string& p : unlisted) std::printf("\n      %s", p.c_str());
                for (const std::string& p : late) std::printf("\n      late %s", p.c_str());
                std::printf("\n");
            }
        }
    }
    std::printf("\n%-10s %7s %9s %9s %9s %9s %9s %9s %9s\n", "class", "casts", "accepted", "listed ok", "exact", "on time", "with ts", "hits", "landed");
    size_t total = 0, accepted = 0, matched = 0, exact = 0, refused_listed = 0;
    std::vector<double> all_ratios;
    for (const auto& [cls, t] : tally) {
        std::printf("%-10s %7zu %9zu %9zu %9zu %9zu %9zu %9zu %9zu\n", cls.c_str(), t.total, t.accepted, t.matched, t.exact, t.timed, t.with_ts, t.hit_cases, t.hit_landed);
        total += t.total;
        exact += t.exact;
        accepted += t.accepted;
        matched += t.matched;
        refused_listed += t.refused_listed;
        all_ratios.insert(all_ratios.end(), t.ratios.begin(), t.ratios.end());
    }
    std::printf("%-10s %7zu %9zu %9zu %9zu\n", "all", total, accepted, matched, exact);
    std::printf("\n%zu casts were made after the state their requirement names was given to the caster (a stealth, a stack, an enchant, a summoned pet's effect) [modelled]", states_granted);
    std::printf("\ncast at: %zu at the creature, %zu at the caster (an ally's place), %zu with no target; retail's targets: ", at_npc, at_self, at_air);
    for (const auto& [k, n] : kinds) std::printf("%s %zu  ", k.c_str(), n);
    std::printf("\n");
    // the amounts: our hit's total stat mod over retail's, where the hit landed and both name the same stat mods (the creature's and the caster's stats differ
    // from retail's, so this is a distribution)
    std::printf("\namounts (ours / retail's, hits that landed on both sides with the same stat mods):\n");
    for (const auto& [cls, t] : tally) {
        if (t.ratios.empty()) continue;
        std::vector<double> v = t.ratios;
        std::sort(v.begin(), v.end());
        const auto q = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))]; };
        std::printf("  %-9s %5zu hits: quartiles %.2f / %.2f / %.2f   (mods the same set: %zu, a different set: %zu)\n", cls.c_str(), v.size(), q(0.25), q(0.5), q(0.75), t.mods_same, t.mods_diff);
    }
    if (!all_ratios.empty()) {
        std::sort(all_ratios.begin(), all_ratios.end());
        std::printf("  all       %5zu hits: quartiles %.2f / %.2f / %.2f\n", all_ratios.size(), all_ratios[all_ratios.size() / 4], all_ratios[all_ratios.size() / 2], all_ratios[all_ratios.size() * 3 / 4]);
    }
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
    std::printf("%zu casts are refused and listed in kKnown with the reason\n", refused_listed);
    if (only_class.empty() && only_ability.empty() && only_capture == 0) {
        CHECKF(total == n_cases, "every cast of the header was run (%zu of %zu)", total, n_cases);
        CHECKF(accepted + refused_listed == total, "every cast is accepted, or its refusal is listed (%zu + %zu of %zu)", accepted, refused_listed, total);
        CHECKF(matched == total, "every accepted cast matches retail's reply (%zu of %zu)", matched, total);
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_ability_retail.cpp: the generic ability runtime against what the retail server answered (plan D2, stage 1)
//
//  capture_ability_replies.h (tools/capture/gen_ability_replies.py) holds every ServerTriggerAbility of a Bard ability in the retail captures
//  39, 41 and 44 and the server's reply to it, item by item with its time. The calls of one capture are replayed in order on one level-25 Bard of
//  the real content.db (the ranks the game grants at 25 and the talents the retail character had) on a simulated clock, each at its own time
//  (so the cooldowns and charges, the Glee that is still running, the Resonance stacks are what they were), and the runtime must
//    - accept it: the 14 golden cases of the research (Fortissimo, Flourish, Shielding, Nimble, Maddening, Misdirection, Saga, Jest, a song, a
//      story) and every other captured trigger of a Bard ability (Mesmerizing, the other songs, the Bard_Song_ abilities),
//    - send the ability item the retail server sent (the ability, its MontageGroup, the montage's static NetGUID, the caster as the target when
//      the call named one) and the Cooldowns item (InitialCooldown, Charges left), each within kTolerance of retail's time and in retail's
//      order (the item, then the cost's packet),
//    - move Mana by retail's amount to the decimal (56.875, 85.3125, 113.75, 199.0625: k x ManaCostMult(25)) and set Bard_Resonance to
//      retail's value (+1; -5 for Fortissimo, which spends five stacks), in the packet of the Cooldowns item,
//    - send the effects of the ability that retail sent (the effects its hits reach: the others in the reply are another ability's: a song's
//      buff still running, the Sprint's pause) with retail's Duration, TickTime and stacks at retail's time; what it does not model yet is
//      listed below, with the reason, and the list is exact: a difference that is not listed fails.
//  Without a content.db the test says so and is skipped.
// ============================================================================
#include "ability_rig.h"
#include "capture_ability_replies.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>

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
// [C_Y_B x C-O-A]

struct RItem {
    std::string              kind;
    int                      ms = 0;
    std::vector<std::string> f;   // the remaining fields
};

std::vector<RItem> parse_reply(const char* text) {
    std::vector<RItem> out;
    std::istringstream in(text);
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

std::map<int64_t, double> parse_before(const char* text) {
    std::map<int64_t, double> out;
    std::istringstream in(text);
    long long rec;
    double v;
    while (in >> rec >> v) out[rec] = v;
    return out;
}

double  to_d(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
int64_t to_i(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }
uint64_t to_u(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }

// The talents the retail Bard had, read off what its replies show: Glee's 0.03 (Merriment), Flourish_Power (Dance_Power) and Jest.
const std::vector<std::string> kTalents = {"Bard_Passive_Merriment", "Bard_Passive_Dance_Power", "Bard_Passive_QuickWit_Jest", "Bard_QuickWit_Jest",
                                           "Bard_Passive_Song_Serenade_Spirited"};

constexpr double kTolerance = 0.20;   // seconds: "within a small tolerance"

// The effects of an ability's own hits that retail sent and the runtime does not (yet), by ability: the effect and why. The list is exact.
struct NotModelled {
    const char* ability;
    const char* effect;
    const char* why;
    bool        absent;   // the runtime does not send it at all (else it sends the first application)
};
const NotModelled kNotModelled[] = {
    {"Bard_Dance_Nimble", "Bard_Dance_Nimble", "retail sends it again every ~0.3 s while the client stands in the zone (its LingerTick requests): the zone's ticks are stage 2; the first application is compared", false},
    {"Bard_Saga", "Bard_Pep", "the Saga is a composite of six sagas whose montages fire different hits at their notifies (the data has no notify times); retail's Joyousness Saga gives Pep 940 ms in, which no data row tells; the other effects of the Saga are compared", true},
};

// every effect the ability can put on its caster: its own, those of its hits, projectiles' and zones' hits, and what those effects fire
void closure(const abilities::Catalog& cat, const abilities::AbilityDef& def, std::set<int64_t>& effects) {
    std::set<int64_t> hits_seen;
    std::vector<int64_t> hits;
    for (const auto& [key, e] : def.effects) effects.insert(e);
    for (const content::AbilityHitRow& h : def.hits) hits.push_back(h.hit_guid);
    while (!hits.empty()) {
        const int64_t g = hits.back();
        hits.pop_back();
        if (!hits_seen.insert(g).second) continue;
        const abilities::HitDef* h = cat.hit(g);
        if (!h) continue;
        for (const content::HitEffectRow& x : h->effects) {
            if (effects.insert(x.effect_guid).second) {
                if (const abilities::EffectDef* e = cat.effect(x.effect_guid)) {
                    for (const auto& [phase, hit] : e->hits) hits.push_back(hit);
                }
            }
        }
    }
}

// ---- the Cooldowns items of a whole capture (plan D2, review) -------------------------------------------------------------------------------

struct ChainItem {
    bool    deleted = false;
    int     ms = 0;
    int64_t ability = 0;
    double  initial = 0;
    int     charges = 0;
    int     start_off = 0, end_off = 0;   // ms from the chain's start
};

std::vector<ChainItem> parse_chain(const char* text) {
    std::vector<ChainItem> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kind;
        ChainItem c;
        ls >> kind >> c.ms >> c.ability;
        c.deleted = kind == "CDD";
        if (!c.deleted) ls >> c.initial >> c.charges >> c.start_off >> c.end_off;
        out.push_back(c);
    }
    return out;
}

/// What the retail server did with the Cooldowns items of an ability that has charges: the first use starts the chain, a second use changes the item
/// (Charges 0, the same start and end), and when a charge returns the item keeps the FIRST use's start and its end moves one cooldown on (Jest: start S,
/// end S + 16, Charges 1); the last charge back deletes it. The replay of the capture's triggers must give the same items: the same Charges, the same
/// offsets from the chain's start, at the same distance in time from the first item, and the same deletions.
void compare_chains(Rig& r, int capture, const std::set<int64_t>& abilities) {
    r.run_to(r.clock + 60.0);   // every cooldown ends
    std::vector<ChainItem> retail;
    for (const auto& ch : capture_ability_replies::kChains) {
        if (ch.capture == capture) retail = parse_chain(ch.items);
    }
    // ours: the same lines, offsets from our chain's start (the first item of the ability since its last deletion), times from the capture's clock (5 s)
    std::map<int64_t, double> chain_start;
    std::vector<ChainItem> mine;
    for (const Seen& s : r.log) {
        for (const auto& cd : s.change.cooldowns_set) {
            if (!abilities.count(cd.ability_guid)) continue;
            if (!chain_start.count(cd.ability_guid)) chain_start[cd.ability_guid] = cd.start_time;
            ChainItem c;
            c.ms = static_cast<int>(std::lround((s.at - 5.0) * 1000.0));
            c.ability = cd.ability_guid;
            c.initial = cd.initial_cooldown;
            c.charges = cd.charges;
            c.start_off = static_cast<int>(std::lround((cd.start_time - chain_start[cd.ability_guid]) * 1000.0));
            c.end_off = static_cast<int>(std::lround((cd.end_time - chain_start[cd.ability_guid]) * 1000.0));
            mine.push_back(c);
        }
        for (const int64_t a : s.change.cooldowns_removed) {
            if (!abilities.count(a)) continue;
            chain_start.erase(a);
            ChainItem c;
            c.deleted = true;
            c.ms = static_cast<int>(std::lround((s.at - 5.0) * 1000.0));
            c.ability = a;
            mine.push_back(c);
        }
    }
    for (const int64_t ability : abilities) {
        std::vector<ChainItem> want, got;
        for (const ChainItem& c : retail)
            if (c.ability == ability) want.push_back(c);
        for (const ChainItem& c : mine)
            if (c.ability == ability) got.push_back(c);
        if (want.empty()) continue;
        const abilities::AbilityDef* def = r.catalog->ability(ability);
        const std::string name = def ? def->row.name : std::to_string(ability);
        // retail's capture may end before the last charge returns: the replay runs on and has more at the end, never fewer
        CHECKF(got.size() >= want.size(), "capture %d, %s: %zu Cooldowns items (retail %zu)", capture, name.c_str(), got.size(), want.size());
        const double first_want = want.front().ms, first_got = got.empty() ? 0.0 : got.front().ms;
        for (size_t i = 0; i < want.size() && i < got.size(); ++i) {
            const ChainItem &w = want[i], &g = got[i];
            CHECKF(w.deleted == g.deleted, "capture %d, %s item %zu: %s (retail %s)", capture, name.c_str(), i, g.deleted ? "a deletion" : "an item", w.deleted ? "a deletion" : "an item");
            if (w.deleted != g.deleted) break;
            CHECKF(std::fabs((g.ms - first_got) - (w.ms - first_want)) <= kTolerance * 1000 + 150,
                   "capture %d, %s item %zu: %.0f ms after the chain's first (retail %.0f)", capture, name.c_str(), i, g.ms - first_got, w.ms - first_want);
            if (w.deleted) continue;
            CHECKF(g.charges == w.charges && g.initial == w.initial, "capture %d, %s item %zu: Charges %d of InitialCooldown %.1f (retail %d of %.1f)", capture, name.c_str(), i,
                   g.charges, g.initial, w.charges, w.initial);
            CHECKF(g.start_off == w.start_off && std::abs(g.end_off - w.end_off) <= 2, "capture %d, %s item %zu: start +%d ms, end +%d ms of the chain (retail +%d, +%d)", capture, name.c_str(), i,
                   g.start_off, g.end_off, w.start_off, w.end_off);
        }
    }
}
// (Cyb + C O A)

}  // namespace

int main() {
    if (!real_db_present()) {
        std::printf("no content.db at %s (cd services && go run ./cmd/contentc): the retail acceptance test is skipped\n", real_db_path().c_str());
        return 0;
    }
    const size_t n_cases = sizeof(capture_ability_replies::kCases) / sizeof(capture_ability_replies::kCases[0]);
    std::printf("the generic ability runtime against %zu retail replies\n", n_cases);
    std::set<int> captures;
    for (const auto& c : capture_ability_replies::kCases) captures.insert(c.capture);
    size_t accepted = 0;
    for (const int capture : captures) {
        Rig r(2, 25, kTalents);
        if (!r.ok()) {
            std::printf("  [FAIL] the rig: %s\n", r.problem.c_str());
            return 1;
        }
        std::vector<int64_t> known;
        for (const auto& c : capture_ability_replies::kCases) {
            if (c.capture == capture) known.push_back(c.ability);
        }
        r.pipe->set_known(known);
        r.pipe->set_data_timings(true);
        const int64_t resonance_effect = r.effect("Bard_Resonance");
        // Saga plays what the Bard has told: it needs three Notes of one theme (Bard_Saga_Any; the retail Saga of capture 44 is the Joyousness
        // one, which asks three of Note_Comedy, the stories' note), which Stories give. The effects of the stories' reach (a party) are stage 2, so
        // the test gives the character of capture 44 the notes it had
        if (capture == 44) CHECK(r.pipe->grant_effect(r.effect("Bard_Note_Comedy"), 3, 1.0), "three Note_Comedy are given to the character of capture 44");
        r.take();
        std::printf("capture %d\n", capture);
        std::string previous;
        for (const capture_ability_replies::Case& c : capture_ability_replies::kCases) {
            if (c.capture != capture) continue;
            const std::string was = previous;
            previous = c.name;
            const std::vector<RItem> reply = parse_reply(c.reply);
            const std::map<int64_t, double> before = parse_before(c.before);
            const abilities::AbilityDef* def = r.catalog->ability(c.ability);
            if (!def) {
                CHECKF(false, "%s: the catalog has the ability", c.name);
                continue;
            }
            const double t0 = 5.0 + c.at_ms / 1000.0 + 0.0123;   // (the clock of the capture; off the regeneration's whole seconds)
            r.run_to(t0 - 0.2);
            // the state the retail character had: Resonance, and Mana to spend (the rig's Bard has less of it, the amounts are what is compared)
            const auto res_it = before.find(kResonanceStat);
            const int want_stacks = res_it == before.end() ? 0 : static_cast<int>(std::lround(res_it->second));
            if (r.pipe->stacks_of(resonance_effect) != want_stacks) {
                r.pipe->remove_effect(resonance_effect, t0 - 0.19);
                if (want_stacks > 0) r.pipe->grant_effect(resonance_effect, want_stacks, t0 - 0.19);
                r.take();
            }
            // Saga is a composite of six sagas whose montages fire different hits at their notifies (the data has no notify times): stage 1 applies the
            // self hits of all of them at the first one, so Glee comes from it, which retail's Saga did not give: the next Jest starts without it unless
            // retail's own earlier Jest left it running
            if (std::string(c.name) == "Bard_Misc_QuickWit_Jest" && was != "Bard_Misc_QuickWit_Jest") r.pipe->remove_effect(r.effect("Bard_Glee"), t0 - 0.19);
            r.take();
            r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
            r.run_to(t0 - 0.001);
            const double mana_before = r.pawn.stat(content::kGuidMana);
            const size_t first_event = r.log.size();
            TriggerInfo info;
            info.animation_index = c.animation_index;
            info.target_count = static_cast<uint32_t>(c.targets);
            const Refusal refusal = r.pipe->trigger(c.ability, c.client_instance_id, t0, info);
            r.take();
            // what the client asked while the cast ran (RequestHitVerification_Server: the hits it wants applied, at its own times)
            if (refusal == Refusal::None) {
                for (const RItem& q : parse_reply(c.requests)) {
                    if (q.kind != "REQ") continue;
                    const double t = t0 + q.ms / 1000.0;
                    r.run_to(t);
                    const auto verdict = r.pipe->hit_request(to_i(q.f[0]), to_i(q.f[1]), to_i(q.f[2]), static_cast<size_t>(to_i(q.f[3])), static_cast<size_t>(to_i(q.f[4])), t);
                    if (std::getenv("AOC_VERBOSE")) std::printf("    %s: request at %d ms (hit %lld): %s%c", c.name, q.ms, static_cast<long long>(to_i(q.f[2])), AbilityPipeline::verdict_name(verdict), 10);
                    r.take();
                }
            }
            r.run_to(t0 + c.window_ms / 1000.0);
            CHECKF(refusal == Refusal::None, "%s (capture %d line %d): the trigger is accepted (%s)", c.name, c.capture, c.line, refusal_name(refusal));
            if (refusal != Refusal::None) continue;
            ++accepted;
            CHECKF(r.pawn.broken.empty(), "%s: the pawn takes every change (%s)", c.name, r.pawn.broken.c_str());
// [C O A :: C | Y | B]

            // what the runtime sent, with the times relative to the trigger
            std::vector<const Seen*> events;
            for (size_t i = first_event; i < r.log.size(); ++i) events.push_back(&r.log[i]);
            const bootstrap::ActiveAbilityItem* my_item = nullptr;
            double my_item_at = 0;
            const bootstrap::CooldownItem* my_cd = nullptr;
            double my_cd_at = -1;
            double my_mana_after = mana_before, my_mana_at = -1;
            for (const Seen* e : events) {
                if (!my_item && !e->change.abilities_added.empty()) {
                    my_item = &e->change.abilities_added[0];
                    my_item_at = e->at - t0;
                }
                if (!my_cd) {
                    for (const auto& cd : e->change.cooldowns_set) {
                        if (cd.ability_guid == c.ability) {
                            my_cd = &cd;
                            my_cd_at = e->at - t0;
                        }
                    }
                }
                for (const auto& s : e->change.stats) {
                    if (s.record == content::kGuidMana && my_mana_at < 0 && s.value < mana_before) {
                        my_mana_after = s.value;
                        my_mana_at = e->at - t0;
                    }
                }
            }

            // the ability item
            for (const RItem& it : reply) {
                if (it.kind != "AB" || to_i(it.f[0]) != c.ability || to_u(it.f[3]) == 0) continue;   // the item that carries the montage
                const double ms = it.ms / 1000.0;
                CHECKF(my_item != nullptr, "%s: the runtime sends the ability item", c.name);
                if (!my_item) break;
                CHECKF(my_item->ability_guid == c.ability && my_item->montage_group == it.f[2], "%s: the item's ability and MontageGroup %s (%s)", c.name, it.f[2].c_str(), my_item->montage_group.c_str());
                CHECKF(my_item->montage_guid == to_u(it.f[3]), "%s: the montage's NetGUID %llu (%llu)", c.name, static_cast<unsigned long long>(to_u(it.f[3])),
                       static_cast<unsigned long long>(my_item->montage_guid));
                CHECKF(my_item->targets_self == (to_i(it.f[5]) > 0), "%s: the caster is the item's target when the call named one", c.name);
                CHECKF(std::fabs(my_item_at - ms) <= kTolerance, "%s: the item at %.0f ms (retail %.0f)", c.name, my_item_at * 1000, ms * 1000);
                break;
            }
            // the Cooldowns item, and that the cost's packet is not before the item (retail: the item first or in the same packet)
            for (const RItem& it : reply) {
                if (it.kind != "CD" || to_i(it.f[0]) != c.ability) continue;
                const double ms = it.ms / 1000.0;
                CHECKF(my_cd != nullptr, "%s: the runtime sends the Cooldowns item", c.name);
                if (!my_cd) break;
                CHECKF(my_cd->triggered && my_cd->initial_cooldown == static_cast<float>(to_d(it.f[1])), "%s: InitialCooldown %.1f (%.1f)", c.name, to_d(it.f[1]), my_cd->initial_cooldown);
                CHECKF(my_cd->charges == to_i(it.f[2]), "%s: Charges %lld left (%d)", c.name, static_cast<long long>(to_i(it.f[2])), my_cd->charges);
                CHECKF(std::fabs(my_cd_at - ms) <= kTolerance, "%s: the Cooldowns item at %.0f ms (retail %.0f)", c.name, my_cd_at * 1000, ms * 1000);
                CHECKF(!my_item || my_item_at <= my_cd_at + 1e-9, "%s: the ability item comes before or with the Cooldowns item", c.name);
                break;
            }
            // Mana: retail's step at the Cooldowns item's time, from the Mana retail had just before it
            {
                int cd_ms = -1;
                for (const RItem& it : reply) {
                    if (it.kind == "CD" && to_i(it.f[0]) == c.ability) cd_ms = it.ms;
                }
                const RItem* step = nullptr;
                double retail_before = before.count(content::kGuidMana) ? before.at(content::kGuidMana) : -1;
                if (cd_ms >= 0) {
                    for (const RItem& it : reply) {
                        if (it.kind != "STAT" || to_i(it.f[1]) != content::kGuidMana) continue;
                        if (it.ms == cd_ms) {
                            step = &it;
                            break;
                        }
                        if (it.ms < cd_ms) retail_before = to_d(it.f[2]);
                    }
                }
                if (step && retail_before >= 0) {
                    const double retail_delta = retail_before - to_d(step->f[2]);
                    CHECKF(retail_delta > 1, "%s: retail's step is a cost (%.4f)", c.name, retail_delta);
                    CHECKF(my_mana_at >= 0 && std::fabs((mana_before - my_mana_after) - retail_delta) < 1e-3, "%s: Mana falls by %.4f (%.4f)", c.name, retail_delta,
                           mana_before - my_mana_after);
                    CHECKF(my_mana_at >= 0 && std::fabs(my_mana_at * 1000 - step->ms) <= kTolerance * 1000, "%s: Mana moves at %.0f ms (retail %d)", c.name, my_mana_at * 1000, step->ms);
                }
            }
            // Bard_Resonance: retail's value in the cost's packet
            for (const RItem& it : reply) {
                if (it.kind != "STAT" || to_i(it.f[1]) != kResonanceStat) continue;
                CHECKF(std::fabs(r.pawn.stat(kResonanceStat) - to_d(it.f[2])) < 1e-6, "%s: Bard_Resonance %.0f (%.0f)", c.name, to_d(it.f[2]), r.pawn.stat(kResonanceStat));
                break;
            }
            // the ability's effects
            std::set<int64_t> reach;
            closure(*r.catalog, *def, reach);
            reach.insert(resonance_effect);
            std::set<int64_t> first_seen;
            int cost_ms = 0;
            for (const RItem& it : reply) {
                if (it.kind == "CD" && to_i(it.f[0]) == c.ability) cost_ms = it.ms;
            }
            for (const RItem& it : reply) {
                if (it.kind != "EFF") continue;
                const int64_t guid = to_i(it.f[0]);
                if (!reach.count(guid)) continue;                    // another ability's: a song's buff still running, the Sprint's pause
                if (!first_seen.insert(guid).second) continue;       // a zone's again (stage 2): the first application is compared
                if (it.ms > cost_ms + 800) continue;                 // later than the cast's own steps (a tick, a refresh)
                const abilities::EffectDef* ed = r.catalog->effect(guid);
                const std::string name = ed ? ed->row.name : std::to_string(guid);
                bool matched = false, produced = false;
                for (const Seen* e : events) {
                    for (const auto* list : {&e->change.effects_added, &e->change.effects_changed}) {
                        for (const auto& x : *list) {
                            if (x.effect_guid != guid) continue;
                            produced = true;
                            const bool inf = to_d(it.f[1]) > 1e30;
                            // (a refresh that extends the effect shows the remaining time and the full one: its start is the cast's, so within the tolerance)
                            const bool extends = ed && ed->row.stack_duration_refresh.find("ExtendDuration") != std::string::npos;
                            const bool same = inf ? x.duration > 1e30f : std::fabs(x.duration - to_d(it.f[1])) < (extends ? kTolerance : 1e-3);
                            if (same && std::fabs(x.tick_time - to_d(it.f[2])) < 1e-4 && x.stacks == to_i(it.f[3]) && std::fabs((e->at - t0) * 1000 - it.ms) <= kTolerance * 1000)
                                matched = true;
                        }
                    }
                }
                bool listed = false, absent_ok = false;
                for (const NotModelled& n : kNotModelled) {
                    if (std::string(n.ability) == c.name && name == n.effect) {
                        listed = true;
                        absent_ok = n.absent;
                    }
                }
                if (listed) {
                    CHECKF(matched || produced || absent_ok, "%s: %s (listed as not modelled beyond its first application) is sent once", c.name, name.c_str());
                    continue;
                }
                CHECKF(matched, "%s: the effect %s with duration %s, tick %s and %s stack(s) at about %d ms%s", c.name, name.c_str(), it.f[1].c_str(), it.f[2].c_str(),
                       it.f[3].c_str(), it.ms, produced ? " (the runtime sends it differently)" : " (the runtime does not send it)");
            }
        }
        std::set<int64_t> abilities;
        for (const int64_t a : known) abilities.insert(a);
        compare_chains(r, capture, abilities);
    }
    std::printf("%zu of %zu retail triggers accepted\n", accepted, n_cases);
    CHECK(accepted == n_cases, "every captured trigger of a Bard ability is accepted");
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

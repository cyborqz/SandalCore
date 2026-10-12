// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_effects.cpp: effect state, expression programs and their host (plan D2) against the shared corpus
//  services/internal/statprog writes (stats/testdata/corpus.txt, the fxcase blocks):
//    - every fxcase gives the same stats as the Go engine, bit for bit, with its effects and ranks;
//    - every expression program gives the Go engine's value, bit for bit, with the host the case describes (the
//      SplitMix source runs on through the prog lines, so the order of the random draws is checked too);
//  and a few things of its own: the three forms of an effect mod by hand, the retail values of the research, what the
//  effects answer the state functions, the inputs the engine refuses, the programs the evaluator refuses, SplitMix.
// ============================================================================
#include "stats/stat_engine.h"
#include "stats/stat_program.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace aoc::worldd::stats;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

#ifndef STATS_CORPUS_PATH
#define STATS_CORPUS_PATH "server/worldd/stats/testdata/corpus.txt"
#endif

namespace {
// CoA x C_Y_B

std::vector<uint8_t> base64_decode(const std::string& s) {
    static int8_t t[256];
    static bool init = false;
    if (!init) {
        std::memset(t, -1, sizeof(t));
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        init = true;
    }
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        int8_t v = t[static_cast<uint8_t>(c)];
        if (v < 0) return {};
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

double bits_to_double(uint64_t b) {
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
uint64_t double_bits(double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}
uint64_t hex64(const std::string& s) { return std::stoull(s, nullptr, 16); }

// TableState is the StatState of a case's "state" lines.
class TableState : public StatState {
public:
    std::map<std::pair<uint64_t, int64_t>, double> effects;
    std::map<std::string, double> tags;
    std::set<std::tuple<uint64_t, int64_t, uint64_t, int64_t>> skills;
    bool combat = false, moving_ = false, unkillable_ = false;
    double actor = 0, npc = 0;
    double dist[8] = {};
    std::map<std::string, double> professions;
    std::map<std::pair<uint64_t, int64_t>, double> consumed;

    double effect_count(const RecordRef& r) const override {
        auto it = effects.find({r.type_id, r.guid});
        return it == effects.end() ? 0 : it->second;
    }
    double effect_count_by_tag(const std::string& t) const override {
        auto it = tags.find(t);
        return it == tags.end() ? 0 : it->second;
    }
    bool skill_purchased(const RecordRef& c, const RecordRef& s) const override {
        return skills.count({c.type_id, c.guid, s.type_id, s.guid}) != 0;
    }
    bool in_combat() const override { return combat; }
    bool moving() const override { return moving_; }
    bool unkillable() const override { return unkillable_; }
    double actor_guid() const override { return actor; }
    double profession_level(double, const std::string& p) const override {
        auto it = professions.find(p);
        return it == professions.end() ? 1 : it->second;
    }
    double consumed_item_stat(const RecordRef& r) const override {
        auto it = consumed.find({r.type_id, r.guid});
        return it == consumed.end() ? 0 : it->second;
    }
    double npc_elite_rank() const override { return npc; }
    double npc_stat_distribution(int i) const override { return i >= 0 && i < 8 ? dist[i] : 0; }
};

// TableHost is the ExprHost of a case's "host" lines.
class TableHost : public ExprHost {
public:
    SplitMix rng;
    std::map<std::string, double> progress;
    std::map<std::pair<uint64_t, int64_t>, double> inst;
    std::set<std::string> tags, flags;
    double targets = 0, xp_debt = 0;
    std::map<std::string, double> fns;   // by "FN|NAME"
    double host_fn(int fn, const std::string& name, const double* args, int n) override {
        const auto it = fns.find(std::to_string(fn) + "|" + name);
        double v = it == fns.end() ? 0 : it->second;
        if (fn >= kHostNodeBiome && fn <= kHostNodeLast) return v;   // a node read's answer is a rank or an ordinal: the numbers it is given are not added to it
        for (int i = 0; i < n; ++i) v += args[i];
        return v;
    }
    double ability_progress(const std::string& g) override {
        auto it = progress.find(g);
        return it == progress.end() ? 0 : it->second;
    }
    double effect_count_by_instigator(const RecordRef& r) override {
        auto it = inst.find({r.type_id, r.guid});
        return it == inst.end() ? 0 : it->second;
    }
    double rand_double() override { return rng.rand_double(); }
    bool hit_has_tag(const std::string& t) override { return tags.count(t) != 0; }
    bool hit_has_flag(const std::string& f) override { return flags.count(f) != 0; }
    double hit_targets() override { return targets; }
    double xp_debt_ratio() override { return xp_debt; }
};

// Another actor of a case's hit (the corpus's "actor" blocks): the inputs and state it runs with, by role.
struct FxActor {
    int role = 0;
    StatInputs in;
    TableState state;
};

struct FxCase {
    std::string name, bundle;
    StatInputs in;
    TableState state;
    std::vector<std::shared_ptr<FxActor>> actors;
    TableHost host;
    std::vector<std::pair<int64_t, uint64_t>> want;
    std::vector<std::pair<int64_t, uint64_t>> progs;  // program index, value bits
    struct Retail {
        bool is_prog;      // a program's value exactly, or a stat's rounded to `digits` decimals
        int64_t id;
        double value;
        int digits;
    };
    std::vector<Retail> retail;
    uint64_t digest = 0;
    size_t members = 0;
    int faults = 0, ignored = 0, ignored_fx = 0, eval_faults = 0;
};
// Cyb - C-O-A

struct Corpus {
    std::map<std::string, std::vector<uint8_t>> bundles;
    std::vector<std::pair<std::string, std::string>> derived;  // a bundle switched to double arithmetic
    std::vector<FxCase> cases;
};

bool load_corpus(const std::string& path, Corpus& c, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::string line, cur_bundle, b64;
    size_t bundle_len = 0, line_no = 0;
    FxCase* cur = nullptr;
    StatInputs* in_t = nullptr;   // where the lines of the inputs go: the case's, or the actor's inside an actor block
    TableState* st_t = nullptr;
    bool in_actor = false;
    bool skipping = false;  // a plain case: test_worldd_stats reads those
    while (std::getline(f, line)) {
        ++line_no;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::vector<std::string> w;
        for (std::string x; is >> x;) w.push_back(x);
        auto bad = [&](const char* m) {
            err = "line " + std::to_string(line_no) + ": " + m;
            return false;
        };
        if (!cur_bundle.empty()) {
            if (w[0] == "endbundle") {
                std::vector<uint8_t> raw = base64_decode(b64);
                if (raw.size() != bundle_len) return bad("bundle length");
                c.bundles[cur_bundle] = std::move(raw);
                cur_bundle.clear();
            } else {
                b64 += w[0];
            }
            continue;
        }
        const std::string& k = w[0];
        if (skipping) {
            if (k == "end") skipping = false;
            continue;
        }
        if (k == "bundle") {
            if (w.size() != 3) return bad("bundle NAME LEN");
            cur_bundle = w[1];
            bundle_len = std::stoul(w[2]);
            b64.clear();
        } else if (k == "derive") {
            if (w.size() != 4) return bad("derive");
            c.derived.push_back({w[1], w[2]});
        } else if (k == "case" || k == "nodehost") {   // a plain case is test_worldd_stats's; a nodehost block is test_worldd_city_nodes's
            skipping = true;
        } else if (k == "fxcase") {
            if (w.size() != 5) return bad("fxcase");
            c.cases.emplace_back();
            cur = &c.cases.back();
            cur->name = w[1];
            cur->bundle = w[2];
            cur->in.class_enum = std::stoi(w[3]);
            cur->in.level = std::stoi(w[4]);
            in_t = &cur->in;
            st_t = &cur->state;
        } else if (k == "end") {
            cur = nullptr;
        } else if (k == "actor" && cur) {
            if (w.size() != 4) return bad("actor ROLE CLASS LEVEL");
            cur->actors.push_back(std::make_shared<FxActor>());
            FxActor& a = *cur->actors.back();
            a.role = std::stoi(w[1]);
            a.in.class_enum = std::stoi(w[2]);
            a.in.level = std::stoi(w[3]);
            in_t = &a.in;
            st_t = &a.state;
            in_actor = true;
        } else if (k == "endactor" && cur) {
            in_t = &cur->in;
            st_t = &cur->state;
            in_actor = false;
        } else if (cur) {
            if (k == "mod") in_t->mods.push_back({std::stoll(w[1]), bits_to_double(hex64(w[2]))});
            else if (k == "tag") in_t->equipped.push_back({w[1], w[2]});
            else if (k == "want") cur->want.push_back({std::stoll(w[1]), hex64(w[2])});
            else if (k == "digest") {
                cur->digest = hex64(w[1]);
                cur->members = std::stoul(w[2]);
                cur->faults = std::stoi(w[3]);
                cur->ignored = std::stoi(w[4]);
            } else if (k == "fx") {
                ActiveEffect e;
                e.effect = {std::stoull(w[1]), std::stoll(w[2])};
                e.stacks = std::stoi(w[3]);
                in_t->effects.push_back(std::move(e));
            } else if (k == "fxtag") {
                in_t->effects.back().tags.push_back(w[1]);
            } else if (k == "fxmod") {
                in_t->effects.back().mods.push_back({std::stoll(w[1]), bits_to_double(hex64(w[2]))});
            } else if (k == "rank") {
                in_t->ranks.push_back({{std::stoull(w[1]), std::stoll(w[2])}, {std::stoull(w[3]), std::stoll(w[4])}});
            } else if (k == "ignoredfx") {
                cur->ignored_fx = std::stoi(w[1]);
            } else if (k == "evalfaults") {
                cur->eval_faults = std::stoi(w[1]);
            } else if (k == "host") {
                if (w[1] == "seed") cur->host.rng.state = std::stoull(w[2]);
                else if (w[1] == "progress") cur->host.progress[w[2] == "-" ? std::string() : w[2]] = bits_to_double(hex64(w[3]));
                else if (w[1] == "inst") cur->host.inst[{std::stoull(w[2]), std::stoll(w[3])}] = bits_to_double(hex64(w[4]));
                else if (w[1] == "hittag") cur->host.tags.insert(w[2]);
                else if (w[1] == "hitflag") cur->host.flags.insert(w[2]);
                else if (w[1] == "targets") cur->host.targets = bits_to_double(hex64(w[2]));
                else if (w[1] == "xpdebt") cur->host.xp_debt = bits_to_double(hex64(w[2]));
                else if (w[1] == "fn") cur->host.fns[w[2] + "|" + (w[3] == "-" ? std::string() : w[3])] = bits_to_double(hex64(w[4]));
                else return bad("unknown host line");
            } else if (k == "prog") {
                cur->progs.push_back({std::stoll(w[1]), hex64(w[2])});
            } else if (k == "retail") {
                if (w[1] == "prog") cur->retail.push_back({true, std::stoll(w[2]), bits_to_double(hex64(w[3])), 0});
                else if (w[1] == "stat") cur->retail.push_back({false, std::stoll(w[2]), bits_to_double(hex64(w[3])), std::stoi(w[4])});
                else return bad("unknown retail line");
            } else if (k == "state") {
                TableState& s = *st_t;
                const std::string& what = w[1];
                if (what == "effect") s.effects[{std::stoull(w[2]), std::stoll(w[3])}] = bits_to_double(hex64(w[4]));
                else if (what == "tag") s.tags[w[2]] = bits_to_double(hex64(w[3]));
                else if (what == "skill") s.skills.insert({std::stoull(w[2]), std::stoll(w[3]), std::stoull(w[4]), std::stoll(w[5])});
                else if (what == "combat") s.combat = true;
                else if (what == "moving") s.moving_ = true;
                else if (what == "unkillable") s.unkillable_ = true;
                else if (what == "actor") s.actor = bits_to_double(hex64(w[2]));
                else if (what == "prof") s.professions[w[2]] = bits_to_double(hex64(w[3]));
                else if (what == "consumed") s.consumed[{std::stoull(w[2]), std::stoll(w[3])}] = bits_to_double(hex64(w[4]));
                else if (what == "npc") s.npc = bits_to_double(hex64(w[2]));
                else if (what == "dist") {
                    const int i = std::stoi(w[2]);
                    if (i < 0 || i >= 8) return bad("state dist: there are 8 stats");
                    s.dist[i] = bits_to_double(hex64(w[3]));
                } else return bad("unknown state line");
            } else {
                return bad("unknown fxcase line");
            }
        }
        // other top-level lines (derive, refuse, fuzz, hostile) are test_worldd_stats's
    }
    if (!cur_bundle.empty() || cur || skipping || in_actor) {
        err = "unterminated bundle or case";
        return false;
    }
    return true;
}
// C | O | A : Cyb

// The digest of a result: FNV-1a 64 as statprog.Digest.
uint64_t digest(const StatResult& r) {
    uint64_t h = 14695981039346656037ull;
    auto put = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    auto u64 = [&](uint64_t v) {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
        put(b, 8);
    };
    for (const StatOut& s : r.stats) {
        u64(static_cast<uint64_t>(s.guid));
        u64(double_bits(s.value));
        u64(double_bits(s.base));
        u64(double_bits(s.equip));
        if (s.has_regen) {
            const uint8_t one = 1;
            put(&one, 1);
            u64(double_bits(s.regen));
        } else {
            const uint8_t zero = 0;
            put(&zero, 1);
        }
    }
    return h;
}

void test_corpus(Corpus& c, std::map<std::string, StatBundle>& bundles) {
    std::printf("bundles\n");
    for (const auto& [name, raw] : c.bundles) {
        StatBundle b;
        std::string err;
        const bool ok = parse_bundle(raw, b, err);
        CHECKF(ok, "bundle %s: %s", name.c_str(), err.c_str());
        if (ok) bundles[name] = std::move(b);
    }
    for (const auto& [name, of] : c.derived) {
        auto it = c.bundles.find(of);
        CHECKF(it != c.bundles.end(), "derive %s: no bundle %s", name.c_str(), of.c_str());
        if (it == c.bundles.end()) continue;
        std::vector<uint8_t> raw = it->second;
        raw[6] = 2;  // FlagDouble
        raw[7] = 0;
        StatBundle b;
        std::string err;
        const bool ok = parse_bundle(raw, b, err);
        CHECKF(ok && b.mode == Mode::Double, "derived bundle %s: %s", name.c_str(), err.c_str());
        if (ok) bundles[name] = std::move(b);
    }
    std::printf("fxcases\n");
    size_t ncase = 0, nwant = 0, nprog = 0, nretail = 0, nactors = 0;
    for (FxCase& cs : c.cases) {
        auto it = bundles.find(cs.bundle);
        CHECKF(it != bundles.end(), "fxcase %s: no bundle %s", cs.name.c_str(), cs.bundle.c_str());
        if (it == bundles.end()) continue;
        const StatBundle& b = it->second;
        StatResult res;
        std::string err;
        const bool ran = run_stats(b, cs.in, cs.state, res, err);
        CHECKF(ran, "fxcase %s: %s", cs.name.c_str(), err.c_str());
        if (!ran) continue;
        ++ncase;
        CHECKF(res.stats.size() == cs.members && res.faults == cs.faults && res.ignored_mods == cs.ignored && res.ignored_effect_mods == cs.ignored_fx,
               "fxcase %s: %zu stats, %d faults, %d ignored mods, %d ignored effect mods; the corpus has %zu, %d, %d, %d", cs.name.c_str(),
               res.stats.size(), res.faults, res.ignored_mods, res.ignored_effect_mods, cs.members, cs.faults, cs.ignored, cs.ignored_fx);
        bool values_ok = cs.want.empty() || cs.want.size() == res.stats.size();
        for (size_t i = 0; values_ok && i < cs.want.size(); ++i) {
            ++nwant;
            if (cs.want[i].first != res.stats[i].guid || cs.want[i].second != double_bits(res.stats[i].value)) {
                values_ok = false;
                std::printf("    stat %lld is %016llx, Go has %016llx\n", static_cast<long long>(res.stats[i].guid),
                            static_cast<unsigned long long>(double_bits(res.stats[i].value)), static_cast<unsigned long long>(cs.want[i].second));
            }
        }
        CHECKF(values_ok, "fxcase %s: the values differ from the Go engine's", cs.name.c_str());
        CHECKF(digest(res) == cs.digest, "fxcase %s: digest %016llx, Go has %016llx", cs.name.c_str(),
               static_cast<unsigned long long>(digest(res)), static_cast<unsigned long long>(cs.digest));
        CHECKF(res.values.size() == b.stats.size(), "fxcase %s: the run holds every stat's value", cs.name.c_str());
        for (const FxCase::Retail& r : cs.retail) {
            if (r.is_prog) continue;
            const auto at = b.by_guid.find(r.id);
            const double p = std::pow(10.0, r.digits);
            CHECKF(at != b.by_guid.end() && std::round(res.values[at->second] * p) / p == r.value, "fxcase %s: the retail value %g of stat %lld", cs.name.c_str(), r.value,
                   static_cast<long long>(r.id));
            ++nretail;
        }
        if (!cs.progs.empty()) {
            ExprEvaluator ev(b, cs.in, cs.state, res);
            CHECKF(ev.ok(), "fxcase %s: %s", cs.name.c_str(), ev.error().c_str());
            if (!ev.ok()) continue;
            // the other actors of the hit, each run on its own (their results live as long as the evaluator is used)
            std::vector<StatResult> actor_results(cs.actors.size());
            bool actors_ok = true;
            for (size_t i = 0; i < cs.actors.size(); ++i) {
                FxActor& a = *cs.actors[i];
                std::string aerr;
                const bool aran = run_stats(b, a.in, a.state, actor_results[i], aerr) && ev.set_actor(a.role, a.in, a.state, actor_results[i], aerr);
                CHECKF(aran, "fxcase %s: actor %d: %s", cs.name.c_str(), a.role, aerr.c_str());
                actors_ok = actors_ok && aran;
            }
            if (!actors_ok) continue;
            nactors += cs.actors.size();
            std::map<int64_t, double> value_of;
            for (const auto& [prog, want] : cs.progs) {
                double v = 0;
                std::string e;
                const bool good = ev.eval(prog, cs.host, v, e);
                value_of[prog] = v;
                ++nprog;
                CHECKF(good && double_bits(v) == want, "fxcase %s: program %lld gives %016llx (%s), Go has %016llx", cs.name.c_str(), static_cast<long long>(prog),
                       static_cast<unsigned long long>(double_bits(v)), e.c_str(), static_cast<unsigned long long>(want));
            }
            CHECKF(ev.faults() == cs.eval_faults, "fxcase %s: %d evaluation faults, Go has %d", cs.name.c_str(), ev.faults(), cs.eval_faults);
            for (const FxCase::Retail& r : cs.retail) {
                if (!r.is_prog) continue;
                const auto at = value_of.find(r.id);
                CHECKF(at != value_of.end() && at->second == r.value, "fxcase %s: the retail value %g of program %lld", cs.name.c_str(), r.value, static_cast<long long>(r.id));
                ++nretail;
            }
        }
    }
    std::printf("  %zu fxcases, %zu values, %zu expression results and %zu retail numbers checked\n", ncase, nwant, nprog, nretail);
    CHECK(ncase >= 14, "the corpus has its fxcases");
    CHECK(nactors >= 6, "the corpus has the other actors of a hit");
    CHECK(nprog >= 3000, "the corpus has its expression results");
    CHECK(nretail >= 12, "the corpus has the retail numbers: the Mana costs, the Sprint, Glee and Note_Comedy");
}

// ---- by hand, on the tiny graph (the guids are statprog.TinyGraph's: 1000 + n) ----------------------------------------------
// ~ C_Y_B :: CoA ~

constexpr uint64_t kEffectType = 3377240744094973029ull, kClassType = 297887639323616934ull, kSkillType = 4146438674247114715ull;

RecordRef effect_ref(int64_t g) { return {kEffectType, g}; }

const StatOut* find(const StatResult& r, int n) {
    for (const StatOut& s : r.stats)
        if (s.guid == 1000 + n) return &s;
    return nullptr;
}

double f32(double x) { return static_cast<double>(static_cast<float>(x)); }

void test_forms_by_hand(const StatBundle& b) {
    std::printf("the three forms of an effect mod\n");
    StatInputs in;
    in.class_enum = 2;
    in.level = 20;
    StatResult bare, res;
    std::string err;
    CHECK(run_stats(b, in, StatState{}, bare, err), "a bare Bard");
    ActiveEffect e1;
    e1.effect = effect_ref(7);
    e1.stacks = 2;
    e1.mods = {{1019, 0.25}, {1020, 1.05}, {1017, 0.1}};
    ActiveEffect e2;
    e2.effect = effect_ref(8);
    e2.stacks = 1;
    e2.mods = {{1019, 0.125}, {1020, 1.1}, {1017, 0.5}, {424242, 7}};
    ActiveEffect e3;
    e3.effect = effect_ref(9);
    e3.stacks = 0;
    e3.mods = {{1019, 100}};
    in.effects = {e1, e2, e3};
    CHECK(run_stats(b, in, StatState{}, res, err), "with effects");
    CHECKF(res.ignored_effect_mods == 1 && res.faults == bare.faults, "a mod for a stat the bundle does not have is ignored and counted, and the effects add no fault (%d ignored, %d faults, bare %d)", res.ignored_effect_mods, res.faults, bare.faults);
    const double speed0 = find(bare, 19)->value, heal0 = find(bare, 20)->value, compl0 = find(bare, 17)->value;
    CHECK(speed0 == 1 && heal0 == f32(1 + 80.0 / 4000.0) && compl0 == f32(1 - 80.0 / 1000.0), "the bare values");
    CHECK(find(res, 19)->value == f32(1 + 0.25 * 2 + 0.125), "an additive stat adds the mod times the stacks");
    CHECK(find(res, 20)->value == f32(heal0 * 1.05 * 1.05 * 1.1), "a multiplying stat multiplies once per stack");
    CHECK(find(res, 17)->value == f32(compl0 * (1 - 0.1) * (1 - 0.1) * (1 - 0.5)), "a complement stat multiplies by one minus the mod, once per stack");
    CHECK(find(res, 1)->value == find(bare, 1)->value && find(res, 3)->value == find(bare, 3)->value, "a stat no effect names is untouched");

    // the retail values: Mitigation 0.9652 -> 0.9362 under 0.03 (x (1 - v)); HealingMult 1.1025 -> 1.1576 under 1.05 (x v)
    StatInputs m;
    m.class_enum = 2;
    m.level = 20;
    m.mods = {{1017, 0.9652 - f32(1 - 80.0 / 1000.0)}};
    ActiveEffect glee;
    glee.effect = effect_ref(1);
    glee.stacks = 1;
    glee.mods = {{1017, 0.03}};
    StatResult before, after;
    CHECK(run_stats(b, m, StatState{}, before, err) && std::fabs(find(before, 17)->value - 0.9652) < 1e-6, "Mitigation 0.9652");
    m.effects = {glee};
    CHECK(run_stats(b, m, StatState{}, after, err) && std::round(find(after, 17)->value * 1e4) / 1e4 == 0.9362 &&
              std::fabs(find(after, 17)->value - 0.9652 * 0.97) < 1e-6, "...is 0.9362 under 0.03");
    StatInputs h;
    h.class_enum = 2;
    h.level = 20;
    h.mods = {{1020, 1.1025 - f32(1 + 80.0 / 4000.0)}};
    CHECK(run_stats(b, h, StatState{}, before, err) && std::fabs(find(before, 20)->value - 1.1025) < 1e-6, "HealingMult 1.1025");
    ActiveEffect comedy;
    comedy.effect = effect_ref(2);
    comedy.stacks = 1;
    comedy.mods = {{1020, 1.05}};
    h.effects = {comedy};
    CHECK(run_stats(b, h, StatState{}, after, err) && std::round(find(after, 20)->value * 1e4) / 1e4 == 1.1576 &&
              std::fabs(find(after, 20)->value - 1.1025 * 1.05) < 1e-6, "...is 1.1576 under 1.05");

    // the limits bind the result
    StatInputs big;
    big.class_enum = 2;
    big.level = 20;
    ActiveEffect many;
    many.effect = effect_ref(1);
    many.stacks = kMaxEffectStacks;
    many.mods = {{1019, 1}, {1017, 0.999}};
    big.effects = {many};
    CHECK(run_stats(b, big, StatState{}, res, err) && find(res, 19)->value == 10 && find(res, 17)->value >= 0 && find(res, 17)->value < 1e-30,
          "Speed stops at its maximum 10, a complement after 4096 stacks of 0.999 is near 0");

    // effects feed what reads the stat
    StatInputs feed;
    feed.class_enum = 1;
    feed.level = 20;
    StatResult nofx, withfx;
    ActiveEffect str;
    str.effect = effect_ref(1);
    str.stacks = 1;
    str.mods = {{1001, 10}};
    CHECK(run_stats(b, feed, StatState{}, nofx, err), "a Ranger");
    feed.effects = {str};
    CHECK(run_stats(b, feed, StatState{}, withfx, err) && find(withfx, 1)->value == find(nofx, 1)->value + 10 && find(withfx, 2)->value != find(nofx, 2)->value,
          "Str + 10 moves Dex, which reads it");
    CHECK(find(withfx, 1)->equip == 0 && find(withfx, 1)->base == find(nofx, 1)->base, "the equipment and base of a stat are not the effects'");
}
// Cyb - C O A

void test_state_functions(const StatBundle& b) {
    std::printf("what the effects answer the state functions\n");
    StatInputs in;
    in.class_enum = 1;
    in.level = 20;
    StatResult res;
    std::string err;
    CHECK(run_stats(b, in, StatState{}, res, err) && find(res, 7)->value == 10000, "Ctx with no state: Fishing level 1 -> 10000");
    ActiveEffect a, c, d;
    a.effect = effect_ref(1);
    a.stacks = 3;
    a.tags = {"pet.summon.wolf"};
    c.effect = effect_ref(5);
    c.stacks = 4;
    c.tags = {"pet.summon", "other"};
    d.effect = effect_ref(6);
    d.stacks = 5;
    d.tags = {"pet.summoner", "pet"};  // a sibling and the parent: neither matches
    in.effects = {a, c, d};
    in.ranks = {{{kClassType, 2}, {kSkillType, 3}}};
    CHECK(run_stats(b, in, StatState{}, res, err) && find(res, 7)->value == 10000.0 + 6 + 7 + 100, "stacks of the record, of a tag (and its descendants), and a held rank");
    in.ranks = {{{kClassType, 2}, {kSkillType, 4}}};
    CHECK(run_stats(b, in, StatState{}, res, err) && find(res, 7)->value == 10000.0 + 6 + 7, "a rank of another skill is nothing");
    TableState st;
    st.effects[{kEffectType, 1}] = 3;
    st.tags["pet.summon"] = 2;
    in.ranks = {{{kClassType, 2}, {kSkillType, 3}}};
    CHECK(run_stats(b, in, st, res, err) && find(res, 7)->value == 10000.0 + 2 * (3 + 3) + (7 + 2) + 100, "the state's counts are added to the effects'");
}

void test_refusals(const StatBundle& b) {
    std::printf("inputs the engine refuses, programs the evaluator refuses\n");
    const StatState none;  // the evaluators keep it by reference
    StatInputs in;
    in.class_enum = 1;
    in.level = 1;
    StatResult res;
    std::string err;
    ActiveEffect e;
    e.effect = effect_ref(1);
    e.stacks = -1;
    in.effects = {e};
    CHECK(!run_stats(b, in, StatState{}, res, err), "negative stacks");
    in.effects[0].stacks = kMaxEffectStacks + 1;
    CHECK(!run_stats(b, in, StatState{}, res, err), "too many stacks");
    in.effects.assign(kMaxActiveEffects + 1, ActiveEffect{});
    CHECK(!run_stats(b, in, StatState{}, res, err), "too many effects");
    ActiveEffect heavy;
    heavy.effect = effect_ref(1);
    heavy.stacks = kMaxEffectStacks;
    heavy.mods.assign(300, EffectMod{1020, 1});
    in.effects.assign(1000, heavy);
    CHECK(!run_stats(b, in, StatState{}, res, err), "stacks times mods past the bound");
    ActiveEffect top;
    top.effect = effect_ref(1);
    top.stacks = kMaxEffectStacks;
    top.mods = {{1020, 1.0000001}};
    in.effects = {top};
    CHECK(run_stats(b, in, StatState{}, res, err), "the most stacks, and one mod, is taken");

    in.effects.clear();
    CHECK(run_stats(b, in, StatState{}, res, err), "a run");
    in.level = 20;
    ExprEvaluator ev(b, in, none, res);
    CHECK(ev.ok(), "an evaluator");
    double v = 0;
    std::string e2;
    TableHost host;
    CHECK(!ev.eval(b.stats[1].base, host, v, e2) && !e2.empty(), "a stat formula is not an expression");
    CHECK(!ev.eval(-1, host, v, e2) && !ev.eval(static_cast<int64_t>(b.programs.size()), host, v, e2), "programs outside the bundle");
    StatResult empty;
    ExprEvaluator no_values(b, in, none, empty);
    CHECK(!no_values.ok() && !no_values.eval(static_cast<int64_t>(b.exprs[0]), host, v, e2), "an evaluator with no values refuses");
    StatInputs lvl0 = in;
    lvl0.level = 0;
    CHECK(!ExprEvaluator(b, lvl0, none, res).ok(), "an evaluator at level 0");
    CHECK(!b.exprs.empty() && b.is_expr(static_cast<int64_t>(b.exprs[0])) && !b.is_expr(-1) && !b.is_expr(b.stats[1].base), "is_expr");
}

void test_splitmix() {
    std::printf("SplitMix\n");
    SplitMix s;
    CHECK(s.next() == 0xE220A8397B1DCDAFull && s.next() == 0x6E789E6AA1B965F4ull && s.next() == 0x06C45D188009454Full, "the first outputs of seed 0");
    SplitMix t;
    t.state = 1;
    bool in_range = true;
    for (int i = 0; i < 100000; ++i) {
        const double v = t.rand_double();
        in_range = in_range && v >= 0 && v < 1;
    }
    CHECK(in_range, "rand_double is in [0, 1)");
    SplitMix z;
    CHECK(z.rand_double() == static_cast<double>(0xE220A8397B1DCDAFull >> 11) / 9007199254740992.0, "the top 53 bits over 2^53");
}

}  // namespace

int main() {
    Corpus corpus;
    std::string err;
    if (!load_corpus(STATS_CORPUS_PATH, corpus, err)) {
        std::printf("  [FAIL] corpus %s: %s\n", STATS_CORPUS_PATH, err.c_str());
        return 1;
    }
    std::map<std::string, StatBundle> bundles;
    test_corpus(corpus, bundles);
    if (bundles.count("tiny")) {
        test_forms_by_hand(bundles["tiny"]);
        test_state_functions(bundles["tiny"]);
        test_refusals(bundles["tiny"]);
    } else {
        CHECK(false, "the corpus has the bundle tiny");
    }
    test_splitmix();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

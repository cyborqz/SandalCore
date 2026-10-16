// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_stats.cpp: the stat engine's interpreter (plan B3) against the shared
//  corpus that services/internal/statprog writes (stats/testdata/corpus.txt):
//    - every bundle parses and verifies;
//    - every case gives the same stats as the Go engine, bit for bit (every value
//      where the corpus lists them, and a digest of all outputs);
//    - the captured level-1 Bard and retail level-20 Ranger compare with the capture
//      as the corpus says (the Bard: all 442 equal as float32);
//    - every hostile blob is refused;
//  and a few things of its own: truncations and random damage never crash the reader,
//  whatever the reader accepts runs, and the engine refuses a bad class or level.
// ============================================================================
#include "stats/character_stats.h"
#include "stats/damage.h"
#include "stats/gather_host.h"
#include "stats/gear.h"
#include "stats/stat_engine.h"
#include "stats/stat_program.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
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
// C.O.A.C Y B

// ---- the corpus ----

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

std::vector<uint8_t> hex_decode(const std::string& s) {
    std::vector<uint8_t> out;
    if (s == "-") return out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
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
// C.O.A : C_Y_B

// A stat view read from a corpus "dview".
class MapView : public StatView {
public:
    std::map<int64_t, double> v;
    double stat_value(int64_t guid) const override {
        auto it = v.find(guid);
        return it == v.end() ? 0 : it->second;
    }
};
struct DamageCall {
    std::string name, profile, instigator, target;
    int fn = 0;
    DamageHit hit;
    uint64_t want = 0;
};

struct Want {
    int64_t guid;
    uint64_t value;
};
struct Cap {
    int64_t guid;
    uint32_t bits;
    std::string match;
};
struct Case {
    std::string name, bundle;
    StatInputs in;
    TableState state;
    std::vector<Want> want;
    uint64_t digest = 0;
    size_t members = 0;
    int faults = 0, ignored = 0;
    std::vector<Cap> caps;
};
struct Hostile {
    std::string name;
    bool fix;
    bool whole = false;   // FIX 2: `ins` is the whole blob
    size_t pos, del;
    std::vector<uint8_t> ins;
};
struct FuzzLine {
    std::string bundle;
    uint64_t seed;
    int n;
    size_t accepted;
    uint64_t hash;
};
struct Refusal {
    std::string name, bundle;
    int class_enum, level;
};
// A `gatherhost` block (Gathering stage 2): a harvest's context and the reads the host must answer (services/internal/statprog gather.go writes them).
struct GatherQuery {
    int fn = 0;
    std::string name;   // "" for none ("-" in the corpus)
    int role = 0;
    uint64_t want = 0;  // the answer's float64 bits
};
struct GatherBlock {
    std::string name;
    GatherContext ctx;
    std::vector<GatherQuery> queries;
};
struct Corpus {
    std::vector<GatherBlock> gathers;
    std::vector<FuzzLine> fuzzes;
    std::vector<Refusal> refusals;
    std::map<std::string, std::vector<uint8_t>> bundles;
    std::vector<std::pair<std::string, std::string>> derived;
    std::vector<Case> cases;
    std::vector<Hostile> hostiles;
    std::map<std::string, DamageProfile> profiles;  // the damage formulas' part of the corpus (stats/damage.h)
    std::map<std::string, MapView> views;
    std::vector<DamageCall> damages;
};
// (C_Y_B | C | O | A)

bool load_corpus(const std::string& path, Corpus& c, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::string line;
    std::string cur_bundle, b64;
    size_t bundle_len = 0, line_no = 0;
    Case* cur = nullptr;
    bool skipping = false;  // an fxcase (plan D2: effects, ranks, expressions): test_worldd_effects reads those
    DamageProfile* cur_profile = nullptr;
    MapView* cur_view = nullptr;
    GatherBlock* cur_gather = nullptr;
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
        if (k == "fxcase" || k == "nodehost") {   // the effects test reads the fxcases, test_worldd_city_nodes the nodehost blocks
            skipping = true;
            continue;
        }
        if (k == "gatherhost") {
            if (w.size() != 2) return bad("gatherhost NAME");
            c.gathers.emplace_back();
            cur_gather = &c.gathers.back();
            cur_gather->name = w[1];
            continue;
        }
        if (cur_gather && k != "end") {
            GatherContext& g = cur_gather->ctx;
            if (k == "quality" || k == "duration") {
                if (w.size() != 2 || w[1].size() != 16) return bad("quality|duration F64");
                (k == "quality" ? g.quality : g.base_duration) = bits_to_double(hex64(w[1]));
            } else if (k == "rarity" || k == "tier") {
                if (w.size() != 2) return bad("rarity|tier N");
                (k == "rarity" ? g.item_rarity : g.source_tier) = std::stoi(w[1]);
            } else if (k == "rating") {
                if (w.size() != 3 || w[2].size() != 16) return bad("rating STATGUID F64");
                g.ratings[std::stoll(w[1])] = bits_to_double(hex64(w[2]));
            } else if (k == "q") {
                if (w.size() != 6 || w[4] != "=" || w[5].size() != 16) return bad("q FN NAME ROLE = F64");
                GatherQuery q;
                q.fn = std::stoi(w[1]);
                q.name = w[2] == "-" ? "" : w[2];
                q.role = std::stoi(w[3]);
                q.want = hex64(w[5]);
                cur_gather->queries.push_back(std::move(q));
            } else {
                return bad("a line that is no part of a gatherhost block");
            }
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
        } else if (k == "case") {
            if (w.size() != 5) return bad("case");
            c.cases.emplace_back();
            cur = &c.cases.back();
            cur->name = w[1];
            cur->bundle = w[2];
            cur->in.class_enum = std::stoi(w[3]);
            cur->in.level = std::stoi(w[4]);
        } else if (k == "dprofile") {
            if (w.size() != 2) return bad("dprofile NAME");
            cur_profile = &c.profiles[w[1]];
        } else if (k == "dp" && cur_profile) {
            if (w.size() != 3 || !set_profile_key(*cur_profile, w[1], std::stoll(w[2]))) return bad("dp KEY GUID");
        } else if (k == "de" && cur_profile) {
            if (w.size() != 6) return bad("de TAG D M A MIT");
            cur_profile->elements.push_back({w[1], std::stoll(w[2]), std::stoll(w[3]), std::stoll(w[4]), std::stoll(w[5])});
        } else if (k == "dview") {
            if (w.size() != 2) return bad("dview NAME");
            cur_view = &c.views[w[1]];
        } else if (k == "v" && cur_view) {
            if (w.size() != 3) return bad("v GUID F64");
            cur_view->v[std::stoll(w[1])] = bits_to_double(hex64(w[2]));
        } else if (k == "damage") {
            if (w.size() != 10) return bad("damage NAME PROFILE FN TAG PHYS FLAGS INSTIGATOR TARGET WANT");
            DamageCall d;
            d.name = w[1];
            d.profile = w[2];
            d.fn = std::stoi(w[3]);
            d.hit.tag = w[4] == "-" ? "" : w[4];
            d.hit.physical = w[5] == "1";
            d.hit.flags = static_cast<uint8_t>(std::stoi(w[6]));
            d.instigator = w[7];
            d.target = w[8];
            d.want = hex64(w[9]);
            c.damages.push_back(std::move(d));
        } else if (k == "end") {
            cur = nullptr;
            cur_profile = nullptr;
            cur_view = nullptr;
            cur_gather = nullptr;
        } else if (k == "fuzz") {
            if (w.size() != 6) return bad("fuzz");
            c.fuzzes.push_back({w[1], std::stoull(w[2]), std::stoi(w[3]), std::stoul(w[4]), hex64(w[5])});
        } else if (k == "refuse") {
            if (w.size() != 5) return bad("refuse");
            c.refusals.push_back({w[1], w[2], std::stoi(w[3]), std::stoi(w[4])});
        } else if (k == "hostile") {
            if (w.size() != 6) return bad("hostile");
            Hostile h;
            h.name = w[1];
            h.fix = w[2] == "1";
            h.whole = w[2] == "2";
            h.pos = std::stoul(w[3]);
            h.del = std::stoul(w[4]);
            h.ins = hex_decode(w[5]);
            c.hostiles.push_back(std::move(h));
        } else if (cur) {
            if (k == "mod") cur->in.mods.push_back({std::stoll(w[1]), bits_to_double(hex64(w[2]))});
            else if (k == "tag") cur->in.equipped.push_back({w[1], w[2]});
            else if (k == "want") cur->want.push_back({std::stoll(w[1]), hex64(w[2])});
            else if (k == "digest") {
                cur->digest = hex64(w[1]);
                cur->members = std::stoul(w[2]);
                cur->faults = std::stoi(w[3]);
                cur->ignored = std::stoi(w[4]);
            } else if (k == "cap") cur->caps.push_back({std::stoll(w[1]), static_cast<uint32_t>(hex64(w[2])), w[3]});
            else if (k == "state") {
                TableState& s = cur->state;
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
            } else if (k == "rounding" || k == "item") {
                // plan B3: the worn items a case was made from; the engine's own test does not use them (test_worldd_character_setup does)
            } else return bad("unknown case line");
        } else {
            return bad("a line outside a case");
        }
    }
    if (!cur_bundle.empty() || cur || cur_profile || cur_view) {
        err = "unterminated bundle or case";
        return false;
    }
    return true;
}
// (C-Y-B ~ CoA)

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

// compare is how a computed value compares with a captured float32 (statprog's compare).
const char* compare(double got, double want) {
    const double g = static_cast<double>(static_cast<float>(got));
    if (g == want) return "exact";
    if (std::fabs(g - want) <= 1e-6 * std::max(1.0, std::fabs(want))) return "near";
    return "miss";
}

std::vector<uint8_t> apply_edit(const std::vector<uint8_t>& orig, const Hostile& h, bool& ok) {
    if (h.whole) {
        ok = true;
        return h.ins;
    }
    ok = h.pos <= orig.size() && h.pos + h.del <= orig.size();
    if (!ok) return {};
    std::vector<uint8_t> out(orig.begin(), orig.begin() + static_cast<std::ptrdiff_t>(h.pos));
    out.insert(out.end(), h.ins.begin(), h.ins.end());
    out.insert(out.end(), orig.begin() + static_cast<std::ptrdiff_t>(h.pos + h.del), orig.end());
    if (h.fix && out.size() >= 16) {
        const uint32_t len = static_cast<uint32_t>(out.size() - 16);
        for (int i = 0; i < 4; ++i) out[8 + i] = static_cast<uint8_t>(len >> (8 * i));
        // crc32 of the body
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 16; i < out.size(); ++i) {
            c ^= out[i];
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        c ^= 0xFFFFFFFFu;
        for (int i = 0; i < 4; ++i) out[12 + i] = static_cast<uint8_t>(c >> (8 * i));
    }
    return out;
}
// built with C | O | A and C_Y_B

void fix_crc(std::vector<uint8_t>& b) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 16; i < b.size(); ++i) {
        c ^= b[i];
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    c ^= 0xFFFFFFFFu;
    for (int i = 0; i < 4; ++i) b[12 + i] = static_cast<uint8_t>(c >> (8 * i));
}

void test_corpus(const Corpus& c, std::map<std::string, StatBundle>& bundles) {
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
    std::printf("  %zu bundles parsed and verified\n", bundles.size());

    std::printf("cases\n");
    size_t ncase = 0, nwant = 0, ncap = 0;
    for (const Case& cs : c.cases) {
        auto it = bundles.find(cs.bundle);
        CHECKF(it != bundles.end(), "case %s: no bundle %s", cs.name.c_str(), cs.bundle.c_str());
        if (it == bundles.end()) continue;
        StatResult res;
        std::string err;
        const bool ran = run_stats(it->second, cs.in, cs.state, res, err);
        CHECKF(ran, "case %s: %s", cs.name.c_str(), err.c_str());
        if (!ran) continue;
        ++ncase;
        CHECKF(res.stats.size() == cs.members && res.faults == cs.faults && res.ignored_mods == cs.ignored,
               "case %s: %zu stats, %d faults, %d ignored mods; the corpus has %zu, %d, %d", cs.name.c_str(), res.stats.size(),
               res.faults, res.ignored_mods, cs.members, cs.faults, cs.ignored);
        bool values_ok = true;
        if (!cs.want.empty()) {
            if (cs.want.size() != res.stats.size()) values_ok = false;
            for (size_t i = 0; values_ok && i < res.stats.size(); ++i) {
                ++nwant;
                if (cs.want[i].guid != res.stats[i].guid || cs.want[i].value != double_bits(res.stats[i].value)) {
                    values_ok = false;
                    std::printf("    stat %lld is %016llx, Go has %016llx\n", static_cast<long long>(res.stats[i].guid),
                                static_cast<unsigned long long>(double_bits(res.stats[i].value)),
                                static_cast<unsigned long long>(cs.want[i].value));
                }
            }
        }
        CHECKF(values_ok, "case %s: the values differ from the Go engine's", cs.name.c_str());
        CHECKF(digest(res) == cs.digest, "case %s: digest %016llx, Go has %016llx", cs.name.c_str(),
               static_cast<unsigned long long>(digest(res)), static_cast<unsigned long long>(cs.digest));
        if (!cs.caps.empty()) {
            std::map<int64_t, double> got;
            for (const StatOut& s : res.stats) got[s.guid] = s.value;
            size_t exact = 0, wrong = 0;
            for (const Cap& cp : cs.caps) {
                ++ncap;
                auto g = got.find(cp.guid);
                const char* m = g == got.end() ? "missing" : compare(g->second, static_cast<double>(std::bit_cast<float>(cp.bits)));
                if (cp.match == "exact" && std::strcmp(m, "exact") == 0) ++exact;
                if (cp.match != m) ++wrong;
            }
            CHECKF(wrong == 0, "case %s: %zu of %zu captured stats compare differently from the Go engine's", cs.name.c_str(), wrong, cs.caps.size());
            std::printf("  %s: %zu of %zu captured stats equal as float32\n", cs.name.c_str(), exact, cs.caps.size());
            if (cs.name == "bard_l1") CHECK(exact == 442 && cs.caps.size() == 442, "the captured Bard: 442 of 442 stats equal as float32");
        }
    }
    std::printf("  %zu cases, %zu values and %zu captured stats checked\n", ncase, nwant, ncap);
    CHECK(ncase >= 30, "the corpus has its cases");

    std::printf("a valid bundle that asks too much\n");
    for (const Refusal& r : c.refusals) {
        auto it = bundles.find(r.bundle);
        CHECKF(it != bundles.end(), "refuse %s: no bundle %s", r.name.c_str(), r.bundle.c_str());
        if (it == bundles.end()) continue;
        StatInputs in;
        in.class_enum = r.class_enum;
        in.level = r.level;
        StatResult res;
        std::string e;
        CHECKF(!run_stats(it->second, in, StatState{}, res, e), "refuse %s: the run finished", r.name.c_str());
        std::printf("  %s: %s\n", r.name.c_str(), e.c_str());
    }
    CHECK(!c.refusals.empty(), "the corpus has a bundle that asks too much");

    std::printf("hostile bundles\n");
    auto tiny = c.bundles.find("tiny");
    CHECK(tiny != c.bundles.end(), "the corpus has the bundle tiny");
    size_t refused = 0;
    if (tiny != c.bundles.end()) {
        for (const Hostile& h : c.hostiles) {
            bool ok = false;
            std::vector<uint8_t> blob = apply_edit(tiny->second, h, ok);
            CHECKF(ok, "hostile %s: the edit does not fit", h.name.c_str());
            StatBundle b;
            std::string err;
            const bool accepted = ok && parse_bundle(blob, b, err);
            CHECKF(!accepted, "hostile %s was accepted", h.name.c_str());
            if (!accepted) ++refused;
        }
    }
    std::printf("  %zu of %zu hostile blobs refused\n", refused, c.hostiles.size());
    CHECK(c.hostiles.size() >= 60, "the corpus has its hostile blobs");
}

// fuzz_parity is statprog.FuzzParity: the same generator, the same damage, the same verdicts.
void fuzz_parity(const std::vector<uint8_t>& base, uint64_t seed, int n, size_t& accepted, uint64_t& hash) {
    uint64_t x = seed;
    auto next = [&]() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    hash = 14695981039346656037ull;
    auto fold = [&](uint8_t b) {
        hash ^= b;
        hash *= 1099511628211ull;
    };
    accepted = 0;
    std::string err;
    for (int i = 0; i < n; ++i) {
        std::vector<uint8_t> m = base;
        for (int k = static_cast<int>(next() % 4) + 1; k > 0; --k) {
            const uint64_t o = next() % 5;
            const size_t at = 16 + static_cast<size_t>(next() % (m.size() - 16));
            if (o <= 2) {
                const unsigned bit = static_cast<unsigned>(next() % 8);
                m[at] = static_cast<uint8_t>(m[at] ^ (1u << bit));
            } else if (o == 3) {
                m[at] = static_cast<uint8_t>(next());
            } else {
                m.erase(m.begin() + static_cast<std::ptrdiff_t>(at));
                const uint32_t len = static_cast<uint32_t>(m.size() - 16);
                for (int j = 0; j < 4; ++j) m[8 + j] = static_cast<uint8_t>(len >> (8 * j));
            }
        }
        if (next() % 10 != 0) fix_crc(m);
        StatBundle b;
        if (!parse_bundle(m, b, err)) {
            fold(0);
            continue;
        }
        ++accepted;
        if (b.classes.empty()) {
            fold(3);
            continue;
        }
        StatInputs in;
        in.class_enum = b.classes[0].enum_value;
        in.level = 20;
        StatResult res;
        if (!run_stats(b, in, StatState{}, res, err)) {
            fold(2);
            continue;
        }
        fold(1);
        const uint64_t d = digest(res);
        for (int j = 0; j < 8; ++j) fold(static_cast<uint8_t>(d >> (8 * j)));
    }
}
// -- C Y B x C.O.A --

// The damage formulas: every call of the corpus must give the Go engine's bits.
void test_damage_corpus(const Corpus& c) {
    std::printf("damage formulas\n");
    size_t n = 0, bad = 0;
    for (const DamageCall& d : c.damages) {
        auto p = c.profiles.find(d.profile);
        auto iv = c.views.find(d.instigator);
        auto tv = c.views.find(d.target);
        if (p == c.profiles.end() || iv == c.views.end() || tv == c.views.end()) {
            CHECKF(false, "damage %s: no profile %s or view %s or %s", d.name.c_str(), d.profile.c_str(), d.instigator.c_str(), d.target.c_str());
            continue;
        }
        const double got = p->second.calculate(d.fn, iv->second, tv->second, d.hit);
        ++n;
        if (double_bits(got) != d.want) {
            ++bad;
            if (bad <= 5)
                std::printf("    damage %s: function %d gives %016llx, Go has %016llx\n", d.name.c_str(), d.fn, static_cast<unsigned long long>(double_bits(got)),
                            static_cast<unsigned long long>(d.want));
        }
    }
    CHECKF(bad == 0, "%zu of %zu damage calls differ from the Go engine's", bad, n);
    CHECK(n >= 500, "the corpus has its damage calls");
    std::printf("  %zu damage calls checked\n", n);
}

void test_fuzz_parity(const Corpus& c) {
    std::printf("differential fuzz\n");
    for (const FuzzLine& f : c.fuzzes) {
        auto it = c.bundles.find(f.bundle);
        CHECKF(it != c.bundles.end(), "fuzz: no bundle %s", f.bundle.c_str());
        if (it == c.bundles.end()) continue;
        size_t acc = 0;
        uint64_t h = 0;
        fuzz_parity(it->second, f.seed, f.n, acc, h);
        CHECKF(acc == f.accepted && h == f.hash, "fuzz %s: %zu accepted with hash %016llx; Go has %zu and %016llx", f.bundle.c_str(), acc,
               static_cast<unsigned long long>(h), f.accepted, static_cast<unsigned long long>(f.hash));
        std::printf("  %s: %d damaged bundles, %zu accepted and run, the verdicts and results hash alike\n", f.bundle.c_str(), f.n, acc);
    }
    CHECK(c.fuzzes.size() >= 3, "the corpus has its differential fuzz");
}

// ---- what the C++ side checks by itself ----

uint64_t g_rng = 88172645463325252ull;
uint64_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

void test_damage(const std::vector<uint8_t>& raw, const char* label) {
    std::printf("damaged bytes (%s)\n", label);
    StatBundle b;
    std::string err;
    // every truncation of a prefix of the bundle (and a few lengths of the rest) is refused
    size_t refused = 0, tried = 0;
    for (size_t n = 0; n < raw.size(); n += (n < 400 ? 1 : 97)) {
        ++tried;
        if (!parse_bundle(raw.data(), n, b, err)) ++refused;
    }
    CHECKF(refused == tried, "%zu of %zu truncations refused", refused, tried);
    // random damage: never a crash; what is accepted runs
    size_t accepted = 0;
    for (int i = 0; i < 6000; ++i) {
        std::vector<uint8_t> m = raw;
        for (int k = static_cast<int>(rnd() % 4) + 1; k > 0; --k) {
            switch (rnd() % 5) {
                case 0: case 1: case 2: m[16 + rnd() % (m.size() - 16)] ^= static_cast<uint8_t>(1u << (rnd() % 8)); break;
                case 3: m[16 + rnd() % (m.size() - 16)] = static_cast<uint8_t>(rnd()); break;
                default: {
                    const size_t at = 16 + rnd() % (m.size() - 16);
                    m.erase(m.begin() + static_cast<std::ptrdiff_t>(at));
                    const uint32_t len = static_cast<uint32_t>(m.size() - 16);
                    for (int j = 0; j < 4; ++j) m[8 + j] = static_cast<uint8_t>(len >> (8 * j));
                }
            }
        }
        if (rnd() % 10) fix_crc(m);
        StatBundle mb;
        if (!parse_bundle(m, mb, err)) continue;
        ++accepted;
        for (const ClassDef& cd : mb.classes)
            for (int level : {1, 30, kMaxLevel}) {
                StatInputs in;
                in.class_enum = cd.enum_value;
                in.level = level;
                in.mods.push_back({1001, 1.0});
                StatResult res;
                if (!run_stats(mb, in, StatState{}, res, err)) {
                    CHECKF(false, "a verified bundle failed to run: %s", err.c_str());
                    return;
                }
            }
    }
    CHECK(true, "random damage never crashes the reader or the engine");
    std::printf("  %zu of 6000 damaged bundles passed the verifier and ran\n", accepted);
}
// C | O | A.C | Y | B

void test_inputs(const StatBundle& b) {
    std::printf("inputs\n");
    StatResult res;
    std::string err;
    StatInputs in;
    in.class_enum = b.classes[0].enum_value;
    for (int level : {0, -1, kMaxLevel + 1}) {
        in.level = level;
        CHECKF(!run_stats(b, in, StatState{}, res, err), "level %d is refused", level);
    }
    in.level = 1;
    in.class_enum = 0;
    CHECK(!run_stats(b, in, StatState{}, res, err), "class 0 is refused");
    in.class_enum = 99;
    CHECK(!run_stats(b, in, StatState{}, res, err), "class 99 is refused");
    in.class_enum = b.classes[0].enum_value;
    in.mods = {{b.stats[0].guid, 5}, {b.stats[0].guid, 2.5}, {123456789, 1}};
    CHECK(run_stats(b, in, StatState{}, res, err), "a good run");
    CHECK(res.ignored_mods == 1, "a mod for a stat the bundle does not have is ignored and counted");
    // the same mods summed in one
    StatInputs one = in;
    one.mods = {{b.stats[0].guid, 7.5}};
    StatResult res2;
    CHECK(run_stats(b, one, StatState{}, res2, err) && digest(res) == digest(res2), "repeated mods sum");
    CHECK(tag_matches("Item.Gear.Weapon.Shield", "Item.Gear.Weapon.Shield") && tag_matches("Item.Gear.Weapon.Shield.Tower", "Item.Gear.Weapon.Shield") &&
              !tag_matches("Item.Gear.Weapon.ShieldBash", "Item.Gear.Weapon.Shield") && !tag_matches("Item.Gear", "Item.Gear.Weapon"),
          "tags match themselves and their descendants");
}


// ---- plan B3: the worn items' stat data (gear.h) and the character's stats (character_stats.h) ----------------------------

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

/// A catalog made of tables.
class TableCatalog : public GearCatalog {
public:
    std::map<int64_t, ItemGear> items;
    std::map<int64_t, SetBonus> sets;
    std::map<int, std::string> slots = {{2, "Chest"}, {15, "Primary"}, {16, "Secondary"}};
    const ItemGear* item(int64_t r) const override {
        const auto it = items.find(r);
        return it == items.end() ? nullptr : &it->second;
    }
    const SetBonus* set(int64_t g) const override {
        const auto it = sets.find(g);
        return it == sets.end() ? nullptr : &it->second;
    }
    std::string slot_name(int slot) const override {
        const auto it = slots.find(slot);
        return it != slots.end() ? it->second : "Slot" + std::to_string(slot);
    }
};

/// The default quality curve: Common 1-5, Uncommon 6-15, Rare 16-30, Heroic 31-50, Epic 51-70, Legendary 71-100, Artifact 101-200.
void default_curve(ItemGear& g) {
    const double xs[] = {1, 6, 16, 31, 51, 71, 101}, ys[] = {5, 15, 30, 50, 70, 100, 200};
    g.has_curve = true;
    for (size_t i = 0; i < 7; ++i) g.bands[i] = {xs[i], ys[i]};
}

/// A stat block entry whose Min and Max of each rarity are given (Common first).
BlockStat block_stat(int64_t stat, std::array<double, 7> mn, std::array<double, 7> mx) {
    BlockStat b;
    b.stat = stat;
    b.min = mn;
    b.max = mx;
    return b;
}

double mod_of(const GearInputs& g, int64_t stat, bool* found = nullptr) {
    for (const StatMod& m : g.mods) {
        if (m.stat == stat) {
            if (found) *found = true;
            return m.value;
        }
    }
    if (found) *found = false;
    return 0;
}

void test_gear_rules() {
    std::printf("gear: what the worn items add\n");
    TableCatalog cat;
    ItemGear a;
    a.record = 1;
    default_curve(a);
    // stat 100: Common 10-20, Uncommon 30-40, the rest 50-50, 60-60 ...; stat 101: 7 at every rarity
    a.stats = {block_stat(100, {10, 30, 50, 60, 70, 80, 0}, {20, 40, 50, 60, 70, 80, 0}), block_stat(101, {7, 7, 7, 7, 7, 7, 7}, {7, 7, 7, 7, 7, 7, 7})};
    a.tags = {"Item.Gear.Armor.Light", "Slot.Chest"};
    cat.items[1] = a;
    const auto one = [&](int quality, Rounding r) {
        const WornItem w{1, quality, 2};
        return gear_inputs(cat, std::span<const WornItem>(&w, 1), r);
    };
    CHECK(mod_of(one(1, Rounding::None), 100) == 10 && mod_of(one(5, Rounding::None), 100) == 20, "Common 1-5: Min at 1, Max at 5");
    CHECK(mod_of(one(3, Rounding::None), 100) == 15, "quality 3: 10 + 10 * (3 - 1) / (5 - 1) = 15");
    CHECK(mod_of(one(2, Rounding::None), 100) == 12.5 && mod_of(one(2, Rounding::HalfUp), 100) == 13, "quality 2: 12.5, or 13 with half-up rounding");
    CHECK(mod_of(one(4, Rounding::None), 100) == 17.5 && mod_of(one(4, Rounding::HalfUp), 100) == 18, "quality 4: 17.5, 18 half-up (a half goes up)");
    CHECK(mod_of(one(6, Rounding::None), 100) == 30 && mod_of(one(15, Rounding::None), 100) == 40 && std::fabs(mod_of(one(7, Rounding::None), 100) - (30 + 10.0 / 9.0)) < 1e-12,
          "Uncommon 6-15: 30 at 6, 40 at 15, 7 in between is 30 + 10 / 9");
    CHECK(mod_of(one(20, Rounding::None), 100) == 50 && mod_of(one(60, Rounding::None), 100) == 70 && mod_of(one(90, Rounding::None), 100) == 80, "Rare, Epic and Legendary read their own band");
    CHECK(mod_of(one(1, Rounding::None), 101) == 7 && mod_of(one(150, Rounding::None), 101) == 7, "a stat that is the same at every rarity is the same at every quality");
    CHECK(mod_of(one(150, Rounding::None), 100) == 0, "Artifact 101-200: 0 here");
    // below and past the curve: extrapolated in the nearest band, as Go's gear.go does
    CHECK(mod_of(one(0, Rounding::None), 100) == 10 + 10.0 * (0 - 1) / 4 && mod_of(one(250, Rounding::None), 100) == 0, "quality 0 reads the Common band, extrapolated; 250 reads Artifact");
    // an item with no curve reads CommonMin
    ItemGear nc = a;
    nc.record = 2;
    nc.has_curve = false;
    cat.items[2] = nc;
    {
        const WornItem w{2, 40, 2};
        CHECK(mod_of(gear_inputs(cat, std::span<const WornItem>(&w, 1), Rounding::None), 100) == 10, "no quality curve: CommonMin, whatever the quality");
    }
    // tags go under the name of the slot
    const GearInputs g = one(1, Rounding::None);
    CHECK(g.equipped.size() == 2 && g.equipped[0].slot == "Chest" && g.equipped[0].tag == "Item.Gear.Armor.Light" && g.equipped[1].tag == "Slot.Chest", "an item's tags, in order, under the slot's name");
    {
        const WornItem w{1, 1, 77};
        const GearInputs odd = gear_inputs(cat, std::span<const WornItem>(&w, 1), Rounding::None);
        CHECK(odd.equipped.size() == 2 && odd.equipped[0].slot == "Slot77", "a slot value with no name is Slot<N>");
    }
    // several items: sums per stat, mods by ascending stat guid; an unknown record adds nothing and is counted
    ItemGear b;
    b.record = 3;
    b.stats = {block_stat(50, {1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 1}), block_stat(100, {5, 5, 5, 5, 5, 5, 5}, {5, 5, 5, 5, 5, 5, 5})};
    cat.items[3] = b;
    const WornItem worn[] = {{1, 1, 2}, {3, 1, 15}, {999, 1, 16}};
    const GearInputs sum = gear_inputs(cat, worn, Rounding::None);
    CHECK(sum.mods.size() == 3 && sum.mods[0].stat == 50 && sum.mods[1].stat == 100 && sum.mods[2].stat == 101 && mod_of(sum, 100) == 15 && sum.unknown_items == 1,
          "two items: the stat they share is summed (10 + 5), the mods come out by stat guid, the unknown record is counted and adds nothing");
    // sets: tiers up to the worn count, at the lowest rarity among the worn pieces
    SetBonus set;
    SetTier t2;
    t2.pieces = 2;
    SetEntry e1;
    e1.stat = 200;
    e1.has = {true, false, true, false, false, false, false};
    e1.value = {10, 0, 30, 0, 0, 0, 0};
    SetEntry e2;
    e2.stat = 201;   // no value at any rarity: a zero mod
    t2.entries = {e1, e2};
    SetTier t4;
    t4.pieces = 4;
    SetEntry e3;
    e3.stat = 200;
    e3.has = {true, false, true, false, false, false, false};
    e3.value = {5, 0, 8, 0, 0, 0, 0};
    t4.entries = {e3};
    set.tiers = {t2, t4};
    cat.sets[7] = set;
    for (int64_t r : {10, 11, 12, 13}) {
        ItemGear p;
        p.record = r;
        default_curve(p);
        p.sets = {7};
        cat.items[r] = p;
    }
    const WornItem pieces_1[] = {{10, 20, 2}};
    CHECK(gear_inputs(cat, pieces_1, Rounding::None).mods.empty(), "one piece of a set of 2: no bonus");
    const WornItem pieces_2[] = {{10, 20, 2}, {11, 1, 15}};   // Rare (quality 20) and Common (quality 1): the lowest is Common
    const GearInputs two = gear_inputs(cat, pieces_2, Rounding::None);
    bool f200 = false, f201 = false;
    CHECK(mod_of(two, 200, &f200) == 10 && f200 && mod_of(two, 201, &f201) == 0 && f201 && two.mods.size() == 2,
          "two pieces, the lowest Common: tier 2 gives 10 (Common), and the bonus with no value is a zero mod");
    const WornItem pieces_4[] = {{10, 20, 2}, {11, 20, 15}, {12, 20, 16}, {13, 20, 17}};   // all Rare
    const GearInputs four = gear_inputs(cat, pieces_4, Rounding::None);
    CHECK(mod_of(four, 200) == 30 + 8, "four Rare pieces: both tiers apply, 30 + 8");
    const WornItem pieces_3[] = {{10, 20, 2}, {11, 20, 15}, {12, 20, 16}};
    CHECK(mod_of(gear_inputs(cat, pieces_3, Rounding::None), 200) == 30, "three pieces: the tier of 2 only");
    // a set the catalog does not know is counted
    ItemGear orphan;
    orphan.record = 20;
    orphan.sets = {999};
    cat.items[20] = orphan;
    const WornItem o[] = {{20, 1, 2}};
    const GearInputs og = gear_inputs(cat, o, Rounding::None);
    CHECK(og.unknown_sets == 1 && og.mods.empty(), "a set the catalog does not know gives no bonus and is counted");
    // nothing worn
    CHECK(gear_inputs(cat, {}, Rounding::None).mods.empty() && gear_inputs(cat, {}, Rounding::None).equipped.empty(), "nothing worn: no mods, no tags");
}

int64_t guid_named(const StatBundle& b, const char* name) {
    for (size_t i = 0; i < b.stats.size(); ++i) {
        if (b.stat_name(i) == name) return b.stats[i].guid;
    }
    return 0;
}

uint32_t bits_of_f(float f) { return std::bit_cast<uint32_t>(f); }

void test_character_stats(const StatBundle& b) {
    std::printf("character stats: join, recompute, deltas, the current values\n");
    const int64_t kCon = guid_named(b, "Constitution"), kMent = guid_named(b, "Mentality"), kHealth = guid_named(b, "Health"), kMaxHealth = guid_named(b, "MaxHealth"),
                  kMana = guid_named(b, "Mana"), kMaxMana = guid_named(b, "MaxMana"), kPower = guid_named(b, "Artisan_Gathering_Fishing_Sport_Power");
    CHECK(kCon && kMent && kHealth && kMaxHealth && kMana && kMaxMana && kPower, "the stats are in the real graph");
    TableCatalog cat;
    ItemGear heavy;   // Constitution +100 and Mentality +100 at every rarity
    heavy.record = 1;
    heavy.stats = {block_stat(kCon, {100, 100, 100, 100, 100, 100, 100}, {100, 100, 100, 100, 100, 100, 100}),
                   block_stat(kMent, {100, 100, 100, 100, 100, 100, 100}, {100, 100, 100, 100, 100, 100, 100})};
    cat.items[1] = heavy;
    ItemGear rod;   // the fishing rod: the stat that replicates base and equipment
    rod.record = 2;
    rod.stats = {block_stat(kPower, {100, 100, 100, 100, 100, 100, 100}, {100, 100, 100, 100, 100, 100, 100})};
    cat.items[2] = rod;
    ItemGear plain;   // an item with nothing for the engine
    plain.record = 3;
    cat.items[3] = plain;
    const std::vector<WornItem> none;
    const WornItem with_heavy[] = {{1, 1, 2}};
    const WornItem with_rod[] = {{2, 1, 35}};
    const WornItem with_plain[] = {{3, 1, 71}};

    CharacterStats cs(b, cat, 2, 1);
    std::vector<StatValue> changed;
    std::string e;
    CHECK(!cs.started() && !cs.update(none, changed, e) && contains(e, "not started"), "update before start is refused");
    CHECK(cs.start(none, e) && cs.started() && cs.values().size() == 466, "start: the Bard's 466 stats");
    CHECK(cs.find(kHealth) && cs.find(kMaxHealth) && cs.find(kHealth)->value == cs.find(kMaxHealth)->value && cs.find(kHealth)->held() && !cs.find(kMaxHealth)->held(),
          "Health starts at its maximum and is a held (current) value, MaxHealth is not");
    CHECK(cs.find(kPower) && cs.find(kPower)->buckets() && cs.find(kPower)->equip == 0, "the fishing stat replicates base and equipment");
    CHECK(cs.faults() == 0 && cs.ignored_mods() == 0 && cs.unknown_items() == 0, "no fault, no ignored mod");

    // nothing changes: nothing is returned
    CHECK(cs.update(none, changed, e) && changed.empty(), "the same gear again: no change");
    CHECK(cs.update(with_plain, changed, e) && changed.empty(), "an item that gives the engine nothing: no change");

    // the heavy chest: more health and mana, and what follows from Constitution and Mentality; Health stays (the current value)
    const float old_health = cs.find(kHealth)->value, old_max = cs.find(kMaxHealth)->value;
    CHECK(cs.update(with_heavy, changed, e) && changed.size() > 5, "wearing it changes stats");
    bool has_max = false, has_health = false, has_con = false;
    for (const StatValue& v : changed) {
        has_max = has_max || v.guid == kMaxHealth;
        has_health = has_health || v.guid == kHealth;
        has_con = has_con || v.guid == kCon;
    }
    CHECK(has_max && has_con && !has_health, "MaxHealth and Constitution changed, Health did not: a current value stays where it is");
    CHECK(cs.find(kMaxHealth)->value > old_max && cs.find(kHealth)->value == old_health, "...MaxHealth is up, Health is as it was");
    bool in_order = true;
    for (size_t i = 1; i < changed.size(); ++i) {
        int64_t prev = -1, now = -1;
        for (size_t k = 0; k < cs.values().size(); ++k) {
            if (cs.values()[k].guid == changed[i - 1].guid) prev = static_cast<int64_t>(k);
            if (cs.values()[k].guid == changed[i].guid) now = static_cast<int64_t>(k);
        }
        in_order = in_order && prev < now;
    }
    CHECK(in_order, "the changed stats come in stat order");
    for (const StatValue& v : changed) {
        const StatValue* f = cs.find(v.guid);
        CHECK(f && same_on_the_wire(*f, v), "(each changed stat is the character's value now)");
        break;
    }

    // current values: the player is hurt, then the gear comes off
    cs.set_current(kHealth, 100.0f);
    cs.set_current(kMaxHealth, 5.0f);   // not a held stat: ignored
    CHECK(cs.find(kHealth)->value == 100.0f && cs.find(kMaxHealth)->value != 5.0f, "set_current sets a held stat and ignores one that is not");
    CHECK(cs.update(none, changed, e), "taking the gear off");
    CHECK(cs.find(kHealth)->value == std::min(100.0f, cs.find(kMaxHealth)->value) && cs.find(kMaxHealth)->value == old_max, "the maximum is back to the bare one");
    const float bare_max = old_max;
    if (bare_max < 100.0f) {
        CHECK(cs.find(kHealth)->value == bare_max, "Health 100 above the new maximum is clamped to it");
        bool clamp_sent = false;
        for (const StatValue& v : changed) clamp_sent = clamp_sent || (v.guid == kHealth && v.value == bare_max);
        CHECK(clamp_sent, "...and the clamp is a change like any other");
    } else {
        CHECK(cs.find(kHealth)->value == 100.0f, "Health 100 is below the bare maximum: it stays");
    }
    // heavy, then hurt below the bare maximum, then off: no clamp
    CHECK(cs.update(with_heavy, changed, e), "the gear on again");
    cs.set_current(kHealth, 10.0f);
    cs.set_current(kMana, 10.0f);
    CHECK(cs.update(none, changed, e), "and off again");
    bool health_in = false, mana_in = false;
    for (const StatValue& v : changed) {
        health_in = health_in || v.guid == kHealth;
        mana_in = mana_in || v.guid == kMana;
    }
    CHECK(cs.find(kHealth)->value == 10.0f && cs.find(kMana)->value == 10.0f && !health_in && !mana_in, "Health and Mana of 10 are below any maximum: the gear changes nothing of them");
    // the current value of one that is above: Health 10000 clamped; Mana likewise
    cs.set_current(kHealth, 10000.0f);
    cs.set_current(kMana, 10000.0f);
    CHECK(cs.update(with_plain, changed, e) && cs.find(kHealth)->value == cs.find(kMaxHealth)->value && cs.find(kMana)->value == cs.find(kMaxMana)->value,
          "a current value above its maximum is clamped at the next recompute, even if the gear did not move the maximum");
// [CoA ~ C_Y_B]

    // the fishing rod: base and equipment travel with the value
    const float base_before = cs.find(kPower)->base;
    CHECK(cs.update(with_rod, changed, e), "a fishing rod");
    const StatValue* power = cs.find(kPower);
    bool power_changed = false;
    for (const StatValue& v : changed) power_changed = power_changed || v.guid == kPower;
    CHECK(power && power_changed && power->equip == 100.0f && power->base == base_before && power->value == base_before + 100.0f,
          "its stat changes: the equipment 100 on top of the base, and the base is not touched");

    // a refusal changes nothing: a level the engine does not run
    CharacterStats bad(b, cat, 99, 1);
    CHECK(!bad.start(none, e) && !e.empty() && !bad.started(), "an unknown class is refused by start");
    CharacterStats low(b, cat, 2, 0);
    CHECK(!low.start(none, e), "level 0 is refused");
    // wire equality
    StatValue x, y;
    x.value = 1.0f;
    y.value = 1.0f;
    CHECK(same_on_the_wire(x, y), "equal values are the same on the wire");
    y.value = std::nextafter(1.0f, 2.0f);
    CHECK(!same_on_the_wire(x, y), "one ulp is a difference");
    x.flags = y.flags = kStatBuckets;
    y.value = 1.0f;
    y.equip = 1.0f;
    CHECK(!same_on_the_wire(x, y), "base and equipment count for a stat that carries them");
    x.flags = y.flags = 0;
    CHECK(same_on_the_wire(x, y), "...and not for one that does not");

    // the bits decide, not the numbers: +0.0 and -0.0 are two values on the wire, and a NaN is the same value as itself
    StatValue pz, nz, nan1, nan2;
    pz.value = 0.0f;
    nz.value = -0.0f;
    nan1.value = std::numeric_limits<float>::quiet_NaN();
    nan2.value = nan1.value;
    CHECK(pz.value == nz.value && !same_on_the_wire(pz, nz), "+0.0 and -0.0 compare equal as numbers and are a difference on the wire");
    CHECK(nan1.value != nan2.value && same_on_the_wire(nan1, nan2), "a NaN is not equal to itself as a number and is no difference on the wire");
    StatValue pb, nb;
    pb.flags = nb.flags = kStatBuckets;
    nb.base = -0.0f;
    CHECK(!same_on_the_wire(pb, nb), "...also for the base of a stat that carries it");
    nb = pb;
    nb.equip = -0.0f;
    CHECK(!same_on_the_wire(pb, nb), "...and its equipment");
    // the clamp of a current value
    CHECK(clamp_current(100.0f, 50.0f) == 50.0f && clamp_current(10.0f, 50.0f) == 10.0f && clamp_current(50.0f, 50.0f) == 50.0f, "a current value is clamped to a lower maximum and stays below a higher one");
    CHECK(bits_of_f(clamp_current(-0.0f, 0.0f)) == bits_of_f(-0.0f) && bits_of_f(clamp_current(0.0f, -0.0f)) == bits_of_f(0.0f),
          "equal numbers keep the stored float (and its sign of zero): the value is not rewritten");
    CHECK(clamp_current(std::numeric_limits<float>::quiet_NaN(), 50.0f) == 50.0f, "a NaN current value is no value: the maximum replaces it");

    // prepare() leaves the stats alone until commit()
    CharacterStats pc(b, cat, 2, 1);
    CHECK(pc.start(none, e), "(a fresh character)");
    const std::vector<StatValue> at_start = pc.values();
    CharacterStats::Next next;
    CHECK(pc.prepare(with_heavy, changed, next, e) && !changed.empty() && next.faults == 0, "prepare: the heavy chest would change stats");
    bool untouched = pc.values().size() == at_start.size();
    for (size_t i = 0; untouched && i < at_start.size(); ++i) untouched = bits_of_f(pc.values()[i].value) == bits_of_f(at_start[i].value);
    CHECK(untouched, "...and the character's stats are as they were");
    std::vector<StatValue> again;
    CharacterStats::Next next2;
    CHECK(pc.prepare(with_heavy, again, next2, e) && again.size() == changed.size(), "preparing again gives the same change (nothing was committed)");
    pc.commit(std::move(next));
    float start_max = 0;
    for (const StatValue& v : at_start) start_max = v.guid == kMaxHealth ? v.value : start_max;
    CHECK(pc.find(kMaxHealth)->value > start_max, "commit makes them the character's");
    CHECK(pc.prepare(with_heavy, again, next2, e) && again.empty(), "and the same gear again is no change");
    CHECK(!CharacterStats(b, cat, 2, 1).prepare(with_heavy, again, next2, e) && contains(e, "not started"), "prepare before start is refused");

    // a NaN current value (set_current is given a float, whatever it is) is repaired by the next recompute, and the repair is a change
    pc.set_current(kHealth, std::numeric_limits<float>::quiet_NaN());
    CHECK(pc.update(with_heavy, changed, e) && pc.find(kHealth)->value == pc.find(kMaxHealth)->value, "a NaN Health is the maximum after the next recompute");
    bool health_sent = false;
    for (const StatValue& v : changed) health_sent = health_sent || v.guid == kHealth;
    CHECK(health_sent, "...and the repair is sent");
    // a held value that is -0.0 stays -0.0 under a maximum above it: the same bits, no change
    pc.set_current(kHealth, -0.0f);
    CHECK(pc.update(with_heavy, changed, e), "(Health -0.0)");
    CHECK(pc.update(with_heavy, changed, e) && changed.empty(), "a current value of -0.0 under the same maximum is no change");
}

/// A worn item whose stat is clamped (Hook_Rate's maximum is 100) and whose contribution is more than any float32: the clamp keeps the VALUE finite,
/// so the fault must come from the equipment's sum, which the client is sent too (a bucket stat). Nothing that is not a finite float32 may reach
/// the character's stats.
void test_equipment_beyond_a_float(const StatBundle& b, const StatBundle* dbl) {
    std::printf("character stats: an equipment sum no float32 holds, behind a clamp\n");
    const int64_t kHook = guid_named(b, "Artisan_Gathering_Fishing_Sport_Hook_Rate");
    CHECK(kHook != 0, "(Hook_Rate is in the real graph)");
    const double inf = std::numeric_limits<double>::infinity();
    struct Case { const char* name; double v; };
    const Case cases[] = {{"1e39", 1e39}, {"-1e39", -1e39}, {"+inf", inf}, {"-inf", -inf}, {"3.5e38", 3.5e38}};
    for (const Case& cs : cases) {
        TableCatalog cat;
        ItemGear item;
        item.record = 1;
        item.stats = {block_stat(kHook, {cs.v, cs.v, cs.v, cs.v, cs.v, cs.v, cs.v}, {cs.v, cs.v, cs.v, cs.v, cs.v, cs.v, cs.v})};
        cat.items[1] = item;
        const WornItem worn[] = {{1, 1, 14}};
        // the engine alone: a fault, and the equipment is a 0 (never the infinity)
        StatInputs in;
        in.class_enum = 2;
        in.level = 1;
        in.mods = gear_inputs(cat, worn, Rounding::None).mods;
        StatResult res;
        std::string err;
        CHECKF(run_stats(b, in, StatState{}, res, err), "%s: the run", cs.name);
        const StatOut* hook = nullptr;
        for (const StatOut& o : res.stats) hook = o.guid == kHook ? &o : hook;
        CHECKF(hook && std::isfinite(hook->equip) && hook->equip == 0 && std::isfinite(hook->value) && res.faults != 0,
               "%s: the run faults (the clamp hid the sum from the value) and the equipment is 0", cs.name);
        // the character's stats: prepare and update report it; nothing non-finite is in what they hold
        CharacterStats stats(b, cat, 2, 1);
        std::string e;
        CHECKF(stats.start({}, e) && stats.faults() == 0, "%s: (a bare start, no fault)", cs.name);
        CharacterStats::Next next;
        std::vector<StatValue> changed;
        CHECKF(stats.prepare(worn, changed, next, e) && next.faults != 0, "%s: prepare reports the fault", cs.name);
        bool finite = true;
        for (const StatValue& v : next.values) finite = finite && std::isfinite(v.value) && std::isfinite(v.base) && std::isfinite(v.equip);
        CHECKF(finite, "%s: no value, base or equipment is infinite or NaN", cs.name);
        CHECKF(stats.update(worn, changed, e) && stats.faults() != 0, "%s: update reports it too (faults())", cs.name);
    }
    // a sum that fits is no fault
    {
        TableCatalog cat;
        ItemGear item;
        item.record = 1;
        item.stats = {block_stat(kHook, {3e38, 3e38, 3e38, 3e38, 3e38, 3e38, 3e38}, {3e38, 3e38, 3e38, 3e38, 3e38, 3e38, 3e38})};
        cat.items[1] = item;
        const WornItem worn[] = {{1, 1, 14}};
        CharacterStats stats(b, cat, 2, 1);
        std::string e;
        std::vector<StatValue> changed;
        const StatValue* v = nullptr;
        CHECK(stats.start({}, e) && stats.update(worn, changed, e) && (v = stats.find(kHook)) != nullptr && stats.faults() == 0 && v->equip == 3e38f && v->value == 100.0f,
              "an equipment of 3e38 fits a float32: no fault, the equipment is held as it is and the value is the clamp's");
    }
    // a bundle in double arithmetic holds 1e39 itself: the conversion to the client's float is where it fails
    if (dbl) {
        const int64_t hook_d = guid_named(*dbl, "Artisan_Gathering_Fishing_Sport_Hook_Rate");
        TableCatalog cat;
        ItemGear item;
        item.record = 1;
        item.stats = {block_stat(hook_d, {1e39, 1e39, 1e39, 1e39, 1e39, 1e39, 1e39}, {1e39, 1e39, 1e39, 1e39, 1e39, 1e39, 1e39})};
        cat.items[1] = item;
        const WornItem worn[] = {{1, 1, 14}};
        CharacterStats stats(*dbl, cat, 2, 1);
        std::string e;
        CharacterStats::Next next;
        std::vector<StatValue> changed;
        CHECK(stats.start({}, e) && stats.prepare(worn, changed, next, e) && next.faults != 0, "a double-mode bundle: 1e39 is a number to the engine and a fault in the float the client holds");
        bool finite = true;
        for (const StatValue& v : next.values) finite = finite && std::isfinite(v.value) && std::isfinite(v.base) && std::isfinite(v.equip);
        CHECK(finite, "...and what the stats hold is finite");
    }
}
// (C O A | C-Y-B)

// The gathering reads (Gathering stage 2): gather_host_fn against the corpus's gatherhost blocks, which services/internal/statprog's GatherContext wrote.
void test_gather_reads(const Corpus& c) {
    std::printf("the gathering reads against the shared corpus\n");
    CHECKF(c.gathers.size() >= 4, "the corpus has %zu gatherhost blocks, want 4", c.gathers.size());
    size_t asked = 0, right = 0, rated = 0;
    for (const GatherBlock& b : c.gathers) {
        for (const GatherQuery& q : b.queries) {
            double args[1] = {static_cast<double>(q.role)};
            double out = -12345;
            const bool known = gather_host_fn(b.ctx, q.fn, q.name, q.fn == kHostArtisanStat ? args : nullptr, q.fn == kHostArtisanStat ? 1 : 0, out);
            ++asked;
            if (q.fn == kHostArtisanStat && q.want != 0) ++rated;
            if (known && std::memcmp(&out, &q.want, sizeof out) == 0) {
                ++right;
            } else {
                std::printf("  [FAIL] %s: fn %d \"%s\" role %d = %g (known %d), the corpus says %g\n", b.name.c_str(), q.fn, q.name.c_str(), q.role, out, known ? 1 : 0,
                            bits_to_double(q.want));
                ++g_fail;
            }
        }
    }
    CHECKF(asked >= 40 && right == asked, "%zu reads asked, %zu as the corpus says", asked, right);
    CHECKF(rated >= 4, "%zu of the reads are ratings a stat has", rated);
    // a function outside 180..184 is no gathering read, and the answers do not touch out
    GatherContext g;
    double out = 7;
    CHECK(!gather_host_fn(g, kHostNodeLast, "", nullptr, 0, out) && !gather_host_fn(g, 179, "", nullptr, 0, out) && !gather_host_fn(g, 185, "", nullptr, 0, out) &&
              !gather_host_fn(g, kHostPlayerLevel, "", nullptr, 0, out) && out == 7,
          "a function outside 180..184 is no gathering read");
    // by hand: the player's rating, another actor's, a stat with none, a name that is no guid
    g.ratings[1001] = 215.5;
    g.quality = 31;
    g.item_rarity = 3;
    g.source_tier = 2;
    g.base_duration = 11.25;
    double role0[1] = {0}, role1[1] = {1};
    CHECK(gather_host_fn(g, kHostArtisanStat, "1001", role0, 1, out) && out == 215.5, "the caster's rating");
    CHECK(gather_host_fn(g, kHostArtisanStat, "1001", role1, 1, out) && out == 0, "the target has none");
    CHECK(gather_host_fn(g, kHostArtisanStat, "1001", nullptr, 0, out) && out == 215.5, "no role given: the caster");
    CHECK(gather_host_fn(g, kHostArtisanStat, "1002", role0, 1, out) && out == 0, "a stat with no rating reads 0");
    CHECK(gather_host_fn(g, kHostArtisanStat, "10x", role0, 1, out) && out == 0 && gather_host_fn(g, kHostArtisanStat, "", role0, 1, out) && out == 0, "a name that is no guid reads 0");
    CHECK(gather_host_fn(g, kHostRolledItemQuality, "", nullptr, 0, out) && out == 31 && gather_host_fn(g, kHostItemRarity, "", nullptr, 0, out) && out == 3 &&
              gather_host_fn(g, kHostSourceCertTier, "", nullptr, 0, out) && out == 2 && gather_host_fn(g, kHostGatherBaseSecs, "", nullptr, 0, out) && out == 11.25,
          "the quality, the rarity, the tier and the base duration");
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
    test_damage_corpus(corpus);
    test_gather_reads(corpus);
    test_fuzz_parity(corpus);
    if (bundles.count("main")) test_damage(corpus.bundles["main"], "the real graph");
    if (bundles.count("tiny")) {
        test_damage(corpus.bundles["tiny"], "the tiny graph");
        test_inputs(bundles["tiny"]);
    }
    test_gear_rules();
    if (bundles.count("main")) test_character_stats(bundles["main"]);
    if (bundles.count("main")) test_equipment_beyond_a_float(bundles["main"], bundles.count("main_double") ? &bundles["main_double"] : nullptr);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

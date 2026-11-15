// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_fishing.cpp  (Fishing, milestone 1)
//
//  Fishing without a session, on the real content.db (AOC_CONTENT_DB, else dist/content/content.db of the working tree; "[skip]" without it) and the real water file
//  (dist/content/fishing_water.tsv: the lakes and rivers of the Riverlands):
//
//  the predicates   the ten reward predicates of the fishing tables, which the stat programs cannot compile, run on the data's own text for every certification and rod tier; an unknown
//                   function, a text that does not parse, and the world events' objective answer as the roller expects
//  the data         the eleven fishing interactions (a Novice, the rod, 15 s), their reward tables, and the picks of the tier tables set to one
//  the rolls        20,000 harvests of the Riverlands' lakes for each rod and certification: half the cycles catch nothing, the fish split by the data's weights, never two fish, the
//                   experience only with a catch, the rod of another tier than the table's gives nothing
//  the water        the real file: the bodies, outlines and swim volumes; a point inside a lake's outline and outside it; a river's boxes; the fishing data every body names has an interaction
//  the throw        where a cast lands (the arc's own numbers), from the shore, across, away from the water, from a cliff, over a river; the water file's errors
//  the cast         the timeline of one cast on a clock: the reply, the loop, the bite, the result, the geometric wait, the cancel and the failed cast
// ============================================================================
#include "test_support.h"

#include "fishing/cast.h"
#include "fishing/fishing.h"
#include "fishing/predicate.h"
#include "fishing/water_map.h"
#include "gather/gathering.h"
#include "session/player_world.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace fs = std::filesystem;
using std::chrono::seconds;

static int g_pass = 0, g_fail = 0;
#undef CHECK
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;

namespace id {
inline constexpr int64_t kFishing = 108752770061964ll;                 // the profession
inline constexpr int64_t kLakes = 6064631029846245378ll;               // Fish_Riverlands_Lakes_T1
inline constexpr int64_t kRivers = 6064630921524806928ll;              // Fish_Riverlands_Rivers_T1
inline constexpr int64_t kBluegill = 6064629326176452611ll, kBass = 6064629326234910728ll, kSalmon = 6064629326234976266ll;
inline constexpr int64_t kPoleStarter = 6064631034410303488ll;         // Gear_Artisan_Fishing_Tool_Pole_Starter (tier 0)
inline constexpr int64_t kPoleT2 = 6064632236645351446ll;              // Gear_Artisan_Fishing_Tool_Pole_T2_0 (tier 1)
}  // namespace id

static const char* db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef FISHING_CONTENT_DB_PATH
    if (fs::exists(FISHING_CONTENT_DB_PATH)) return FISHING_CONTENT_DB_PATH;
#endif
    return nullptr;
}

static const char* water_path() {
#ifdef FISHING_WATER_PATH
    if (fs::exists(FISHING_WATER_PATH)) return FISHING_WATER_PATH;
#endif
    return nullptr;
}

struct World {
    std::optional<content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    gather::Gathering* g = nullptr;

    bool open(const char* path) {
        std::string error;
        db = content::ContentDb::open(path, error);
        if (!db) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            return false;
        }
        world = std::make_unique<PlayerWorld>(&*db, "", bootstrap::GameStateMode::Tier2, kServerId, guids);
        if (!world->ready()) {
            std::printf("  [FAIL] the world is not ready: %s\n", world->content_problem().c_str());
            return false;
        }
        gather::Config cfg;
        if (!world->start_gathering(cfg, error)) {
            std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
            return false;
        }
        g = world->gathering();
        return true;
    }
    const abilities::Catalog& catalog() const { return *world->ability_catalog(); }
    std::unique_ptr<stats::CharacterStats> make_stats(int level = 25) {
        const bootstrap::StatContext ctx = world->stat_context();
        auto cs = std::make_unique<stats::CharacterStats>(*ctx.bundle, *ctx.gear, 2, level, ctx.rounding);
        std::string err;
        if (!cs->start({}, err)) {
            std::printf("  [FAIL] the character's stats do not start: %s\n", err.c_str());
            std::exit(2);
        }
        return cs;
    }
};

// ---- the predicates ---------------------------------------------------------------------------------------------------------------------------------------------

static fishing::Facts facts_of(int certification, int tier) {
    fishing::Facts f;
    f.certification = [certification](std::string_view p) { return p == "Fishing" ? certification : -1; };
    f.equipment_tier = [tier](std::string_view slot) { return slot == "FishingRod" ? tier : -1; };
    return f;
}

static void test_predicates(World& w) {
    std::printf("the reward predicates the stat programs cannot compile, on the data's own text\n");
    // the predicates are found by their text, in the order of their ab_expr ids: the ids are contentc's own numbering, which moves when a contentc version seeds more of the
    // ability tables (2095 to 2373 in a version 44 file; version 46's node boons put them about 140 later), and the order stays the data's: nine predicates of a certification
    // and a rod's tier, and the world events' one, which also asks for an objective
    std::vector<int64_t> rod_exprs, event_exprs;
    for (int64_t id = 1; id <= static_cast<int64_t>(w.catalog().exprs()); ++id) {   // (contentc numbers ab_expr from 1 with no gap)
        const content::ExprRow* e = w.catalog().expr(id);
        if (!e || e->text.find("GetEquipmentCertificationTier") == std::string::npos || e->text.find("EEquipSlot::FishingRod") == std::string::npos) continue;
        (e->text.find("Player_HasObjective") == std::string::npos ? rod_exprs : event_exprs).push_back(id);
    }
    CHECK(rod_exprs.size() == 9 && event_exprs.size() == 1, "the tables hold nine predicates of a certification and a rod's tier, and the world events' one");
    // each predicate, in that order: the first (the certification) and the second (the rod's tier) number it names: `cert >= A && tier == B` (or >= B)
    struct Case {
        int  cert_min, tier;
        bool tier_at_least;
    };
    const Case cases[] = {{0, 0, false}, {1, 1, false}, {2, 2, false}, {0, 0, false}, {1, 1, false}, {2, 2, false}, {3, 3, false}, {4, 4, false}, {3, 3, false}};
    size_t unsupported = 0, checked = 0;
    bool all_ok = true;
    for (size_t i = 0; i < std::size(cases) && i < rod_exprs.size(); ++i) {
        const Case& c = cases[i];
        const int64_t expr = rod_exprs[i];
        const content::ExprRow* e = w.catalog().expr(expr);
        unsupported += (e->program < 0 && !e->error.empty()) ? 1 : 0;
        for (int cert = 0; cert <= 4; ++cert) {
            for (int tier = 0; tier <= 4; ++tier) {
                const std::optional<double> v = fishing::evaluate_predicate(e->text, facts_of(cert, tier));
                const bool want = cert >= c.cert_min && (c.tier_at_least ? tier >= c.tier : tier == c.tier);
                ++checked;
                if (!v || (*v != 0.0) != want) {
                    all_ok = false;
                    std::printf("      (expression %lld: certification %d, rod tier %d gives %s, want %d)\n", static_cast<long long>(expr), cert, tier, v ? (*v != 0 ? "true" : "false") : "nothing", want);
                }
            }
        }
    }
    CHECK(unsupported == 9, "the stat programs cannot compile any of the nine (their error column says unsupported function)");
    CHECK(all_ok && checked == 9 * 25, "every certification and rod tier gives what the text says: the certification at least the table's, the rod's tier its own");
    // the world events' fish also ask for an objective: none is tracked, so the answer is false whatever the rest says
    if (const content::ExprRow* e = event_exprs.empty() ? nullptr : w.catalog().expr(event_exprs.front())) {
        const std::optional<double> v = fishing::evaluate_predicate(e->text, facts_of(4, 4));
        CHECK(v && *v == 0.0, "the world events' predicate (an objective as well as the rod) is false: no objective is tracked");
    } else {
        CHECK(false, "the world events' predicate is in the tables");
    }
    // texts that are not of these shapes
    const fishing::Facts f = facts_of(1, 1);
    CHECK(!fishing::evaluate_predicate("GetStat(GetInstigator(), <Health>) > 1", f), "a function the evaluator does not know is no answer (the roller counts it as before)");
    CHECK(!fishing::evaluate_predicate("GetEquipmentCertificationTier(GetInstigator(), EEquipSlot::FishingRod) ==", f), "a text that does not parse is no answer");
    CHECK(!fishing::evaluate_predicate("1 + $", f), "a character of no shape is no answer");
    CHECK(!fishing::evaluate_predicate("", f), "an empty text is no answer");
    const auto v1 = fishing::evaluate_predicate("// a comment\n(1 + 1 == 2) && !(0 > 1) || false", f);
    CHECK(v1 && *v1 == 1.0, "comments, parentheses, !, && and || are read");
    const auto v3 = fishing::evaluate_predicate("!0 == 2", f);
    CHECK(v3 && *v3 == 0.0, "a ! binds tighter than a comparison, as in C: (!0) == 2 is false");
    const auto v4 = fishing::evaluate_predicate("!(1 == 2) && !0 && !!3", f);
    CHECK(v4 && *v4 == 1.0, "a ! before a parenthesis, a number or another ! negates only that");
    const auto v2 = fishing::evaluate_predicate("GetEquipmentCertificationTier(GetInstigator(), EEquipSlot::Pickaxe) == 0", f);
    CHECK(v2 && *v2 == 0.0, "a slot with no tool is -1, never a tier");
}

// ---- the data -----------------------------------------------------------------------------------------------------------------------------------------------------

static void test_data(World& w) {
    std::printf("the fishing interactions and their reward tables\n");
    const int64_t guids[] = {6064630921524806928ll, 6064631007450562560ll, 6064631029846245378ll, 6064632990160125952ll, 6064632990162616321ll, 6064633453313130538ll,
                             6064633453318242347ll, 6064633453323223084ll, 6064633571124248576ll, 6064633611578245120ll, 6064633820038823936ll};
    size_t found = 0, rod = 0, novice = 0, fifteen = 0;
    for (const int64_t guid : guids) {
        const content::GatherDefRow* d = w.g->interaction(guid);
        if (!d) continue;
        ++found;
        rod += d->tool_slot == 35 ? 1 : 0;
        novice += (d->cert_tier == 0 && d->profession_guid == id::kFishing) ? 1 : 0;
        fifteen += d->base_duration == 15.0 ? 1 : 0;
    }
    CHECK(found == 11, "all eleven fishing interactions are in the gathering tables");
    CHECK(rod == 11 && novice == 11 && fifteen == 11, "each is Fishing, a Novice's, the rod's slot, 15 seconds");
    CHECK(w.g->fishing_one_of() > 0, "the tier tables' pick of two is read as one (the data's default of 2 would give a fish at every cycle)");
    std::printf("      (%zu tier tables read as one pick)\n", w.g->fishing_one_of());
    // the rod's tiers: the T1 and the Starter pole are tier 0, the T2 poles 1 (the predicates' second number)
    const content::GatherToolRow* starter = w.g->tool(id::kPoleStarter);
    const content::GatherToolRow* t2 = w.g->tool(id::kPoleT2);
    CHECK(starter && starter->cert_tier == 0 && starter->tool_slot == 35 && starter->profession_guid == id::kFishing, "the Starter pole is a tier 0 fishing rod");
    CHECK(t2 && t2->cert_tier == 1, "the T2 pole is tier 1");
    // the channel: 15 s at rating 0
    auto cs = w.make_stats();
    std::string err;
    auto run = cs->evaluation({}, err);
    const content::GatherDefRow* lake = w.g->interaction(id::kLakes);
    std::optional<double> s = (run && lake) ? w.g->channel_seconds(*lake, *run, err) : std::nullopt;
    CHECK(s && std::fabs(*s - 15.0) < 1e-6, "the channel of a lake at rating 0 is the interaction's 15 s");
}

// ---- the rolls ------------------------------------------------------------------------------------------------------------------------------------------------------

struct Tally {
    int n = 0, nothing = 0, bluegill = 0, bass = 0, salmon = 0, other = 0, two = 0, xp_without_catch = 0, catch_without_xp = 0, adventuring = 0;
    double mean_xp = 0;
    int    rarities[7] = {0, 0, 0, 0, 0, 0, 0};
};

static Tally roll_many(World& w, int64_t water, int cert, int tier, int n, uint64_t seed) {
    Tally t;
    auto cs = w.make_stats();
    std::string err;
    auto run = cs->evaluation({}, err);
    const content::GatherDefRow* def = w.g->interaction(water);
    stats::SplitMix rng;
    rng.state = seed;
    const fishing::Facts facts = facts_of(cert, tier);
    double xp = 0;
    int catches = 0;
    for (int i = 0; i < n && run && def; ++i) {
        auto h = w.g->harvest(*def, *run, 25, world::NodeSite{}, nullptr, rng, err, &facts);
        if (!h) continue;
        ++t.n;
        if (h->items.empty()) ++t.nothing;
        if (h->items.size() > 1) ++t.two;
        for (const combat::RolledItem& it : h->items) {
            if (it.item == id::kBluegill) ++t.bluegill;
            else if (it.item == id::kBass) ++t.bass;
            else if (it.item == id::kSalmon) ++t.salmon;
            else ++t.other;
        }
        if (h->items.empty() && h->profession_xp > 0) ++t.xp_without_catch;
        if (!h->items.empty() && h->profession_xp <= 0) ++t.catch_without_xp;
        if (!h->items.empty()) {
            ++catches;
            xp += static_cast<double>(h->profession_xp);
            if (h->adventuring_xp > 0) ++t.adventuring;
            t.rarities[std::clamp(h->primary_rarity, 0, 6)]++;
        }
    }
    t.mean_xp = catches > 0 ? xp / catches : 0;
    return t;
}

static bool near_share(int count, int n, double want, double sigmas = 3.5) {
    const double p = want, got = static_cast<double>(count) / n;
    return std::fabs(got - p) <= sigmas * std::sqrt(p * (1 - p) / n) + 0.002;
}

static void test_rolls(World& w) {
    std::printf("the rolls: 20,000 harvests of a lake for a Novice with a tier 0 rod, and the other rods\n");
    constexpr int N = 20000;
    const Tally a = roll_many(w, id::kLakes, /*cert=*/0, /*tier=*/0, N, 11);
    char line[256];
    std::snprintf(line, sizeof line, "a Novice with the tier 0 rod: nothing %.1f %%, Bluegill %.1f %%, Largemouth Bass %.1f %%, Salmon %d, other %d (the data: 50, 25, 25)", 100.0 * a.nothing / a.n,
                  100.0 * a.bluegill / a.n, 100.0 * a.bass / a.n, a.salmon, a.other);
    CHECK(a.n == N && near_share(a.nothing, N, 0.50) && near_share(a.bluegill, N, 0.25) && near_share(a.bass, N, 0.25) && a.salmon == 0 && a.other == 0, line);
    CHECK(a.two == 0, "never two fish in one cycle");
    CHECK(a.xp_without_catch == 0 && a.catch_without_xp == 0, "the profession's experience comes with a catch and only with a catch");
    CHECK(a.adventuring == (N - a.nothing), "Adventuring's experience comes with every catch");
    CHECK(a.mean_xp > 0, "a catch's experience is the data's, above zero");
    const double common = static_cast<double>(a.rarities[0] + a.rarities[1] * 0) / std::max(1, N - a.nothing);
    std::snprintf(line, sizeof line, "the rarity of a catch follows the _T1_Only variation: %.1f %% Common (the table: 85.4 %% at rating 0)", 100.0 * a.rarities[0] / std::max(1, N - a.nothing));
    CHECK(std::fabs(common - 0.854) < 0.02, line);
    // the Apprentice's pole and certification: Salmon joins, with the data's weights 20, 20, 10 (and 50 for nothing)
    const Tally b = roll_many(w, id::kLakes, 1, 1, N, 12);
    std::snprintf(line, sizeof line, "an Apprentice with the tier 1 rod: nothing %.1f %%, Bluegill %.1f %%, Bass %.1f %%, Salmon %.1f %% (the data: 50, 20, 20, 10)", 100.0 * b.nothing / b.n, 100.0 * b.bluegill / b.n,
                  100.0 * b.bass / b.n, 100.0 * b.salmon / b.n);
    CHECK(near_share(b.nothing, N, 0.50) && near_share(b.bluegill, N, 0.20) && near_share(b.bass, N, 0.20) && near_share(b.salmon, N, 0.10) && b.two == 0, line);
    // a rod of another tier than the table's, or a certification below it, gives nothing at all
    const Tally c = roll_many(w, id::kLakes, 0, 1, 2000, 13);   // a Novice with the tier 1 rod: the tier 1 table asks an Apprentice, the tier 0 table a tier 0 rod
    CHECK(c.nothing == c.n, "a Novice with a tier 1 rod catches nothing");
    const Tally d = roll_many(w, id::kLakes, 1, 0, 2000, 14);   // an Apprentice with the tier 0 rod: the tier 0 table takes it (the certification only has to reach it)
    CHECK(near_share(d.nothing, 2000, 0.50) && d.salmon == 0, "an Apprentice with a tier 0 rod fishes the tier 0 table");
    const Tally e = roll_many(w, id::kLakes, 0, -1, 2000, 15);  // no rod
    CHECK(e.nothing == e.n, "no rod, no fish");
    // the same data on a river
    const Tally r = roll_many(w, id::kRivers, 0, 0, 4000, 16);
    CHECK(r.n == 4000 && r.nothing > 0 && r.nothing < r.n && r.two == 0, "the rivers' table rolls too: some cycles catch, some do not, never two fish");
}

// ---- the water --------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_water_file(World& w) {
    std::printf("the real water file\n");
    const char* path = water_path();
    if (!path) {
        std::printf("  [skip] dist/content/fishing_water.tsv is not there (run scripts/extract_game_data.ps1 -Only tools,waterbodies)\n");
        return;
    }
    fishing::WaterMap m;
    std::string err;
    CHECK(m.load(path, err), "the file reads");
    CHECK(m.bodies() == 106 && m.lakes() == 64 && m.rivers() == 42, "106 fishable waters: 64 lakes and 42 rivers");
    CHECK(m.outlines() == 64 && m.boxes() == 1102, "64 lake outlines and 1,102 swim volumes");
    const auto data = m.fishing_data();
    size_t resolved = 0;
    for (const auto& [guid, count] : data) resolved += w.g->interaction(guid) ? 1 : 0;
    CHECK(data.size() == 6 && resolved == 6, "the six fishing data the waters name are all interactions of the gathering tables");
    size_t lake_bodies = 0, river_bodies = 0;
    for (const auto& [guid, count] : data) {
        if (guid == id::kLakes) lake_bodies = count;
        if (guid == id::kRivers) river_bodies = count;
    }
    CHECK(lake_bodies == 60 && river_bodies == 39, "60 bodies name the Riverlands' lakes' data and 39 the rivers'");
    // every outline: a swim volume's centre that is inside it is on that water; the nearest water to an outline's own vertex is at that vertex
    size_t inside_hits = 0, vertices_found = 0, lakes_probed = 0;
    for (size_t b = 0; b < m.bodies(); ++b) {
        const fishing::WaterMap::BodyInfo info = m.info(b);
        if (info.river || info.outline_points == 0) continue;
        ++lakes_probed;
        const auto n = m.nearest(info.x, info.y, 1.0e9);   // some water near the body's place (an outline's vertex)
        if (n) ++vertices_found;
        if (const auto hit = m.at(info.x, info.y); hit && hit->body == b) ++inside_hits;
    }
    CHECK(lakes_probed == 64 && vertices_found == 64, "every lake has an outline point to find");
    CHECK(inside_hits > 0, "some lake's place (its actor's origin) is inside its own outline");
    CHECK(!m.at(0, 0) && !m.at(-1.0e6, 1.0e6), "far from every lake is dry land");
    // a point nudged out of an outline vertex by 5 m, away from the lake's place, is dry unless another water is there; one nudged in, towards the place, is wet for a lake that is wide enough
    size_t wet_in = 0, probes = 0;
    for (size_t b = 0; b < m.bodies(); ++b) {
        const fishing::WaterMap::BodyInfo info = m.info(b);
        if (info.river || info.outline_points == 0) continue;
        const auto n = m.nearest(info.x, info.y, 1.0e9);
        if (!n || n->body != b) continue;
        const double dx = info.x - n->x, dy = info.y - n->y, len = std::hypot(dx, dy);
        if (len < 4000) continue;   // a lake wide enough that 20 m inside the nearest vertex is inside it
        ++probes;
        const auto in = m.at(n->x + dx / len * 2000, n->y + dy / len * 2000);
        if (in && in->body == b) ++wet_in;
    }
    CHECK(probes > 0 && wet_in * 10 >= probes * 8, "20 m inside an outline's vertex, towards the lake's middle, is on the lake in at least four cases of five (a vertex on a narrow neck is not)");
    // a river: its first swim volume's centre is on it
    size_t river_hits = 0, rivers_probed = 0, river_water = 0;
    for (size_t b = 0; b < m.bodies(); ++b) {
        const fishing::WaterMap::BodyInfo info = m.info(b);
        if (!info.river) continue;
        ++rivers_probed;
        const auto c = m.box_centre(b);
        if (!c) continue;
        const auto hit = m.at(c->first, c->second);
        if (hit) ++river_water;
        if (hit && hit->river) ++river_hits;
    }
    CHECK(rivers_probed == 42 && river_water == 42 && river_hits == 40, "the centre of each river's first swim volume is on water, and on the river itself but for two that begin inside a lake (the lake answers first)");
}

// ---- the water's errors and the synthetic lake ----------------------------------------------------------------------------------------------------

static std::string tmp_file(const char* name, const std::string& text) {
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream(p, std::ios::binary) << text;
    return p.string();
}

/// A lake 100 m square with its surface at z 1000 (the outline's corners) and a river of one volume 30 m by 6 m turned 30 degrees.
static const char* kLakeText =
    "# a test water\n"
    "body\t0\tlake\tTestLake\t6064631029846245378\t0\t0\t1000\n"
    "outline\t0\t-5000,-5000\t5000,-5000\t5000,5000\t-5000,5000\n"
    "body\t1\triver\tTestRiver\t6064630921524806928\t30000\t0\t900\n"
    "box\t1\t30000\t0\t900\t1500\t300\t100\t30\n";

static void test_water_errors() {
    std::printf("the water file's errors and the small map\n");
    fishing::WaterMap m;
    std::string err;
    CHECK(m.parse(kLakeText, err) && m.bodies() == 2 && m.lakes() == 1 && m.rivers() == 1, "the small map reads");
    CHECK(m.at(0, 0) && m.at(0, 0)->surface_z == 1000.0 && !m.at(0, 0)->river && m.at(0, 0)->fishing == id::kLakes, "the lake's middle is the lake, its surface at 1000");
    CHECK(!m.at(6000, 0) && !m.at(0, -5001), "just outside the outline is dry");
    CHECK(m.at(4999, 4999) && m.at(-4999, -4999), "just inside its corners is wet");
    // the river's box, turned 30 degrees: its long side goes along 30 degrees
    const double c = std::cos(30.0 * 3.14159265358979 / 180.0), s = std::sin(30.0 * 3.14159265358979 / 180.0);
    CHECK(m.at(30000 + c * 1400, s * 1400) && m.at(30000 - c * 1400, -s * 1400), "the river's volume is along its yaw");
    CHECK(!m.at(30000 + c * 1600, s * 1600) && !m.at(30000 - s * 400, c * 400), "its ends and its sides stop where the box does");
    CHECK(m.at(30000, 0)->surface_z == 900.0 && m.at(30000, 0)->river, "a river's surface is its volume's height");
    const auto near = m.nearest(5200, 5100, 400);
    CHECK(near && near->body == 0 && near->distance <= 400 && near->x == 5000 && near->y == 5000, "the nearest water to a point beside the lake's corner is that corner, within the radius");
    CHECK(!m.nearest(8000, 8000, 400), "none within a radius that is too short");
    // the errors, one at a time
    const std::pair<const char*, const char*> bad[] = {
        {"body\t0\tlake\tA\t1\t0\t0\n", "a body line has 8 fields"},
        {"body\t0\tpond\tA\t1\t0\t0\t0\n", "a body is a lake or a river"},
        {"body\t0\tlake\tA\t0\t0\t0\t0\n", "fishing data or place"},
        {"body\t0\tlake\tA\t1\t0\t0\t0\nbody\t0\tlake\tB\t1\t0\t0\t0\n", "listed twice"},
        {"outline\t7\t0,0\t1,1\t2,2\n", "names no body"},
        {"body\t0\tlake\tA\t1\t0\t0\t0\noutline\t0\t0,0\t1,1\n", "fewer than three points"},
        {"body\t0\tlake\tA\t1\t0\t0\t0\noutline\t0\t0,0\t1;1\t2,2\n", "not x,y"},
        {"body\t0\tlake\tA\t1\t0\t0\t0\nbox\t0\t0\t0\t0\t0\t10\t10\t0\n", "has no size"},
        {"body\t0\tlake\tA\t1\t0\t0\t0\nwater\t0\n", "unknown line kind"},
        {"# only a comment\n", "no water body"},
        {"body\t0\tlake\tA\t1\t0\t0\t0\n", "neither an outline nor a swim volume"},
    };
    size_t good = 0;
    for (const auto& [text, want] : bad) {
        fishing::WaterMap x;
        std::string e;
        if (!x.parse(text, e) && e.find(want) != std::string::npos) ++good;
        else std::printf("      (the text %.30s... answered \"%s\", wanted \"%s\")\n", text, e.c_str(), want);
    }
    CHECK(good == sizeof(bad) / sizeof(bad[0]), "every malformed line is refused with a reason that names it");
    fishing::WaterMap missing;
    CHECK(!missing.load("no/such/file.tsv", err) && err.find("cannot open") != std::string::npos, "a file that is not there is refused");
}

// ---- the throw ---------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_throw(World& w) {
    std::printf("where a cast lands\n");
    // the arc's own numbers, worked here from the projectile's: speed 3500, 20 degrees, the arcing gravity scale 15 on 980
    const fishing::Throw t;
    double flight = 0;
    const double reach0 = fishing::flight_distance(t, 100.0, &flight);
    const double rad = 20.0 * 3.14159265358979 / 180.0, vx = 3500 * std::cos(rad), vz = 3500 * std::sin(rad), g = 980.0 * 15.0;
    const double want_t = (vz + std::sqrt(vz * vz + 2 * g * 100.0)) / g;
    CHECK(std::fabs(flight - want_t) < 1e-9 && std::fabs(reach0 - vx * want_t) < 1e-6, "the lure's flight from 100 cm above the water: the arc's own time and reach");
    CHECK(reach0 > 700 && reach0 < 760, "about 7.4 m for a throw over water level with the feet");
    CHECK(fishing::flight_distance(t, 500.0) > reach0 && fishing::flight_distance(t, 0.0) < reach0, "a lower surface is farther, a higher one nearer");
    CHECK(fishing::flight_distance(t, -400.0) == 0.0, "a surface far above the throw cannot be reached");

    const std::string path = tmp_file("fishing_test_water.tsv", kLakeText);
    fishing::Config cfg;
    cfg.water_path = path;
    std::string err;
    auto f = fishing::Fishing::make(cfg, w.catalog(), *w.g, err);
    CHECK(f != nullptr, "the world's fishing starts on the small map");
    if (!f) return;
    CHECK(f->montage_guid() == 17322920347082018633ull && f->montage_path().find("Fishing_Montage_New") != std::string::npos, "the ability's montage and its static id are the data's");
    CHECK(f->throw_model().speed_cm_s == 3500.0 && f->throw_model().range_cm == 15000.0, "the projectile's speed and range are read from the tables");
    // the player stands on the shore of the lake: its edge is at x = -5000.  Feet at the water's surface (z 1000), facing +x (yaw 0)
    const auto lands = [&](double x, double y, double feet, double yaw) { return f->plan_cast(x, y, feet, yaw); };
    fishing::Landing a = lands(-5200, 0, 1000, 0);
    // the lure leaves 40 cm behind and 150 cm to the right of the character and flies reach0 forward
    const double want_x = -5200 - 40 + reach0, want_y = 150;   // yaw 0: forward is +x, right is +y
    CHECK(a.lands && std::fabs(a.x - want_x) < 1 && std::fabs(a.y - want_y) < 1 && a.water && a.water->fishing == id::kLakes && a.def != nullptr, "on the shore facing the lake the lure comes down on it, 7 m out");
    CHECK(a.def && a.def->name == "Fish_Riverlands_Lakes_T1", "and the interaction is the lake's fishing data");
    fishing::Landing away = lands(-5200, 0, 1000, 180);
    CHECK(!away.lands && !away.why.empty(), "facing away from the lake it comes down on dry land");
    fishing::Landing far = lands(-9000, 0, 1000, 0);
    CHECK(!far.lands, "from 40 m away the lure does not reach the water");
    fishing::Landing side = lands(-5200, 0, 1000, 90);
    CHECK(!side.lands, "along the shore it falls on land");
    fishing::Landing cliff = lands(-5200, 0, 1000 + 2000, 0);
    CHECK(!cliff.lands, "from 20 m above the water the lure does not reach it");
    fishing::Landing bank = lands(-5400, 0, 1000 + 300, 0);
    CHECK(bank.lands && bank.distance_cm > a.distance_cm - 400, "from a bank 3 m above the water the lure flies farther and still lands");
    // a lake's surface above the feet: a player standing in a hole 2 m below the surface cannot throw onto it
    fishing::Landing hole = lands(-5200, 0, 1000 - 200, 0);
    CHECK(!hole.lands, "from 2 m below the surface the lure cannot come down onto it");
    // the river: its volume is 30 m long, 6 m wide at x 30000, turned 30 degrees; the player stands on its bank and throws across
    const double cr = std::cos(30.0 * 3.14159265358979 / 180.0), sr = std::sin(30.0 * 3.14159265358979 / 180.0);
    const double yaw_across = 30.0 + 90.0;   // the river's width is across its length
    const double fx = std::cos(yaw_across * 3.14159265358979 / 180.0), fy = std::sin(yaw_across * 3.14159265358979 / 180.0);
    // stand so that the landing (in front, 7 m minus 40 cm, and 150 cm to the right) is on the river's centre line
    const double rx = -fy, ry = fx;
    const double sx = 30000 - fx * (reach0 - 40) - rx * 150, sy = 0 - fy * (reach0 - 40) - ry * 150;
    (void)cr;
    (void)sr;
    fishing::Landing river = lands(sx, sy, 900, yaw_across);
    CHECK(river.lands && river.water && river.water->river && river.def && river.def->name == "Fish_Riverlands_Rivers_T1", "a cast across a river lands on it and names the river's data");
    CHECK(river.water && river.water->surface_z == 900.0, "a river's surface is its volume's");
    // errors of the world's fishing
    fishing::Config bad;
    bad.water_path = "no/such/file.tsv";
    std::string e2;
    auto none = fishing::Fishing::make(bad, w.catalog(), *w.g, e2);
    CHECK(!none && e2.find("no/such/file.tsv") != std::string::npos && e2.find("extract_game_data") != std::string::npos, "a missing water file is no fishing, and the reason names the file and how to make it");
    // a water file that names fishing data the gathering tables do not have
    const std::string odd = tmp_file("fishing_test_odd.tsv", "body\t0\tlake\tOdd\t4242\t0\t0\t1000\noutline\t0\t-5000,-5000\t5000,-5000\t5000,5000\t-5000,5000\n");
    fishing::Config oddc;
    oddc.water_path = odd;
    auto odd_world = fishing::Fishing::make(oddc, w.catalog(), *w.g, e2);
    CHECK(odd_world != nullptr, "such a map still starts");
    if (odd_world) {
        fishing::Landing o = odd_world->plan_cast(-5200, 0, 1000, 0);
        CHECK(!o.lands && o.why.find("gathering tables do not have") != std::string::npos, "but a cast on that water fails and says why");
    }
    fs::remove(path);
    fs::remove(odd);
}

// ---- the real lake next to the login spot ---------------------------------------------------------------------------------------------------------------

static void test_login_lake(World& w) {
    std::printf("the lake next to the login spot, from the real water file\n");
    const char* path = water_path();
    if (!path) {
        std::printf("  [skip] dist/content/fishing_water.tsv is not there\n");
        return;
    }
    fishing::Config cfg;
    cfg.water_path = path;
    std::string err;
    auto f = fishing::Fishing::make(cfg, w.catalog(), *w.g, err);
    CHECK(f != nullptr, "the world's fishing starts on the real file");
    if (!f) return;
    // the login spot is at (-777664.5, 616578.7); a lake of 18,000 square metres lies 66 m from it, its surface at z 15640; from 3 m outside its edge, facing it, a cast comes down on it
    const double sx = -776630, sy = 610280, yaw = -46.0, surface = 15640.0;
    bool all = true;
    for (const double above : {0.0, 100.0, 200.0, 300.0}) {
        const fishing::Landing l = f->plan_cast(sx, sy, surface + above, yaw);
        if (!l.lands || !l.water || l.water->fishing != id::kLakes || std::fabs(l.water->surface_z - surface) > 1.0) {
            all = false;
            std::printf("      (feet %.0f cm above the surface: %s)\n", above, l.lands ? "lands on other water" : l.why.c_str());
        }
    }
    CHECK(all, "from 3 m outside its near edge, facing it, a cast lands on that lake with the feet at the surface and up to 3 m above it");
    const fishing::Landing away = f->plan_cast(sx, sy, surface + 200, yaw + 180.0);
    CHECK(!away.lands, "facing away from it the cast comes down on dry land");
    const fishing::Landing far = f->plan_cast(-777664.5, 616578.7, surface + 200, yaw);
    CHECK(!far.lands, "from the login spot itself, 66 m from the lake, it does not reach");
    const auto near = f->water().nearest(-777664.5, 616578.7, 10000.0);
    CHECK(near && near->distance > 6000 && near->distance < 7000, "the nearest water to the login spot is that lake, 66 m away");
}

// ---- the cast on a clock --------------------------------------------------------------------------------------------------------------------------------------

static stats::SplitMix forced(const std::vector<double>& draws) {
    stats::SplitMix m;
    static std::deque<std::vector<double>> keep;   // (a deque: the generator points at its vector, which must not move)
    keep.push_back(draws);
    m.script = &keep.back();
    return m;
}

static std::vector<fishing::Event> run_to(fishing::Cast& c, double until, stats::SplitMix& rng, double step = 0.02) {
    std::vector<fishing::Event> all;
    for (double t = 0; t <= until + 1e-9; t += step) {
        for (const fishing::Event& e : c.advance(t, rng)) all.push_back(e);
    }
    return all;
}

static const fishing::Event* find(const std::vector<fishing::Event>& v, fishing::Step s, size_t nth = 0) {
    size_t n = 0;
    for (const fishing::Event& e : v) {
        if (e.step == s && n++ == nth) return &e;
    }
    return nullptr;
}

static void test_cast() {
    std::printf("the cast on a clock\n");
    const fishing::Timing t;
    // a draw of 0 is the first tick after the channel: the bite comes one tick after it
    {
        fishing::Cast c(10.0, /*lands=*/true, /*loop=*/15.0);
        stats::SplitMix rng = forced({0.0, 0.0, 0.0});
        const auto ev = run_to(c, 10.0 + 60.0, rng);
        const auto* reply = find(ev, fishing::Step::Reply);
        const auto* cd = find(ev, fishing::Step::CooldownEnd);
        const auto* bite = find(ev, fishing::Step::Bite);
        const auto* result = find(ev, fishing::Step::Result);
        const auto* bite2 = find(ev, fishing::Step::Bite, 1);
        CHECK(reply && std::fabs(reply->at - 10.1) < 0.021, "the reply comes 100 ms after the trigger");
        CHECK(cd && std::fabs(cd->at - 11.1) < 0.021, "the cooldown ends a second after the reply");
        const double want_bite = 10.0 + t.reply + t.lure + 15.0 + t.tick;
        CHECK(bite && std::fabs(bite->at - want_bite) < 0.021, "the first bite: the lure's moment, the channel of 15 s and one tick");
        CHECK(result && std::fabs(result->at - (bite->at + t.bite_to_result)) < 0.021, "the result comes 4.37 s after the bite");
        CHECK(bite2 && std::fabs(bite2->at - (result->at + 15.0 + t.tick)) < 0.021, "the next loop starts at the result: its bite is a channel and a tick later");
        CHECK(c.bites() >= 2 && c.results() >= 1, "bites and results are counted");
        CHECK(c.active() && !c.ending(), "the cast runs");
    }
    // a draw of 0.5 is the median of the geometric wait: 69 ticks at one in a hundred
    {
        fishing::Cast c(0.0, true, 15.0);
        stats::SplitMix rng = forced({0.5});
        const auto ev = run_to(c, 40.0, rng);
        const auto* bite = find(ev, fishing::Step::Bite);
        const double ticks = std::floor(std::log(0.5) / std::log(0.99)) + 1.0;
        CHECK(bite && std::fabs(bite->at - (t.reply + t.lure + 15.0 + ticks * t.tick)) < 0.021 && ticks == 69.0, "a draw of one half waits 69 ticks (4.6 s) after the channel");
    }
    // the geometric wait: the mean tick count is 100
    {
        stats::SplitMix rng;
        rng.state = 77;
        double sum = 0;
        constexpr int N = 40000;
        for (int i = 0; i < N; ++i) {
            fishing::Cast c(0.0, true, 0.0);
            c.advance(1.2, rng);   // the reply (which draws the first bite's wait) and the cooldown's end: the bite is next
            sum += (c.next_due() - (t.reply + t.lure)) / t.tick;
        }
        const double mean = sum / N;
        char line[160];
        std::snprintf(line, sizeof line, "the wait after the channel is geometric: the mean of %d draws is %.1f ticks (the branch's one in a hundred)", N, mean);
        CHECK(std::fabs(mean - 100.0) < 3.0, line);
    }
    // a cancel: before the first check it cannot be taken, after it the loop ends in the retail's steps
    {
        fishing::Cast c(0.0, true, 15.0);
        stats::SplitMix rng = forced({0.0});
        CHECK(!c.cancel(0.05, fishing::Stop::Moving), "before the reply the branch cannot be taken");
        c.advance(0.1, rng);
        CHECK(!c.cancel(0.15, fishing::Stop::Moving), "nor before the montage's first check (0.11 s after the reply)");
        CHECK(c.cancel(0.30, fishing::Stop::Moving), "after it the movement branch ends the cast");
        CHECK(!c.cancel(0.31, fishing::Stop::Combat), "a cast that is ending cannot end again");
        const auto ev = run_to(c, 5.0, rng);
        const auto* cancel = find(ev, fishing::Step::Cancel);
        const auto* off = find(ev, fishing::Step::BuffOff);
        const auto* clear = find(ev, fishing::Step::Clear);
        CHECK(cancel && std::fabs(cancel->at - 0.30) < 0.021 && find(ev, fishing::Step::Bite) == nullptr, "the montage jumps to AbilityEnd at once and no bite comes");
        CHECK(off && std::fabs(off->at - (0.30 + t.buff_off_after_end)) < 0.021, "the buff goes about 100 ms after the jump");
        CHECK(clear && std::fabs(clear->at - (0.30 + t.clear_after_end)) < 0.021, "the montage is cleared 2.0 s after the jump");
        CHECK(!c.active() && c.stopped_by() == fishing::Stop::Moving, "the cast is over, and says what ended it");
    }
    // a cancel while a fish is being reeled in loses the fish: no result
    {
        fishing::Cast c(0.0, true, 15.0);
        stats::SplitMix rng = forced({0.0, 0.0});
        std::vector<fishing::Event> ev;
        double now = 0;
        for (; now < 40.0 && !c.reeling(); now += 0.02) for (const fishing::Event& e : c.advance(now, rng)) ev.push_back(e);
        CHECK(c.reeling(), "the fish bit");
        CHECK(c.cancel(now, fishing::Stop::Combat), "the combat branch can be taken while reeling");
        for (const fishing::Event& e : run_to(c, now + 10.0, rng)) ev.push_back(e);
        CHECK(find(ev, fishing::Step::Result) == nullptr && !c.result_due(now + 100), "the fish is lost: no result");
    }
    // result_due: the caller checks the room before advancing
    {
        fishing::Cast c(0.0, true, 15.0);
        stats::SplitMix rng = forced({0.0, 0.0});
        double now = 0;
        for (; now < 40.0 && !c.reeling(); now += 0.02) c.advance(now, rng);
        CHECK(!c.result_due(now), "right after the bite the result is not due");
        CHECK(c.result_due(now + t.bite_to_result + 0.1), "it is due 4.37 s later");
        const auto ev = c.advance(now + t.bite_to_result + 0.1, rng);
        CHECK(find(ev, fishing::Step::Result) != nullptr && !c.result_due(now + t.bite_to_result + 0.1), "and taken once");
    }
    // a cast that lands on nothing: the montage jumps to CastFail, then the buff and the montage go
    {
        fishing::Cast c(0.0, /*lands=*/false, 0.0);
        stats::SplitMix rng = forced({});
        const auto ev = run_to(c, 10.0, rng);
        const auto* fail = find(ev, fishing::Step::CastFail);
        const auto* off = find(ev, fishing::Step::BuffOff);
        const auto* clear = find(ev, fishing::Step::Clear);
        CHECK(fail && std::fabs(fail->at - (t.reply + t.lure)) < 0.021, "a failed cast jumps to CastFail at the lure's moment");
        CHECK(off && fail && std::fabs(off->at - (fail->at + t.fail_buff_off)) < 0.021 && clear && std::fabs(clear->at - (fail->at + t.fail_clear)) < 0.021, "and its buff and montage go on the montage's own times");
        CHECK(find(ev, fishing::Step::Bite) == nullptr && find(ev, fishing::Step::Result) == nullptr && !c.active() && !c.lands(), "no bite, no result, and the cast is over");
    }
    // drop: a replaced cast says nothing more
    {
        fishing::Cast c(0.0, true, 15.0);
        stats::SplitMix rng = forced({0.0});
        c.advance(0.2, rng);
        c.drop();
        CHECK(!c.active() && c.advance(100.0, rng).empty() && !c.buff_on(), "a cast that is dropped queues nothing and holds no buff");
    }
}

int main() {
    spdlog::set_level(spdlog::level::warn);
    std::printf("test_worldd_fishing\n");
    test_cast();
    test_water_errors();
    const char* path = db_path();
    if (!path) {
        std::printf("  [skip] the content.db is not there (run scripts/extract_game_data.ps1)\n");
        std::printf("%d passed, %d failed\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    World w;
    if (!w.open(path)) return 2;
    test_predicates(w);
    test_data(w);
    test_rolls(w);
    test_water_file(w);
    test_throw(w);
    test_login_lake(w);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

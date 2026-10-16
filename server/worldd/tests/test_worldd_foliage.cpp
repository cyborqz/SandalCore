// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_foliage.cpp  (Gathering stage 3, part "Foliage gathering")
//
//  The painted foliage without a session: the map, the state of every instance and its regrowth, the cell actors, and the data of the real content.db (version 15).
//
//  the map          dist/content/region_foliage.tsv: 84,218 instances of 73 defs in 671 owners with 10,613 components; the three instances the captures harvested are the rows the client
//                   named (1428's Oak, 1451's Daffodil, 1519's Oak) at the positions the file has; a damaged line stops the load naming it; a key twice is refused
//  the store        Empty -> Infancy -> Juvenile -> Adult with a fake clock: an Oak large 300 + 1800 + 1800 s, a plant 1800 + 300 s, a stone 840 s; the multiplier scales every phase (the
//                   launcher's 0.01: 3 + 18 + 18 s); a long pause brings an instance through all its phases in order; a harvest at any state is Empty again and the old timer is void; the
//                   instance is cut by one player at a time
//  the cells        every cell has its state actor from the start (28 components for the captured cell, in byte order, at the cell's centre); a harvest is an item {index, 3}, a
//                   growing instance 0 or 1, an instance whole again leaves the list; a connection's first look lists the Empty instances as 11
//  the client's     (the fix after the user's test 1, 2026-10-12: no tree, plant or stone showed a prompt because an untouched cell had no state actor) the 671 actors are read back from
//  needs            their bits the way the client builds FoliageStateActorLookup and FoliageStateComponentLookup, and each of the 84,218 rows of the TSV, read again here, resolves to an
//                   initialized component through UE's FName split (trees, plants, stones); an untouched cell's open equals the retail opens of capture 1198's login and 1428, the
//                   actor block, the spawn location and 166 component blocks, bit for bit but for the retail server's own AuthServerIDReplicated; the cost of making them (time, memory)
//  the data         content.db's foliage tables resolve the map's 73 defs (69 harvestable, the duplicate name once); the Oak's and the Daffodil's states, interactions and reach cover
//                   the three captured starts with a margin of 50 cm or more; the Oak, Ash and Daffodil rewards reproduce the captured items (record, quantity, quality) of 1428, 1451, 1463,
//                   1465, 1468 and 1519
// ============================================================================
#include "capture_foliage.h"
#include "capture_gather.h"
#include "test_support.h"

#include "foliage/foliage.h"
#include "foliage/foliage_cells.h"
#include "foliage/foliage_map.h"
#include "foliage/foliage_store.h"
#include "gather/gathering.h"
#include "protocol/repl/fname.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"
#include "protocol/server_guid_allocator.h"
#include "session/player_world.h"
// == C-Y-B | CoA ==

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace fol = aoc::worldd::foliage;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
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

static const char* db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef FOLIAGE_CONTENT_DB_PATH
    if (fs::exists(FOLIAGE_CONTENT_DB_PATH)) return FOLIAGE_CONTENT_DB_PATH;
#endif
    return nullptr;
}

static const char* tsv_path() {
    const char* env = std::getenv("AOC_FOLIAGE_MAP");
    if (env && *env) return env;
#ifdef FOLIAGE_TSV_PATH
    if (fs::exists(FOLIAGE_TSV_PATH)) return FOLIAGE_TSV_PATH;
#endif
    return nullptr;
}

// ---- the map -----------------------------------------------------------------------------------------------------------------------------

static std::string tsv_line(const char* def, const char* owner, const char* comp, int index, double x = 100, double y = 200, double z = 300) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "1\t%s\tCELL\t%s\t%s\t%d\t%.1f\t%.1f\t%.1f\t0\t0\t0\t1\t1\t1\tmesh\t\n", def, owner, comp, index, x, y, z);
    return buf;
}
// [C-Y-B + C | O | A]

static void test_map_synthetic() {
    std::printf("the map: a small file, and the damaged ones\n");
    std::string text = "# region_foliage v2\n# def_id\tdef_name\t...\n";
    text += tsv_line("Tree_Oak_Large_1", "InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_10", 0, 10, 20, 30);
    text += tsv_line("Tree_Oak_Large_1", "InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_2", 5, 11, 21, 31);
    text += tsv_line("Plant_Daffodil_2", "InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_1", 0, 12, 22, 32);
    text += tsv_line("Plant_Daffodil_2", "InstancedFoliageActor_25600_3_-4_1", "IntrepidGatherableFISMC_0", 7, 13, 23, 33);
    std::string error;
    const auto m = fol::FoliageMap::parse(text, error);
    CHECK(m.has_value(), "a file of four instances loads");
    if (!m) return;
    CHECK(m->rows().size() == 4 && m->def_names().size() == 2 && m->owners().size() == 2 && m->components() == 4, "4 instances, 2 defs, 2 owners, 4 components");
    const auto& cell = m->owners()[0];
    CHECK(cell.name == "InstancedFoliageActor_25600_-41_19_0" && cell.cell_x == -41 && cell.cell_y == 19, "the owner's cell comes from its name (negative numbers too)");
    CHECK(cell.components == std::vector<std::string>({"IntrepidGatherableFISMC_1", "IntrepidGatherableFISMC_10", "IntrepidGatherableFISMC_2"}), "an owner's components are sorted by name: _1, _10, _2 (byte order, as the retail opens list them)");
    CHECK(cell.z_lo_tenths == 300 && cell.z_hi_tenths == 320 && cell.centre_z_tenths() == 310 && cell.centre_z_cm() == 31,
          "the owner's height is the middle of its lowest and highest instance: in tenths of a cm, then the nearest cm");
    {   // retail's cells (capture 1198): -31_16 is 12807.5 in the header and 12807 in the block (a tie goes down), -30_16 12986.8 and 12987
        std::string heights = "# t\n";
        heights += tsv_line("A", "InstancedFoliageActor_25600_-31_16_0", "IntrepidGatherableFISMC_0", 0, 1, 2, 12024.0);
        heights += tsv_line("A", "InstancedFoliageActor_25600_-31_16_0", "IntrepidGatherableFISMC_0", 1, 1, 2, 13591.0);
        heights += tsv_line("A", "InstancedFoliageActor_25600_-30_16_0", "IntrepidGatherableFISMC_0", 0, 1, 2, 11064.6);
        heights += tsv_line("A", "InstancedFoliageActor_25600_-30_16_0", "IntrepidGatherableFISMC_0", 1, 1, 2, 14909.1);
        std::string e2;
        const auto hm = fol::FoliageMap::parse(heights, e2);
        CHECK(hm && hm->owners().size() == 2 && hm->owners()[0].centre_z_tenths() == 128075 && hm->owners()[0].centre_z_cm() == 12807 && hm->owners()[1].centre_z_tenths() == 129868 &&
                  hm->owners()[1].centre_z_cm() == 12987,
              "retail's cells: 12807.5 cm is 128075 tenths and 12807 cm (a tie down), 12986.85 is 129868 tenths (cut at the tenth) and 12987 cm");
    }
    CHECK(m->find("InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_2", 5) == 1u && m->find("InstancedFoliageActor_25600_3_-4_1", "IntrepidGatherableFISMC_0", 7) == 3u, "an instance is found by its owner, component and index");
    CHECK(!m->find("InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_2", 4) && !m->find("InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_3", 5) &&
              !m->find("InstancedFoliageActor_25600_9_9_0", "IntrepidGatherableFISMC_2", 5) && !m->find("IFA_NodeLayoutReplicator_108646108518008_4_0", "IntrepidGatherableFISMC_2", 12),
          "a wrong index, component, owner and a town's runtime foliage find nothing");
    double cx = 0, cy = 0;
    m->cell_center(0, cx, cy);
    CHECK(cx == (-41 + 0.5) * 25600.0 && cy == (19 + 0.5) * 25600.0, "the cell's center is ((cx + 0.5) * 25,600, (cy + 0.5) * 25,600)");
    // the damaged files
    const auto fails = [&](const std::string& body, const char* want) {
        std::string e;
        const auto bad = fol::FoliageMap::parse(body, e);
        return !bad && e.find(want) != std::string::npos;
    };
    CHECK(fails("# c\nonly\tthree\tcolumns\n", "line 2: 3 columns"), "a line with the wrong column count stops the load naming it");
    CHECK(fails(tsv_line("A", "InstancedFoliageActor_25600_1_1_0", "C", 0) + "1\tA\tCELL\tInstancedFoliageActor_25600_1_1_0\tC\tx\t1\t2\t3\t0\t0\t0\t1\t1\t1\tm\t\n", "line 2: the index is no number"), "an index that is no number");
    CHECK(fails("1\tA\tCELL\tInstancedFoliageActor_25600_1_1_0\tC\t0\t1\tnan\t3\t0\t0\t0\t1\t1\t1\tm\t\n", "the position is no number"), "a position that is no number");
    CHECK(fails(tsv_line("A", "NotAnOwner", "C", 0), "no InstancedFoliageActor_25600"), "an owner that is no cell actor");
    CHECK(fails(tsv_line("A", "InstancedFoliageActor_25600_1_1_0", "C", 0) + tsv_line("B", "InstancedFoliageActor_25600_1_1_0", "C", 0), "listed twice"), "the same instance twice");
    CHECK(fails("# nothing\n", "holds no instance"), "a file with no instance");
    CHECK(!fol::FoliageMap::load("no/such/file.tsv", error) && error.find("cannot open") != std::string::npos, "a missing file says so");
}

static void test_map_real() {
    std::printf("the map: the Riverlands' region_foliage.tsv\n");
    const char* path = tsv_path();
    if (!path) {
        std::printf("  [skip] dist/content/region_foliage.tsv is not there\n");
        return;
    }
    std::string error;
    const auto m = fol::FoliageMap::load(path, error);
    CHECK(m.has_value(), "the file loads");
    if (!m) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    CHECK(m->rows().size() == 84218 && m->def_names().size() == 73 && m->owners().size() == 671 && m->components() == 10613, "84,218 instances, 73 defs, 671 owners and 10,613 components");
    size_t pillars = 0, max_comps = 0;
    for (const auto& o : m->owners()) max_comps = std::max(max_comps, o.components.size());
    CHECK(max_comps == 38, "an owner has at most 38 components");
    for (const std::string& n : m->def_names()) pillars += n.rfind("Riverlands_", 0) == 0 ? 1 : 0;
    CHECK(pillars == 4, "four defs are Riverlands_ scenery pillars");
    // the three harvests of the captures name rows of the file exactly [R: capture_foliage.h kStarts]
    struct Want {
        int capture;
        const char* def;
        double x, y, z;
    };
    const Want wants[] = {{1428, "Tree_Oak_Large_1", -1032805.5, 495823.7, 12846.0}, {1451, "Plant_Daffodil_2", -992441.0, 502971.0, 14781.1}, {1519, "Tree_Oak_Large_3", -1040048.0, 473107.0, 15217.5}};
    size_t matched = 0;
    for (const auto& s : capture_foliage::kStarts) {
        for (const Want& w : wants) {
            if (s.capture != w.capture) continue;
            const auto row = m->find(s.owner, s.component, s.has_instance ? s.instance : 0u);
            if (!row) continue;
            const auto& r = m->rows()[*row];
            if (m->def_names()[r.def] == w.def && std::fabs(r.x - w.x) < 0.2 && std::fabs(r.y - w.y) < 0.2 && std::fabs(r.z - w.z) < 0.2) ++matched;
        }
    }
    CHECK(matched == 3, "1428's Oak, 1451's Daffodil and 1519's Oak are the rows of the file the client named, at its positions");
    // a painted cell: its center is the one the retail open shows (1428: the cell (-40, 19) opens at (-1,011,200, 499,200))
    bool centre = false;
    for (const auto& o : capture_foliage::kIfaOpens) {
        if (std::string(o.name) != "InstancedFoliageActor_25600_-40_19_0") continue;
        const auto owner = m->owner_of(o.name);
        double cx = 0, cy = 0;
        if (owner) m->cell_center(*owner, cx, cy);
        centre = owner && static_cast<int64_t>(cx) == o.location[0] && static_cast<int64_t>(cy) == o.location[1] && m->owners()[*owner].components.size() == o.component_count;
    }
    CHECK(centre, "the cell (-40, 19) is where the retail open puts its state actor and has the 28 components of that open");
}
// C_Y_B : CoA

// ---- the store ---------------------------------------------------------------------------------------------------------------------------

static fol::DefTimes oak() {
    fol::DefTimes d;
    d.paint = fol::kAdult;
    d.empty_seconds = 300;
    d.has[0] = d.has[1] = d.has[2] = true;
    d.life[0] = 1800;
    d.life[1] = 1800;
    d.life[2] = 0;
    return d;
}
static fol::DefTimes plant() {
    fol::DefTimes d;
    d.paint = fol::kJuvenile;
    d.empty_seconds = 1800;
    d.has[0] = d.has[1] = true;
    d.life[0] = 300;
    return d;
}
static fol::DefTimes stone() {
    fol::DefTimes d;
    d.paint = fol::kAdult;
    d.empty_seconds = 840;
    d.has[2] = true;
    return d;
}

static void test_store() {
    std::printf("the store: regrowth with a fake clock\n");
    // rows 0 and 3 are Oaks, 1 a plant, 2 a stone
    fol::FoliageStore s({0, 1, 2, 0}, {oak(), plant(), stone()}, {});
    CHECK(s.state_of(0) == fol::kAdult && s.state_of(1) == fol::kJuvenile && s.state_of(2) == fol::kAdult && s.deviations() == 0, "an instance starts at the state its def paints it in; nothing deviates");
    CHECK(fol::FoliageStore::chain(oak()) == std::vector<int>({0, 1, 2}) && fol::FoliageStore::chain(plant()) == std::vector<int>({0, 1}) && fol::FoliageStore::chain(stone()) == std::vector<int>({2}),
          "the chains: an Oak passes Infancy, Juvenile and Adult, a plant Infancy then Juvenile, a stone goes straight back");
    const auto c0 = s.harvest(0, 100.0);
    CHECK(c0 && c0->row == 0 && c0->state == fol::kEmpty && !c0->painted && s.state_of(0) == fol::kEmpty && s.deviations() == 1 && s.deviates(0), "a harvest makes the instance Empty");
    CHECK(!s.harvest(0, 101.0) && !s.harvest(99, 101.0), "an Empty instance and an unknown one cannot be harvested");
    CHECK(s.advance(399.9).empty() && s.state_of(0) == fol::kEmpty, "Empty for 300 s");
    auto up = s.advance(400.0);
    CHECK(up.size() == 1 && up[0].state == fol::kInfancy && !up[0].painted, "at 300 s: Infancy");
    CHECK(s.advance(2199.9).empty(), "Infancy for 1800 s");
    up = s.advance(2200.0);
    CHECK(up.size() == 1 && up[0].state == fol::kJuvenile && !up[0].painted && s.state_of(0) == fol::kJuvenile, "at 2,100 s: Juvenile");
    CHECK(s.advance(3999.9).empty(), "Juvenile for 1800 s");
    up = s.advance(4000.0);
    CHECK(up.size() == 1 && up[0].state == fol::kAdult && up[0].painted && s.state_of(0) == fol::kAdult && !s.deviates(0) && s.deviations() == 0, "at 3,900 s: Adult, its painted state, whole again and out of the list");
    CHECK(s.regrow_seconds(0) == 3900.0 && s.regrow_seconds(1) == 2100.0 && s.regrow_seconds(2) == 840.0, "an Oak takes 3,900 s from harvest to whole, a plant 2,100 s, a stone 840 s");
    // a plant, and a stone
    s.harvest(1, 0.0);
    CHECK(s.advance(1799.0).empty(), "a plant: Empty for 1,800 s");
    up = s.advance(1800.0);
    CHECK(up.size() == 1 && up[0].state == fol::kInfancy, "... then Infancy");
    up = s.advance(2100.0);
    CHECK(up.size() == 1 && up[0].state == fol::kJuvenile && up[0].painted, "... for 300 s, then Juvenile, its painted state");
    s.harvest(2, 0.0);
    up = s.advance(840.0);
    CHECK(up.size() == 1 && up[0].state == fol::kAdult && up[0].painted, "a stone: Empty for 840 s, then whole at once");
    // a long pause brings an instance through all its phases, in order
    s.harvest(3, 10000.0);
    up = s.advance(1e9);
    CHECK(up.size() == 3 && up[0].state == fol::kInfancy && up[1].state == fol::kJuvenile && up[2].state == fol::kAdult && up[2].painted && s.deviations() == 0, "a pause past the whole regrowth lists every phase in order");
    // harvested again at Juvenile: Empty again, and the old timer is void
    s.harvest(0, 0.0);
    s.advance(2100.0);   // Juvenile at 2,100
    CHECK(s.state_of(0) == fol::kJuvenile, "(an Oak at Juvenile)");
    const auto again = s.harvest(0, 2150.0);
    CHECK(again && again->state == fol::kEmpty && s.state_of(0) == fol::kEmpty, "a tree cut as a sapling is Empty again");
    CHECK(s.advance(2449.0).empty(), "the old timer (Adult at 3,900) is void: nothing happens before the new Empty has run");
    up = s.advance(2450.0);
    CHECK(up.size() == 1 && up[0].state == fol::kInfancy, "the new regrowth begins after its own 300 s");
    CHECK(s.next_due() && *s.next_due() > 2450.0, "the next transition is known");
    // seconds_until_whole
    fol::FoliageStore t({0}, {oak()}, {});
    t.harvest(0, 0.0);
    const double left = t.seconds_until_whole(0, 100.0);
    t.advance(5000.0);
    CHECK(left == 3800.0 && t.seconds_until_whole(0, 5000.0) == 0.0, "the seconds left of a growing instance (200 s of Empty and the two growing phases), 0 for a whole one");
}

static void test_store_mult() {
    std::printf("the store: the multiplier scales every phase\n");
    fol::FoliageStore s({0}, {oak()}, fol::FoliageStore::Config{0.01});
    s.harvest(0, 0.0);
    CHECK(s.advance(2.99).empty() && s.advance(3.0).size() == 1 && s.state_of(0) == fol::kInfancy, "x0.01: an Oak is Empty for 3 s");
    CHECK(s.advance(20.9).empty() && s.advance(21.0).size() == 1 && s.state_of(0) == fol::kJuvenile, "... a sapling for 18 s");
    const auto up = s.advance(39.0);
    CHECK(up.size() == 1 && up[0].painted, "... medium for 18 s, then whole (the launcher's 0.01)");
    CHECK(s.regrow_seconds(0) == 39.0, "(39 s in all)");
    fol::FoliageStore bad({0}, {oak()}, fol::FoliageStore::Config{-3.0});
    CHECK(bad.config().regrow_mult == 1.0, "a multiplier that is none is 1");
}

static void test_locks() {
    std::printf("the store: one player cuts an instance at a time\n");
    fol::FoliageStore s({0, 0}, {oak()}, {});
    CHECK(s.lock(0, 7) && s.locked_by(0) == 7 && s.locks() == 1, "the first player gets the lock");
    CHECK(s.lock(0, 7), "and gets it again (a repeated start)");
    CHECK(!s.lock(0, 8) && s.locked_by(0) == 7, "a second player is refused");
    s.unlock(0, 8);
    CHECK(s.locked_by(0) == 7, "another player cannot release it");
    CHECK(s.lock(1, 8) && s.locks() == 2, "another instance is free");
    s.unlock(0, 7);
    CHECK(s.locked_by(0) == 0 && s.lock(0, 8), "released, it is free for the next player");
    // a def whose middle state has no time does not stall
    fol::DefTimes d = oak();
    d.life[0] = 0;
    fol::FoliageStore z({0}, {d}, {});
    z.harvest(0, 0.0);
    const auto up = z.advance(10000.0);
    CHECK(up.size() == 3 && up.back().painted, "a state of no time is passed at once, not waited in for ever");
}
// ~ C O A :: Cyb ~

// ---- the world ----------------------------------------------------------------------------------------------------------------------------

struct World {
    std::optional<content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    gather::Gathering* g = nullptr;
    fol::Foliage* f = nullptr;

    bool open(const char* path, const char* tsv, bool all_cells = true) {
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
        fol::Config fc;
        fc.map_path = tsv;
        fc.all_cells = all_cells;
        if (!world->start_foliage(fc, error)) {
            std::printf("  [FAIL] foliage does not start: %s\n", error.c_str());
            return false;
        }
        f = world->foliage();
        return true;
    }
    uint32_t row_of(const std::string& owner, const std::string& comp, uint32_t index) const {
        const auto r = f->map().find(owner, comp, index);
        if (!r) {
            std::printf("  [FAIL] no instance %s %s %u\n", owner.c_str(), comp.c_str(), index);
            std::exit(2);
        }
        return *r;
    }
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

static constexpr const char* kCell41_19 = "InstancedFoliageActor_25600_-41_19_0";

static void test_data(World& w) {
    std::printf("the data: the map's defs against content.db's foliage tables\n");
    CHECK(w.f->defs() == 73 && w.f->unresolved_defs() == 0, "all 73 defs of the map resolve to a foliage_def");
    CHECK(w.f->harvestable_defs() == 69, "69 can be harvested (the four Riverlands_ pillars have no interaction)");
    CHECK(w.f->duplicated_names() == 0, "no def of the map has two records in the design data (the one repeated name, Stone_Limestone_Large_2, is no def of the Riverlands)");
    CHECK(w.g->interactions() == 236, "Gathering holds all 236 interactions");
    const uint32_t oak_row = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 0);
    const fol::DefInfo& oak_def = w.f->def_of(oak_row);
    CHECK(oak_def.name == "Tree_Oak_Large_1" && oak_def.interactions[2] && oak_def.interactions[1] && !oak_def.interactions[0], "1428's Oak: Adult and Juvenile have a harvest, a sapling none");
    CHECK(oak_def.interactions[2] && oak_def.interactions[2]->name == "Tree_Oak_Large" && oak_def.interactions[2]->base_duration == 15.0 && oak_def.interactions[2]->tool_slot == 29 &&
              oak_def.interactions[1]->name == "Tree_Oak_Medium" && oak_def.interactions[1]->base_duration == 7.5,
          "Adult: Tree_Oak_Large, 15 s, the Axe (slot 29); Juvenile: Tree_Oak_Medium, 7.5 s");
    CHECK(w.f->store().state_of(oak_row) == fol::kAdult && w.f->store().regrow_seconds(oak_row) == 3900.0, "painted Adult; 300 + 1800 + 1800 s back");
    const uint32_t daffodil_row = w.row_of("InstancedFoliageActor_25600_-39_19_0", "IntrepidGatherableFISMC_16", 9);
    const fol::DefInfo& daffodil_def = w.f->def_of(daffodil_row);
    CHECK(daffodil_def.name == "Plant_Daffodil_2" && w.f->store().state_of(daffodil_row) == fol::kJuvenile && daffodil_def.interactions[1] && daffodil_def.interactions[1]->base_duration == 11.25 &&
              daffodil_def.interactions[1]->tool_slot == 33 && w.f->store().regrow_seconds(daffodil_row) == 2100.0,
          "1451's Daffodil: painted Juvenile, 11.25 s with the Sickle (slot 33), 1800 + 300 s back");
    // the reach covers the three captured starts with a margin of 50 cm or more [R: the pawn's last move before each start, tools/capture/fol_mv.py]
    struct Start {
        const char* owner;
        const char* comp;
        uint32_t index;
        int state;
        double px, py;
    };
    const Start starts[] = {{kCell41_19, "IntrepidGatherableFISMC_0", 0, 2, -1032839.01, 495968.38},
                            {"InstancedFoliageActor_25600_-39_19_0", "IntrepidGatherableFISMC_16", 9, 1, -992420.92, 503043.62},
                            {"InstancedFoliageActor_25600_-41_18_0", "IntrepidGatherableFISMC_12", 1, 2, -1040165.38, 472990.0}};
    bool covered = true;
    double least = 1e9;
    for (const Start& s : starts) {
        const uint32_t row = w.row_of(s.owner, s.comp, s.index);
        const auto& r = w.f->map().rows()[row];
        const double distance = std::hypot(r.x - s.px, r.y - s.py);
        const double reach = w.f->reach_cm(row, s.state);
        least = std::min(least, reach - distance);
        covered = covered && reach - distance >= 50.0;
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "the reach covers 1428's, 1451's and 1519's starts (148, 75 and 166 cm away) by at least 50 cm (the least margin %.0f cm)", least);
    CHECK(covered, msg);
    CHECK(w.f->reach_cm(oak_row, 2) == 1055.0, "an Adult Oak reaches 1,055 cm: max(125, 250) + 500 + 205 + 100");
    CHECK(w.f->reach_cm(oak_row, 0) == 0.0 && w.f->reach_cm(oak_row, 3) == 0.0, "a sapling and an Empty instance have no reach");
}
// (C:Y:B : C.O.A)

static void test_rewards(World& w) {
    std::printf("the rewards: the captured items from the data's tables\n");
    // each harvest's item the captures show: the record, quantity and quality of 1428 (Oak), 1451 (Daffodil), 1463 (Ash, Juvenile), 1465, 1468 (Oak, Juvenile), 1519 (Oak)
    struct Seen {
        uint64_t record;
        int quantity, quality;
    };
    std::map<int, Seen> by_capture;
    for (const auto& it : capture_foliage::kItems) by_capture[it.capture] = {it.record, static_cast<int>(it.quantity), static_cast<int>(it.quality)};
    auto stats_owner = w.make_stats();
    std::string why;
    auto run = stats_owner->evaluation({}, why);
    if (!run) {
        std::printf("  [FAIL] the stat run does not start: %s\n", why.c_str());
        return;
    }
    // ratings so that the quantity bonus and the better qualities happen: the captured players had them
    std::map<int64_t, double> ratings;
    const aoc::worldd::stats::StatBundle& bundle = *w.world->stat_bundle();
    for (size_t i = 0; i < bundle.stats.size(); ++i) {
        const std::string& n = bundle.stat_name(i);
        if (n.rfind("Artisan", 0) == 0 && n.size() > 7 && n.compare(n.size() - 7, 7, "_Rating") == 0) ratings[bundle.stats[i].guid] = 8409.0;
    }
    w.g->set_ratings(ratings);
    struct Case {
        const char* what;
        const char* def;
        int state;
        std::set<int> captures;
    };
    const Case cases[] = {{"an Adult Oak", "Tree_Oak_Large_1", 2, {1428, 1519}},
                          {"a Juvenile Oak", "Tree_Oak_Large_1", 1, {1465, 1468, 1515}},
                          {"a Daffodil", "Plant_Daffodil_2", 1, {1429, 1451}}};
    for (const Case& c : cases) {
        const fol::DefInfo* def = nullptr;
        for (uint32_t r = 0; r < w.f->map().rows().size() && !def; ++r) {
            if (w.f->name_of(r) == c.def) def = &w.f->def_of(r);
        }
        const content::GatherDefRow* it = def ? def->interactions[c.state] : nullptr;
        if (!it) {
            std::printf("  [FAIL] %s: no interaction\n", c.what);
            continue;
        }
        std::set<std::tuple<uint64_t, int, int>> outcomes;
        aoc::worldd::stats::SplitMix rng;
        rng.state = 77;
        const aoc::worldd::world::NodeSite site{};
        size_t others = 0;
        for (int i = 0; i < 6000; ++i) {
            const auto h = w.g->harvest(*it, *run, 25, site, nullptr, rng, why);
            if (!h || h->items.empty()) continue;
            outcomes.emplace(static_cast<uint64_t>(h->items[0].item), static_cast<int>(h->items[0].quantity), h->items[0].quality);
        }
        bool all = true, one_record = true;
        for (const int cap : c.captures) {
            const auto s = by_capture.find(cap);
            all = all && s != by_capture.end() && outcomes.count({s->second.record, s->second.quantity, s->second.quality}) == 1;
        }
        const uint64_t record = outcomes.empty() ? 0 : std::get<0>(*outcomes.begin());
        for (const auto& o : outcomes) one_record = one_record && std::get<0>(o) == record;
        (void)others;
        char msg[200];
        std::snprintf(msg, sizeof msg, "%s rolls the captured items (%zu distinct outcomes in 6,000 harvests, all of one record)", c.what, outcomes.size());
        CHECK(all && one_record, msg);
    }
    w.g->set_ratings({});
    // the Ash of 1463/1464 (Juvenile x2 q6, Adult x5 q1)
    const fol::DefInfo* ash = nullptr;
    for (uint32_t r = 0; r < w.f->map().rows().size() && !ash; ++r) {
        if (w.f->name_of(r).rfind("Tree_Ash_Large", 0) == 0) ash = &w.f->def_of(r);
    }
    CHECK(ash && ash->interactions[2] && ash->interactions[2]->name.find("Ash") != std::string::npos, "the Ash trees have a harvest of their own (Tree_Ash_*)");
    // the channel's length at rating 0 is the base (the captured players had ratings: base x a factor between a half and 1)
    w.g->set_ratings({});
    bool lengths = true;
    for (const auto& f : capture_foliage::kFoliageData) {
        if (f.duration <= 0.0f) continue;
        lengths = lengths && f.duration >= 5.0f && f.duration <= 15.0f;
    }
    CHECK(lengths, "(the captured channels are 5 to 15 s: base x the speed curve)");
}

// ---- the state actor of every cell: what the real client needs ----------------------------------------------------------------------------
//
//  The user's test 1 (2026-10-12): no tree, plant or stone of the world showed an interact option.  The client drops a foliage hit unless FoliageStateActorLookup[the IFA's FName] holds a
//  state actor whose FoliageStateComponentLookup[the FISMC's FName] is a component with bInitialized (UIntrepidFoliageSystem at 0x1433dbb40 and 0x1433dd7b0; the scan 0x146b456f0).  Retail
//  sends every cell's state actor at login (capture 1198, packets 244 to 313).  These tests read the actors back the way the client builds its lookups: from the bits.

/// UE's FName(const TCHAR*) for the names the foliage uses ("InstancedFoliageActor_25600_-41_19_0"): a final "_<digits>" of 1 to 9 digits with no leading zero (a lone 0 is fine) is split off
/// and the number is the digits + 1.  Written here again, apart from the code under test.
static std::pair<std::string, int32_t> client_fname(const std::string& s) {
    const size_t us = s.rfind('_');
    if (us == std::string::npos || us == 0 || us + 1 >= s.size()) return {s, 0};
    const std::string tail = s.substr(us + 1);
    if (tail.size() > 9 || (tail.size() > 1 && tail[0] == '0') || tail.find_first_not_of("0123456789") != std::string::npos) return {s, 0};
    return {s.substr(0, us), std::stoi(tail) + 1};
}

using ClientName = std::pair<std::string, int32_t>;

/// What a client holds of one state actor after its open: the actor's FoliageName and, by FoliageComponentUUID, each component's bInitialized.
struct WireCell {
    ClientName                   name;
    std::map<ClientName, bool>   components;   // FoliageStateComponentLookup: FISMC FName -> bInitialized
    size_t                       blocks = 0;
};

static std::optional<ClientName> name_of(const repl::Value* v) {
    const repl::NameValue* n = v ? v->get_if<repl::NameValue>() : nullptr;
    if (!n || n->hardcoded) return std::nullopt;
    return ClientName{n->text, n->number};
}

/// The open of `actor` on a new channel, read back from its bits: the actor block and a block per component, as the client's package map and RepLayout readers take them.
static bool read_cell_from_wire(const repl::Actor& actor, WireCell& out) {
    const schema::ClassSchema* ifa = gen::find_class_schema("AIntrepidStateIFA");
    const schema::ClassSchema* comp = gen::find_class_schema("UIntrepidFoliageStateComponent");
    if (!ifa || !comp) return false;
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> blocks;
    if (!repl::collect_open_blocks(actor, shadow, blocks) || blocks.empty() || blocks[0].sub) return false;
    out.blocks = blocks.size();
    size_t pos = 0;
    const auto a = repl::read_rep_block(*ifa, blocks[0].payload.data(), blocks[0].payload.bit_pos(), pos);
    if (!a) return false;
    const auto name = name_of(a->get(*ifa, "FoliageName"));
    if (!name) return false;
    out.name = *name;
    for (size_t i = 1; i < blocks.size(); ++i) {
        size_t p = 0;
        const auto c = repl::read_rep_block(*comp, blocks[i].payload.data(), blocks[i].payload.bit_pos(), p);
        if (!c) return false;
        const auto uuid = name_of(c->get(*comp, "FoliageComponentUUID"));
        const repl::Value* init = c->get(*comp, "bInitialized");
        if (!uuid) return false;
        out.components[*uuid] = init && init->get_if<bool>() && *init->get_if<bool>();
    }
    return true;
}
// C.O.A.Cyb

static void test_every_instance_has_its_state(World& w) {
    std::printf("the cells: every instance of the map resolves the way the client looks it up\n");
    // the client's FoliageStateActorLookup, built from the bits of every state actor in the world
    std::map<ClientName, WireCell> client;
    size_t actors = 0, unreadable = 0, doubled = 0, components = 0, not_init = 0;
    w.world->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind != world::ActorKind::Foliage) return;
        ++actors;
        WireCell cell;
        if (!read_cell_from_wire(a.actor, cell)) {
            ++unreadable;
            return;
        }
        components += cell.components.size();
        for (const auto& [n, init] : cell.components) not_init += init ? 0 : 1;
        if (!client.emplace(cell.name, cell).second) ++doubled;
    });
    char msg[240];
    std::snprintf(msg, sizeof msg, "%zu state actors in the world, %zu read back from their bits, %zu FoliageName twice, %zu components, %zu not initialized", actors, actors - unreadable, doubled, components, not_init);
    CHECK(actors == 671 && unreadable == 0 && doubled == 0 && components == 10613 && not_init == 0 && client.size() == 671, msg);
    // every row of dist/content/region_foliage.tsv, read again here: trees, plants and stones
    const char* path = tsv_path();
    std::ifstream in(path, std::ios::binary);
    std::string line;
    size_t rows = 0, resolved = 0, no_actor = 0, no_component = 0, uninitialized = 0;
    size_t trees = 0, plants = 0, stones = 0, trees_ok = 0, plants_ok = 0, stones_ok = 0, foreign = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == '\r') line.pop_back();
        std::vector<std::string> col;
        size_t from = 0;
        for (int i = 0; i < 6; ++i) {
            const size_t t = line.find('\t', from);
            col.push_back(line.substr(from, t == std::string::npos ? std::string::npos : t - from));
            if (t == std::string::npos) break;
            from = t + 1;
        }
        if (col.size() < 6) continue;
        ++rows;
        const std::string& def = col[1];
        const bool stone = def.rfind("Stone_", 0) == 0 || def.rfind("Riverlands_Rock", 0) == 0;
        const bool tree = def.rfind("Tree_", 0) == 0;
        (stone ? stones : tree ? trees : plants) += 1;
        // the extraction (tools/mapc Collect.cs) admits only exports whose cooked class is IntrepidGatherableFISMC, the one class the client's scan accepts (0x146b456f0: IsA
        // UIntrepidGatherableFISMC; the exe has no other class of that name), so a row's component name says so: stones too (mapc foliage-detail on the stone's cell: its FISMC_6 is one)
        if (col[4].rfind("IntrepidGatherableFISMC_", 0) != 0) ++foreign;
        bool ok = false;
        const auto cell = client.find(client_fname(col[3]));
        if (cell == client.end()) {
            ++no_actor;
        } else {
            const auto comp = cell->second.components.find(client_fname(col[4]));
            if (comp == cell->second.components.end()) ++no_component;
            else if (!comp->second) ++uninitialized;
            else ok = true;
        }
        resolved += ok ? 1 : 0;
        (stone ? stones_ok : tree ? trees_ok : plants_ok) += ok ? 1 : 0;
    }
    std::snprintf(msg, sizeof msg, "%zu of the %zu rows resolve to an initialized component (%zu without a state actor, %zu without a component, %zu not initialized)", resolved, rows, no_actor, no_component,
                  uninitialized);
    CHECK(rows == 84218 && resolved == rows, msg);
    std::snprintf(msg, sizeof msg, "... %zu of %zu trees, %zu of %zu plants, %zu of %zu stones (and the four rock pillars)", trees_ok, trees, plants_ok, plants, stones_ok, stones);
    CHECK(trees == 42250 && trees_ok == trees && plants == 21046 && plants_ok == plants && stones == 20922 && stones_ok == stones, msg);
    CHECK(foreign == 0, "every row, the 20,922 stones among them, is in an IntrepidGatherableFISMC_<n> component: the class the client's target scan accepts");
}

/// An untouched cell's open against the retail opens of capture 1198's login and 1428 (the ten painted cells the generator kept whole): the actor block, the spawn location and
/// each component block, bit for bit.  A component the retail server had cut something in (an IndexedStates field after the RepLayout block) is compared up to that field.
static void test_cells_equal_retail(World& w) {
    std::printf("the cells: an untouched cell's open against the retail opens (1198's login, 1428)\n");
    fol::FoliageCells& cells = w.f->cells();
    size_t painted = 0, found = 0, actor_ok = 0, spawn_ok = 0, order_ok = 0, comps_total = 0, comps_ok = 0, whole_total = 0, whole_ok = 0;
    std::set<uint64_t> server_ids;   // the AuthServerIDReplicated values the ten retail blocks carry
    for (const auto& open : capture_foliage::kIfaOpens) {
        const auto owner = w.f->map().owner_of(open.name);
        if (!owner) continue;   // a town's runtime foliage: no row in the map
        ++painted;
        const world::WorldActor* a = w.world->registry().find(cells.actor_of(*owner));
        if (!a) continue;
        ++found;
        repl::ActorShadow shadow;
        std::vector<repl::ContentBlock> blocks;
        if (!repl::collect_open_blocks(a->actor, shadow, blocks) || blocks.size() != 1 + open.component_count) {
            std::printf("    %s: our open has %zu blocks, retail's %u components + the actor\n", open.name, blocks.size(), open.component_count);
            continue;
        }
        {   // the actor block, with the one property that is the retail server's own (below) at retail's value
            const tsupport::Bits theirs = tsupport::bits_of_hex(open.actor.hex, open.actor.bits);
            const schema::ClassSchema& ifa = *gen::find_class_schema("AIntrepidStateIFA");
            const auto retail_state = tsupport::with_exact(theirs, [&](const uint8_t* d, size_t, size_t n) {
                size_t pos = 0;
                return repl::read_rep_block(ifa, d, n, pos);
            });
            const repl::Value* server = retail_state ? retail_state->get(ifa, "AuthServerIDReplicated") : nullptr;
            const uint64_t* server_id = server ? server->get_if<uint64_t>() : nullptr;
            repl::Actor patched = a->actor;   // (the registry's actor is never touched)
            const repl::Value* now = patched.obj.state.get(*patched.obj.schema, "AuthServerIDReplicated");
            const bool ours_zero = now && now->get_if<uint64_t>() && *now->get_if<uint64_t>() == 0;
            if (server_id) patched.obj.state.set(*patched.obj.schema, "AuthServerIDReplicated", repl::Value::u64(*server_id));
            repl::ActorShadow patched_shadow;
            std::vector<repl::ContentBlock> patched_blocks;
            const bool wrote = repl::collect_open_blocks(patched, patched_shadow, patched_blocks) && !patched_blocks.empty();
            const tsupport::Bits ours = wrote ? tsupport::Bits(patched_blocks[0].payload) : tsupport::Bits();
            if (wrote && server_id && ours_zero && ours == theirs) {
                ++actor_ok;
                server_ids.insert(*server_id);
            } else {   // (the diagnostic: where the first different bit is)
                size_t at = 0;
                while (at < ours.size() && at < theirs.size() && ours.b[at] == theirs.b[at]) ++at;
                std::printf("    %s: ours %zu bits, retail %zu; the first different bit is %zu\n", open.name, ours.size(), theirs.size(), at);
            }
        }
        const auto& q = a->actor.open.spawn.location;
        if (q && q->mode == repl::QuantVec::Mode::Scaled && open.spawn_scaled && q->x == open.spawn[0] && q->y == open.spawn[1] && q->z == open.spawn[2]) ++spawn_ok;
        bool order = true;
        for (uint32_t i = 0; i < open.component_count; ++i) {
            const capture_foliage::Component& c = open.components[i];
            ++comps_total;
            const tsupport::Bits ours(blocks[1 + i].payload);
            const repl::Subobject& sub = a->actor.subobjects[i];
            const repl::Value* uuid = sub.obj.schema ? sub.obj.state.get(*sub.obj.schema, "FoliageComponentUUID") : nullptr;
            const repl::NameValue* uuid_name = uuid ? uuid->get_if<repl::NameValue>() : nullptr;
            order = order && sub.dynamic_class.has_value() && uuid_name && repl::fname_text(*uuid_name) == c.uuid;
            if (c.has_states) {   // the retail server had cut something here: the RepLayout block (the UUID and bInitialized) comes first, the IndexedStates field after it
                comps_ok += ours.size() <= static_cast<size_t>(c.block.bits) && ours == tsupport::bits_of_hex(c.block.hex, ours.size()) ? 1 : 0;
            } else {
                ++whole_total;
                const bool same = ours == tsupport::bits_of_hex(c.block.hex, c.block.bits);
                whole_ok += same ? 1 : 0;
                comps_ok += same ? 1 : 0;
            }
        }
        order_ok += order ? 1 : 0;
    }
    CHECK(painted == 10, "ten of the twelve retail opens are painted cells of the map (the other two are a town's runtime foliage)");
    CHECK(found == painted, "... and each has a state actor in the world from the start");
    // AuthServerIDReplicated is the one property of the block that is the retail server's own: 0 in most cells, 48 in the three of capture 1198's login at y = 17, 37 in 1428's cell at
    // (-40, 19) (the retail server mesh: a cell's id is its game server's).  The emulator is one server and sends 0, the class default, on every actor [R: constants.h]: not reproduced.
    std::string ids;
    for (const uint64_t id : server_ids) ids += (ids.empty() ? "" : ", ") + std::to_string(id);
    std::printf("    (AuthServerIDReplicated in the ten retail blocks: %s)\n", ids.c_str());
    char actor_msg[300];
    std::snprintf(actor_msg, sizeof actor_msg,
                  "the actor block (520 bits: AuthServerIDReplicated, ReplicatedMovement, RemoteRole, Role, FoliageName) is the retail block to the last bit in each, but for that one server id (%zu values in the ten)",
                  server_ids.size());
    CHECK(actor_ok == painted && server_ids == std::set<uint64_t>({0, 37, 48}), actor_msg);
    CHECK(spawn_ok == painted, "SerializeNewActor's location (x and y the cell's centre, z the middle of its instances' lowest and highest) is the retail location in each");
    CHECK(order_ok == painted, "the components are the same, in the same byte order (_0, _1, _10 ... _19, _2 ...), in each");
    char msg[200];
    std::snprintf(msg, sizeof msg, "all %zu component blocks are the retail blocks (%zu of them with no IndexedStates: equal to the last bit; the rest up to the IndexedStates field)", comps_total, whole_total);
    CHECK(comps_total == 166 && comps_ok == comps_total && whole_ok == whole_total && whole_total > 0, msg);
}
// == C:Y:B | CoA ==

// ---- the cells --------------------------------------------------------------------------------------------------------------------------

static std::string name_text(const repl::ObjectState& o, const char* path) {
    const repl::Value* v = o.state.get(*o.schema, path);
    const repl::NameValue* n = v ? v->get_if<repl::NameValue>() : nullptr;
    return n ? repl::fname_text(*n) : std::string();
}

static size_t states_listed(const repl::Actor& a, uint64_t state) {
    size_t n = 0;
    for (const repl::Subobject& sub : a.subobjects) {
        const repl::FastArrayState* fa = sub.obj.arrays.find(repl::fast_array_layout(*sub.obj.schema, "IndexedStates")->field);
        if (!fa) continue;
        for (const auto& item : fa->items()) {
            const auto s = item.values.values.find(1);
            n += s != item.values.values.end() && *s->second.get_if<uint64_t>() == state ? 1 : 0;
        }
    }
    return n;
}

static Clock::time_point after(Clock::time_point t, double seconds_later) {
    return t + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds_later));
}

static void test_cell_actor(World& w) {
    std::printf("the cells: the state actor of the cell capture 1428 opened\n");
    fol::FoliageCells& cells = w.f->cells();
    const auto& map = w.f->map();
    // a stone of the cell (-40, 19): the cell whose retail open is in capture_foliage.h (28 components, 520 bits, at (-1,011,200, 499,200))
    const uint32_t stone_row = w.row_of("InstancedFoliageActor_25600_-40_19_0", "IntrepidGatherableFISMC_18", 0);
    const uint16_t owner = map.rows()[stone_row].owner;
    CHECK(w.f->name_of(stone_row) == "Stone_Granite_Large_3", "(a Granite stone)");
    const uint64_t actor_id = cells.actor_of(owner);
    CHECK(actor_id != 0 && cells.actors() == 671 && cells.items_of(owner) == 0, "every cell has its state actor before anything in it is cut: 671, this one with no item");
    const size_t in_registry = w.world->registry().size();
    const Clock::time_point t0{seconds(1000)};
    const auto change = w.f->harvested(stone_row, t0);
    CHECK(change && change->state == fol::kEmpty, "the harvest is Empty");
    CHECK(cells.actor_of(owner) == actor_id && cells.actors() == 671 && w.world->registry().size() == in_registry, "the first cut of the cell changed its actor and made no other");
    const world::WorldActor* a = w.world->registry().find(actor_id);
    CHECK(a && a->kind == world::ActorKind::Foliage && a->snapshot_only && a->keep_fast_shadow && a->dormancy_delay == fol::kStateActorOpen && !a->never_dormant,
          "it is a snapshot-only foliage actor with a 265 ms open, whose fast arrays the view keeps");
    if (!a) return;
    CHECK(a->actor.subobjects.size() == map.owners()[owner].components.size() && a->actor.subobjects.size() == 28, "it has a component for each FISMC of the owner: 28, as the retail open of that cell");
    double cx = 0, cy = 0;
    map.cell_center(owner, cx, cy);
    CHECK(a->x == cx && a->y == cy && a->x == -1011200.0 && a->y == 499200.0, "it stands at the cell's centre (-1,011,200, 499,200), where the retail open puts it");
    CHECK(a->actor.open.archetype.size() == 2 && a->actor.open.archetype[0].guid.ObjectId == 18292086725290287385ull && a->actor.subobjects[0].dynamic_class &&
              a->actor.subobjects[0].dynamic_class->front().guid.ObjectId == 8959184318458680037ull,
          "its archetype is Default__IntrepidStateIFA and its components are IntrepidFoliageStateComponent (the ids of capture 1198's export)");
    CHECK(name_text(a->actor.obj, "FoliageName") == "InstancedFoliageActor_25600_-40_19_0", "FoliageName is the owner");
    CHECK(name_text(a->actor.subobjects[0].obj, "FoliageComponentUUID") == "IntrepidGatherableFISMC_0" && name_text(a->actor.subobjects[1].obj, "FoliageComponentUUID") == "IntrepidGatherableFISMC_1" &&
              name_text(a->actor.subobjects[2].obj, "FoliageComponentUUID") == "IntrepidGatherableFISMC_10",
          "the components are in byte order: _0, _1, _10 ... (the retail open's)");
    CHECK(cells.shown_state(stone_row) == 3u && cells.items_of(owner) == 1, "one item: the instance, Empty (3)");
    // a stone has no growing phase: Empty for its EmptyDuration, then whole at once and out of the list
    const double back = w.f->store().regrow_seconds(stone_row);
    w.f->tick(after(t0, back - 1.0));
    CHECK(cells.shown_state(stone_row) == 3u, "(a stone is still Empty a second before its time)");
    const uint32_t version = w.world->registry().find(actor_id)->version;
    w.f->tick(after(t0, back + 1.0));
    CHECK(!cells.shown_state(stone_row) && cells.items_of(owner) == 0 && w.world->registry().find(actor_id)->version > version, "then it is whole: the item leaves the list and the actor's version moved");
}

static void test_cell_regrowth(World& w) {
    std::printf("the cells: an Oak's items through its regrowth\n");
    fol::FoliageCells& cells = w.f->cells();
    const uint32_t oak_row = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 0);   // 1428's Oak
    const uint16_t owner = w.f->map().rows()[oak_row].owner;
    const Clock::time_point t0{seconds(20000)};
    w.f->harvested(oak_row, t0);
    const uint64_t actor_id = cells.actor_of(owner);
    CHECK(actor_id != 0 && cells.shown_state(oak_row) == 3u, "a harvest: the instance is Empty (3) in its cell's list");
    const world::WorldActor* a = w.world->registry().find(actor_id);
    CHECK(a && a->actor.subobjects.size() == 8, "the cell (-41, 19) has 8 components");
    const uint32_t v0 = a ? a->version : 0;
    w.f->tick(after(t0, 10));
    CHECK(cells.shown_state(oak_row) == 3u, "(nothing is due after 10 s)");
    w.f->tick(after(t0, 301));
    CHECK(cells.shown_state(oak_row) == 0u && w.world->registry().find(actor_id)->version > v0, "after 300 s: Infancy (0), and the actor's version moved: the clients are sent it");
    w.f->tick(after(t0, 301 + 1800));
    CHECK(cells.shown_state(oak_row) == 1u, "after 1,800 s more: Juvenile (1)");
    w.f->tick(after(t0, 301 + 3600 + 5));
    CHECK(!cells.shown_state(oak_row) && cells.items_of(owner) == 0 && w.f->store().deviations() == 0, "after 1,800 s more: whole, the item is deleted from the list");
    // a connection's first look at a cell lists the Empty instances as 11 [R: 1198], the actor itself keeps 3
    const uint32_t other = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 1);
    const Clock::time_point t1{seconds(30000)};
    w.f->harvested(oak_row, t1);
    w.f->harvested(other, t1);
    const world::WorldActor* b = w.world->registry().find(actor_id);
    repl::Actor fresh = b->actor;
    b->first_open_form(fresh);
    CHECK(states_listed(fresh, 3) == 2 && states_listed(fresh, 11) == 0, "two instances cut a moment ago are a live harvest to a first look: still 3 (the client plays the fall)");
    w.f->tick(after(t1, 60));
    const uint32_t third = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 2);
    w.f->harvested(third, after(t1, 60));
    repl::Actor copy = b->actor;
    b->first_open_form(copy);
    CHECK(states_listed(copy, 11) == 2 && states_listed(copy, 3) == 1 && states_listed(b->actor, 3) == 3 && states_listed(b->actor, 11) == 0,
          "a minute later a first look lists the two older Empty instances as 11 and the new one as 3; the actor itself holds 3 for all");
    const repl::FastArrayState* live = nullptr;
    const repl::FastArrayState* shown = nullptr;
    for (size_t i = 0; i < b->actor.subobjects.size() && !live; ++i) {
        const auto layout = repl::fast_array_layout(*b->actor.subobjects[i].obj.schema, "IndexedStates");
        const repl::FastArrayState* l = b->actor.subobjects[i].obj.arrays.find(layout->field);
        if (l && !l->items().empty()) {
            live = l;
            shown = copy.subobjects[i].obj.arrays.find(layout->field);
        }
    }
    CHECK(live && shown && live->array_key() == shown->array_key() && live->items()[0].key == shown->items()[0].key && live->items()[0].id == shown->items()[0].id,
          "and the copy keeps the array's key and the items' keys and IDs (only the value changed: what a connection has is the server's count)");
}
// [C O A x Cyb]

static void test_duplicate_name(World& w) {
    std::printf("the data: a def name with two records\n");
    // a map with the schema's one repeated name: Stone_Limestone_Large_2 has two GatheringFoliageData records alike in every number; the first by guid is used and the log says so
    const fs::path path = fs::temp_directory_path() / "aoc_foliage_dup.tsv";
    {
        FILE* f = std::fopen(path.string().c_str(), "wb");
        const std::string text = "# test\n" + tsv_line("Stone_Limestone_Large_2", "InstancedFoliageActor_25600_1_1_0", "IntrepidGatherableFISMC_0", 0) +
                                 tsv_line("Tree_Oak_Large_1", "InstancedFoliageActor_25600_1_1_0", "IntrepidGatherableFISMC_1", 0);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    fol::Config cfg;
    cfg.map_path = path.string();
    std::string error;
    world::ActorRegistry registry;
    auto f = fol::Foliage::make(*w.db, *w.g, registry, [&] { return w.world->mint_guid(); }, cfg, error);
    CHECK(f != nullptr, "a map with the repeated name loads");
    if (f) {
        const auto range = w.g->foliage_data()->defs_by_name.equal_range("Stone_Limestone_Large_2");
        int64_t lowest = INT64_MAX;
        size_t records = 0;
        for (auto it = range.first; it != range.second; ++it, ++records) lowest = std::min(lowest, it->second);
        CHECK(records == 2 && f->duplicated_names() == 1 && f->def_of(0).row && f->def_of(0).row->guid == lowest && f->unresolved_defs() == 0,
              "the design data has two records of it; the first by guid is the def, counted once");
        CHECK(f->def_of(0).row->empty_duration == 5700.0 && f->def_of(0).row->paint_state == fol::kAdult, "(5,700 s empty, painted Adult)");
    }
    std::error_code ec;
    fs::remove(path, ec);
    fol::Config missing;
    missing.map_path = "no/such/map.tsv";
    CHECK(!fol::Foliage::make(*w.db, *w.g, registry, [&] { return w.world->mint_guid(); }, missing, error) && error.find("cannot open") != std::string::npos && error.find("mapc extract") != std::string::npos,
          "a missing map is an error that names the file and `mapc extract`");
}

// ---- the load: what the state actors of all 671 cells cost ---------------------------------------------------------------------------------

static size_t working_set_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return static_cast<size_t>(pmc.WorkingSetSize);
#endif
    return 0;
}

static void test_load(const char* db, const char* tsv) {
    std::printf("the load: the 671 state actors made at start\n");
    World w;
    if (!w.open(db, tsv, /*all_cells=*/false)) return;
    const size_t registry_before = w.world->registry().size();
    CHECK(w.f->cells().actors() == 0, "(the first draft's way, an actor at a cell's first cut: none yet, for the measurement)");
    const size_t ws0 = working_set_bytes();
    const auto t0 = std::chrono::steady_clock::now();
    const size_t made = w.f->cells().make_all();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const size_t ws1 = working_set_bytes();
    size_t subobjects = 0, total_bits = 0, max_bits = 0, opens = 0;
    w.world->registry().for_each([&](const world::WorldActor& a) {
        if (a.kind != world::ActorKind::Foliage) return;
        subobjects += a.actor.subobjects.size();
        repl::ActorShadow shadow;
        std::vector<repl::ContentBlock> blocks;
        if (!repl::collect_open_blocks(a.actor, shadow, blocks)) return;
        size_t bits = 0;
        for (const repl::ContentBlock& b : blocks) bits += b.payload.bit_pos();
        total_bits += bits;
        max_bits = std::max(max_bits, bits);
        ++opens;
    });
    const double grown_mb = ws1 > ws0 ? static_cast<double>(ws1 - ws0) / (1024.0 * 1024.0) : 0.0;
    std::printf("    %zu state actors with %zu components made in %.0f ms; the working set grew by %.1f MB (%.1f KB an actor, %.0f bytes a component)\n", made, subobjects, ms, grown_mb,
                made ? grown_mb * 1024.0 / static_cast<double>(made) : 0.0, subobjects ? grown_mb * 1024.0 * 1024.0 / static_cast<double>(subobjects) : 0.0);
    std::printf("    a cell's open is %zu bits of blocks on average (%zu at most, a cell of 38 components); all %zu cells together %.1f KB\n", opens ? total_bits / opens : 0, max_bits, opens,
                static_cast<double>(total_bits) / 8.0 / 1024.0);
    CHECK(made == 671 && subobjects == 10613 && w.world->registry().size() == registry_before + 671 && opens == 671, "671 actors with 10,613 components were made and each builds its open");
    CHECK(ms < 2000.0, "they are made in under two seconds, once, at start");
    CHECK(ws0 == 0 || grown_mb < 100.0, "and they hold under 100 MB of the process (the measurement: see above)");
}
// -- CoA & C.Y.B --

int main() {
    spdlog::set_level(spdlog::level::warn);
    test_map_synthetic();
    test_map_real();
    test_store();
    test_store_mult();
    test_locks();
    const char* db = db_path();
    const char* tsv = tsv_path();
    if (!db || !tsv) {
        std::printf("\n[skip] the world checks need dist/content/content.db (version 15) and dist/content/region_foliage.tsv\n");
    } else {
        World w;
        if (w.open(db, tsv)) {
            test_data(w);
            test_rewards(w);
            test_every_instance_has_its_state(w);
            test_cells_equal_retail(w);
            test_cell_actor(w);
            test_cell_regrowth(w);
            test_duplicate_name(w);
        }
        test_load(db, tsv);
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_mob_hit_windows_tests.inc
//
//  Registered tests of test_worldd_combat: the creatures' moves whose montage lands the hit from a hit-window notify state (the goblins' Headbutts, the Minotaur Berserker's Rage
//  Strike, the Ram's Charge, the Plant Monsters' Pollen Swipe) and the moves whose harm is on a montage an obstruction branch enters (the Mycelions' Launch: see the review file).
//  The window's rule is fought through on the real rows: the hit lands on the tick of the open window on the players its targeting record reaches, once for each and not before the
//  window; a second player who comes in during the window is hit then, one who leaves before it opens is not; two goblins side by side and a reset in the middle of a window are covered;
//  the Ram's recorded hit range and the point-notify moves (the Deer's Headbutt) are the two controls.
//
//  Most tests write the build's own tables with a `hit_windows` column on the montages they use and a `branches` column on the creature table, and lift the reader's hold on the
//  ability they play, then load them: they test the mechanism on windows of their own.  The goblin Headbutts read the extractor's windows from the build's own tables.  A table
//  without the columns keeps loading as it did (the build's own real tables are loaded by every other test of this program).
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

namespace mob_windows {

// The program's CHECK prints only a failure; these checks print the pass too, so the green run's log holds the line of the same check that failed red.
#define MOVE_CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); std::fflush(stdout); ++g_fail; } \
} while (0)
#define MOVE_CHECKF(cond, ...) do { \
    char move_line[1024]; \
    std::snprintf(move_line, sizeof move_line, __VA_ARGS__); \
    if (cond) { std::printf("  [ok ] %s\n", move_line); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", move_line); std::fflush(stdout); ++g_fail; } \
} while (0)

// The abilities and their hit records, read from the data (the names are the data's; the guids are the rows' own).
constexpr int64_t kGoblinStationaryHeadbutt = 6064629665039909073ll;
constexpr int64_t kGoblinHeadbutt = 6064629664340312070ll;
constexpr int64_t kGoblinHeadbuttHit = 6064629664433438729ll;
constexpr int64_t kRageStrike = 6064632699434172492ll;
constexpr int64_t kRageStrikeHit = 6064630180887200952ll;
constexpr int64_t kMycelletteLaunch = 6064631913484518364ll;
constexpr int64_t kMycelletteImpactFirst = 6064631913488843741ll;
constexpr int64_t kMycelletteImpactSecond = 6064631913645671663ll;
constexpr int64_t kMycelletteLaunchOnly = 6064629733676154880ll;
constexpr int64_t kRamCharge = 6064633491734462544ll;
constexpr int64_t kRamChargeHit = 6064633944846237696ll;
constexpr int64_t kPollenSwipe = 6064631997117760130ll;
constexpr int64_t kPollenSwipeHit = 6064631997141549701ll;
constexpr int64_t kDeerHeadbutt = 6064630854764200333ll;
constexpr int64_t kDeerHeadbuttHit = 6064633939205162532ll;

const char* const kGoblinMontage = "/Game/DesignAssets/Population/NPC/Humanoid/Goblin/AbilityMontages/Montage_GoblinBasic_HeadButt.Montage_GoblinBasic_HeadButt";
const char* const kGoblinStationaryMontage = "/Game/DesignAssets/Population/NPC/Humanoid/Goblin/AbilityMontages/Montage_GoblinBasic_StationaryHeadButt.Montage_GoblinBasic_StationaryHeadButt";
const char* const kRageMontage1 = "/Game/DesignAssets/Population/NPC/Humanoid/Minotaur/Berserker/AbilityMontages/Minotaur_Berserker_AxeSwing_01.Minotaur_Berserker_AxeSwing_01";
const char* const kRageMontage2 = "/Game/DesignAssets/Population/NPC/Humanoid/Minotaur/Berserker/AbilityMontages/Minotaur_Berserker_AxeSwing_02.Minotaur_Berserker_AxeSwing_02";
const char* const kRamMontage = "/Game/DesignAssets/Population/NPC/Beast/Ram/AbilityMontages/Ram_Charge.Ram_Charge";
const char* const kPollenMontage = "/Game/DesignAssets/Population/NPC/Monster/Plantoid/PlantMonster/PlantMonster_Arcane/PlantMonster_PollenSwipe.PlantMonster_PollenSwipe";
const char* const kLaunchImpact = "/Game/DesignAssets/Population/NPC/Monster/Plantoid/Mycellion/AbilityMontages/Mycellette_Launch_Impact.Mycellette_Launch_Impact";

const char* const kGoblinPath = "/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C";
const char* const kRagePath = "/Humanoid/Minotaur/Berserker/Minotaur_Berserker1.Default__Minotaur_Berserker1_C";
const char* const kMycellettePath = "/Monster/Plantoid/Mycellion/Mycellette.Default__Mycellette_C";
const char* const kArcanePath = "/Monster/Plantoid/PlantMonster/PlantMonster_Arcane1.Default__PlantMonster_Arcane1_C";
const char* const kRamPath = "/Beast/Ram/Ram.Default__Ram_C";
const char* const kDeerPath = "/Beast/Deer/Deer.Default__Deer_C";

/// The cells of one table line (empty cells kept).
std::vector<std::string> cells_of(const std::string& line) {
    std::vector<std::string> out;
    size_t from = 0;
    while (true) {
        const size_t tab = line.find('\t', from);
        out.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
        if (tab == std::string::npos) return out;
        from = tab + 1;
    }
}

std::string joined(const std::vector<std::string>& cells) {
    std::string line;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i != 0) line += '\t';
        line += cells[i];
    }
    return line;
}

/// What a test changes in the creature table: one ability's rows (all of its Blueprints) are made playable (unknown 0, no `why`), their conditions, cooldown, gates and starts may be
/// replaced, and every one of them carries the ability's `branches` text.
struct RowEdit {
    int64_t     ability = 0;
    bool        set_conds = false;
    std::string conds;
    bool        set_cooldown = false;
    double      cooldown = 0;
    bool        set_gates = false;
    std::string gates;
    bool        set_starts = false;
    std::string starts;
    std::string branches;
    bool        set_min_distance = false;   // (the tree's own minimum distance, which the world's approach does not reach: see the Ram's Charge)
    double      min_distance = 0;
};

/// The build's creature table with a `branches` column (the last one), the rows the edits name changed as they say; false, with `error`, when the table does not read.
bool abilities_with(const std::string& dir, const std::vector<RowEdit>& edits, std::string& path, std::string& error) {
    namespace fs = std::filesystem;
    std::ifstream in(dir + "npc_abilities.tsv", std::ios::binary);
    if (!in) {
        error = "npc_abilities.tsv cannot be opened";
        return false;
    }
    const fs::path out = fs::temp_directory_path() / ("aoc_mob_windows_abilities_" + unique_temp_suffix() + ".tsv");
    std::ofstream o(out, std::ios::binary);
    std::string line;
    bool first = true, has_branches = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> c = cells_of(line);
        if (first) {
            first = false;
            has_branches = line.size() >= 9 && line.compare(line.size() - 9, 9, "\tbranches") == 0;
            o << (has_branches ? line : line + "\tbranches") << '\n';
            continue;
        }
        std::string branches = has_branches && !c.empty() ? c.back() : std::string();
        if (c.size() > 1) {
            const int64_t ability = std::strtoll(c[1].c_str(), nullptr, 10);
            for (const RowEdit& e : edits) {
                if (ability != e.ability) continue;
                c[10] = "0";   // unknown
                c[13] = "";    // why
                if (e.set_conds && c.size() > 12) c[12] = e.conds;
                if (e.set_cooldown && c.size() > 3) c[3] = std::to_string(e.cooldown);
                if (e.set_min_distance && c.size() > 6) c[6] = std::to_string(e.min_distance);
                if (e.set_gates && c.size() > 9) c[9] = e.gates;
                if (e.set_starts && c.size() > 14) c[14] = e.starts;
                branches = e.branches;
            }
        }
        if (has_branches) {
            c.back() = branches;
            o << joined(c) << '\n';
        } else {
            o << joined(c) << '\t' << branches << '\n';
        }
    }
    if (first) {
        error = "npc_abilities.tsv is empty";
        return false;
    }
    path = out.string();
    return true;
}

/// What a test changes in the montage table: the montages whose `hit_windows` column it sets (the column is added as the last one).
struct WindowEdit {
    std::string montage;
    std::string windows;   // "start:end:key" entries, ';'-joined
};

/// The build's montage table with a `hit_windows` column (the last one); false, with `error`, when the table does not read.
bool montages_with(const std::string& dir, const std::vector<WindowEdit>& edits, std::string& path, std::string& error) {
    namespace fs = std::filesystem;
    std::ifstream in(dir + "npc_montages.tsv", std::ios::binary);
    if (!in) {
        error = "npc_montages.tsv cannot be opened";
        return false;
    }
    const fs::path out = fs::temp_directory_path() / ("aoc_mob_windows_montages_" + unique_temp_suffix() + ".tsv");
    std::ofstream o(out, std::ios::binary);
    std::string line;
    bool first = true, has_windows = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            first = false;
            has_windows = line.size() >= 12 && line.compare(line.size() - 12, 12, "\thit_windows") == 0;
            o << (has_windows ? line : line + "\thit_windows") << '\n';
            continue;
        }
        std::string windows;
        if (!line.empty()) {
            const std::string montage = line.substr(0, line.find('\t'));
            for (const WindowEdit& e : edits) {
                if (montage == e.montage) windows = e.windows;
            }
            if (has_windows && windows.empty()) {
                const size_t last = line.rfind('\t');
                if (last != std::string::npos) windows = line.substr(last + 1);
            }
        }
        if (has_windows) {
            const size_t last = line.rfind('\t');
            o << (last == std::string::npos ? line : line.substr(0, last)) << '\t' << windows << '\n';
        } else {
            o << line << '\t' << windows << '\n';
        }
    }
    if (first) {
        error = "npc_montages.tsv is empty";
        return false;
    }
    path = out.string();
    return true;
}

/// The creature tables of the build with the changes of a test: loaded with the build's targeting table.  `what` names the load in the check.
bool load_with_moves(Fixture&, const std::vector<RowEdit>& rows, const std::vector<WindowEdit>& windows, const std::string& what, combat::NpcAbilityData& out) {
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string abilities, montages, error;
    const bool written = abilities_with(dir, rows, abilities, error) && montages_with(dir, windows, montages, error);
    if (!written) {
        std::printf("    (%s)\n", error.c_str());
        MOVE_CHECK(false, what.c_str());
        return false;
    }
    const bool loaded = out.load(abilities, montages, dir + "npc_targeting.tsv", error);
    if (!loaded) std::printf("    (%s)\n", error.c_str());
    MOVE_CHECK(loaded, what.c_str());
    return loaded;
}

/// A cast of a fight as the client saw it begin (its montage started), and which ability it was.
struct CastAt {
    double  t = 0;
    int64_t ability = 0;
};

std::vector<CastAt> casts_of(const std::vector<LungeSample>& samples) {
    std::vector<CastAt> out;
    for (const LungeSample& s : samples) {
        if (s.cast_began) out.push_back({s.t, s.ability});
    }
    return out;
}

/// The first hit of one record that a player took.
const combat::IncomingHit* first_hit(const std::vector<combat::IncomingHit>& hits, int64_t record) {
    for (const combat::IncomingHit& h : hits) {
        if (h.hit_guid == record) return &h;
    }
    return nullptr;
}

size_t hits_of(const std::vector<combat::IncomingHit>& hits, int64_t record) {
    size_t n = 0;
    for (const combat::IncomingHit& h : hits) n += h.hit_guid == record ? 1 : 0;
    return n;
}

/// The Health one hit takes (positive: how much).
double health_taken(const combat::IncomingHit& hit) {
    double v = 0;
    for (const combat::ResolvedMod& m : hit.mods) {
        if (m.stat_guid == content::kGuidHealth && m.value < 0 && std::isfinite(m.value)) v -= m.value;
    }
    return v;
}

/// The world's open for a creature of `path`: the Blueprint's own abilities from the tables the test wrote.
bool make_open(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, BanditOpen& made) {
    return make_bandit_open(f, tables, path, made);
}

}  // namespace mob_windows

// R5 (the control): a move whose montage has a point hit notify still lands at that notify, not at the window.  The Deer's Headbutt has one at 0.402 s and the ability is one the
// creature tables already read: the hit the player takes after the cast is the notify's, within a tick.
void test_a_point_notify_move_still_lands_at_its_notify(Fixture& f) {
    std::printf("the Deer's Headbutt lands at its montage's 0.402 s hit notify, as before the hit windows\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_windows::kDeerPath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Deer");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the Deer");
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    if (!run_lunge_fight(f, tables, path, 20.0, 0, 200, nullptr, samples, &hits, false, -1, nullptr, 40000.0, made.open)) return;
    const mob_windows::CastAt* first = nullptr;
    for (const mob_windows::CastAt& c : mob_windows::casts_of(samples)) {
        if (c.ability == mob_windows::kDeerHeadbutt) {
            first = &c;
            break;
        }
    }
    MOVE_CHECK(first != nullptr, "the Deer plays its Headbutt");
    if (!first) return;
    const combat::IncomingHit* hit = mob_windows::first_hit(hits, mob_windows::kDeerHeadbuttHit);
    MOVE_CHECK(hit != nullptr, "the player takes the Headbutt's hit");
    if (!hit) return;
    MOVE_CHECKF(hit->at - first->t >= 0.39 && hit->at - first->t <= 0.48, "the hit lands %.3f s after the cast: the montage's 0.402 s notify, within a tick", hit->at - first->t);
}

AOC_REGISTER_TEST(test_a_point_notify_move_still_lands_at_its_notify);

// R1 on the goblins: the Headbutt's montage has no point notify; its hit is the hit window's.  With the real tree (the fight clock the tree rolls between 20 and 30 s, then the Rally
// that readies the Headbutts) the goblin plays the tree's pair of Headbutt tasks, the stationary one and then the moving one, and the player takes exactly one hit of the shared record for
// each cast, inside the cast's own window.
void test_a_goblin_headbutt_lands_inside_its_window(Fixture& f) {
    std::printf("a Goblin's Headbutt plays its montage from the 20-30 s clock and lands once per cast inside its hit window\n");
    // the tables as the extractor wrote them (no window of a test's own): each of the pair's montages carries its own hit window, and the tree's rows for the pair play as they stand
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const combat::MontageData* moving_montage = tables.montage(mob_windows::kGoblinMontage);
    const combat::MontageData* stationary_montage = tables.montage(mob_windows::kGoblinStationaryMontage);
    const bool has_windows = moving_montage && stationary_montage && moving_montage->windows.size() == 1 && stationary_montage->windows.size() == 1;
    MOVE_CHECK(has_windows, "each of the pair's montages carries one hit window in the tables");
    if (!has_windows) return;
    const combat::MontageData::Window moving_window = moving_montage->windows.front();
    const combat::MontageData::Window stationary_window = stationary_montage->windows.front();
    std::printf("  the data's windows: the stationary Headbutt %.3f to %.3f s, the moving one %.3f to %.3f s\n", stationary_window.start, stationary_window.end, moving_window.start,
                moving_window.end);
    const std::string path = creature_path(f, mob_windows::kGoblinPath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Bonebinder goblin");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the goblin");
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    if (!run_lunge_fight(f, tables, path, 80.0, 0, 800, nullptr, samples, &hits, false, -1, nullptr, 40000.0, made.open)) return;
    std::vector<double> headbutts, stationary;
    for (const mob_windows::CastAt& c : mob_windows::casts_of(samples)) {
        if (c.ability == mob_windows::kGoblinHeadbutt) headbutts.push_back(c.t);
        if (c.ability == mob_windows::kGoblinStationaryHeadbutt) stationary.push_back(c.t);
    }
    // each task lands the shared record with its own ability as the hit's source, so each cast is measured by its own identity
    size_t head_hits = 0, stationary_hits = 0;
    for (const combat::IncomingHit& h : hits) {
        if (h.hit_guid != mob_windows::kGoblinHeadbuttHit) continue;
        head_hits += h.source_guid == mob_windows::kGoblinHeadbutt ? 1 : 0;
        stationary_hits += h.source_guid == mob_windows::kGoblinStationaryHeadbutt ? 1 : 0;
    }
    std::printf("  the moving Headbutt at:");
    for (const double t : headbutts) std::printf(" %.2f", t);
    std::printf(" s, the stationary one at:");
    for (const double t : stationary) std::printf(" %.2f", t);
    std::printf(" s; %zu and %zu hit(s)\n", head_hits, stationary_hits);
    MOVE_CHECKF(!headbutts.empty(), "the goblin plays its Headbutt from the tree (the montage starts) (%zu casts)", headbutts.size());
    MOVE_CHECKF(!stationary.empty(), "it plays the stationary Headbutt of the tree's pair too (%zu casts)", stationary.size());
    MOVE_CHECKF(!headbutts.empty() && headbutts.front() >= 20.0 && headbutts.front() < 40.0, "the first Headbutt waits for the 20-30 s the tree rolls for the fight (at %.2f s)",
                headbutts.empty() ? -1.0 : headbutts.front());
    MOVE_CHECKF(head_hits == headbutts.size(), "the player takes one Headbutt hit for each cast, and no more (%zu hits, %zu casts)", head_hits, headbutts.size());
    MOVE_CHECKF(stationary_hits == stationary.size(), "the stationary Headbutt lands one hit for each of its casts too (%zu hits, %zu casts)", stationary_hits, stationary.size());
    MOVE_CHECKF(mob_windows::hits_of(hits, mob_windows::kGoblinHeadbuttHit) == head_hits + stationary_hits, "no other source lands the pair's record (%zu hits of it in all, %zu counted)",
                mob_windows::hits_of(hits, mob_windows::kGoblinHeadbuttHit), head_hits + stationary_hits);
    // each hit lands inside the data's window of the cast that made it, and on the first tick the window is open (the goblin stands at the player: a tick is 1/15 s)
    const double tick = 1.0 / 15.0;
    bool inside = true, first_tick = true, damaged = true;
    for (const combat::IncomingHit& h : hits) {
        if (h.hit_guid != mob_windows::kGoblinHeadbuttHit) continue;
        const bool from_stationary = h.source_guid == mob_windows::kGoblinStationaryHeadbutt;
        const std::vector<double>& casts = from_stationary ? stationary : headbutts;
        const combat::MontageData::Window& window = from_stationary ? stationary_window : moving_window;
        bool in_window = false, on_first_tick = false;
        for (const double t : casts) {
            in_window = in_window || (h.at >= t + window.start - 1e-6 && h.at <= t + window.end + 1e-6);
            on_first_tick = on_first_tick || (h.at >= t + window.start - 1e-6 && h.at <= t + window.start + tick + 1e-6);
        }
        inside = inside && in_window;
        first_tick = first_tick && on_first_tick;
        damaged = damaged && mob_windows::health_taken(h) > 0;
    }
    MOVE_CHECK(inside, "... each inside the data's window of its cast");
    MOVE_CHECK(first_tick, "... and on the first tick that window is open");
    MOVE_CHECK(damaged, "... and each takes the Headbutt's own Health");
}

AOC_REGISTER_TEST(test_a_goblin_headbutt_lands_inside_its_window);

// R1 on who: the Pollen Swipe's 400 cm volume hits the player who stands in it once, the second player who walks in during the window is hit then, and the player who walks out
// before the window opens is not hit.  Three players, one move.
void test_a_hit_window_reaches_a_second_player_and_a_player_who_walks_out(Fixture& f) {
    std::printf("a Pollen Swipe's window hits the player inside once, the second player who walks in during it then, and not the one who walked out\n");
    combat::NpcAbilityData tables;
    if (!mob_windows::load_with_moves(f, {{mob_windows::kPollenSwipe, true, "c0:T", true, 60, true, "", true, ""}}, {{mob_windows::kPollenMontage, "0.5:1.0:0"}},
                                      "the creature tables with the Pollen Swipe's hit window load", tables))
        return;
    const std::string path = creature_path(f, mob_windows::kArcanePath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Arcane Bloomerias");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the Arcane Bloomerias");
    if (!made.open) return;
    Rig rig1(4, 20, {}), rig2(4, 20, {}), rig3(4, 20, {});
    combat_rig::World w;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const auto bp = f.motion.find(path);
    const uint64_t caster = w.spawn(path, 0, 0, 7, 900, bp->half_height, made.open, &*bp);
    MOVE_CHECK(caster != 0, "the Arcane Bloomerias is put in the world");
    if (caster == 0) return;
    combat_rig::FakePlayer inside(rig1, 77, {0, 200, 1098});
    combat_rig::FakePlayer walker_in(rig2, 78, {0, 900, 1098});
    combat_rig::FakePlayer walker_out(rig3, 79, {0, -200, 1098});
    world.add_player(&inside);
    world.add_player(&walker_in);
    world.add_player(&walker_out);
    aoc::protocol::repl::ActorShadow shadow;
    {
        aoc::protocol::repl::ExportAcks acks;
        bool failed = false;
        const auto open = aoc::protocol::repl::open_actor(w.registry.find(caster)->actor, shadow, acks, &failed);
        if (!open.has_value() || failed) return;
    }
    self_effects::CastLog log;
    double cast_at = -1;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 20.0 + 1e-9; now += dt) {
        // the walkers move inside the window's own time: the one out walks in at 0.6 s, the one in walks out at 0.2 s
        if (cast_at >= 0) {
            if (now >= cast_at + 0.6 && now < cast_at + 0.6 + dt) walker_in.rig_fighter().move_to({0, 250, 1098});
            if (now >= cast_at + 0.2 && now < cast_at + 0.2 + dt) walker_out.rig_fighter().move_to({0, -900, 1098});
        }
        for (combat_rig::FakePlayer* p : {&inside, &walker_in, &walker_out}) {
            if (const stats::StatValue* max = p->rig().cs->find(content::kGuidMaxHealth)) p->rig().cs->set_current(content::kGuidHealth, max->value);
            const combat::Vec3 at = p->rig_fighter().position();
            w.npcs->note_player(p->actor_id(), at.x, at.y, now);
        }
        const GremCastView before = grem_cast_view(w.registry, caster);
        w.npcs->tick(now);
        world.tick(now);
        const GremCastView after = grem_cast_view(w.registry, caster);
        if (cast_at < 0 && after.montage != 0 && after.ability == mob_windows::kPollenSwipe && (before.montage == 0 || before.element != after.element)) cast_at = now;
        log.look(w.registry, caster, now);
        if (cast_at >= 0 && now >= cast_at + 2.0) break;
    }
    std::printf("  the Pollen Swipe at %.2f s: %zu hit(s) inside, %zu walked in, %zu walked out\n", cast_at, mob_windows::hits_of(inside.hits, mob_windows::kPollenSwipeHit),
                mob_windows::hits_of(walker_in.hits, mob_windows::kPollenSwipeHit), mob_windows::hits_of(walker_out.hits, mob_windows::kPollenSwipeHit));
    MOVE_CHECK(cast_at > 0, "the Arcane Bloomerias plays its Pollen Swipe");
    if (cast_at <= 0) return;
    const combat::IncomingHit* hit1 = mob_windows::first_hit(inside.hits, mob_windows::kPollenSwipeHit);
    const combat::IncomingHit* hit2 = mob_windows::first_hit(walker_in.hits, mob_windows::kPollenSwipeHit);
    MOVE_CHECKF(mob_windows::hits_of(inside.hits, mob_windows::kPollenSwipeHit) == 1, "the player who stands in the 400 cm volume is hit once (%zu)", mob_windows::hits_of(inside.hits, mob_windows::kPollenSwipeHit));
    MOVE_CHECKF(hit1 && hit1->at >= cast_at + 0.49 && hit1->at <= cast_at + 1.01, "the hit lands inside the window (%.3f s after the cast)", hit1 ? hit1->at - cast_at : -1.0);
    MOVE_CHECKF(hit2 && mob_windows::hits_of(walker_in.hits, mob_windows::kPollenSwipeHit) == 1, "the second player who walks in during the window is hit then, once (%zu)",
                mob_windows::hits_of(walker_in.hits, mob_windows::kPollenSwipeHit));
    MOVE_CHECKF(hit2 && hit2->at >= cast_at + 0.59 && hit2->at <= cast_at + 1.01, "... at a tick after he came in (%.3f s after the cast)", hit2 ? hit2->at - cast_at : -1.0);
    MOVE_CHECKF(mob_windows::hits_of(walker_out.hits, mob_windows::kPollenSwipeHit) == 0, "the player who walked out before the window opened is not hit (%zu)",
                mob_windows::hits_of(walker_out.hits, mob_windows::kPollenSwipeHit));
}

AOC_REGISTER_TEST(test_a_hit_window_reaches_a_second_player_and_a_player_who_walks_out);

// R1 on the Minotaur Berserker: the Rage Strike's Axe Swings have no point notify; the window lands its hit once for each cast, at the window, with the ability's own damage.
void test_the_rage_strike_lands_at_its_window(Fixture& f) {
    std::printf("the Minotaur Berserker's Rage Strike plays its Axe Swing and lands its hit once inside the hit window\n");
    combat::NpcAbilityData tables;
    if (!mob_windows::load_with_moves(f, {{mob_windows::kRageStrike, true, "c0:T", true, 60, true, "", true, ""}},
                                      {{mob_windows::kRageMontage1, "0.7:1.2:0"}, {mob_windows::kRageMontage2, "0.7:1.2:0"}}, "the creature tables with the Rage Strike's hit window load", tables))
        return;
    const std::string path = creature_path(f, mob_windows::kRagePath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Minotaur Berserker");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the Minotaur Berserker");
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    if (!run_lunge_fight(f, tables, path, 20.0, 0, 300, nullptr, samples, &hits, false, -1, nullptr, 40000.0, made.open)) return;
    std::vector<double> casts;
    for (const mob_windows::CastAt& c : mob_windows::casts_of(samples)) {
        if (c.ability == mob_windows::kRageStrike) casts.push_back(c.t);
    }
    const size_t rage_hits = mob_windows::hits_of(hits, mob_windows::kRageStrikeHit);
    std::printf("  %zu Rage Strike(s), the first at %.2f s, %zu hit(s)\n", casts.size(), casts.empty() ? -1.0 : casts.front(), rage_hits);
    MOVE_CHECKF(!casts.empty(), "the Minotaur Berserker plays the Rage Strike's montage (the tree's effect gate made to hold) (%zu casts)", casts.size());
    if (casts.empty()) return;
    MOVE_CHECKF(rage_hits == casts.size(), "the player takes one Rage Strike hit for each cast, and no more (%zu hits, %zu casts)", rage_hits, casts.size());
    bool inside = true, damaged = true;
    for (const combat::IncomingHit& h : hits) {
        if (h.hit_guid != mob_windows::kRageStrikeHit) continue;
        bool in_window = false;
        for (const double t : casts) in_window = in_window || (h.at >= t + 0.69 && h.at <= t + 1.21);
        inside = inside && in_window;
        damaged = damaged && mob_windows::health_taken(h) > 0;
    }
    MOVE_CHECK(inside, "... each inside the window of its cast (0.7 to 1.2 s after the cast)");
    MOVE_CHECK(damaged, "... and each takes the Rage Strike's own Health");
}

AOC_REGISTER_TEST(test_the_rage_strike_lands_at_its_window);

// R1 (the owner's case, the one recorded no-notify move): the Ram's Charge lands inside the 0.198-2.039 s the retail recordings show (median 1.034 s, 8 hits), one hit per charge,
// and the hit comes later when the Ram starts farther away: the box at 130 cm has to meet the player.
void test_a_ram_charge_lands_inside_the_recorded_range(Fixture& f) {
    std::printf("a Ram's Charge lands once inside the recorded 0.198-2.039 s, earlier from 7 m than from 8.5 m\n");
    combat::NpcAbilityData tables;
    // the Charge's own minimum of 6 m is cleared here: the world's approach walks a creature to the reach of a shorter move it can play, so the tree is never asked for the Charge at 8 m
    // (a gap of the approach, not of the window; the Riverlands' own window moves have no minimum distance).  The window, the montage and the hit are the real ones.
    if (!mob_windows::load_with_moves(f, {{mob_windows::kRamCharge, false, "", false, 0, false, "", false, "", "", true, -1}}, {{mob_windows::kRamMontage, "1.3667:3.0:0"}},
                                      "the creature tables with the Charge's hit window load", tables))
        return;
    const std::string path = creature_path(f, mob_windows::kRamPath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Ram");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the Ram");
    if (!made.open) return;
    double after[2] = {-1, -1};
    const double range[2] = {700, 850};
    for (int i = 0; i < 2; ++i) {
        bool steered = false;
        // the player stands straight ahead of the Ram's own facing (its Charge wants a target within 30 degrees): the tree picks the Charge at once, not the shorter moves
        const LungeSteer steer = [&](double, const LungeSample*, double& px, double& py, const world::WorldActor& creature) {
            if (steered) return false;
            constexpr double kPi = 3.14159265358979323846;
            const double rad = creature.yaw_degrees * kPi / 180.0;
            px = creature.x + std::cos(rad) * range[i];
            py = creature.y + std::sin(rad) * range[i];
            steered = true;
            return true;
        };
        std::vector<LungeSample> samples;
        std::vector<combat::IncomingHit> hits;
        if (!run_lunge_fight(f, tables, path, 8.0, 0, range[i], steer, samples, &hits, false, -1, nullptr, 40000.0, made.open)) return;
        double cast_at = -1;
        for (const mob_windows::CastAt& c : mob_windows::casts_of(samples)) {
            if (c.ability == mob_windows::kRamCharge) {
                cast_at = c.t;
                break;
            }
        }
        const combat::IncomingHit* hit = mob_windows::first_hit(hits, mob_windows::kRamChargeHit);
        MOVE_CHECKF(cast_at > 0, "the Ram charges (the montage starts) from %.1f m", range[i] / 100.0);
        if (cast_at <= 0) return;
        MOVE_CHECKF(mob_windows::hits_of(hits, mob_windows::kRamChargeHit) == 1, "the player takes the Charge's hit once from %.1f m (%zu)", range[i] / 100.0,
                    mob_windows::hits_of(hits, mob_windows::kRamChargeHit));
        MOVE_CHECKF(hit && hit->at - cast_at >= 0.198 && hit->at - cast_at <= 2.039, "... %.3f s after the cast: inside the recorded 0.198-2.039 s", hit ? hit->at - cast_at : -1.0);
        after[i] = hit ? hit->at - cast_at : -1;
    }
    MOVE_CHECKF(after[0] > 0 && after[1] > after[0], "the farther charge lands later: %.3f s from 7 m, %.3f s from 8.5 m", after[0], after[1]);
}

AOC_REGISTER_TEST(test_a_ram_charge_lands_inside_the_recorded_range);

// R1 and R3 with company: two goblins side by side each roll the fight clock and each plays its Headbutt; the player is hit once by each, its own record and its own window.
void test_two_goblins_side_by_side_each_headbutt(Fixture& f) {
    std::printf("two goblins side by side each headbutt, and the player takes one hit of each goblin's Headbutt\n");
    combat::NpcAbilityData tables;   // (as the extractor wrote them: the pair's own windows)
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, mob_windows::kGoblinPath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Bonebinder goblin");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the goblin");
    if (!made.open) return;
    Rig rig(4, 20, {});
    combat_rig::World w;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const auto bp = f.motion.find(path);
    const uint64_t left = w.spawn(path, -500, 0, 7, 1200, bp->half_height, made.open, &*bp);
    const uint64_t right = w.spawn(path, 500, 0, 7, 1200, bp->half_height, made.open, &*bp);
    MOVE_CHECK(left != 0 && right != 0, "two goblins are put in the world side by side");
    if (left == 0 || right == 0) return;
    combat_rig::FakePlayer player(rig, 77, {0, 700, 1098});
    world.add_player(&player);
    self_effects::CastLog log_left, log_right;
    const double dt = 1.0 / 15.0;
    for (double now = dt; now <= 90.0 + 1e-9; now += dt) {
        if (const stats::StatValue* max = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max->value);
        const combat::Vec3 at = player.rig_fighter().position();
        w.npcs->note_player(player.actor_id(), at.x, at.y, now);
        w.npcs->tick(now);
        world.tick(now);
        log_left.look(w.registry, left, now);
        log_right.look(w.registry, right, now);
    }
    // the two Headbutt tasks of each goblin's tree share one hit record; the record's source says which task cast it, so each is counted by its own identity
    size_t moving_left = 0, moving_right = 0, stationary_left = 0, stationary_right = 0;
    for (const combat::IncomingHit& h : player.hits) {
        if (h.hit_guid != mob_windows::kGoblinHeadbuttHit) continue;
        const bool on_left = h.instigator == left;
        if (h.source_guid == mob_windows::kGoblinHeadbutt) {
            if (on_left) ++moving_left; else ++moving_right;
        } else if (h.source_guid == mob_windows::kGoblinStationaryHeadbutt) {
            if (on_left) ++stationary_left; else ++stationary_right;
        }
    }
    std::printf("  headbutts: left %zu (moving %zu, stationary %zu), right %zu (moving %zu, stationary %zu)\n",
                log_left.count(mob_windows::kGoblinHeadbutt) + log_left.count(mob_windows::kGoblinStationaryHeadbutt), log_left.count(mob_windows::kGoblinHeadbutt),
                log_left.count(mob_windows::kGoblinStationaryHeadbutt), log_right.count(mob_windows::kGoblinHeadbutt) + log_right.count(mob_windows::kGoblinStationaryHeadbutt),
                log_right.count(mob_windows::kGoblinHeadbutt), log_right.count(mob_windows::kGoblinStationaryHeadbutt));
    std::printf("  hits: left %zu (moving %zu, stationary %zu), right %zu (moving %zu, stationary %zu)\n", moving_left + stationary_left, moving_left, stationary_left,
                moving_right + stationary_right, moving_right, stationary_right);
    MOVE_CHECKF(log_left.count(mob_windows::kGoblinHeadbutt) >= 1 && log_right.count(mob_windows::kGoblinHeadbutt) >= 1, "each goblin plays its own Headbutt (%zu and %zu)",
                log_left.count(mob_windows::kGoblinHeadbutt), log_right.count(mob_windows::kGoblinHeadbutt));
    MOVE_CHECKF(log_left.count(mob_windows::kGoblinStationaryHeadbutt) >= 1 && log_right.count(mob_windows::kGoblinStationaryHeadbutt) >= 1,
                "each goblin plays the stationary Headbutt of its tree's pair too (%zu and %zu)", log_left.count(mob_windows::kGoblinStationaryHeadbutt),
                log_right.count(mob_windows::kGoblinStationaryHeadbutt));
    MOVE_CHECKF(moving_left == log_left.count(mob_windows::kGoblinHeadbutt) && moving_right == log_right.count(mob_windows::kGoblinHeadbutt),
                "the player takes one hit for each goblin's cast (%zu and %zu)", moving_left, moving_right);
    MOVE_CHECKF(stationary_left == log_left.count(mob_windows::kGoblinStationaryHeadbutt) && stationary_right == log_right.count(mob_windows::kGoblinStationaryHeadbutt),
                "the stationary Headbutt lands one hit for each of its casts too (%zu and %zu)", stationary_left, stationary_right);
}

AOC_REGISTER_TEST(test_two_goblins_side_by_side_each_headbutt);

// R1 and the other order: a reset in the middle of a window (after the cast began, before the window opens) drops the attack: no hit of the dropped window ever lands.
void test_a_reset_in_the_middle_of_a_window_drops_the_attack(Fixture& f) {
    std::printf("a goblin reset before its hit window opens lands none of the window's hit\n");
    combat::NpcAbilityData tables;
    if (!mob_windows::load_with_moves(f, {{mob_windows::kGoblinHeadbutt, true, "c0:T", true, 60, false, "", false, "", ""}}, {{mob_windows::kGoblinMontage, "1.0:2.0:0"}},
                                      "the creature tables with the Headbutt's late window load", tables))
        return;
    const std::string path = creature_path(f, mob_windows::kGoblinPath);
    MOVE_CHECK(!path.empty() && f.motion.find(path), "the Blueprint table has the Bonebinder goblin");
    if (path.empty() || !f.motion.find(path)) return;
    BanditOpen made;
    MOVE_CHECK(mob_windows::make_open(f, tables, path, made), "the world makes an open for the goblin");
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    bool reset = false;
    double reset_at = -1, cast_at = -1;
    const LungeStep step = [&](LungeStepContext& c) {
        if (!c.after) return;
        for (const LungeSample& s : samples) {
            if (cast_at < 0 && s.cast_began && s.ability == mob_windows::kGoblinHeadbutt) cast_at = s.t;
        }
        if (!reset && cast_at > 0 && c.now >= cast_at + 0.8) {
            reset = c.world.gm_reset(c.creature, c.now) != combat::CombatWorld::GmReset::Dead;
            reset_at = c.now;
        }
    };
    if (!run_lunge_fight(f, tables, path, 12.0, 0, 200, nullptr, samples, &hits, false, -1, nullptr, 40000.0, made.open, 0, 0, step)) return;
    for (const LungeSample& s : samples) {
        if (s.cast_began && s.ability == mob_windows::kGoblinHeadbutt && cast_at < 0) cast_at = s.t;
    }
    std::printf("  the Headbutt at %.2f s, the reset at %.2f s, %zu hit(s)\n", cast_at, reset_at, mob_windows::hits_of(hits, mob_windows::kGoblinHeadbuttHit));
    MOVE_CHECKF(cast_at > 0, "the goblin begins its Headbutt before the reset (a window of the montage)");
    MOVE_CHECK(reset && reset_at > cast_at, "its fight is reset while the window is still to open");
    MOVE_CHECKF(mob_windows::hits_of(hits, mob_windows::kGoblinHeadbuttHit) == 0, "the dropped window lands nothing (%zu hits)", mob_windows::hits_of(hits, mob_windows::kGoblinHeadbuttHit));
}

AOC_REGISTER_TEST(test_a_reset_in_the_middle_of_a_window_drops_the_attack);

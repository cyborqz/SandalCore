// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/combat_display_names_tests.inc
//
//  The names the game shows for the mobs: the Blueprint table (dist/content/npc_display_names.tsv, tools/npcabilities) and the population data's names (dist/content/npc_names.tsv).  Both are
//  optional; the world's mob log lines carry the name beside the Blueprint's leaf, a spawn that names itself keeps its own name, and nothing a client sees changes.  The tables in the tests
//  hold rows of the game's data (a Wolf, two goblins, an instance of Slate Wolves the data names).
// ============================================================================

// These checks print their passing line too, so a run shows what each one held as well as what failed.
#define CHECK_NAMED(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] %s\n", msg); ++g_fail; } \
} while (0)
#define CHECKF_NAMED(cond, ...) do { \
    if (cond) { std::printf("  [ok ] "); std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); ++g_pass; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } \
} while (0)

/// The tables a test writes into the temp folder: each file has a name of its own (two gates can run this program at once) and goes when the test is over.
struct DisplayTempFiles {
    std::vector<std::string> paths;
    const std::string suffix = unique_temp_suffix();
    ~DisplayTempFiles() {
        for (const std::string& p : paths) {
            std::error_code ignored;
            std::filesystem::remove(p, ignored);
        }
    }
    std::string next(const char* what) {
        paths.push_back((std::filesystem::temp_directory_path() / ("aoc_display_names_" + std::string(what) + "_" + suffix + "_" + std::to_string(paths.size()) + ".tsv")).string());
        return paths.back();
    }
    /// A display name table (blueprint, display_name, source).
    std::string table(const std::vector<std::pair<std::string, std::string>>& rows, const char* line_end = "\n") {
        const std::string path = next("blueprints");
        std::ofstream out(path, std::ios::binary);
        out << "blueprint\tdisplay_name\tsource" << line_end;
        for (const auto& [blueprint, name] : rows) out << blueprint << "\t" << name << "\tinfo" << line_end;
        return path;
    }
    struct PopulationRow {
        const char* kind;
        int64_t     source;
        int         member;
        const char* text;
    };
    /// The population data's names (kind, source, member, text).
    std::string population(const std::vector<PopulationRow>& rows) {
        const std::string path = next("population");
        std::ofstream out(path, std::ios::binary);
        out << "kind\tsource\tmember\ttext\n";
        for (const PopulationRow& r : rows) out << r.kind << "\t" << r.source << "\t" << r.member << "\t" << r.text << "\n";
        return path;
    }
    /// A path no test writes: the table is absent there.
    std::string absent(const char* what) { return (std::filesystem::temp_directory_path() / ("aoc_display_names_absent_" + std::string(what) + "_" + suffix + ".tsv")).string(); }
};

static const char* const kWolfArchetype = "/Game/DesignAssets/Population/NPC/Beast/Canine/Wolf.Default__Wolf_C";
static const char* const kGravehideArchetype =
    "/Game/DesignAssets/Population/NPC/Humanoid/Goblin/GoblinCaster/GoblinShaman/GoblinFlameShaman/Goblin_FlameShaman_Gravehide.Default__Goblin_FlameShaman_Gravehide_C";
static const char* const kBonebinderArchetype =
    "/Game/DesignAssets/Population/NPC/Humanoid/Goblin/GoblinCaster/GoblinSeer/Goblin_Seer_Bonebinder.Default__Goblin_Seer_Bonebinder_C";
/// An instance of the data's Slate Wolves that the population data names (npc_names.tsv: "Starving Slate Wolf").
static const int64_t kStarvingWolfInstance = 6064630270761784960;

/// A world with a player and a Wolf, the creature tables and an Aftermath (no reward book: the death line only), for the log lines.
struct DisplayScene {
    Fixture&                            f;
    Rig                                 rig;
    combat_rig::World                   w;
    combat::NpcStatBook                 book;
    combat::NpcAbilityData              tables;
    npc::SensesTable                    senses;
    bool                                senses_loaded = false;
    std::unique_ptr<combat::CombatWorld> world;
    std::unique_ptr<combat_rig::FakePlayer> player;
    std::unique_ptr<combat::Aftermath>  after;
    uint64_t                            wolf = 0;
    uint64_t                            next_guid = 9000;
    double                              now = 0;
    bool                                skipped = false;

    /// `table_path` and `population_path` are read by a direct call, or, with `through_config`, by the world's constructor from its configuration, as worldd does.
    explicit DisplayScene(Fixture& fx, const std::string& table_path, const std::string& population_path = std::string(), bool through_config = false)
        : f(fx), rig(4, 20, {}), book(*fx.ctx->bundle, *fx.ctx->gear) {
        if (!rig.ok() || !w.problem.empty()) return;
        if (!load_creature_tables(tables)) { skipped = true; return; }
        combat::CombatWorldConfig cc;
        cc.seed = 11;
        if (through_config) {
            cc.display_names_path = table_path;
            cc.population_names_path = population_path;
        }
        world = std::make_unique<combat::CombatWorld>(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cc);
        world->set_ability_data(&tables);
        std::string senses_dir = NPC_MOTION_PATH;
        senses_dir.erase(senses_dir.find_last_of("/\\") + 1);
        std::string senses_error;
        senses_loaded = senses.load(senses_dir + "npc_senses.tsv", senses_dir + "npc_aggro_curves.tsv", senses_error);
        if (senses_loaded) world->set_senses_data(&senses);
        if (!through_config) {
            world->load_display_names(table_path);
            if (!population_path.empty()) world->load_population_names(population_path);
        }
        player = std::make_unique<combat_rig::FakePlayer>(rig, 77, combat::Vec3{600, 0, 1098});
        world->add_player(player.get());
        auto mint = [this]() {
            aoc::protocol::emit::FIntrepidNetworkGUID g;
            g.ObjectId = next_guid;
            g.ServerId = 60;
            g.Randomizer = 0x6ee87ee4u;
            next_guid += 2;
            return g;
        };
        after = std::make_unique<combat::Aftermath>(w.registry, *world, nullptr, mint, combat::AftermathConfig{});
        after->install();
        const std::string wolf_path = wolf_archetype(f);
        if (!wolf_path.empty()) wolf = w.spawn(wolf_path, 0, 0, 10, 800.0);
    }
    bool ok() const { return world && player && after && wolf != 0; }

    void run(double seconds) {
        const double end = now + seconds;
        while (now < end) {
            now += 0.1;
            const combat::Vec3 at = player->rig_fighter().position();
            w.npcs->note_player(77, at.x, at.y, now);
            w.npcs->tick(now);
            world->tick(now);
            after->tick(now);
        }
    }
    void kill() {
        combat::Resolved r;
        r.mods.push_back({1, content::kGuidHealth, -1.0e9});
        world->apply_hit_to_npc(wolf, 77, r, now);
        after->tick(now);
    }
    /// The name the spawn's own open carries (a person the data names): it must keep it whatever the Blueprint's table says.
    void name_it(const std::string& name) {
        w.registry.modify(wolf, world::Change::State, [&](world::WorldActor& a) { a.npc.name = name; });
    }
    /// The population instance and asset the spawn came from.
    void tag(int64_t instance, int64_t asset) {
        w.registry.modify(wolf, world::Change::State, [&](world::WorldActor& a) {
            a.npc.instance_guid = instance;
            a.npc.asset_guid = asset;
        });
    }
};

static std::string capture_display_log(const std::function<void()>& body) {
    std::ostringstream log;
    const auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("display_names", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    body();
    logger->flush();
    spdlog::set_default_logger(previous);
    return log.str();
}

void test_mob_display_names_table(Fixture& f) {
    std::printf("the mob display names: the table is optional and names a Blueprint\n");
    DisplayTempFiles tmp;
    DisplayScene s(f, tmp.table({{kWolfArchetype, "Slate Wolf"}, {kGravehideArchetype, "Gravehide Flamespeaker"}, {kBonebinderArchetype, "Bonebinder Seer"}}));
    if (s.skipped) return;
    CHECKF_NAMED(s.ok(), "the scene: %s %s", s.rig.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    CHECK_NAMED(s.world->display_names() == 3, "the table reads its three rows");
    CHECK_NAMED(s.world->display_name(kGravehideArchetype) == "Gravehide Flamespeaker" && s.world->display_name(kBonebinderArchetype) == "Bonebinder Seer",
          "the two goblins are in the table");
    CHECK_NAMED(s.world->log_name(kGravehideArchetype, "Goblin_FlameShaman_Gravehide") == "Gravehide Flamespeaker (Goblin_FlameShaman_Gravehide)",
          "the log name carries the display name beside the Blueprint's leaf");
    CHECK_NAMED(s.world->log_name("/Game/No/Such.Try.Default__Try_C", "Try") == "Try", "a Blueprint the table does not name keeps its leaf");
    CHECK_NAMED(s.world->load_display_names(tmp.table({{kWolfArchetype, "Slate Wolf"}})) == 1 && s.world->load_display_names(tmp.table({{kWolfArchetype, "Slate Wolf"}})) == 1 &&
              s.world->display_name(kWolfArchetype) == "Slate Wolf" && s.world->display_name(kGravehideArchetype).empty(),
          "reading a table again replaces it, it does not add a second copy");
    CHECK_NAMED(s.world->load_display_names(tmp.table({{kWolfArchetype, "Slate Wolf"}}, "\r\n")) == 1 && s.world->display_name(kWolfArchetype) == "Slate Wolf",
          "a table with Windows line ends reads the same");
    CHECK_NAMED(s.world->load_display_names(tmp.absent("blueprints")) == 0 && s.world->display_names() == 0,
          "an absent table is no error: it is empty");
    CHECK_NAMED(s.world->log_name(kGravehideArchetype, "Goblin_FlameShaman_Gravehide") == "Goblin_FlameShaman_Gravehide", "...and the log lines keep the leaf");
    CHECK_NAMED(s.world->load_display_names(tmp.table({{kGravehideArchetype, ""}})) == 0 && s.world->log_name(kGravehideArchetype, "Goblin_FlameShaman_Gravehide") == "Goblin_FlameShaman_Gravehide",
          "a row with no name names nothing");
    CHECK_NAMED(s.world->load_display_names(tmp.table({{"/Game/A/Raven.Default__Raven_C", "Raven"}})) == 1 && s.world->log_name("/Game/A/Raven.Default__Raven_C", "Raven") == "Raven",
          "a name that is the leaf itself is said once");
}

void test_mob_log_lines_carry_the_display_name(Fixture& f) {
    std::printf("the mob log lines carry the display name beside the Blueprint's leaf\n");
    DisplayTempFiles tmp;
    DisplayScene with(f, tmp.table({{kWolfArchetype, "Slate Wolf"}}));
    if (with.skipped) return;
    CHECKF_NAMED(with.ok(), "the scene with the table: %s %s", with.rig.problem.c_str(), with.w.problem.c_str());
    if (!with.ok()) return;
    const std::string text = capture_display_log([&] {
        with.run(12.0);
        with.kill();
    });
    CHECKF_NAMED(text.find("Slate Wolf (Wolf)") != std::string::npos, "the mob's log lines name it: %s", text.substr(0, 400).c_str());
    CHECK_NAMED(text.find("Slate Wolf (Wolf) (") != std::string::npos && text.find("began a fight with player") != std::string::npos, "the fight line carries the name");
    CHECK_NAMED(text.find("(Slate Wolf (Wolf)) plays ") != std::string::npos, "the attack line carries the name");
    CHECK_NAMED(text.find("Slate Wolf (Wolf) (level 10) hit the player") != std::string::npos, "the hit line carries the name");
    CHECK_NAMED(text.find("[loot] Slate Wolf (Wolf, level 10) died") != std::string::npos, "the death and loot line carry the name");
}

void test_mob_log_lines_without_the_table(Fixture& f) {
    std::printf("with no display name table the mob log lines keep the Blueprint's leaf\n");
    DisplayTempFiles tmp;
    DisplayScene without(f, tmp.absent("blueprints"));
    if (without.skipped) return;
    CHECKF_NAMED(without.ok(), "the scene without the table: %s %s", without.rig.problem.c_str(), without.w.problem.c_str());
    if (!without.ok()) return;
    CHECK_NAMED(without.world->display_names() == 0, "no table is read");
    const std::string text = capture_display_log([&] {
        without.run(12.0);
        without.kill();
    });
    CHECK_NAMED(text.find("(Wolf)") != std::string::npos, "the lines still name the Blueprint's leaf");
    CHECK_NAMED(text.find("Slate Wolf") == std::string::npos, "and carry no name they do not have");
    CHECK_NAMED(text.find("hit the player") != std::string::npos, "the fight runs the same way");
    CHECK_NAMED(text.find("[loot] Wolf (Wolf, level 10) died") != std::string::npos, "the death line names the leaf");
}

void test_named_spawn_keeps_its_name(Fixture& f) {
    std::printf("a spawn that names itself keeps its own name on every line, not the Blueprint's table name\n");
    DisplayTempFiles tmp;
    DisplayScene s(f, tmp.table({{kWolfArchetype, "Slate Wolf"}}));
    if (s.skipped) return;
    CHECKF_NAMED(s.ok(), "the scene: %s %s", s.rig.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    s.name_it("Marrowrot Flamespeaker");
    const std::string text = capture_display_log([&] {
        s.run(12.0);
        s.kill();
    });
    CHECK_NAMED(text.find("Marrowrot Flamespeaker (Wolf) (") != std::string::npos && text.find("began a fight with player") != std::string::npos, "the fight line carries the name the spawn has");
    CHECK_NAMED(text.find("(Marrowrot Flamespeaker (Wolf)) plays ") != std::string::npos, "the attack line does");
    CHECK_NAMED(text.find("Marrowrot Flamespeaker (Wolf) (level 10) hit the player") != std::string::npos, "the hit line does");
    CHECK_NAMED(text.find("[loot] Marrowrot Flamespeaker (Wolf, level 10) died") != std::string::npos, "the name the open carries is the one the death line shows");
    CHECK_NAMED(text.find("Slate Wolf") == std::string::npos, "the Blueprint's table name replaces it nowhere");
}

void test_population_names_name_the_creature(Fixture& f) {
    std::printf("a creature its population instance or asset names is logged by that name\n");
    DisplayTempFiles tmp;
    const std::string population = tmp.population({
        {"instance", kStarvingWolfInstance, 0, "Starving Slate Wolf"},
        {"instance", 6064630270761766867, 0, ""},   // an instance whose only entry is blank: the Blueprint's name stays (and the asset's list is not asked)
        {"instance", 41, 0, "First Name"},
        {"instance", 41, 1, "Second Name"},
        {"instance", 41, 2, "First Name"},
        {"asset", 7001, 0, "Asset Wolf"},
    });
    DisplayScene s(f, tmp.table({{kWolfArchetype, "Slate Wolf"}}), population);
    if (s.skipped) return;
    CHECKF_NAMED(s.ok(), "the scene: %s %s", s.rig.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    CHECK_NAMED(s.world->population_names() == 4, "the file's four lists are read (three instances and an asset)");
    const auto shown = [&] { return s.world->shown_name(s.wolf, kWolfArchetype, "Wolf"); };
    CHECK_NAMED(shown() == "Slate Wolf", "an actor with no population instance or asset is named by its Blueprint");
    s.tag(kStarvingWolfInstance, 7001);
    CHECK_NAMED(shown() == "Starving Slate Wolf", "the instance's name comes before the asset's");
    s.tag(0, 7001);
    CHECK_NAMED(shown() == "Asset Wolf", "an instance with no list leaves the name to the asset");
    s.tag(6064630270761766867, 7001);
    CHECK_NAMED(shown() == "Slate Wolf", "an instance with a blank entry leaves the Blueprint's name, not the asset's");
    s.tag(41, 0);
    CHECK_NAMED(shown() == "First Name / Second Name", "a list of several names says each once (the game picks one of them)");
    s.tag(99, 98);
    CHECK_NAMED(shown() == "Slate Wolf", "an instance and an asset the file does not list leave the Blueprint's name");
    s.tag(kStarvingWolfInstance, 0);
    CHECK_NAMED(s.world->log_name(s.wolf, kWolfArchetype, "Wolf") == "Starving Slate Wolf (Wolf)", "the log name puts the Blueprint's leaf beside it");
    s.name_it("Marrowrot Flamespeaker");
    CHECK_NAMED(shown() == "Marrowrot Flamespeaker", "a name the spawn itself carries comes first");
    s.name_it("Wolf");
    CHECK_NAMED(shown() == "Starving Slate Wolf", "a name that is only the Blueprint's leaf is no name");
    s.name_it("");
    const std::string text = capture_display_log([&] {
        s.run(12.0);
        s.kill();
    });
    CHECK_NAMED(text.find("Starving Slate Wolf (Wolf) (") != std::string::npos && text.find("began a fight with player") != std::string::npos, "the fight line carries the instance's name");
    CHECK_NAMED(text.find("(Starving Slate Wolf (Wolf)) plays ") != std::string::npos, "the attack line does");
    CHECK_NAMED(text.find("Starving Slate Wolf (Wolf) (level 10) hit the player") != std::string::npos, "the hit line does");
    CHECK_NAMED(text.find("[loot] Starving Slate Wolf (Wolf, level 10) died") != std::string::npos, "the death and loot line do");
    CHECK_NAMED(text.find("[loot] Slate Wolf") == std::string::npos && text.find("(Slate Wolf (Wolf)") == std::string::npos, "the Blueprint's name is not on any of them");
}

void test_real_population_names(Fixture& f) {
    std::printf("the game's own population names file names the instance of a Wolf\n");
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    const std::string path = dir + "npc_names.tsv";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] %s is not here: skipped\n", path.c_str());
        return;
    }
    DisplayTempFiles tmp;
    DisplayScene s(f, tmp.absent("blueprints"), path);
    if (s.skipped) return;
    CHECKF_NAMED(s.ok(), "the scene: %s %s", s.rig.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    CHECKF_NAMED(s.world->population_names() > 3000, "the real file has thousands of lists (%zu)", s.world->population_names());
    s.tag(kStarvingWolfInstance, 0);
    CHECK_NAMED(s.world->shown_name(s.wolf, kWolfArchetype, "Wolf") == "Starving Slate Wolf", "an instance of the data's Slate Wolves the data names is the Starving Slate Wolf");
}

void test_real_display_names_file(Fixture& f) {
    std::printf("the game's own mob display names file names the kinds it lists as the game shows them\n");
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    const std::string path = dir + "npc_display_names.tsv";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] %s is not here: skipped\n", path.c_str());
        return;
    }
    DisplayScene s(f, path);
    if (s.skipped) return;
    CHECKF_NAMED(s.ok(), "the scene: %s %s", s.rig.problem.c_str(), s.w.problem.c_str());
    if (!s.ok()) return;
    CHECKF_NAMED(s.world->display_names() > 2000, "the real file names thousands of Blueprints (%zu)", s.world->display_names());
    struct Known {
        const char* archetype;
        const char* name;
    };
    const Known known[] = {
        {kWolfArchetype, "Slate Wolf"},
        {kGravehideArchetype, "Gravehide Flamespeaker"},
        {kBonebinderArchetype, "Bonebinder Seer"},
        {"/Game/DesignAssets/Population/NPC/Humanoid/Goblin/Goblin_Bonebinder.Default__Goblin_Bonebinder_C", "Bonebinder Creeper"},
        {"/Game/DesignAssets/Population/NPC/Beast/Bear/Bear.Default__Bear_C", "Kaelan Brown Bear"},
        {"/Game/DesignAssets/Population/NPC/Beast/Bear/Bear1.Default__Bear1_C", "Kaelan Grizzly Bear"},
        {"/Game/DesignAssets/Population/NPC/Beast/Canine/AlphaWolf.Default__AlphaWolf_C", "Alpha Wolf"},
        {"/Game/DesignAssets/Population/NPC/Beast/Tidebreaker/Tidebreaker.Default__Tidebreaker_C", "Riverdancer Otter"},
        {"/Game/DesignAssets/Population/NPC/Humanoid/Minotaur/BattlePriest/Minotaur_BattlePriest.Default__Minotaur_BattlePriest_C", "Ashen Flame Disciple"},
        {"/Game/DesignAssets/Population/NPC/Monster/Undead/Skeleton/Skeleton_Knight.Default__Skeleton_Knight_C", "Skeleton Soldier"},
    };
    for (const Known& k : known) CHECKF_NAMED(s.world->display_name(k.archetype) == k.name, "%s is %s (the table says \"%s\")", k.archetype, k.name, s.world->display_name(k.archetype).c_str());
}

void test_display_names_load_through_the_config(Fixture& f) {
    std::printf("the world's constructor reads both tables from its configuration, and an absent file is no error\n");
    DisplayTempFiles tmp;
    const std::string population = tmp.population({{"instance", kStarvingWolfInstance, 0, "Starving Slate Wolf"}});
    {
        DisplayScene s(f, tmp.table({{kWolfArchetype, "Slate Wolf"}, {kGravehideArchetype, "Gravehide Flamespeaker"}}), population, true);
        if (s.skipped) return;
        CHECKF_NAMED(s.ok(), "the scene: %s %s", s.rig.problem.c_str(), s.w.problem.c_str());
        if (!s.ok()) return;
        CHECK_NAMED(s.world->display_names() == 2 && s.world->population_names() == 1, "both tables are read with no call but the constructor's");
        CHECK_NAMED(s.world->log_name(kGravehideArchetype, "Goblin_FlameShaman_Gravehide") == "Gravehide Flamespeaker (Goblin_FlameShaman_Gravehide)", "a Blueprint the table names is named");
        s.tag(kStarvingWolfInstance, 0);
        const std::string text = capture_display_log([&] {
            s.run(12.0);
            s.kill();
        });
        CHECK_NAMED(text.find("[loot] Starving Slate Wolf (Wolf, level 10) died") != std::string::npos, "a fight in that world names the creature as the population data does");
    }
    {
        DisplayScene s(f, tmp.absent("blueprints"), tmp.absent("population"), true);
        if (s.skipped) return;
        CHECKF_NAMED(s.ok(), "the scene with no files: %s %s", s.rig.problem.c_str(), s.w.problem.c_str());
        if (!s.ok()) return;
        CHECK_NAMED(s.world->display_names() == 0 && s.world->population_names() == 0, "absent files read as empty tables");
        CHECK_NAMED(s.world->log_name(s.wolf, kWolfArchetype, "Wolf") == "Wolf", "and the log lines keep the leaf");
    }
}

AOC_REGISTER_TEST(test_mob_display_names_table);
AOC_REGISTER_TEST(test_mob_log_lines_carry_the_display_name);
AOC_REGISTER_TEST(test_mob_log_lines_without_the_table);
AOC_REGISTER_TEST(test_named_spawn_keeps_its_name);
AOC_REGISTER_TEST(test_population_names_name_the_creature);
AOC_REGISTER_TEST(test_real_population_names);
AOC_REGISTER_TEST(test_real_display_names_file);
AOC_REGISTER_TEST(test_display_names_load_through_the_config);

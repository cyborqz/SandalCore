// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_table_errors_tests.inc: a creature table that does not read says which column and what it holds
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
// ============================================================================

// The strict loader of the creature tables used to say only "a line of <first column> does not read": the name of the file and the line's first column, not the value that failed. It names the
// column now and shows what the column holds, for each of the three tables.
void test_creature_table_errors_name_the_column() {
    std::printf("a creature table that does not read: the error names the file, the line, the column and what the column holds\n");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / ("aoc_creature_tables_columns_" + unique_temp_suffix());   // (a folder of its own: two gates can run this program at once)
    fs::create_directories(dir, ec);
    const std::string abilities_header =
        "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";
    const std::string montages_header = "montage\tguid\tpackage\tpackage_guid\tlength\trate_scale\tsections\thits\troot_motion\troot_unreadable\tanim_end\tfires";
    const std::string targeting_header =
        "ability\tkey\tdistance\tmin_distance\tangle\tangle_center\tcone_check\tfreeze\tfreeze_distance\tfreeze_scale\tignore_non_target\tdiscovery\tcheck_validity\tlos_check\tdistance_center\tvolumes\tshapes";
    const auto write = [&](const char* name, const std::string& header, const std::string& row) {
        std::ofstream out(dir / name, std::ios::binary);
        out << header << "\n";
        if (!row.empty()) out << row << "\n";
        return (dir / name).string();
    };
    const std::string no_abilities = write("abilities.tsv", abilities_header, "");
    const std::string no_montages = write("montages.tsv", montages_header, "");
    const std::string no_targeting = write("targeting.tsv", targeting_header, "");
    // the message the load gives when ONE of the three tables holds one line, and that line is `row`
    enum class Table { Abilities, Montages, Targeting };
    const auto error_of = [&](Table which, const std::string& row) {
        const std::string a = which == Table::Abilities ? write("bad_abilities.tsv", abilities_header, row) : no_abilities;
        const std::string m = which == Table::Montages ? write("bad_montages.tsv", montages_header, row) : no_montages;
        const std::string t = which == Table::Targeting ? write("bad_targeting.tsv", targeting_header, row) : no_targeting;
        combat::NpcAbilityData tables;
        std::string err;
        const bool loaded = tables.load(a, m, t, err);
        return loaded ? std::string() : err;
    };
    const auto has = [](const std::string& text, const char* part) { return text.find(part) != std::string::npos; };
    // archetype, ability, task, cooldown, stop_movement, order, min_distance, max_distance, max_angle, gates, unknown, fight, conds, why, starts, rotate, arrival
    const auto ability_row = [](const char* cooldown, const char* stop, const char* max_distance, const char* fight) {
        return std::string("Wolf\t7\tBite\t") + cooldown + "\t" + stop + "\t0\t0\t" + max_distance + "\t180\t\t0\t" + fight + "\t\t\t\t\t0";
    };
    const std::string cooldown = error_of(Table::Abilities, ability_row("nan", "1", "300", "1"));
    CHECK(has(cooldown, "abilities.tsv") && has(cooldown, "a line of Wolf") && has(cooldown, "the column cooldown holds \"nan\""),
          "a NaN cooldown: the error names the table, the line (Wolf) and the column (cooldown) with what it holds (nan)");
    const std::string reach = error_of(Table::Abilities, ability_row("2.5", "1", "inf", "1"));
    CHECK(has(reach, "the column max_distance holds \"inf\""), "an infinite reach: the column is max_distance and it holds inf");
    const std::string flag = error_of(Table::Abilities, ability_row("2.5", "1", "300", "yes"));
    CHECK(has(flag, "the column fight holds \"yes\""), "a flag that is neither 0 nor 1: the column is fight and it holds yes");
    const std::string stop = error_of(Table::Abilities, ability_row("2.5", "2", "300", "1"));
    CHECK(has(stop, "the column stop_movement holds \"2\""), "another flag: the column is stop_movement and it holds 2");
    // montage, guid, package, package_guid, length, rate_scale, sections, hits, root_motion, root_unreadable, anim_end, fires
    const std::string length = error_of(Table::Montages, "/Game/M\t5\tP\t6\tnan\t1\t\t\t\t0\t\t");
    CHECK(has(length, "montages.tsv") && has(length, "a line of /Game/M") && has(length, "the column length holds \"nan\""), "a montage whose length is NaN: the column is length");
    const std::string hits = error_of(Table::Montages, "/Game/M\t5\tP\t6\t2\t1\t\t0.5-x\t\t0\t\t");
    CHECK(has(hits, "the column hits holds \"0.5-x\""), "a montage with a hit that is no time:key pair: the column is hits and it holds the text");
    // ability, key, distance, min_distance, angle, angle_center, cone_check, freeze, freeze_distance, freeze_scale, ignore_non_target, discovery, check_validity, los_check, distance_center, volumes, shapes
    const std::string angle = error_of(Table::Targeting, "7\t1\t100\t0\tnan\t0\t0\tnone\t0\t1\t0\tnone\t0\t0\t0\t0\t");
    CHECK(has(angle, "targeting.tsv") && has(angle, "a line of 7") && has(angle, "the column angle holds \"nan\""), "a targeting record whose angle is NaN: the column is angle");
    const std::string freeze = error_of(Table::Targeting, "7\t1\t100\t0\t90\t0\t0\tsometimes\t0\t1\t0\tnone\t0\t0\t0\t0\t");
    CHECK(has(freeze, "the column freeze holds \"sometimes\""), "a freeze that is none, pause or stop and neither: the column is freeze");
    // a table that is fine loads (the other side of every check above)
    CHECK(error_of(Table::Abilities, ability_row("2.5", "1", "300", "1")).empty(), "(a good line loads)");
    fs::remove_all(dir, ec);
}
AOC_REGISTER_TEST(test_creature_table_errors_name_the_column);

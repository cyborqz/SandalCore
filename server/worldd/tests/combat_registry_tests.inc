// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_registry_tests.inc: the test registry's own failure paths (tests/test_registry.h)
//  (registered with AOC_REGISTER_TEST: included from combat_registered.inc, so test_worldd_combat.cpp runs it through aoc_tests::run)
//
//  The runner is run on a list of its own, with its messages going to a temporary file: a name registered twice, a test made for another fixture type and a test
//  that throws each cost one problem and one "[FAIL] ..." line, and the tests that are fine still run.
// ============================================================================

void test_registry_failure_paths(Fixture& f) {
    std::printf("the test registry reports its own failure paths\n");
    struct OtherFixture {};   // a fixture type that is not the program's
    int ran = 0;
    std::vector<aoc_tests::detail::Entry> list;
    list.push_back({"fine", __FILE__, __LINE__, nullptr, [&](void*) { ++ran; }});
    list.push_back({"fine", __FILE__, __LINE__, nullptr, [&](void*) { ++ran; }});   // the same name again
    list.push_back({"for_another_fixture", __FILE__, __LINE__, aoc_tests::detail::type_tag<OtherFixture>(), [&](void*) { ++ran; }});
    list.push_back({"throws", __FILE__, __LINE__, nullptr, [](void*) { throw std::runtime_error("boom"); }});
    list.push_back({"after_the_throw", __FILE__, __LINE__, nullptr, [&](void*) { ++ran; }});
    std::FILE* out = std::tmpfile();
    CHECK(out != nullptr, "(a temporary file for the runner's messages)");
    if (!out) return;
    const int problems = aoc_tests::run_list(list, f, out);
    std::rewind(out);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof buf, out)) text += buf;
    std::fclose(out);
    CHECKF(problems == 3, "three problems are counted: a name twice, a test made for another fixture, a test that threw (%d)", problems);
    CHECK(text.find("the name fine is registered twice") != std::string::npos && text.find("for_another_fixture") != std::string::npos && text.find("takes another fixture type") != std::string::npos &&
              text.find("throws threw: boom") != std::string::npos,
          "...and each one is told with the test's name");
    CHECKF(ran == 2, "the tests that were fine ran once each, the one after the throw too (%d)", ran);
}
AOC_REGISTER_TEST(test_registry_failure_paths);

void test_registry_only_filter(Fixture& f) {
    std::printf("the test registry runs only the tests that an AOC_TEST_ONLY filter names\n");
    int ran_a = 0, ran_b = 0, ran_c = 0;
    std::vector<aoc_tests::detail::Entry> list;
    list.push_back({"join_keeps_the_total", __FILE__, __LINE__, nullptr, [&](void*) { ++ran_a; }});
    list.push_back({"join_splits_a_stack", __FILE__, __LINE__, nullptr, [&](void*) { ++ran_b; }});
    list.push_back({"another_thing", __FILE__, __LINE__, nullptr, [&](void*) { ++ran_c; }});
    CHECKF(aoc_tests::select_only(list, nullptr).size() == 3, "no filter selects every test (%zu of 3)", aoc_tests::select_only(list, nullptr).size());
    CHECKF(aoc_tests::select_only(list, "").size() == 3, "an empty filter selects every test (%zu of 3)", aoc_tests::select_only(list, "").size());
    CHECKF(aoc_tests::select_only(list, "join").size() == 2, "a word of two names selects both (%zu of 3)", aoc_tests::select_only(list, "join").size());
    CHECKF(aoc_tests::select_only(list, "splits,another").size() == 2, "comma separated words select the tests of each word (%zu of 3)", aoc_tests::select_only(list, "splits,another").size());
    CHECKF(aoc_tests::select_only(list, "keeps_the_total").size() == 1 && aoc_tests::select_only(list, "keeps_the_total")[0].name == std::string("join_keeps_the_total"),
           "a word inside a name selects that test only (%zu of 3)", aoc_tests::select_only(list, "keeps_the_total").size());
    CHECK(aoc_tests::select_only(list, "nothing_like_this").empty(), "a word that is in no name selects nothing");
    CHECKF(aoc_tests::select_only(list, ",,join_keeps,").size() == 1, "empty words between the commas are ignored (%zu of 3)", aoc_tests::select_only(list, ",,join_keeps,").size());
    std::FILE* out = std::tmpfile();
    CHECK(out != nullptr, "(a temporary file for the runner's messages)");
    if (!out) return;
    const int problems = aoc_tests::run_only(list, "join_keeps", f, out);
    std::rewind(out);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof buf, out)) text += buf;
    std::fclose(out);
    CHECKF(ran_a == 1 && ran_b == 0 && ran_c == 0, "only the test the filter names ran (%d %d %d)", ran_a, ran_b, ran_c);
    CHECK(text.find("[only] 1 of 3 tests") != std::string::npos, "...and the runner says how many of the registered tests it ran");
    CHECKF(problems == 0, "a filtered run that found its test reports no problem (%d)", problems);
    std::FILE* out2 = std::tmpfile();
    CHECK(out2 != nullptr, "(a second temporary file)");
    if (!out2) return;
    const int none = aoc_tests::run_only(list, "no_such_test", f, out2);
    std::rewind(out2);
    std::string text2;
    while (std::fgets(buf, sizeof buf, out2)) text2 += buf;
    std::fclose(out2);
    CHECKF(none == 1 && ran_a == 1 && ran_b == 0 && ran_c == 0, "a filter that matches no test is one problem and runs nothing (%d; %d %d %d)", none, ran_a, ran_b, ran_c);
    CHECK(text2.find("[only] 0 of 3 tests") != std::string::npos && text2.find("[FAIL]") != std::string::npos && text2.find("matches no registered test") != std::string::npos,
          "...and says so with a failure line, so a misspelled name never passes");
}
AOC_REGISTER_TEST(test_registry_only_filter);

void test_registry_shares(Fixture&) {
    std::printf("the test registry shares the tests of one program out among the programs built from its source\n");
    // 400 names of the shape the real tests have: every one belongs to exactly one of 4 parts, each part gets a fair share, and a program of one part runs them all
    std::vector<aoc_tests::detail::Entry> list;
    std::vector<std::string> names;
    for (int i = 0; i < 400; ++i) names.push_back("test_e1_a_session_test_number_" + std::to_string(i * 7919 % 1000));
    for (const std::string& n : names) list.push_back({n.c_str(), __FILE__, __LINE__, nullptr, [](void*) {}});
    std::size_t sum = 0;
    bool each_once = true, stays_the_same = true;
    for (int part = 1; part <= 4; ++part) {
        const std::vector<aoc_tests::detail::Entry> mine = aoc_tests::select_share(list, part, 4);
        sum += mine.size();
        for (const aoc_tests::detail::Entry& e : mine) each_once = each_once && aoc_tests::share_of(e.name, 4) == part;
        CHECKF(mine.size() > 60 && mine.size() < 140, "part %d of 4 gets a fair share of 400 tests (%zu)", part, mine.size());
    }
    for (const std::string& n : names) stays_the_same = stays_the_same && aoc_tests::share_of(n.c_str(), 4) == aoc_tests::share_of(std::string(n.begin(), n.end()).c_str(), 4);
    CHECKF(sum == list.size() && each_once, "the four parts hold every test once between them (%zu of %zu)", sum, list.size());
    CHECK(stays_the_same, "a name belongs to the same part every time it is asked");
    CHECK(aoc_tests::select_share(list, 1, 1).size() == list.size(), "one part of one is every test (a program that is not split runs them all)");
    CHECK(aoc_tests::share_of("test_sprint", 1) == 1 && aoc_tests::in_share("test_sprint", 1, 1), "a name is in part 1 of 1");
    // FNV-1a of "a" is 0xE40C292C: the hash is the one the programs use on every machine, whatever the build
    CHECK(aoc_tests::share_of("a", 4) == static_cast<int>(0xE40C292Cu % 4u) + 1, "the part of a name is its FNV-1a hash modulo the number of parts, plus one");
    // the runner of a list with a part: only the tests of that part run
    int ran_here = 0;
    std::vector<aoc_tests::detail::Entry> small;
    small.push_back({"alpha_test", __FILE__, __LINE__, nullptr, [&](void*) { ++ran_here; }});
    small.push_back({"beta_test", __FILE__, __LINE__, nullptr, [&](void*) { ++ran_here; }});
    small.push_back({"gamma_test", __FILE__, __LINE__, nullptr, [&](void*) { ++ran_here; }});
    small.push_back({"delta_test", __FILE__, __LINE__, nullptr, [&](void*) { ++ran_here; }});
    int total = 0;
    for (int part = 1; part <= 3; ++part) {
        ran_here = 0;
        std::FILE* out = std::tmpfile();
        if (!out) return;
        struct None {};
        None none;
        const int problems = aoc_tests::run_list(aoc_tests::select_share(small, part, 3), none, out);
        std::fclose(out);
        CHECKF(problems == 0, "the tests of part %d of 3 run without a problem (%d)", part, problems);
        total += ran_here;
    }
    CHECKF(total == 4, "three parts run the four tests once each between them (%d)", total);
}
AOC_REGISTER_TEST(test_registry_shares);

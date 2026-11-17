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

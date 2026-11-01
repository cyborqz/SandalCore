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

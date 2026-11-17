// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/elections_mayors_tests.inc: the elections: the cycle, the calendar, the ballot, the runoff, the file, the citizens a node needs, the real rules
//  (included inside test_worldd_elections.cpp's anonymous namespace: its CHECK, kNode, the calendar's times and who())
// ============================================================================

void test_cycle() {
    std::printf("the cycle of a node's elections\n");
    const auto takes = [](int64_t n) { return n == kNode; };
    {
        Elections empty;
        empty.advance(kNov10Noon, takes, {kNode});
        empty.advance(kNov10Noon + 3 * kDay, takes, {kNode});
        CHECK(empty.node(kNode) && empty.node(kNode)->state == ElectionState::AwaitingFirst, "a node with no citizen waits for its first election");
    }
    Elections e;
    int64_t t = kNov10Noon;
    e.set_citizen(kNode, who(1, kAnn, "Ann"));
    e.set_citizen(kNode, who(2, kBob, "Bob"));
    e.set_citizen(kNode, who(3, kCy, "Cy"));
    auto ch = e.advance(t, takes, {kNode, 42});
    CHECK(e.node(kNode) && e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->first && !e.node(42),
          "a node that takes citizens and has some starts its first registration at once; another node does not");
    CHECK(e.node(kNode)->phase_ends_ms == t + kDay && e.node(kNode)->citizens.size() == 3, "... for the first registration's day, with the citizens it had");
    std::string why;
    // the first registration with no candidate runs again
    t += kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->phase_started_ms == t, "a first registration with no candidate runs again");
    CHECK(e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why) && e.register_candidate(kNode, who(2, kBob, "Bob"), t + 1, why), "two citizens stand");
    CHECK(!e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why), "... once each");
    CHECK(!e.register_candidate(kNode, who(9, "0000000000000000000000000000000f", "Stranger"), t, why), "a non-citizen does not stand");
    CHECK(!e.vote(kNode, who(3, kCy, "Cy"), {kAnn}, t, why), "nobody votes in the registration period");
    t += kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Active && e.node(kNode)->phase_ends_ms == t + kDay, "then the first election, for a day");
    CHECK(e.vote(kNode, who(3, kCy, "Cy"), {kBob, kAnn}, t, why) && e.vote(kNode, who(1, kAnn, "Ann"), {kAnn}, t, why) && e.vote(kNode, who(2, kBob, "Bob"), {kBob}, t, why),
          "three citizens vote, one with a ranking");
    CHECK(!e.vote(kNode, who(3, kCy, "Cy"), {"ffffffffffffffffffffffffffffffff"}, t, why), "a vote for no candidate is refused");
    CHECK(e.vote(kNode, who(3, kCy, "Cy"), {kBob}, t, why) && e.node(kNode)->votes.at(3).size() == 1, "a second vote replaces the first");
    t += kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingResults && e.node(kNode)->phase_ends_ms == t + 10 * 1000, "then the first wait for the results, 10 s");
    t += 10 * 1000;
    ch = e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && !ch.empty() && ch.back().results && ch.back().winner == "Bob", "the results: Bob, two votes to one");
    CHECK(e.node(kNode)->mayor.account == 2 && e.mayor_node(2) == kNode && e.mayor_node(1) == 0 && !e.node(kNode)->first, "Bob is the node's mayor; the first election is over");
    CHECK(e.node(kNode)->phase_ends_ms == kNov26, "the term runs to the next month's registration: 26 November, five days before the election of 1 December");
    CHECK(e.set_message(kNode, 2, "Welcome to Halcyon", why) && e.node(kNode)->mayor_message == "Welcome to Halcyon", "the mayor sets a message");
    CHECK(!e.set_message(kNode, 1, "I am mayor", why), "... nobody else");
    // the monthly cycle
    e.advance(kNov26, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->phase_ends_ms == kDec1 && e.node(kNode)->candidates.empty() &&
              e.node(kNode)->mayor.account == 2,
          "the month's registration: until 1 December 00:00, the old candidates gone, the mayor still serving");
    ch = e.advance(kDec1, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && e.node(kNode)->mayor.account == 2 && !ch.empty() && !ch.back().results &&
              e.node(kNode)->phase_ends_ms == kDec27,
          "nobody stood in December: no vote, Bob stays mayor until January's registration (27 December)");
    e.advance(kDec27, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.register_candidate(kNode, who(1, kAnn, "Ann"), kDec27, why), "January's registration: Ann stands");
    e.advance(kJan1, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Active && e.node(kNode)->phase_ends_ms == kJan6, "the election from 1 January 00:00 through the 5th's hour 23 (to 00:00 on the 6th)");
    e.advance(kJan6, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingResults && e.node(kNode)->phase_ends_ms == kJan6Results, "the results one second later, at 00:00:01");
    e.advance(kJan6Results, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && e.node(kNode)->mayor.account == 1 && e.node(kNode)->mayor.at_ms == kJan6Results,
          "at 00:00:01 the post-election state and the elected mayor");
    // a long stop: the phases catch up
    t = kJan6Results + 3 * kHour;
    ch = e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && e.node(kNode)->mayor.account == 1 && e.node(kNode)->phase_ends_ms == kJan27,
          "Ann, the only candidate, is elected unopposed; her term runs to February's registration");
    CHECK(e.node(kNode)->mayor_message.empty(), "... with no message (the office changed hands)");
    // the office goes with the citizenship
    e.remove_citizen(1);
    CHECK(e.node(kNode)->mayor.account == 0 && e.node(kNode)->citizens.count(1) == 0, "the mayor who loses the citizenship loses the office");
    // another node's citizenship moves the account
    e.set_citizen(77, who(3, kCy, "Cy"));
    CHECK(e.node(kNode)->citizens.count(3) == 0 && e.node(77) && e.node(77)->citizens.count(3) == 1, "a citizen of another node is no citizen of this one");
    // a game master's skip
    const auto skipped = e.skip(kNode, t);
    CHECK(skipped && e.node(kNode)->state == ElectionState::Registration, "a game master skips the term: the registration starts");
}
AOC_REGISTER_TEST(test_cycle);

void test_calendar() {
    std::printf("the election calendar of the data\n");
    ElectionRules r;
    CHECK(r.election_start_ms(kNov10Noon) == kDec1, "from 10 November the next election starts on 1 December (its registration on the 26th has not begun)");
    CHECK(r.election_start_ms(kNov26) == kDec1 && r.election_start_ms(kNov26 + 1) == kJan1, "... on the registration's first instant still that one, after it the next month's");
    CHECK(r.election_end_ms(kJan1) == kJan6 && r.results_ms(kJan6) == kJan6Results, "five days through hour 23 of the last (00:00 after it), the results at 00:00:01");
    ElectionRules early = r;
    early.end_hour = 20;
    CHECK(early.election_end_ms(kJan1) == kJan6 - 3 * kHour, "an end hour of 20 ends at 21:00");
}
AOC_REGISTER_TEST(test_calendar);

void test_ballot_and_rank() {
    std::printf("the ballot's limit and the citizens' rank\n");
    Elections e;
    const auto takes = [](int64_t) { return true; };
    e.set_citizen(kNode, who(1, kAnn, "Ann", 300));
    e.set_citizen(kNode, who(2, kBob, "Bob", 100));
    e.set_citizen(kNode, who(3, kCy, "Cy", 200));
    e.set_citizen(kNode, who(4, kDee, "Dee", 200));
    CHECK(e.citizen_rank(kNode, 2) == 1 && e.citizen_rank(kNode, 3) == 2 && e.citizen_rank(kNode, 4) == 3 && e.citizen_rank(kNode, 1) == 4,
          "the rank is the place by when each joined (a tie by account)");
    CHECK(e.citizen_rank(kNode, 9) == 0 && e.citizen_count(kNode) == 4 && e.citizen_count(42) == 0, "no rank for a stranger; four citizens");
    int64_t t = kNov10Noon;
    e.advance(t, takes, {kNode});
    std::string why;
    for (int64_t a = 1; a <= 4; ++a) e.register_candidate(kNode, who(a, a == 1 ? kAnn : a == 2 ? kBob : a == 3 ? kCy : kDee, "x"), t, why);
    t += kDay;
    e.advance(t, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Active && e.vote(kNode, who(1, kAnn, "Ann"), {kDee, kCy, kBob, kAnn}, t, why),
          "a ballot ranking four candidates is taken");
    CHECK(e.node(kNode)->votes.at(1) == (std::vector<std::string>{kDee, kCy, kBob}), "... with its first three: RankedChoiceVoteCountLimit");
}
AOC_REGISTER_TEST(test_ballot_and_rank);

void test_runoff() {
    std::printf("the instant runoff\n");
    NodeElection n;
    n.candidates = {who(1, kAnn, "Ann"), who(2, kBob, "Bob"), who(3, kCy, "Cy")};
    // Ann 2, Bob 2, Cy 1 whose second choice is Bob: Cy goes, Bob wins 3 to 2
    n.votes[10] = {kAnn};
    n.votes[11] = {kAnn};
    n.votes[12] = {kBob};
    n.votes[13] = {kBob};
    n.votes[14] = {kCy, kBob};
    auto w = Elections::tally(n);
    CHECK(w && *w == 1, "the weakest goes and its ballots count for their next choice: Bob");
    // a candidate who withdrew: the ballots naming him count for their next choice still standing (5 x [B, A], 3 x [A], 4 x [C], B gone: A 8 to 4)
    NodeElection gone;
    gone.candidates = {who(1, kAnn, "A"), who(3, kCy, "C")};
    for (int64_t v = 0; v < 5; ++v) gone.votes[20 + v] = {kBob, kAnn};
    for (int64_t v = 0; v < 3; ++v) gone.votes[30 + v] = {kAnn};
    for (int64_t v = 0; v < 4; ++v) gone.votes[40 + v] = {kCy};
    w = Elections::tally(gone);
    CHECK(w && *w == 0, "a candidate who withdrew passes his ballots on: A wins 8 to 4, not C");
    // an exhausted ballot: its only choice is counted out, it counts no more (A 3, B 2, C 2 whose ballots name no one else: C goes, A 3 of 5 wins)
    NodeElection spent;
    spent.candidates = {who(1, kAnn, "A"), who(2, kBob, "B"), who(3, kCy, "C")};
    spent.votes[50] = {kAnn};
    spent.votes[51] = {kAnn};
    spent.votes[52] = {kAnn};
    spent.votes[53] = {kBob};
    spent.votes[54] = {kBob};
    spent.votes[55] = {kCy};
    spent.votes[56] = {kCy};
    w = Elections::tally(spent);
    CHECK(w && *w == 0, "the ballots of the one counted out with no next choice are spent: A, 3 of the 5 still counting");
    // a three-way tie: the later registered go first, the first registered wins
    NodeElection three;
    three.candidates = {who(1, kAnn, "A"), who(2, kBob, "B"), who(3, kCy, "C")};
    three.votes[60] = {kAnn};
    three.votes[61] = {kBob};
    three.votes[62] = {kCy};
    w = Elections::tally(three);
    CHECK(w && *w == 0, "a three-way tie: C goes, then B, A (registered first) wins");
    NodeElection tie;
    tie.candidates = {who(1, kAnn, "Ann"), who(2, kBob, "Bob")};
    tie.votes[10] = {kAnn};
    tie.votes[11] = {kBob};
    w = Elections::tally(tie);
    CHECK(w && *w == 0, "a tie goes to the one who registered first");
    NodeElection none;
    none.candidates = {who(1, kAnn, "Ann")};
    w = Elections::tally(none);
    CHECK(w && *w == 0, "a candidate with no vote is elected unopposed");
    CHECK(!Elections::tally(NodeElection{}), "no candidate: no mayor");
    // a withdrawal during the vote: the candidate's citizenship ends, the ballots naming him go to their next choice
    Elections e;
    const auto takes = [](int64_t) { return true; };
    for (int64_t a = 1; a <= 3; ++a) e.set_citizen(kNode, who(a, a == 1 ? kAnn : a == 2 ? kBob : kCy, a == 1 ? "Ann" : a == 2 ? "Bob" : "Cy"));
    for (int64_t a = 10; a <= 14; ++a) e.set_citizen(kNode, who(a, ("voter" + std::to_string(a)).c_str(), "v"));
    int64_t t = kNov10Noon;
    e.advance(t, takes, {kNode});
    std::string why;
    e.register_candidate(kNode, who(1, kAnn, "Ann"), t, why);
    e.register_candidate(kNode, who(2, kBob, "Bob"), t, why);
    e.register_candidate(kNode, who(3, kCy, "Cy"), t, why);
    t += kDay;
    e.advance(t, takes, {kNode});
    e.vote(kNode, who(10, "voter10", "v"), {kBob, kAnn}, t, why);
    e.vote(kNode, who(11, "voter11", "v"), {kBob, kAnn}, t, why);
    e.vote(kNode, who(12, "voter12", "v"), {kCy}, t, why);
    e.vote(kNode, who(13, "voter13", "v"), {kAnn}, t, why);
    e.vote(kNode, who(14, "voter14", "v"), {kCy}, t, why);
    e.remove_citizen(2);   // Bob leaves the node while the vote runs
    t += kDay;
    e.advance(t + 10 * 1000, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Post && e.node(kNode)->mayor.account == 1, "Bob leaves during the vote: his two ballots go to Ann, who wins 3 to 2");
}
AOC_REGISTER_TEST(test_runoff);

void test_file() {
    std::printf("the elections file\n");
    Elections e;
    e.set_citizen(kNode, who(1, kAnn, "Ann"));
    e.advance(1000, [](int64_t) { return true; }, {kNode});
    std::string why;
    e.register_candidate(kNode, who(1, kAnn, "Ann"), 2000, why);
    Elections back;
    e.set_dues_anchor(kNode, 123456);
    e.set_dues_anchor(kNode, 999);
    CHECK(e.dues_anchor(kNode) == 123456, "a node's dues schedule is set once");
    CHECK(back.from_json(e.to_json(), why) && back.all() == e.all() && back.node(kNode)->first && back.dues_anchor(kNode) == 123456,
          "what is written reads back the same, the first election's mark and the dues schedule too");
    CHECK(!back.from_json("{nope", why) && back.all() == e.all(), "a damaged file is refused and changes nothing");
}
AOC_REGISTER_TEST(test_file);

void test_min_citizens() {
    std::printf("the citizens a node needs to elect a mayor\n");
    ElectionRules r;
    r.min_citizens[kNode] = 3;
    Elections e(r);
    const auto takes = [](int64_t n) { return n == kNode; };
    e.set_citizen(kNode, who(1, kAnn, "Ann"));
    e.set_citizen(kNode, who(2, kBob, "Bob"));
    e.advance(kNov10Noon, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingFirst, "two citizens of a node that needs three: no election yet");
    e.set_citizen(kNode, who(3, kCy, "Cy"));
    e.advance(kNov10Noon + kHour, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->first, "the third citizen starts the first election");
    e.remove_citizen(3);
    e.advance(kNov10Noon + kHour + kDay, takes, {kNode});
    CHECK(e.node(kNode)->state == ElectionState::AwaitingFirst, "too few citizens when the first registration ends: it waits for them again");
    CHECK(r.citizens_for_election(42) == 1, "a node the data gives no minimum needs one citizen");
    const auto skipped = e.skip(kNode, kNov10Noon + 2 * kDay);
    CHECK(skipped && e.node(kNode)->state == ElectionState::Registration && e.node(kNode)->first, "a game master's skip starts the first election even with too few citizens");
}
AOC_REGISTER_TEST(test_min_citizens);

void test_rules() {
    std::printf("the election rules of the real content.db\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : "dist/content/content.db";
    if (!std::filesystem::exists(path)) {
        std::printf("  [skip] SKIP: no %s\n", path.c_str());
        return;
    }
    std::string why;
    const auto r = ElectionRules::load(path, why);
    CHECK(r && r->registration_days == 5 && r->election_days == 5 && r->first_registration_secs == 86400 && r->first_election_secs == 86400 && r->results_secs == 10 &&
              r->election_type == 0,
          "Scientific_Election_Definition: the first registration and election a day each, 10 s for their results; then 5 days of registration and 5 of voting");
    CHECK(r && r->start_day == 1 && r->start_hour == 0 && r->end_hour == 23 && r->min_term_days == 0,
          "... the election on the 1st at 00:00 through hour 23 of its last day, no minimum term (the test definition is not taken)");
    CHECK(r && r->ranked_limit == 3, "a ballot ranks at most 3 candidates (ElectionConstants)");
}
AOC_REGISTER_TEST(test_rules);

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_rest_end_tests.inc: ServerEndAbility ends the Rest and the Sprint by the server's instance id
//  Registered with AOC_REGISTER_TEST; included from ability_session_registered.inc.
// ============================================================================

/// ServerEndAbility with the server's instance id (the number the ability item carries).
static void end_instance(Run& r, int64_t instance) { r.send_reliable(end_bunch(r.h.ability_guid(), instance)); }

void test_rest_and_sprint_end_by_server_instance() {
    std::printf("ServerEndAbility names the server's instance: it ends the Rest and the Sprint\n");
    // the Rest: the client's own id is not the server's instance
    {
        Run e;
        e.next_id = 42;
        e.play(0.5, false);
        const int32_t id = e.trigger(kRest);
        e.play(2.2, false);
        CHECK(e.rt().resting() && e.m.has_effect(kRestEffect), "(the Rest is on)");
        const auto server = e.m.server_of.find(id);
        CHECK(server != e.m.server_of.end(), "(the Rest's item names its ServerInstanceId)");
        if (server != e.m.server_of.end()) end_instance(e, server->second);
        e.play(0.3, false);
        CHECK(!e.rt().resting() && !e.m.has_effect(kRestEffect) && e.m.stats[kHealthRegen] == 0.6f && e.m.stats[kManaRegen] == 0.6f,
              "ServerEndAbility stands the character up: the Rest ends, the montage jumps to its end and the regeneration effect goes");
        CHECK(!e.m.end_before_removal && !e.h.s->abilities_failed() && e.m.unreadable == 0, "(nothing failed)");
    }
    // the Sprint: the same server id, and the client is not told again
    {
        Run s;
        s.next_id = 7;
        s.play(0.5, true);
        const int32_t id = s.trigger(kSprint);
        s.play(0.5, true);
        CHECK(s.rt().sprinting() && s.m.has_ability(kSprint), "(the Sprint runs)");
        const auto server = s.m.server_of.find(id);
        CHECK(server != s.m.server_of.end(), "(the Sprint's item names its ServerInstanceId)");
        if (server != s.m.server_of.end()) end_instance(s, server->second);
        s.play(0.3, true);
        CHECK(!s.rt().sprinting() && !s.m.has_ability(kSprint) && !s.m.has_effect(kSprintEffect) && s.m.stats[kMoveSpeedMult] == 1.0f,
              "ServerEndAbility with the server's instance id ends the Sprint and its effect");
        CHECK(s.m.end_ids().empty(), "... and the client is not told again: it ended it");
    }
}
AOC_REGISTER_TEST(test_rest_and_sprint_end_by_server_instance);

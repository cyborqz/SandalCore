// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/gather_session_sit_tests.inc: a /sit during a harvest ends the channel, as an ordinary ability trigger does
//  (registered with AOC_REGISTER_TEST: test_worldd_gather_session.cpp runs it through aoc_tests::run; it is included from gather_session_registered.inc)
// ============================================================================

/// The client's ServerPlayEmote(EmoteName): a call of the PlayerController.
static BunchWriter gather_emote(const std::string& word) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerPlayEmote");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "EmoteName", repl::Value::str(word))) return BunchWriter();
    return actor_rpc(pc, "ServerPlayEmote", params);
}

static void test_a_sit_ends_a_gather_channel() {
    std::printf("a /sit during a harvest ends the channel, then the character sits down\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 61);
    Player& p = rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
    play(p);
    p.send_now(interact(rig.component_of(node)), p.pawn_ch);
    CHECK(rig.board().state_of(node) == gather::NodeState::Channeling && p.s->harvesting_node() == node, "the harvest channel runs");
    const size_t cancelled = p.s->harvests_cancelled();
    p.send_now(gather_emote("sit"), p.pc_ch);
    rig.run(milliseconds(300));
    CHECK(rig.board().state_of(node) == gather::NodeState::Available && p.s->harvesting_node() == 0 && p.s->harvests_cancelled() == cancelled + 1,
          "the /sit ends the channel: the node is Available and the harvest is counted as cancelled");
    CHECK(p.s->abilities() && p.s->abilities()->resting(), "the sit then starts (the data has no rule against it: out of combat, on foot, alive)");
    CHECK(!p.s->abilities_failed() && !p.s->failed(), "(the session is fine)");
}

AOC_REGISTER_TEST(test_a_sit_ends_a_gather_channel);

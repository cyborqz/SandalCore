// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_budgets_tests.inc
//
//  Registered tests of test_worldd_session: the budgets of the client's requests and the log lines of what the client causes.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

// ============================================================================================================================
//  budgets and the log lines the client can cause
// ============================================================================================================================

static void test_request_budget() {
    std::printf("the request budgets\n");
    RequestBudget b(4, seconds(2));
    int taken = 0;
    for (int i = 0; i < 10; ++i) taken += b.take(t0) ? 1 : 0;
    CHECK(taken == 4, "a fresh budget of 4 gives 4 at once and no more");
    CHECK(!b.take(t0 + seconds(1)) && b.take(t0 + seconds(2)) && !b.take(t0 + seconds(2)), "one more after the interval, not before");
    CHECK(!b.take(t0 + seconds(3) + milliseconds(999)) && b.take(t0 + seconds(4)), "the remainder of an interval carries over");
    taken = 0;
    for (int i = 0; i < 10; ++i) taken += b.take(t0 + seconds(100)) ? 1 : 0;
    CHECK(taken == 4, "a long quiet period refills to the burst, not beyond");

    Harness h;
    h.join();
    h.settle();
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::ClassSchema& pawn_cls = class_named("PlayerPawn_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
    const BunchWriter load = client_rpc(pc_cls, "ServerRequestClientLoad");
    const size_t bytes_before = h.bytes, chains_before = h.got.size();
    g_log.str("");
    // 1,000 loads within one simulated second, as few packets as they fit, each answered as far as the budget goes
    const size_t packets = h.client_repeats(pc_ch, load, 1000);
    h.settle();
    const size_t sent = h.bytes - bytes_before;
    const size_t loads = h.on_channel(pc_ch, chains_before).size() / 6;
    std::printf("    1,000 loads in %zu packets within a second: %zu answered, %zu bytes sent, %zu requests dropped\n", packets, loads, sent,
                h.s->requests_dropped());
    CHECK(!h.s->failed() && !h.conn.failed(), "(the session and the connection took the flood)");
    CHECK(loads == PlayerSession::kLoadBurst && h.s->requests_dropped() == 1000 - PlayerSession::kLoadBurst,
          "the burst's worth are answered, the rest dropped");
    CHECK(sent < 300 * 1024, "under a few hundred KB went out for the 1,000 requests");
    spdlog::default_logger()->flush();
    CHECK(count_lines(g_log.str(), "faster than its budget") == 1, "the first drop is logged, once");
    // after a quiet period a request is served again
    h.t += seconds(5);
    size_t at = h.got.size();
    h.client_sends({h.client.actor(pc_ch, load, true)});
    h.settle();
    CHECK(h.on_channel(pc_ch, at).size() == 6, "a load after a quiet period is answered");

    // the storage payloads have their own, larger budget
    repl::RpcParams params;
    repl::set_rpc_param(*pawn_cls.rpc("ServerRequestStorageData"), params, "StorageContainer", repl::Value(h.s->bootstrap().setup().guids.resource_storage[0]));
    const BunchWriter storage = client_rpc(pawn_cls, "ServerRequestStorageData", params);
    at = h.got.size();
    const size_t dropped_before = h.s->requests_dropped();
    h.client_repeats(pawn_ch, storage, 40);
    h.settle();
    CHECK(h.on_channel(pawn_ch, at).size() == PlayerSession::kStorageBurst && h.s->requests_dropped() == dropped_before + 40 - PlayerSession::kStorageBurst,
          "40 storage requests within a second: the storage burst's worth are answered");
    h.t += PlayerSession::kStorageInterval * 3;
    at = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, storage, true)});
    h.client_sends({h.client.actor(pawn_ch, storage, true)});
    h.client_sends({h.client.actor(pawn_ch, storage, true)});
    h.client_sends({h.client.actor(pawn_ch, storage, true)});
    h.settle();
    CHECK(h.on_channel(pawn_ch, at).size() == 3, "three intervals later: three more, not four");
}
// [C O A x C-Y-B]

AOC_REGISTER_TEST(test_request_budget);

static void test_client_driven_log_lines() {
    std::printf("the log lines the client can cause\n");
    Harness h;
    h.join();
    h.settle();
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    repl::RpcParams mine;
    repl::set_rpc_param(*pc_cls.rpc("ServerAcknowledgePossession"), mine, "P", repl::Value(h.s->bootstrap().setup().guids.pawn));
    const BunchWriter ack = client_rpc(pc_cls, "ServerAcknowledgePossession", mine);
    g_log.str("");
    h.client_repeats(pc_ch, ack, 1000);
    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    CHECK(count_lines(log, "acknowledged possession") == PlayerSession::kLoggedPerKind,
          "1,000 possession acknowledgements: 'acknowledged possession' is logged kLoggedPerKind (3) times, the rest at debug in other words");
    CHECK(count_lines(log, "possession ack #") == 1000 - PlayerSession::kLoggedPerKind, "(the debug lines count the rest)");
    // the reply chains' "sent" lines: the first three at info, the rest at debug (same words, so all are in this debug log)
    const BunchWriter load = client_rpc(pc_cls, "ServerRequestClientLoad");
    g_log.str("");
    h.client_sends({h.client.actor(pc_ch, load, true)});
    h.settle();
    spdlog::default_logger()->flush();
    const std::string lines = g_log.str();
    CHECK(count_lines(lines, "[info] [session] cyb: sent ") == PlayerSession::kLoggedPerKind && count_lines(lines, "[debug] [session] cyb: sent ") == 3,
          "a load's six reply chains: three 'sent' lines at info, three at debug");
}

AOC_REGISTER_TEST(test_client_driven_log_lines);

// The combat settings' budget is asked before anything is read or built from the request: a request over the budget is dropped and counted whatever it carries, also one with no
// value in it (which used to be read first and was never counted).
static void test_combat_settings_budget_comes_first() {
    std::printf("the combat settings' budget is asked before the request is read\n");
    Harness h;
    h.join();
    h.settle();
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const BunchWriter empty = client_rpc(pc_cls, "ServerUpdateAccountCombatSettings");
    CHECK(empty.bit_pos() > 0, "(a combat settings call with no value is written)");
    if (empty.bit_pos() == 0) return;
    const size_t dropped = h.s->requests_dropped();
    h.client_repeats(pc_ch, empty, 200);
    h.settle();
    char msg[200];
    std::snprintf(msg, sizeof msg, "200 settings requests within a second: all but the first few are dropped at the budget (%zu dropped)", h.s->requests_dropped() - dropped);
    CHECK(h.s->requests_dropped() >= dropped + 190, msg);
}

AOC_REGISTER_TEST(test_combat_settings_budget_comes_first);

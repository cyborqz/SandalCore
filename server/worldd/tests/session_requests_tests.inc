// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_requests_tests.inc
//
//  Registered tests of test_worldd_session: the client's requests and the window: the possession bunch and the requests, a full window, the phase-in and the echo that wait, lost and resent exports.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

static void test_possession_bunch_and_requests() {
    std::printf("the possession bunch, storage requests, the unknown and the malformed\n");
    Harness h;
    h.join();
    h.settle();
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const schema::ClassSchema& pawn_cls = class_named("PlayerPawn_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);

    const size_t before = h.got.size();
    const BunchWriter possession = hex_bits(capture_blocks::kClientBunchPossession);
    h.client_sends({h.client.actor(pc_ch, possession, true)});
    h.settle();
    const auto pc = h.on_channel(pc_ch, before);
    CHECK(pc.size() == 7, "the retail client's possession bunch (72 RPCs): seven chains back: the combat-settings echo and the load's six");
    if (pc.size() != 7) return;
    std::vector<std::string> names;
    for (const auto* d : pc) {
        const auto n = rpc_names(*d, pc_cls);
        names.push_back(n.empty() ? "(property block)" : n[0]);
    }
    CHECK(names[0] == "<not an rpc>" || names[0] == "(property block)" || names[0] == "<unreadable>", "the first is a property block, not an RPC: the echo");
    const std::vector<std::string> rest = {"ClientReceiveStoragePayload", "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
                                           "ClientReceiveStoragePayload", "ClientUpdateGroup", "ClientReceiveActionbarSlotPayload"};
    CHECK(std::vector<std::string>(names.begin() + 1, names.end()) == rest, "then the four storage payloads, the group and the action bar");
// ~ C-O-A & C.Y.B ~

    // the echo: all 50 values of the request as the client's own; the nine of pkt#620 among them: eight equal the original's, PartyVFX is the client's
    const auto blocks = repl::read_content_blocks(pc[0]->content.data(), pc[0]->content.bit_pos(), 0);
    std::optional<repl::RepState> echo;
    if (blocks && blocks->size() == 1) {
        size_t p = (*blocks)[0].payload_pos;
        echo = repl::read_rep_block(pc_cls, pc[0]->content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, p);
    }
    const BunchWriter original = hex_bits(capture_bootstrap::kCombatEcho);
    const auto ob = repl::read_content_blocks(original.data(), original.bit_pos(), 0);
    std::optional<repl::RepState> orig;
    if (ob && ob->size() == 1) {
        size_t p = (*ob)[0].payload_pos;
        orig = repl::read_rep_block(pc_cls, original.data(), (*ob)[0].payload_pos + (*ob)[0].payload_bits, p);
    }
    CHECK(echo && orig && echo->values().size() == 50 && pc[0]->reliable && pc[0]->exports.empty(), "the echo is one reliable PlayerController block of the request's 50 values");
    if (echo && orig) {
        // the nine values the original's reply (pkt#620) carries
        constexpr std::array<std::string_view, 9> k_pkt620 = {
            "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MayHarmAngle",
            "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ProximityRadius",
            "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MaxDistance",
            "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ForwardOffset",
            "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.TabTargetMethod",
            "CharacterInGameSettings.CombatSettings.Targeting.bEnableHoverCasting",
            "CharacterInGameSettings.CombatSettings.Targeting.SoftLock.VisualsMask",
            "CharacterInGameSettings.CombatSettings.Input.BufferDuration",
            "CharacterInGameSettings.CombatSettings.Rendering.PartyVFX",
        };
        int equal = 0, party_vfx_differs = 0;
        for (const std::string_view path : k_pkt620) {
            const repl::Value* mine = echo->get(pc_cls, path);
            const repl::Value* theirs = orig->get(pc_cls, path);
            if (mine && theirs && *mine == *theirs) ++equal;
            else if (path.find("PartyVFX") != std::string_view::npos && mine && theirs) ++party_vfx_differs;
        }
        CHECK(equal == 8 && party_vfx_differs == 1,
              "eight of the nine values equal the original's (pkt#620); PartyVFX is the client's own (0 in this client's request, 1 in the original's reply)");
    }
    const auto parsed = repl::read_client_bunch(pc_cls, nullptr, possession.data(), possession.bit_pos());
    std::map<std::string, int> seen;
    if (parsed) for (const auto& r : parsed->rpcs) ++seen[r.call.rpc->name];
    CHECK(parsed && parsed->rpcs.size() == 72 && seen["ServerUpdateAccountCombatSettings"] == 1 && seen["ServerAcknowledgePossession"] == 1 &&
              seen["ServerRequestClientLoad"] == 1 && seen["NotifyAntiCheatState"] == 1 && seen["ServerRequestNodeSummary"] == 34 &&
              seen["ServerRequestCurrentSiegeSummary"] == 34,
          "(the request has 72 RPCs, as the capture says; worldd answers two of them, the node and siege summaries and the client-state notice are ignored)");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("acknowledged possession of another object, not of its pawn") != std::string::npos,
          "the captured acknowledgement names the original session's pawn, not this one's: logged as such");
    repl::RpcParams mine;
    repl::set_rpc_param(*pc_cls.rpc("ServerAcknowledgePossession"), mine, "P", repl::Value(h.s->bootstrap().setup().guids.pawn));
    h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerAcknowledgePossession", mine), true)});
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("the client acknowledged possession of its pawn") != std::string::npos, "the acknowledgement of this session's pawn is logged");

    // ServerRequestStorageData for the resource storage: the pawn's payload (pkt#773); for an unknown container: nothing
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const size_t b2 = h.got.size();
    repl::RpcParams params;
    repl::set_rpc_param(*pawn_cls.rpc("ServerRequestStorageData"), params, "StorageContainer", repl::Value(g.resource_storage[0]));
    h.client_sends({h.client.actor(pawn_ch, client_rpc(pawn_cls, "ServerRequestStorageData", params), true)});
    h.settle();
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    const auto want = pb->storage_payload(bs::StorageKind::Resource0, bs::Role::Pawn);
    const auto reply = h.on_channel(pawn_ch, b2);
    CHECK(reply.size() == 1 && want && same_bits(reply[0]->content, want->emission.payload) && reply[0]->exports.empty(),
          "ServerRequestStorageData(resource storage): the pawn's empty resource payload (pkt#773), no export");
    repl::RpcParams other;
    repl::set_rpc_param(*pawn_cls.rpc("ServerRequestStorageData"), other, "StorageContainer", repl::Value(guid(0x7777777)));
    const size_t b3 = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, client_rpc(pawn_cls, "ServerRequestStorageData", other), true)});
    h.settle();
    CHECK(h.got.size() == b3, "a container worldd does not know gets no reply");

    // a target no actor of the world holds (it changes nothing, so nothing is sent), one on an unknown channel, and bits that are no bunch
    const size_t b4 = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientSetActiveTarget), true)});
    h.client_sends({h.client.actor(40, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    BunchWriter garbage;
    for (int i = 0; i < 12; ++i) garbage.write_bits(0xFFFFFFFFu, 32);
    h.client_sends({h.client.actor(pc_ch, garbage, true)});
    BunchWriter truncated;
    truncated.write_bit_range(possession.data(), 0, 300);   // cut in the middle of a block
    h.client_sends({h.client.actor(pc_ch, truncated, true)});
    h.settle();
    CHECK(h.got.size() == b4 && !h.s->failed(), "ServerSetActiveTarget of an object that is no actor, a bunch on a channel that is not the player's, garbage and a cut bunch: nothing sent");
    h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    h.settle();
    CHECK(h.got.size() == b4 + 6, "and the session still answers afterwards");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("does not read: ignored") != std::string::npos, "the malformed bunches are logged");
}
// [C_Y_B ~ CoA]

AOC_REGISTER_TEST(test_possession_bunch_and_requests);

static void test_join_waits_for_the_window() {
    std::printf("a join that does not fit the window\n");
    Harness h;
    h.client_receives = false;
    clog_channel(h, 3, 248);   // 8 bunches of room on channel 3: the PlayerController's chains need more than that in all
    CHECK(h.conn.unacked_reliable(3) == 248, "(248 reliable bunches in flight on channel 3)");
    CHECK(h.join() && !h.s->failed(), "the session joins");
    const size_t pending = h.s->join_pending();
    CHECK(pending > 0 && pending < 15, "part of the join is on the wire and the rest waits for the window: a chain stays at the head of the queue");
    std::printf("    %zu of the 16 chains were waiting\n", pending);
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    // the client asks for its load while the join is incomplete: the replies come after the join, never before
    h.client_sends({h.client.actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    CHECK(h.s->replies_pending() == 6 && h.s->join_pending() == pending, "the request's six reply steps wait behind the join");
    h.s->tick(h.conn);
    CHECK(h.s->join_pending() == pending, "a tick with the window still full changes nothing");
    h.flush();

    h.client_receives = true;
    h.give_held();
    h.settle();
    CHECK(h.s->join_pending() == 0 && h.s->replies_pending() == 0 && !h.s->failed(), "once the client acknowledges, the join completes and then the replies");
    std::vector<fake::Delivered> mine;   // without the filler's one-byte bunches
    for (const auto& d : h.got)
        if (d.content.bit_pos() != 8) mine.push_back(d);
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    auto seq = pb->join_sequence();
    CHECK(seq && mine.size() == 16 + 6 && compare_sends(mine, 0, *seq, kChannels).empty(),
          "the client has the join's 16 chains in the bootstrap's order, equal to it, and then the six replies");
    bool replies_last = mine.size() == 22;
    for (size_t i = 16; replies_last && i < 22; ++i) replies_last = mine[i].channel == 3 && !rpc_names(mine[i], pc_cls).empty();
    CHECK(replies_last, "the six replies are the last six, on the PlayerController's channel");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("the window of channel 3 is full") != std::string::npos, "the wait is logged (once)");
}

AOC_REGISTER_TEST(test_join_waits_for_the_window);

static void test_window_full() {
    std::printf("a client that stops acknowledging\n");
    Harness h;
    h.client_receives = false;
    CHECK(h.join(), "join");
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    // The client asks for its load over and over and acknowledges nothing: it never sees a packet.  The requests come
    // kLoadInterval apart, what the load's budget allows: the window, not the budget, is what this test fills.
    size_t most_unacked = h.conn.unacked_reliable(pc_ch);
    for (int i = 0; i < 40; ++i) {
        h.t += PlayerSession::kLoadInterval;
        h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
        h.s->tick(h.conn);
        h.flush();
        most_unacked = std::max(most_unacked, h.conn.unacked_reliable(pc_ch));
    }
    CHECK(most_unacked <= Connection::kMaxUnackedReliable, "the channel never holds more than 256 unacknowledged reliable bunches");
    CHECK(h.s->replies_pending() > 0 && !h.s->failed(), "replies wait for the window (they are not dropped, and the session does not fail)");
    CHECK(h.s->replies_pending() <= PlayerSession::kMaxPendingReplies, "and at most kMaxPendingReplies steps wait: the rest of the requests are ignored");
    const size_t waiting = h.s->replies_pending();
    std::printf("    %zu reply steps were waiting, %zu bunches unacknowledged at most\n", waiting, most_unacked);
    CHECK(h.s->requests_dropped() == 0, "no request was over its budget: the waiting is the window's doing");

    // now the client receives everything and acknowledges
    h.client_receives = true;
    h.give_held();
    h.settle();
    CHECK(h.s->replies_pending() == 0 && h.s->join_pending() == 0, "once it acknowledges, every waiting step goes out");
    const auto pc = h.on_channel(pc_ch);
    CHECK(pc.size() >= 8 + 6 * 10 && (pc.size() - 8) % 6 == 0, "the PlayerController's chains are the join's eight and whole loads of six after them");
    if (pc.size() < 8) return;
    const std::vector<std::string> cycle = {"ClientReceiveStoragePayload", "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
                                            "ClientReceiveStoragePayload", "ClientUpdateGroup", "ClientReceiveActionbarSlotPayload"};
    bool in_order = true;
    for (size_t i = 8; i < pc.size(); ++i) {
        const auto n = rpc_names(*pc[i], pc_cls);
        in_order = in_order && n.size() == 1 && n[0] == cycle[(i - 8) % 6];
    }
    CHECK(in_order, "every load arrived whole and in order: storage x4, group, action bar, again and again");
    std::printf("    %zu loads delivered of 40 asked\n", (pc.size() - 8) / 6);
    // the connection's reliable sequence on the channel is the client's view: the join's chains still equal the bootstrap's
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    auto seq = pb->join_sequence();
    std::vector<fake::Delivered> first_16(h.got.begin(), h.got.begin() + std::min<size_t>(16, h.got.size()));
    CHECK(seq && compare_sends(first_16, 0, *seq, kChannels).empty(), "the join itself was delivered in order, first, on its four channels");
}

AOC_REGISTER_TEST(test_window_full);

// ============================================================================================================================
//  replies that wait for the window arrive once
// ============================================================================================================================

static void test_phase_in_waits() {
    std::printf("the phase-in waits for the pawn channel's window\n");
    Harness h;
    h.join();
    h.settle();
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
    // Fill the pawn channel's window with raw reliable bunches the client will receive but not acknowledge yet.
    h.client_receives = false;
    const size_t room = Connection::kMaxUnackedReliable - h.conn.unacked_reliable(pawn_ch);
    clog_channel(h, pawn_ch, room);
    CHECK(h.conn.unacked_reliable(pawn_ch) == Connection::kMaxUnackedReliable, "(the pawn channel's window is full)");
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.s->tick(h.conn);
    h.flush();
    CHECK(h.s->replies_pending() == 1, "the phase-in waits");
    h.client_receives = true;
    h.give_held();
    h.settle();
    // the filler's bunches are 8 bits; the phase-in is the one that is not
    size_t real = 0;
    for (const auto* d : h.on_channel(pawn_ch, before)) if (d->content.bit_pos() != 8) ++real;
    CHECK(real == 1, "after the acknowledgement the phase-in update arrives, exactly once (the checkpoint was restored while it waited)");
    CHECK(h.s->replies_pending() == 0, "nothing pending");
}
// [CoA x C-Y-B]

AOC_REGISTER_TEST(test_phase_in_waits);

static void test_echo_waits() {
    std::printf("the combat-settings echo waits for the PlayerController channel's window\n");
    Harness h;
    h.join();
    h.settle();
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    h.client_receives = false;
    const size_t room = Connection::kMaxUnackedReliable - h.conn.unacked_reliable(pc_ch);
    clog_channel(h, pc_ch, room);
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, hex_bits(capture_blocks::kClientBunchPossession), true)});
    h.s->tick(h.conn);
    h.flush();
    CHECK(h.s->replies_pending() == 7, "the echo and the load's six wait");
    h.client_receives = true;
    h.give_held();
    h.settle();
    size_t real = 0;
    for (const auto* d : h.on_channel(pc_ch, before)) if (d->content.bit_pos() != 8) ++real;
    CHECK(real == 7, "after the acknowledgement: the echo and the six replies, each once (the PlayerController shadow was restored too)");
}

AOC_REGISTER_TEST(test_echo_waits);

static void test_lost_export() {
    std::printf("a lost packet's exports are not marked exported\n");
    Harness h;
    const size_t dropped = join_dropping_pawn_exports(h);
    CHECK(dropped > 0, "(the packets with the pawn's export bunches were dropped)");
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    // The client's packet acknowledges everything but the dropped ones and asks for its load at once.
    h.t += milliseconds(20);
    h.client_sends({});   // the acknowledgement alone: the server learns of the loss (events taken)
    h.t += milliseconds(20);
    h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    h.s->tick(h.conn);
    // do not deliver the resend yet: read the reply's chains straight from the connection
    std::vector<fake::Delivered> pc_chains;
    for (const auto& pkt : h.conn.flush(h.t)) {
        auto r = h.client.receive(pkt);
        for (const auto& d : r.delivered) if (d.channel == pc_ch) pc_chains.push_back(d);
    }
    size_t with_export = 0;
    const size_t big = big_payloads(pc_chains, pc_cls, &with_export);
    CHECK(big == 3 && with_export == 3, "the Backpack, Equipment and QuestStorage payloads still carry their exports: the pawn's export packet was lost");
}
// CoA : C.Y.B

AOC_REGISTER_TEST(test_lost_export);

static void test_lost_then_resent() {
    std::printf("a lost export packet is resent and delivered: later replies leave its exports out\n");
    Harness h;
    join_dropping_pawn_exports(h);
    const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    h.client_receives = true;
    h.settle();   // the client acknowledges (with the hole), the server resends, the client acknowledges the resend
    CHECK(h.s->exports_tracked() == 0, "once the resend is acknowledged nothing is tracked");
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    h.settle();
    std::vector<fake::Delivered> chains;
    for (const auto* d : h.on_channel(pc_ch, before)) chains.push_back(*d);
    size_t with_export = 0;
    const size_t big = big_payloads(chains, pc_cls, &with_export);
    CHECK(big == 3 && with_export == 0, "once the resent packet is acknowledged its exports are marked: the replies carry none");
}

AOC_REGISTER_TEST(test_lost_then_resent);

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/session_join_tests.inc
//
//  Registered tests of test_worldd_session: the join: through the endpoint, refused setups, the channels, the acknowledgements, the phase-in and the first moves.
//  (included by the test program through its registered list; every test registers itself, see tests/README.md)
// ============================================================================

static void test_join_through_endpoint() {
    std::printf("a join through the endpoint\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    g_log.str("");
    Joined j = handshake(rig, kA, t0);
    log_in(rig, j, kA, cyb_record(), t0);
    const Clock::time_point t_join = t0 + milliseconds(40);
    auto got = exchange(rig, j, kA, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)}, t_join);
    CHECK(!find_message(got, conn::Nmt::Failure) && rig.ep.session_count() == 1, "Netspeed and Join are accepted");

    auto want_pb = expected_bootstrap(*fx.content, cyb_record(), world_time_at(t_join), bs::GameStateMode::Tier2);
    const bs::PlayerSetup setup = want_pb->setup();
    auto seq = want_pb->join_sequence();
    CHECK(seq && seq->size() == 16, "the bootstrap's join sequence has 16 chains");
    if (!seq) return;
    CHECK(j.got.size() == 16, "the client receives 16 chains after Join");
    const std::string diff = compare_sends(j.got, 0, *seq, kChannels);
    CHECK(diff.empty(), "they are the bootstrap's sequence, bit for bit and in order, on channels 3, 4, 5, 6");
    if (!diff.empty()) std::printf("    %s\n", diff.c_str());
    const std::array<uint32_t, 15> order = {3, 3, 3, 3, 4, 3, 5, 5, 5, 5, 5, 3, 3, 5, 6};
    bool channels_ok = j.got.size() == 16;
    for (size_t i = 0; channels_ok && i < order.size(); ++i) channels_ok = j.got[i].channel == order[i];
    CHECK(channels_ok, "the order across channels: PlayerController x4, GameState, PlayerController, pawn x5, PlayerController x2, pawn, PlayerState");
    CHECK(j.got.size() == 16 && j.got[0].open && j.got[4].open && j.got[6].open && j.got[14].open &&
              !j.got[1].open && !j.got[5].open && !j.got[13].open,
          "the four actors' first chains open their channels and no other does");

    // what the client can read in them: the right character, the login's identity, minted GUIDs
    if (j.got.size() < 15) return;
    const NewActor pc = read_new_actor(j.got[0].content, true), gs = read_new_actor(j.got[4].content, false),
                   pawn = read_new_actor(j.got[6].content, false), ps = read_new_actor(j.got[14].content, false);
    CHECK(pc.ok && gs.ok && pawn.ok && ps.ok, "the four opens' SerializeNewActor read");
    CHECK(gs.actor == guid(0x1000000) && ps.actor == guid(0x1000002) && pc.actor == guid(0x1000004) && pawn.actor == guid(0x100000a),
          "GUIDs are minted under worldd's ServerId: the GameState's first, then the PlayerState, PlayerController and pawn (even ObjectIds; GlobalGMCommands and the tutorial component come between the controller and the pawn)");
    CHECK(pc.archetype.ObjectId == bs::pc_archetype_export().front().guid.ObjectId &&
              gs.archetype.ObjectId == bs::game_state_archetype_export().front().guid.ObjectId &&
              pawn.archetype.ObjectId == bs::pawn_archetype_export().front().guid.ObjectId &&
              ps.archetype.ObjectId == bs::player_state_archetype_export().front().guid.ObjectId,
          "each open names its class: AoCPlayerControllerBP_C, AoCGameStateBP_C, PlayerPawn_C, AoCPlayerStateBP_C");
    const schema::ClassSchema& base_cls = *gen::find_component_schema("BaseCharacterInfo");
    const auto base = block_of(j.got[6], pawn.end, false, setup.guids.base_character_info, base_cls);
    const repl::Value* name = base ? base->get(base_cls, "CharacterName") : nullptr;
    const repl::Value* a = base ? base->get(base_cls, "CharacterGuid.A") : nullptr;
    const repl::Value* d4 = base ? base->get(base_cls, "CharacterGuid.D") : nullptr;
    CHECK(name && *name == repl::Value::str("cyb") && a && *a == repl::Value::i64(static_cast<int32_t>(0xbbec5c86u)) && d4 &&
              *d4 == repl::Value::i64(static_cast<int32_t>(0xfa22e7d0u)),
          "the pawn's BaseCharacterInfo: CharacterName 'cyb' and the record's Guid bbec5c86 ... fa22e7d0");
    const schema::ClassSchema& ps_cls = class_named("AoCPlayerStateBP_C");
    const auto ps_state = block_of(j.got[14], ps.end, true, {}, ps_cls);
    const repl::Value* uid = ps_state ? ps_state->get(ps_cls, "UniqueId") : nullptr;
    const repl::Value* pname = ps_state ? ps_state->get(ps_cls, "PlayerNamePrivate") : nullptr;
    const auto* u = uid ? uid->get_if<repl::UniqueNetId>() : nullptr;
    CHECK(u && u->flags == 8 && u->id == kMachine && pname && *pname == repl::Value::str(kMachine.substr(0, 20)),
          "the PlayerState: the login's unique id (flags 8) and the first 20 characters of its ?Name=, not the character's name");
    const repl::Value* pid = ps_state ? ps_state->get(ps_cls, "PlayerId") : nullptr;
    CHECK(pid && *pid == repl::Value::i64(1), "PlayerId 1, the first player");

    // acknowledgements: the client's next packet acks all of it; a later reply names the Backpack without its export
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const size_t before = j.got.size();
    got = exchange(rig, j, kA, {}, t_join + milliseconds(100));   // the ack-only packet
    got = exchange(rig, j, kA, {j.client->actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)}, t_join + milliseconds(200));
    CHECK(j.got.size() == before + 6, "ServerRequestClientLoad is answered with six chains on the PlayerController's channel");
    if (j.got.size() == before + 6) {
        const auto names = rpc_names(j.got[before], pc_cls);
        CHECK(j.got[before].channel == 3 && names.size() == 1 && names[0] == "ClientReceiveStoragePayload" && j.got[before].exports.empty(),
              "the Backpack payload: the component's export was acknowledged with the join, so it goes without its path");
    }

    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    CHECK(log.find("[session] cyb: joining as ServerId 60, PlayerId 1") != std::string::npos &&
              log.find("sent PlayerController open on channel 3 (2 parts)") != std::string::npos &&
              log.find("sent GameState open (tier 2) on channel 4") != std::string::npos &&
              log.find("sent PlayerState open on channel 6") != std::string::npos,
          "the log has the join and each step sent: name, channel, parts");
    CHECK(log.find("default spawn") != std::string::npos, "and the setup's notes (the default spawn)");
    CHECK(log.find(kTicket) == std::string::npos, "the ticket is not in the log");
}

AOC_REGISTER_TEST(test_join_through_endpoint);

static void test_setup_refused_at_join() {
    std::printf("a character the bootstrap refuses\n");
    Fixture fx;
    EndpointConfig cfg;
    cfg.players = fx.world.get();
    Rig rig(cfg);
    g_log.str("");
    Joined j = handshake(rig, kA, t0);
    svc::Character rec = cyb_record();
    rec.race_id = 8;   // Tulnar: no player appearance in the design data, never created (character_setup.h): the setup is refused
    log_in(rig, j, kA, rec, t0);
    auto got = exchange(rig, j, kA, {j.client->nmt(conn::Nmt::Join)}, t0 + milliseconds(40));
    const conn::ControlMessage* failure = find_message(got, conn::Nmt::Failure);
    CHECK(failure && failure->error == login_text::kRaceUnsupported && closed_by_server(got),
          "Join of a Tulnar (a race that is never created): NMT_Failure with the race's own text and the channel closes");
    CHECK(j.got.empty(), "nothing was spawned");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("cannot join the world") != std::string::npos && g_log.str().find("race") != std::string::npos,
          "the log has the reason: the race id");

    // control: a setup refused for any other reason keeps the generic text
    Rig other(cfg);
    Joined k = handshake(other, kB, t0);
    svc::Character broken = cyb_record();
    broken.appearance_json = "[";
    log_in(other, k, kB, broken, t0);
    got = exchange(other, k, kB, {k.client->nmt(conn::Nmt::Join)}, t0 + milliseconds(40));
    failure = find_message(got, conn::Nmt::Failure);
    CHECK(failure && failure->error == login_text::kLoginFailed && closed_by_server(got), "(control: broken appearance JSON is refused with the generic 'Login failed')");
}

AOC_REGISTER_TEST(test_setup_refused_at_join);

static void test_channels() {
    std::printf("actor channels\n");
    Harness h;
    CHECK(h.join(), "a session joins a fresh connection");
    CHECK(h.s->channel_of(bs::Role::PlayerController) == 3 && h.s->channel_of(bs::Role::GameState) == 4 &&
              h.s->channel_of(bs::Role::Pawn) == 5 && h.s->channel_of(bs::Role::PlayerState) == 6,
          "it takes the lowest free indexes from 3: 3, 4, 5, 6");

    // channels 3 and 5 open, 4 closing (its close not acknowledged), and 6 free: the session skips them
    Harness busy;
    Chain open;
    open.open = true;
    open.payload.write_bits(1, 8);
    for (uint32_t ch : {3u, 4u, 5u}) {
        open.channel = ch;
        busy.conn.send_chain(open);
    }
    busy.conn.close_channel(4);
    CHECK(busy.conn.channel_open(3) && busy.conn.channel_closing(4) && busy.conn.channel_open(5), "(channels 3 and 5 are open, 4 is closing)");
    CHECK(busy.join(), "a session joins a connection that has actor channels");
    CHECK(busy.s->channel_of(bs::Role::PlayerController) == 6 && busy.s->channel_of(bs::Role::GameState) == 7 &&
              busy.s->channel_of(bs::Role::Pawn) == 8 && busy.s->channel_of(bs::Role::PlayerState) == 9,
          "it skips the open and the closing ones: 6, 7, 8, 9");
    busy.settle();
    CHECK(busy.on_channel(6).size() == 8 && busy.on_channel(7).size() == 1 && busy.on_channel(8).size() == 6 && busy.on_channel(9).size() == 1,
          "and sends on them: eight chains on the PlayerController's (the XPTracker's last), one on the GameState's, six on the pawn's, one on the PlayerState's");
    bool filler = false;
    for (const auto& d : busy.got) filler = filler || d.channel < 6;
    CHECK(filler, "(the filler's own chains on 3, 4 and 5 reached the client as well)");
}

AOC_REGISTER_TEST(test_channels);

static void test_join_and_acks() {
    std::printf("the join and its acknowledgements\n");
    Harness h;
    CHECK(h.join() && !h.s->failed(), "join");
    CHECK(h.s->join_pending() == 0, "the whole join went to the connection at once (the windows had room)");
    h.step();
    const size_t join_packets = h.packets, join_bytes = h.bytes;
    h.settle();
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    auto seq = pb->join_sequence();
    CHECK(seq && h.got.size() == 16 && compare_sends(h.got, 0, *seq, kChannels).empty(), "16 chains, the bootstrap's sequence in order");
    CHECK(!h.s->failed() && h.s->replies_pending() == 0, "nothing pending, nothing failed");
    CHECK(h.s->exports_tracked() == 0, "every export part of the join had its final ack event: no chain is tracked any more");
    std::printf("    the join of cyb (Tier 2) is %zu packets, %zu bytes (the original's: 64 packets, 51 KB in its first 174 ms)\n", join_packets, join_bytes);
// C.O.A + C.Y.B

    // the client acknowledged every packet: the PlayerController's replies leave out the exports of what it has
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    const size_t before = h.got.size();
    h.client_sends({h.client.actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    h.settle();
    const auto pc = h.on_channel(3, before);
    CHECK(pc.size() == 6, "ServerRequestClientLoad: six chains");
    std::vector<std::string> names;
    for (const auto* d : pc) names.push_back(rpc_names(*d, pc_cls).empty() ? "" : rpc_names(*d, pc_cls)[0]);
    const std::vector<std::string> expect = {"ClientReceiveStoragePayload", "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
                                             "ClientReceiveStoragePayload", "ClientUpdateGroup", "ClientReceiveActionbarSlotPayload"};
    CHECK(names == expect, "four storage payloads (Backpack, Equipment, QuestStorage, the resource storage), the group and the action bar, in that order");
    bool no_exports = true;
    for (const auto* d : pc) no_exports = no_exports && d->exports.empty();
    CHECK(no_exports, "all acknowledged: no export bunch in any of them (the join's acks reached mark_exported)");
    CHECK(h.s->exports_tracked() == 0, "(and the replies, which carried no export, left nothing tracked either)");
    const auto* backpack = pc.size() > 0 ? pc[0] : nullptr;
    repl::ActionbarPayload bar;
    auto pb_acked = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2, 1, &bar);
    auto seq2 = pb_acked->join_sequence();
    if (seq2) for (const bs::Send& snd : *seq2) for (const auto& bunch : snd.emission.exported_by_bunch) pb_acked->mark_exported(bunch);
    const auto want_backpack = pb_acked->storage_payload(bs::StorageKind::Backpack, bs::Role::PlayerController);
    CHECK(backpack && want_backpack && same_bits(backpack->content, want_backpack->emission.payload) && want_backpack->emission.exports.empty(),
          "the Backpack payload equals the bootstrap's after every export of the join is acknowledged");

    // The group is the original's block; the action bar is the character's own (plan D1: the weapon bank and the class's skills, built from
    // the class's talent tree), which is the bootstrap's action_bar() of the setup's bar, not the captured Bard's five slots
    const auto want_bar = pb_acked->action_bar(bar);
    CHECK(pc.size() == 6 && pc[4]->content.bit_pos() == bs::captured::kEmptyGroupBlock.bits, "the group is the original's 527 bits (pkt#606)");
    CHECK(pc.size() == 6 && want_bar && same_bits(pc[5]->content, want_bar->emission.payload), "the action bar is the setup's own bar: the weapon bank and the class's skills");

    // control: a client that has acknowledged nothing is sent the exports
    Harness fresh;
    fresh.client_receives = false;
    fresh.join();
    fresh.step();
    fresh.client_sends({fresh.client.actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    fresh.give_held();
    fresh.client_receives = true;
    fresh.step();
    const auto fpc = fresh.on_channel(3);
    CHECK(!fpc.empty() && want_bar && same_bits(fpc.back()->content, want_bar->emission.payload) && fpc.size() >= 13, "(control: the same request from a client that acknowledged nothing yet)");
    size_t with_export = 0;
    for (size_t i = fpc.size() >= 6 ? fpc.size() - 6 : 0; i < fpc.size(); ++i) with_export += fpc[i]->exports.empty() ? 0 : 1;
    CHECK(with_export == 3, "its Backpack, Equipment and QuestStorage payloads carry the component's export (the resource storage has none)");
}

AOC_REGISTER_TEST(test_join_and_acks);

static void test_phase_in_and_moves() {
    std::printf("phase-in and moves\n");
    Harness h;
    h.join();
    h.settle();
    const schema::ClassSchema& pawn_cls = class_named("PlayerPawn_C");
    const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);

    const size_t before = h.got.size();
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    const auto phase = h.on_channel(pawn_ch, before);
    CHECK(phase.size() == 1 && phase[0]->reliable && phase[0]->exports.empty(), "ServerClientIsReadyToPhaseIn: one reliable update on the pawn's channel, no exports");
    if (phase.size() == 1) {
        const auto blocks = repl::read_content_blocks(phase[0]->content.data(), phase[0]->content.bit_pos(), 0);
        CHECK(blocks && blocks->size() == 2, "the actor's block and one subobject's");
        if (blocks && blocks->size() == 2) {
            const repl::BlockView& actor = (*blocks)[0];
            const repl::BlockView& sub = (*blocks)[1];
            size_t p = actor.payload_pos;
            const auto a = repl::read_rep_block(pawn_cls, phase[0]->content.data(), actor.payload_pos + actor.payload_bits, p);
            const schema::ClassSchema& info = *gen::find_component_schema("BaseCharacterInfo");
            size_t q = sub.payload_pos;
            const auto b = repl::read_rep_block(info, phase[0]->content.data(), sub.payload_pos + sub.payload_bits, q);
            const repl::Value* hidden = a ? a->get(pawn_cls, "bHidden") : nullptr;
            const repl::Value* phased = b ? b->get(info, "bIsPhasedOut") : nullptr;
            const repl::Value* ready = b ? b->get(info, "bClientIsReadyToPhaseIn") : nullptr;
            CHECK(a && b && actor.header.is_actor && sub.header.object == h.s->bootstrap().setup().guids.base_character_info && hidden &&
                      *hidden == repl::Value::boolean(false) && phased && *phased == repl::Value::boolean(false) && ready &&
                      *ready == repl::Value::boolean(true),
                  "bHidden 0, BaseCharacterInfo.bIsPhasedOut 0 and bClientIsReadyToPhaseIn 1 (the original's pkt#2523)");
        }
    }
    h.client_sends({h.client.actor(pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
    h.settle();
    CHECK(h.on_channel(pawn_ch, before).size() == 1, "a second request is not answered again: once");

    // moves: three of the capture's, and the largest
    const std::array<const capture_bootstrap::Bits*, 4> moves = {&capture_bootstrap::kClientMovePacked1, &capture_bootstrap::kClientMovePacked2,
                                                                 &capture_bootstrap::kClientMovePacked3, &capture_bootstrap::kClientMovePacked4};
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0), bs::GameStateMode::Tier2);
    const size_t moves_from = h.got.size();
    std::vector<float> stamps;
    bool all_legacy = true;
    for (const auto* m : moves) {
        const BunchWriter bits = hex_bits(*m);
        const auto legacy = legacy_time_stamp(bits);
        all_legacy = all_legacy && legacy.has_value();
        stamps.push_back(legacy.value_or(-1.0f));
        h.client_sends({h.client.actor(pawn_ch, bits, false)});
    }
    h.step();
    CHECK(all_legacy && stamps[0] > 0.8f && stamps[0] < 0.82f && stamps[0] < stamps[1] && stamps[1] < stamps[2],
          "(the captured moves' TimeStamps read by the legacy parser: 0.8125 s and rising)");
    const auto acks = h.on_channel(pawn_ch, moves_from);
    CHECK(acks.size() == 4, "each ServerMovePacked is answered");
    bool unreliable = true, stamps_ok = true, bits_ok = true;
    for (size_t i = 0; i < acks.size() && i < stamps.size(); ++i) {
        unreliable = unreliable && !acks[i]->reliable && acks[i]->exports.empty() && acks[i]->content.bit_pos() == 68;
        const auto ts = response_time_stamp(*acks[i]);
        stamps_ok = stamps_ok && ts && *ts == stamps[i];
        const auto want = pb->good_move_ack(stamps[i]);
        bits_ok = bits_ok && want && same_bits(acks[i]->content, want->emission.payload);
    }
    CHECK(unreliable, "unreliable, 68 bits, on the pawn's channel");
    CHECK(stamps_ok, "ClientMoveResponsePacked carries the move's own TimeStamp (the legacy server's parse of the same bunch), also the 533-bit bunch's");
    CHECK(bits_ok, "equal to the bootstrap's good_move_ack, which equals the original's 68 bits (test_worldd_bootstrap)");
    CHECK(h.s->moves_acknowledged() == 4, "four moves acknowledged");
    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    CHECK(log.find("the client is ready to phase in") != std::string::npos && log.find("sent phase-in on channel 5") != std::string::npos &&
              log.find("the first move is acknowledged (TimeStamp 0.81252") != std::string::npos,
          "the log has the phase-in and the first move acknowledged (once)");
    size_t first_move_lines = 0;
    for (size_t at = log.find("the first move is acknowledged"); at != std::string::npos; at = log.find("the first move is acknowledged", at + 1)) ++first_move_lines;
    CHECK(first_move_lines == 1, "the move acknowledgement is logged once, not per move");

    // a move with a damaged parameter is not acknowledged
    BunchWriter damaged = hex_bits(capture_bootstrap::kClientMovePacked1);
    BunchWriter cut;   // the same bunch with DataBits' packed integer changed: the parameter no longer ends where the field says
    const auto bytes = capture_blocks::bytes_of(capture_bootstrap::kClientMovePacked1.hex);
    std::vector<uint8_t> mangled(bytes.begin(), bytes.end());
    mangled[6] ^= 0x04;   // inside the "send" bit / SerializeIntPacked(DataBits) bits of the parameter
    cut.write_bit_range(mangled.data(), 0, damaged.bit_pos());
    const size_t acked = h.s->moves_acknowledged();
    h.client_sends({h.client.actor(pawn_ch, cut, false)});
    h.step();
    CHECK(h.s->moves_acknowledged() == acked, "a ServerMovePacked whose parameter does not read as the capture's is not acknowledged");
}

AOC_REGISTER_TEST(test_phase_in_and_moves);

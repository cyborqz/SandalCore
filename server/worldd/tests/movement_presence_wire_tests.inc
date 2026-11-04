// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/movement_presence_wire_tests.inc: what another player's client is sent of a player (players seeing each other)
//
//  The other players are shown a player's pawn alone: one actor for each player, no PlayerState.  The text of the login (the ?Name= option, which the client chooses and which names the
//  player's PC) is in no byte another player's client is sent, while the player's own client has it.  The pawn they are shown has no Owner, no Controller, no PlayerState and no
//  UniqueId, and its CharacterName is the character's name.
// ============================================================================
#include "movement_presence_rig.h"

static void test_presence_another_players_client_is_sent_one_pawn_and_no_login_text() {
    std::printf("players seeing each other: another player's client is sent one actor for a player and none of the login's ?Name= text, with 0 and 3 view threads\n");
    char text[420];
    const std::string marker = "ZQXMARKERNAME1234";   // 17 characters: a PlayerState made of the login's ?Name= option would hold all of it
    for (const unsigned threads : {0u, 3u}) {
        g_counter = 1;
        PresenceEndpoints e(threads);
        Rig& rig = *e.rig;
        const world::ActorRegistry& reg = e.fx.world->registry();
        Clock::time_point now = t0;
        std::vector<PresenceClient*> all;
        PresenceClient a(50510, "cyb"), b(50511, "ayla");
        a.needles = {marker};
        b.needles = {marker};
        presence_join(rig, all, a, cyb_record(), now, marker);   // cyb logs in with the marker as its ?Name=
        presence_join(rig, all, b, presence_record(presence_id(2), "ayla", 4), now);
        presence_phase_in(rig, all, a, now);
        presence_phase_in(rig, all, b, now);
        presence_idle(rig, all, now, 25);
        std::snprintf(text, sizeof(text), "%u view thread(s): (the search works: the marker is in the packets of cyb's own client, %zu of %zu)", threads, a.hits, a.packets);
        CHECK(a.hits > 0, text);
        std::snprintf(text, sizeof(text), "%u view thread(s): the marker is in none of the %zu packets the other player's client is sent, as one byte or two per character", threads, b.packets);
        CHECK(b.hits == 0 && b.packets > 10, text);
        // the actors the other player's client opened beyond its own four (channels 3 to 6): one, cyb's pawn
        size_t opens = 0;
        for (const fake::Delivered& d : b.j.got) {
            if (d.open && d.channel > 6) ++opens;
        }
        const uint64_t ida = presence_player_id(rig, a.addr);
        std::snprintf(text, sizeof(text), "%u view thread(s): the registry holds one Player actor for each of the two players and ayla's client opened exactly one actor for cyb (%zu opens, %zu Player actors)", threads, opens,
                      presence_count(reg));
        CHECK(presence_count(reg) == 2 && presence_pawn(reg, ida) && opens == 1, text);
    }
}
AOC_REGISTER_TEST(test_presence_another_players_client_is_sent_one_pawn_and_no_login_text);

static void test_presence_the_pawn_other_players_are_shown_has_nothing_of_the_owners() {
    std::printf("players seeing each other: the pawn other players are shown has no Owner, Controller, PlayerState or UniqueId, and the character's name\n");
    Harness h;
    {
        BunchWriter ready;
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        ready.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        h.client_sends({h.client.actor(h.pawn_ch, ready, true)});
    }
    h.settle();
    const auto pawn = h.s->bootstrap().observer_pawn();
    CHECK(pawn.has_value(), "(the bootstrap builds the pawn other players are shown once the join is built)");
    if (!pawn) return;
    const repl::ObjectState& o = pawn->obj;
    const auto unset = [&](const char* path) { return o.schema->command(path) != nullptr && o.state.get(*o.schema, path) == nullptr; };
    CHECK(unset("Owner") && unset("Controller") && unset("PlayerState"), "the pawn has no Owner, no Controller and no PlayerState (those are objects only its owner's connection has)");
    bool unique_id = false;
    std::string character_name;
    size_t leaked = 0;
    const std::string login_text = h.s->bootstrap().setup().login.name_option;
    const auto scan = [&](const repl::ObjectState& obj) {
        if (obj.schema && obj.schema->command("UniqueId") && obj.state.get(*obj.schema, "UniqueId")) unique_id = true;
        for (const auto& [handle, value] : obj.state.values()) {
            if (const auto* str = value.get_if<std::string>()) {
                if (!login_text.empty() && str->find(login_text.substr(0, 12)) != std::string::npos) ++leaked;
            }
        }
        if (obj.schema && obj.schema->command("CharacterName")) {
            if (const repl::Value* v = obj.state.get(*obj.schema, "CharacterName")) {
                if (const auto* str = v->get_if<std::string>()) character_name = *str;
            }
        }
    };
    scan(pawn->obj);
    for (const repl::Subobject& s : pawn->subobjects) scan(s.obj);
    CHECK(!unique_id, "no object of the pawn has a UniqueId");
    CHECK(leaked == 0, "no text value of the pawn holds the login's ?Name= text");
    CHECK(character_name == "cyb" && !login_text.empty(), "its CharacterName is the character's name, not the login's text");
    CHECK(pawn->subobjects.size() == 5, "it has the five components that show the character (Alignment, BaseCharacterInfo, CombatInfo, Stats, Appearance) and nothing of the owner's");
}
AOC_REGISTER_TEST(test_presence_the_pawn_other_players_are_shown_has_nothing_of_the_owners);

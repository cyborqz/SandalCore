// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/others_gear_level_edges_tests.inc: the edges of other players seeing a player's gear, weapon set and level (Social, seeing other players: gear and level changes)
//
//  Included after tests/others_gear_level_tests.inc (its helpers: parse_states, sent_array, held_slots, level_up_calls, arrays_on ...).  Several changes in one tick, a level-up that reaches
//  the watchers in range and not the one out of range, a player who joins, or logs in again, after a level-up, a player who levels up again after its own relog, a level-up while riding, and
//  the registry's walk of its players.
//
//  Modelled: no recording shows another player's pawn receive these, so the shape is the player's own reply's, kept current on the pawn the watchers hold.
// ============================================================================

/// How many chains of `channel` a watcher got since `from` that carry a held-items array, the weapon flag or the level of the pawn: the updates a change of gear, weapon set or level makes.
static size_t pawn_look_updates(const Harness& watcher, uint32_t channel, const repl::Actor& actor, uint64_t base_id, uint64_t combat_id, size_t from) {
    size_t n = 0;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close) continue;
        const States st = parse_states(d, actor);
        if (sent_array(d, actor, "UPlayerInfo", "HeldItems") || number_in(st, combat_id, "bIsUsingRangedWeapon") || number_in(st, base_id, "Level")) ++n;
    }
    return n;
}

/// Gives the player of `h` experience, a few steps apart while its connection is too busy for the answer, until the level rises; the level it reached (0: it did not rise).
static int64_t gain_levels(Harness& h, int64_t amount) {
    PlayerSession::GmXpResult r;
    for (int tries = 0; tries < 30; ++tries) {
        r = h.s->gm_give_experience(h.conn, amount, h.t, "the test");
        if (r.ok) break;
        h.run(milliseconds(100));
    }
    if (!r.ok) std::printf("  (the experience was refused: %s)\n", r.refusal.c_str());
    return r.ok && r.leveled ? r.level_after : 0;
}

// ---- several changes in one tick ---------------------------------------------------------------------------------------------------------------

static void test_two_gear_changes_in_one_tick_are_one_update() {
    std::printf("two items worn or taken off before the next tick are one update for the watcher, its held-items key moved by 23 for each change\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t look_id = id_of_component(*pawn, "UCharacterAppearanceComponent");
    const fake::Delivered* open = open_on(b, pch);
    const auto held0 = open ? sent_array(*open, pawn_copy, "UPlayerInfo", "HeldItems") : std::nullopt;
    CHECK(held0 && held0->items.size() == 23, "(the watcher's open carries the 23 held items)");
    const auto chest = a.s->inventory().at(Worn, 2);
    const auto book = a.s->inventory().at(Worn, 17);
    CHECK(chest != nullptr && book != nullptr, "(the chest and the ranged book are worn)");
    if (!held0 || !chest || !book) return;
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, chest->instance_guid), true)});
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 17, book->instance_guid), true)});   // (the second request is in before the session's next tick)
    a.run(milliseconds(250));
    CHECK(!a.s->inventory().at(Worn, 2) && !a.s->inventory().at(Worn, 17), "(both are off)");
    const auto chains = arrays_on(b, pch, pawn_copy, from, "UPlayerInfo", "HeldItems");
    CHECK(chains.size() == 1, "the watcher was sent one update of the held items for the two changes");
    if (chains.size() != 1) return;
    CHECK(chains[0].second.items.size() == 23 && chains[0].second.key == held0->key + 46, "...all 23 marked and the array's key moved by 46, 23 for each change");
    const auto slots = held_slots(chains[0].second);
    CHECK(record_in(slots, 17) == 0 && record_in(slots, 15) == fi::kScepter, "...the book's held slot empty, the scepter in place");
    const auto look = array_value_in(parse_states(*chains[0].first, pawn_copy), look_id, "AppearanceIDs");
    CHECK(look && look->elements.size() == 3, "...and the appearance list without the chest");
}
AOC_REGISTER_TEST(test_two_gear_changes_in_one_tick_are_one_update);

static void test_a_gear_change_a_swap_and_a_level_in_one_tick_are_one_update_and_one_effect() {
    std::printf("a gear change, a weapon swap and a level before the next tick are one update of the pawn for the watcher, and one level-up effect\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t base_id = id_of_component(*pawn, "UPlayerInfo");
    const uint64_t combat_id = id_of_component(*pawn, "UPlayerCombatInfo");
    const auto chest = a.s->inventory().at(Worn, 2);
    CHECK(chest != nullptr, "(the chest is worn)");
    if (!chest) return;
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, chest->instance_guid), true)});
    a.client_sends({a.client.actor(a.pawn_ch, ranged_rpc(true), true)});
    a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    a.run(milliseconds(300));
    CHECK(!a.s->inventory().at(Worn, 2) && a.s->using_ranged_weapon() && a.s->progress().level == 2, "(the chest is off, the ranged set is in hand, the player is level 2)");
    CHECK(pawn_look_updates(b, pch, pawn_copy, base_id, combat_id, from) == 1, "the watcher was sent one update of the pawn for the three changes");
    size_t flag = 0, level = 0, held = 0;
    for (size_t i = from; i < b.got.size(); ++i) {
        const fake::Delivered& d = b.got[i];
        if (d.channel != pch || d.close) continue;
        const States st = parse_states(d, pawn_copy);
        if (number_in(st, combat_id, "bIsUsingRangedWeapon") == 1) ++flag;
        if (number_in(st, base_id, "Level") == 2) ++level;
        if (sent_array(d, pawn_copy, "UPlayerInfo", "HeldItems")) ++held;
    }
    CHECK(flag == 1 && level == 1 && held == 1, "...which has the ranged flag, the new level and the held items, once each");
    const auto calls = level_up_calls(b, pch, from);
    CHECK(calls.size() == 1 && calls[0].second == 2, "the level-up effect is sent once");
}
AOC_REGISTER_TEST(test_a_gear_change_a_swap_and_a_level_in_one_tick_are_one_update_and_one_effect);

// ---- who is sent the level-up effect --------------------------------------------------------------------------------------------------------------

static void test_a_level_up_reaches_the_watcher_in_range_and_not_the_one_far_away() {
    std::printf("a level-up is shown to the watcher in range and not to the one 300 m away; the far one, brought near, sees the level and no old effect, and the next level-up reaches both\n");
    Harness a(cyb_record());
    a.play();
    Players all;
    const GoodPosition start = a.s->movement().good();
    auto b = join_world(a, all, other_record(other_id(2), "ayla", 4));
    auto c = join_world(a, all, other_record(other_id(3), "kira", 5, std::make_pair(start.x + 30000.0, start.y)));
    a.run(milliseconds(300));
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr, "(the player's pawn is in the world)");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    const uint32_t pch_b = b->s->actor_view() ? b->s->actor_view()->channel_of(pawn_id) : 0;
    CHECK(pch_b != 0, "(the first watcher holds the player's pawn)");
    CHECK(c->s->actor_view() && c->s->actor_view()->channel_of(pawn_id) == 0, "(the second watcher, 300 m away, holds nothing of the player)");
    if (pch_b == 0) return;
    // the first level-up
    const size_t from_b = b->got.size();
    a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    a.run(milliseconds(300));
    CHECK(a.s->progress().level == 2, "(the player is level 2)");
    const auto first = level_up_calls(*b, pch_b, from_b);
    CHECK(first.size() == 1 && first[0].first == content::kGuidAdventuring && first[0].second == 2, "the watcher in range is sent the level-up effect once (level 2)");
    CHECK(c->s->actor_view()->channel_of(pawn_id) == 0, "the watcher 300 m away holds nothing of the player: nothing is sent to it");
    // the far watcher comes into range
    const GoodPosition at = a.s->movement().good();
    const PlayerSession::GmMoveResult moved = c->s->gm_teleport(c->conn, at.x + 50.0, at.y, at.z, c->t, "the test");
    CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "(the second watcher is put beside the player)");
    a.run(milliseconds(500));
    const world::WorldActor* pawn2 = shown_pawn(a);
    const uint32_t pch_c = pawn2 && c->s->actor_view() ? c->s->actor_view()->channel_of(pawn2->id) : 0;
    const fake::Delivered* open = pch_c != 0 ? open_on(*c, pch_c) : nullptr;
    CHECK(open != nullptr, "(its view opens the player's pawn)");
    if (!open || !pawn2) return;
    const repl::Actor pawn_copy = pawn2->actor;
    CHECK(number_in(parse_states(*open, pawn_copy), id_of_component(*pawn2, "UPlayerInfo"), "Level") == 2, "its open carries the level (2)");
    const size_t after_open = static_cast<size_t>(open - c->got.data()) + 1;
    CHECK(level_up_calls(*c, pch_c, after_open).empty(), "...and the effect of the time before it held the pawn is not played");
    // the next level-up reaches both
    const size_t from_b2 = b->got.size(), from_c2 = c->got.size();
    const int64_t now_level = gain_levels(a, 4000);
    a.run(milliseconds(300));
    CHECK(now_level > 2 && a.s->progress().level == now_level, "(the player gains more levels)");
    const auto second_b = level_up_calls(*b, pch_b, from_b2), second_c = level_up_calls(*c, pch_c, from_c2);
    CHECK(second_b.size() == 1 && second_b[0].second == now_level, "the first watcher is sent the new level's effect once");
    CHECK(second_c.size() == 1 && second_c[0].second == now_level, "the second watcher, now in range, too");
}
AOC_REGISTER_TEST(test_a_level_up_reaches_the_watcher_in_range_and_not_the_one_far_away);

static void test_a_player_who_joins_after_a_level_up_sees_the_level_and_no_old_effect() {
    std::printf("a player who joins the world after another's level-up is shown the level in the open and no old effect, and the next level-up reaches it\n");
    Harness a(cyb_record());
    a.play();
    Players all;
    a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    a.run(milliseconds(300));
    CHECK(a.s->progress().level == 2, "(the player is level 2 before anyone joins)");
    auto b = join_world(a, all, other_record(other_id(2), "ayla", 4));
    a.run(milliseconds(500));
    const world::WorldActor* pawn = shown_pawn(a);
    const uint32_t pch = pawn && b->s->actor_view() ? b->s->actor_view()->channel_of(pawn->id) : 0;
    const fake::Delivered* open = pch != 0 ? open_on(*b, pch) : nullptr;
    CHECK(open != nullptr, "(the player who joined holds the pawn)");
    if (!open || !pawn) return;
    const repl::Actor pawn_copy = pawn->actor;
    CHECK(number_in(parse_states(*open, pawn_copy), id_of_component(*pawn, "UPlayerInfo"), "Level") == 2, "its open carries the level (2)");
    CHECK(level_up_calls(*b, pch, 0).empty(), "no level-up effect has been sent to it");
    const size_t from = b->got.size();
    const int64_t now_level = gain_levels(a, 4000);
    a.run(milliseconds(300));
    CHECK(now_level > 2 && a.s->progress().level == now_level, "(the player gains more levels)");
    const auto calls = level_up_calls(*b, pch, from);
    CHECK(calls.size() == 1 && calls[0].second == now_level, "the next level-up's effect is sent to it once");
}
AOC_REGISTER_TEST(test_a_player_who_joins_after_a_level_up_sees_the_level_and_no_old_effect);

static void test_a_watcher_who_logs_in_again_sees_the_current_character_and_no_old_effect() {
    std::printf("a watcher who logs out and in again is shown the player's current level and gear, no old level-up effect, and the player's next level-up\n");
    Scene sc;
    Harness& a = sc.a;
    const auto chest = a.s->inventory().at(Worn, 2);
    CHECK(chest != nullptr, "(the chest is worn)");
    if (!chest) return;
    a.client_sends({a.client.actor(a.pawn_ch, item_used(4, 2, chest->instance_guid), true)});
    a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    a.run(milliseconds(300));
    CHECK(a.s->progress().level == 2 && !a.s->inventory().at(Worn, 2), "(the player is level 2 and the chest is off)");
    sc.b->s->finish("the test");
    a.run(milliseconds(250));
    auto b2 = join_world(a, sc.all, other_record(other_id(2), "ayla", 4));
    a.run(milliseconds(500));
    const world::WorldActor* pawn = shown_pawn(a);
    const uint32_t pch = pawn && b2->s->actor_view() ? b2->s->actor_view()->channel_of(pawn->id) : 0;
    const fake::Delivered* open = pch != 0 ? open_on(*b2, pch) : nullptr;
    CHECK(open != nullptr, "(the watcher who came back holds the player's pawn)");
    if (!open || !pawn) return;
    const repl::Actor pawn_copy = pawn->actor;
    const States st = parse_states(*open, pawn_copy);
    CHECK(number_in(st, id_of_component(*pawn, "UPlayerInfo"), "Level") == 2, "its open carries the level (2)");
    const auto look = array_value_in(st, id_of_component(*pawn, "UCharacterAppearanceComponent"), "AppearanceIDs");
    CHECK(look && look->elements.size() == 3, "...and the look without the chest");
    CHECK(level_up_calls(*b2, pch, 0).empty(), "no level-up effect has been sent to it");
    const size_t from = b2->got.size();
    const int64_t now_level = gain_levels(a, 4000);
    a.run(milliseconds(300));
    CHECK(now_level > 2 && a.s->progress().level == now_level, "(the player gains more levels)");
    const auto calls = level_up_calls(*b2, pch, from);
    CHECK(calls.size() == 1 && calls[0].second == now_level, "the player's next level-up's effect is sent to it once");
}
AOC_REGISTER_TEST(test_a_watcher_who_logs_in_again_sees_the_current_character_and_no_old_effect);

static void test_a_player_who_logs_in_again_and_levels_up_is_shown_it_once() {
    std::printf("a player who logs in again, with the level it had, is shown to the watcher without an old effect, and its next level-up is sent once\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const size_t from_first = b.got.size();
    a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    a.run(milliseconds(300));
    CHECK(a.s->progress().level == 2 && level_up_calls(b, pch, from_first).size() == 1, "(the player is level 2 and the watcher saw the effect once)");
    const GoodPosition saved = a.s->last_position();
    const size_t from = b.got.size();
    a.s->finish("the test");
    a.run(milliseconds(250));
    CHECK(shown_pawn(a) == nullptr, "(the pawn left the registry with the session)");
    svc::Character again = cyb_record();
    again.level = 2;
    again.has_position = true;
    again.position.x = saved.x;
    again.position.y = saved.y;
    again.position.z = saved.z;
    auto a2 = join_world(a, sc.all, again);
    a.run(milliseconds(600));
    const world::WorldActor* pawn2 = shown_pawn(*a2);
    const uint32_t pch2 = pawn2 ? sc.channel(pawn2->id) : 0;
    const fake::Delivered* open = pch2 != 0 ? open_on(b, pch2, from) : nullptr;
    CHECK(open != nullptr, "(the watcher holds the returned player's pawn)");
    if (!open || !pawn2) return;
    const size_t after_open = static_cast<size_t>(open - b.got.data()) + 1;
    CHECK(level_up_calls(b, pch2, after_open).empty(), "no level-up effect is played for the returned player's open");
    const size_t from2 = b.got.size();
    const int64_t now_level = gain_levels(*a2, 4000);
    a.run(milliseconds(300));
    CHECK(now_level > 2 && a2->s->progress().level == now_level, "(the returned player gains more levels)");
    const auto calls = level_up_calls(b, pch2, from2);
    CHECK(calls.size() == 1 && calls[0].second == now_level, "its next level-up's effect is sent to the watcher once");
}
AOC_REGISTER_TEST(test_a_player_who_logs_in_again_and_levels_up_is_shown_it_once);

static void test_a_level_up_while_riding_is_shown_on_the_seated_pawn() {
    std::printf("a rider's level-up is shown on its seated pawn's channel, with the level and the effect, and the pawn stays seated\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the player rides)");
    if (!a.s->mounted()) return;
    const world::WorldActor* pawn = shown_pawn(a);
    const world::WorldActor* mount = a.mount();
    CHECK(pawn != nullptr && mount != nullptr, "(the watcher holds the player's pawn and its mount)");
    if (!pawn || !mount) return;
    const uint32_t pch = sc.channel(pawn->id);
    CHECK(pch != 0, "(the watcher holds the seated pawn)");
    const emit::FIntrepidNetworkGUID mount_guid = mount->actor.guid;
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t base_id = id_of_component(*pawn, "UPlayerInfo");
    const size_t from = b.got.size();
    a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    a.run(milliseconds(300));
    CHECK(a.s->progress().level == 2 && a.s->mounted(), "(the player is level 2 and still rides)");
    std::vector<int64_t> levels;
    for (size_t i = from; i < b.got.size(); ++i) {
        const fake::Delivered& d = b.got[i];
        if (d.channel != pch || d.close) continue;
        if (const auto v = number_in(parse_states(d, pawn_copy), base_id, "Level")) levels.push_back(*v);
    }
    CHECK(levels.size() == 1 && levels[0] == 2, "the watcher is sent the new level once, on the seated pawn's channel");
    const auto calls = level_up_calls(b, pch, from);
    CHECK(calls.size() == 1 && calls[0].second == 2, "...and the level-up effect once");
    const world::WorldActor* now = shown_pawn(a);
    CHECK(now != nullptr && same_object(actor_object(now->actor, "AttachmentReplication.AttachParent"), mount_guid), "the pawn the watchers hold is still attached to the mount");
}
AOC_REGISTER_TEST(test_a_level_up_while_riding_is_shown_on_the_seated_pawn);

static void test_a_player_who_comes_in_as_another_leaves_is_watched_for_its_level_ups() {
    std::printf("a player whose pawn comes in in the tick another player's pawn leaves (the number of players stays) is still watched: its first level-up reaches the watcher\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    auto c = join_world(a, sc.all, gear_record(3, "kira"));
    a.run(milliseconds(400));
    const world::WorldActor* old_pawn = shown_pawn(*c);
    const uint32_t old_ch = old_pawn ? sc.channel(old_pawn->id) : 0;
    CHECK(old_pawn != nullptr && old_ch != 0, "(the watcher holds the third player's pawn)");
    if (!old_pawn || old_ch == 0) return;
    // in one tick: that pawn leaves the world and another one, the same pawn under a new id, comes in; the watcher's session has not looked at the new one yet
    world::ActorRegistry& reg = a.fx.world->registry();
    world::WorldActor swapped = *old_pawn;
    const uint64_t old_id = swapped.id;
    swapped.id = old_id + 100000;
    swapped.actor.guid.ObjectId = swapped.id;
    swapped.player.level_up_serial = 0;
    CHECK(reg.remove(old_id) && reg.add(std::move(swapped)), "(the third player's pawn goes and another comes in, in one tick)");
    const uint64_t new_id = old_id + 100000;
    a.run(milliseconds(400));
    const uint32_t new_ch = sc.channel(new_id);
    CHECK(new_ch != 0, "(the watcher holds the pawn that came in)");
    if (new_ch == 0) return;
    // its level-up, announced the way the player's session does
    const size_t from = b.got.size();
    CHECK(reg.modify(new_id, world::Change::State,
                     [&](world::WorldActor& w) {
                         ++w.player.level_up_serial;
                         w.player.level_up_category = content::kGuidAdventuring;
                         w.player.level_up_level = 9;
                     }),
          "(the pawn announces a level-up)");
    reg.note_level_up();
    a.run(milliseconds(300));
    const auto calls = level_up_calls(b, new_ch, from);
    CHECK(calls.size() == 1 && calls[0].first == content::kGuidAdventuring && calls[0].second == 9, "the watcher is sent its level-up effect once");
}
AOC_REGISTER_TEST(test_a_player_who_comes_in_as_another_leaves_is_watched_for_its_level_ups);

// ---- the registry's players -------------------------------------------------------------------------------------------------------------------------

static world::WorldActor bare_actor(uint64_t id, world::ActorKind kind) {
    world::WorldActor w;
    w.id = id;
    w.kind = kind;
    w.actor.guid.ObjectId = id;
    return w;
}

static void test_the_registry_walks_its_players_and_counts_those_that_come_and_go() {
    std::printf("the registry walks its players alone, in the order they came in, and counts each one that comes in or goes: a player leaving as another joins moves the count\n");
    world::ActorRegistry reg;
    const auto players = [&]() {
        std::vector<uint64_t> out;
        reg.for_each_player([&](const world::WorldActor& a) { out.push_back(a.id); });
        return out;
    };
    CHECK(players().empty() && reg.player_epoch() == 0, "an empty registry has no player and the count is 0");
    CHECK(reg.add(bare_actor(100, world::ActorKind::Player)) && reg.add(bare_actor(102, world::ActorKind::Npc)) && reg.add(bare_actor(104, world::ActorKind::Player)) &&
              reg.add(bare_actor(106, world::ActorKind::Player)),
          "(three players and a mob come in)");
    CHECK(players() == std::vector<uint64_t>({100, 104, 106}), "the walk holds the three players, in the order they came in, and not the mob");
    CHECK(reg.player_epoch() == 3, "the count moved once for each player and not for the mob");
    CHECK(reg.remove(102) && reg.player_epoch() == 3, "a mob that goes does not move it");
    CHECK(reg.remove(104) && players() == std::vector<uint64_t>({100, 106}) && reg.player_epoch() == 4, "a player that goes is out of the walk and moves it");
    CHECK(reg.add(bare_actor(108, world::ActorKind::Player)) && players() == std::vector<uint64_t>({100, 106, 108}) && reg.player_epoch() == 5,
          "another that comes in: the same number of players as before, and the count differs from the one a session last saw");
    CHECK(!reg.remove(104) && reg.player_epoch() == 5, "an actor that is not held changes nothing");
}
AOC_REGISTER_TEST(test_the_registry_walks_its_players_and_counts_those_that_come_and_go);

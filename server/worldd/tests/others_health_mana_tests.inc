// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/others_health_mana_tests.inc: other players see a player's live Health, MaxHealth, Mana and MaxMana (Social, seeing other players: live Health and Mana)
//
//  A player (cyb) and one to three watchers (tests/mounts_ride_others_rig.h) in one world: the player takes damage, is healed, regenerates Mana and changes its maxima
//  with gear and a level; what a watcher's client is sent is read chain by chain (the update of the pawn the watchers hold).  Also: a player who comes into view after
//  the change, a player who leaves view and comes back, a rider, a relog, the same value twice, and two players side by side.
//
//  Modelled: no recording shows another player's pawn receive a stat delta, so the shape is the owner's own reply's (the four items of StatRepInt32EveryoneProxy),
//  kept current on the pawn the watchers hold.  The rate is the game's own: a value that changes once a second sends one delta, not one a tick.
// ============================================================================

/// The value of the EveryoneProxy stat `record` in the fast array a chain carries (nullopt: the chain does not carry it).
static std::optional<float> stat_in(const SentArray& a, int64_t record) {
    for (const repl::FastArrayItem& it : a.items) {
        const auto found = it.values.values.find(1);
        const auto* t = found != it.values.values.end() ? found->second.get_if<repl::StatInt32Inst>() : nullptr;
        if (t && t->record == record) return std::bit_cast<float>(t->value);
    }
    return std::nullopt;
}

/// One stat delta a watcher's client was sent: the value and when its chain arrived.
struct StatDelta {
    float            value = 0;
    Clock::time_point at{};
};

/// The EveryoneProxy stat `record` as the pawn the watchers hold carries it now (nullopt: the pawn has no such item).
static std::optional<float> shown_stat(const world::WorldActor& pawn, int64_t record) {
    for (const repl::Subobject& sub : pawn.actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAoCStatsComponent") continue;
        const schema::NetField* f = sub.obj.schema->field("StatRepInt32EveryoneProxy");
        const repl::FastArrayState* arr = f ? sub.obj.arrays.find(f->index) : nullptr;
        if (!arr) return std::nullopt;
        for (const repl::FastArrayItem& it : arr->items()) {
            const auto found = it.values.values.find(1);
            const auto* t = found != it.values.values.end() ? found->second.get_if<repl::StatInt32Inst>() : nullptr;
            if (t && t->record == record) return std::bit_cast<float>(t->value);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/// The deltas of the EveryoneProxy stat `record` the watcher's client was sent on `channel` from `from` on, oldest first.
static std::vector<StatDelta> stat_deltas_on(const Harness& watcher, uint32_t channel, const repl::Actor& actor, size_t from, int64_t record) {
    std::vector<StatDelta> out;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close) continue;
        const auto a = sent_array(d, actor, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
        if (!a) continue;
        if (const auto v = stat_in(*a, record)) out.push_back(StatDelta{*v, watcher.got_at[i]});
    }
    return out;
}

/// The current value of a stat of a session's own character (0 when the session has no such stat).
static float session_stat(const PlayerSession& s, int64_t record) {
    const stats::CharacterStats* cs = s.character_stats();
    const stats::StatValue* v = cs ? cs->find(record) : nullptr;
    return v ? v->value : 0.0f;
}

// ---- one change after another --------------------------------------------------------------------------------------------------------

static void test_others_see_a_players_health_and_mana_change() {
    std::printf("other players see a player's Health and Mana change: the open carries the current values, then one delta per change\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const fake::Delivered* open = open_on(b, pch);
    CHECK(open != nullptr, "(the watcher's client has its open)");
    if (!open) return;
    const float max_h = session_stat(*a.s, content::kGuidMaxHealth), max_m = session_stat(*a.s, content::kGuidMaxMana);
    CHECK(max_h > 0 && max_m > 0, "(the player has a maximum Health and Mana)");
    const auto held0 = sent_array(*open, pawn_copy, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
    CHECK(held0 && stat_in(*held0, content::kGuidHealth) == max_h && stat_in(*held0, content::kGuidMaxHealth) == max_h && stat_in(*held0, content::kGuidMana) == max_m &&
              stat_in(*held0, content::kGuidMaxMana) == max_m,
          "the open carries the four current values (full at the join), not only the maxima");
    // damage: the combat path's own (the pipeline's take_damage), which sets the current value on the pawn
    const size_t from_damage = b.got.size();
    const double through = a.s->take_damage(a.conn, 120.0, a.t);
    a.run(milliseconds(300));
    CHECK(through > 0, "(the damage reached the player)");
    const float hurt = session_stat(*a.s, content::kGuidHealth);
    CHECK(hurt < max_h, "(Health is below its maximum)");
    const auto damage = stat_deltas_on(b, pch, pawn_copy, from_damage, content::kGuidHealth);
    CHECK(!damage.empty() && damage.back().value == hurt, "the watcher is sent the Health the player has now");
    CHECK(shown_stat(*shown_pawn(a), content::kGuidHealth) == hurt, "the pawn the watchers hold carries the new Health");
    // the heal to full: one delta, and no regeneration after it
    const size_t from_heal = b.got.size();
    CHECK(a.s->set_current_value(content::kGuidHealth, max_h), "(a heal fills Health)");
    a.run(milliseconds(400));
    const auto heal = stat_deltas_on(b, pch, pawn_copy, from_heal, content::kGuidHealth);
    CHECK(heal.size() == 1 && heal[0].value == max_h, "...one Health delta for the heal to full");
    CHECK(shown_stat(*shown_pawn(a), content::kGuidHealth) == max_h, "the pawn the watchers hold has the full Health");
    // Mana: below the maximum, then filled: the fill is one delta
    CHECK(a.s->set_current_value(content::kGuidMana, max_m - 100.0f), "(Mana is set below its maximum)");
    a.run(milliseconds(1200));
    const size_t from_mana = b.got.size();
    CHECK(a.s->set_current_value(content::kGuidMana, max_m), "(Mana is filled)");
    a.run(milliseconds(400));
    const auto mana = stat_deltas_on(b, pch, pawn_copy, from_mana, content::kGuidMana);
    CHECK(mana.size() == 1 && mana[0].value == max_m, "the watcher is sent one Mana delta for the fill");
    CHECK(shown_stat(*shown_pawn(a), content::kGuidMana) == max_m, "the pawn the watchers hold has the full Mana");
    // full, then a tick with nothing to say: no delta
    const size_t from_quiet = b.got.size();
    a.run(milliseconds(400));
    CHECK(stat_deltas_on(b, pch, pawn_copy, from_quiet, content::kGuidHealth).empty() && stat_deltas_on(b, pch, pawn_copy, from_quiet, content::kGuidMana).empty(),
          "a tick in which nothing changed sends no stat delta");
    // the same value twice: the second request sends nothing
    const size_t from_twice = b.got.size();
    CHECK(a.s->set_current_value(content::kGuidHealth, max_h - 60.0f), "(Health is set once)");
    a.run(milliseconds(400));
    const size_t after_first = b.got.size();
    CHECK(!stat_deltas_on(b, pch, pawn_copy, from_twice, content::kGuidHealth).empty(), "(the change was sent)");
    CHECK(a.s->set_current_value(content::kGuidHealth, session_stat(*a.s, content::kGuidHealth)), "(the value the player has now is set again: a regeneration tick may have moved it since the first set)");
    a.run(milliseconds(400));
    CHECK(stat_deltas_on(b, pch, pawn_copy, after_first, content::kGuidHealth).empty(), "...and the same value again sends nothing");
}
AOC_REGISTER_TEST(test_others_see_a_players_health_and_mana_change);

// ---- the rate of a regeneration ------------------------------------------------------------------------------------------------------

static void test_mana_that_regenerates_is_sent_about_once_a_second() {
    std::printf("Mana that regenerates is sent about once a second, not once a tick\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const float max_m = session_stat(*a.s, content::kGuidMaxMana);
    const float regen = session_stat(*a.s, content::kGuidManaRegeneration);
    CHECK(max_m > 0 && regen > 0, "(the player has a maximum Mana and a regeneration)");
    // eight ticks short of full: at most eight changes, one a second
    CHECK(a.s->set_current_value(content::kGuidMana, std::max(0.0f, max_m - regen * 8.0f)), "(Mana is set eight ticks short of its maximum)");
    a.run(milliseconds(300));   // (the set's own delta goes first: the window starts after it)
    const size_t from = b.got.size();
    a.run(milliseconds(10500));
    const auto deltas = stat_deltas_on(b, pch, pawn_copy, from, content::kGuidMana);
    CHECK(deltas.size() >= 5 && deltas.size() <= 15, "ten and a half seconds of regeneration send a handful of Mana deltas (about one a second), not one for each of the 525 ticks");
    bool spaced = deltas.size() >= 2;
    for (size_t i = 1; i < deltas.size(); ++i) spaced = spaced && deltas[i].at - deltas[i - 1].at >= milliseconds(500);
    CHECK(spaced, "...and no two of them are closer together than half a second");
    CHECK(session_stat(*a.s, content::kGuidMana) >= max_m - regen, "(the bar filled up)");
}
AOC_REGISTER_TEST(test_mana_that_regenerates_is_sent_about_once_a_second);

// ---- a third player who comes into view after the change -----------------------------------------------------------------------------

static void test_a_player_who_comes_into_view_is_shown_the_current_health_in_the_open() {
    std::printf("a player who comes into view after the damage is shown the current Health in the open, not the maximum\n");
    Scene sc(30000);   // the watcher stands 300 m east of where the player starts
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) == 0, "300 m away the watcher holds nothing of the player");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    const float max_h = session_stat(*a.s, content::kGuidMaxHealth);
    a.s->take_damage(a.conn, 90.0, a.t);
    a.run(milliseconds(400));
    const float hurt = session_stat(*a.s, content::kGuidHealth);
    CHECK(hurt < max_h, "(the player is hurt, out of the watcher's sight)");
    CHECK(sc.channel(pawn_id) == 0, "(the watcher still holds nothing of the player)");
    const GoodPosition at = a.s->movement().good();
    const PlayerSession::GmMoveResult moved = b.s->gm_teleport(b.conn, at.x + 50.0, at.y, at.z, b.t, "the test");
    CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "(the watcher is put beside the player)");
    a.run(milliseconds(500));
    const world::WorldActor* pawn2 = shown_pawn(a);
    const uint32_t pch = pawn2 ? sc.channel(pawn2->id) : 0;
    const fake::Delivered* open = pch != 0 ? open_on(b, pch) : nullptr;
    CHECK(open != nullptr, "(its view opens the player's pawn)");
    if (!open) return;
    const auto held = sent_array(*open, pawn2->actor, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
    const auto open_health = held ? stat_in(*held, content::kGuidHealth) : std::nullopt;
    const float now = session_stat(*a.s, content::kGuidHealth);
    CHECK(open_health && *open_health >= hurt && *open_health <= now + 1e-3f && *open_health < max_h,
          "the open carries the Health the player has now (within the regeneration since it was set), not the maximum");
    CHECK(held && stat_in(*held, content::kGuidMaxHealth) == max_h, "...and the maximum");
}
AOC_REGISTER_TEST(test_a_player_who_comes_into_view_is_shown_the_current_health_in_the_open);

// ---- a player who leaves view and comes back ------------------------------------------------------------------------------------------

static void test_a_player_who_leaves_view_and_comes_back_is_shown_the_current_values() {
    std::printf("a player who leaves the watcher's view and comes back is shown the values it has now, and the changes between are not replayed\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    const uint32_t pch = sc.channel(pawn_id);
    const float max_h = session_stat(*a.s, content::kGuidMaxHealth);
    // the watcher leaves: 300 m east
    const GoodPosition at = a.s->movement().good();
    const size_t from = b.got.size();
    CHECK(b.s->gm_teleport(b.conn, at.x + 30000.0, at.y, at.z, b.t, "the test").status == PlayerSession::GmMoveResult::Status::Moved, "(the watcher leaves)");
    a.run(milliseconds(500));
    const auto closes = closes_on(b, pch, from);
    CHECK(closes.size() == 1 && closes[0] == kCloseRelevancy, "the watcher's channel of the pawn was closed with the relevancy close (out of range)");
    CHECK(sc.channel(pawn_id) == 0, "(the watcher holds nothing of the player)");
    // the change happens out of sight
    const size_t from_change = b.got.size();
    a.s->take_damage(a.conn, 70.0, a.t);
    a.run(milliseconds(300));
    const float hurt = session_stat(*a.s, content::kGuidHealth);
    CHECK(hurt < max_h && stat_deltas_on(b, pch, pawn->actor, from_change, content::kGuidHealth).empty(), "(the player is hurt out of sight, and no old delta was sent)");
    // it comes back
    CHECK(b.s->gm_teleport(b.conn, at.x + 50.0, at.y, at.z, b.t, "the test").status == PlayerSession::GmMoveResult::Status::Moved, "(the watcher comes back)");
    a.run(milliseconds(500));
    const world::WorldActor* pawn2 = shown_pawn(a);
    const uint32_t pch2 = pawn2 ? sc.channel(pawn2->id) : 0;
    const fake::Delivered* open = pch2 != 0 ? open_on(b, pch2, from_change) : nullptr;
    CHECK(open != nullptr, "(its view opens the player's pawn again)");
    if (!open) return;
    const auto held = sent_array(*open, pawn2->actor, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
    const auto open_health = held ? stat_in(*held, content::kGuidHealth) : std::nullopt;
    const float now = session_stat(*a.s, content::kGuidHealth);
    CHECK(open_health && *open_health >= hurt && *open_health <= now + 1e-3f && *open_health < max_h,
          "the new open carries the Health the player has now (within the regeneration since it was set), not the maximum");
}
AOC_REGISTER_TEST(test_a_player_who_leaves_view_and_comes_back_is_shown_the_current_values);

// ---- the maxima: a level and a piece of gear -----------------------------------------------------------------------------------------

static void test_a_level_up_is_shown_with_its_new_maxima() {
    std::printf("a player's level-up is shown to the watcher with the new maxima and the values the new maxima clamp\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const float max_h0 = session_stat(*a.s, content::kGuidMaxHealth), max_m0 = session_stat(*a.s, content::kGuidMaxMana);
    const size_t from = b.got.size();
    const PlayerSession::GmXpResult r = a.s->gm_give_experience(a.conn, 350, a.t, "the test");
    CHECK(r.ok && r.leveled && r.level_after == 2, "(350 XP crosses level 2)");
    a.run(milliseconds(400));
    const float max_h1 = session_stat(*a.s, content::kGuidMaxHealth), max_m1 = session_stat(*a.s, content::kGuidMaxMana);
    CHECK(max_h1 > max_h0 && max_m1 > max_m0, "(the level raised both maxima)");
    const auto hp_max = stat_deltas_on(b, pch, pawn_copy, from, content::kGuidMaxHealth);
    const auto mp_max = stat_deltas_on(b, pch, pawn_copy, from, content::kGuidMaxMana);
    CHECK(!hp_max.empty() && hp_max.back().value == max_h1, "the watcher is sent the new maximum Health");
    CHECK(!mp_max.empty() && mp_max.back().value == max_m1, "...and the new maximum Mana");
    CHECK(shown_stat(*shown_pawn(a), content::kGuidMaxHealth) == max_h1 && shown_stat(*shown_pawn(a), content::kGuidHealth) == session_stat(*a.s, content::kGuidHealth),
          "the pawn the watchers hold has the new maximum and the Health the player has");
}
AOC_REGISTER_TEST(test_a_level_up_is_shown_with_its_new_maxima);

/// The starter shield: MaxHealth +14 at the common quality, so wearing it raises the maximum Health.
static constexpr int64_t kShield = 6064630695512702988ll;

static void test_gear_that_changes_the_maximum_is_shown_live() {
    std::printf("a piece of gear that raises the maximum Health is shown to the watcher on the pawn it holds\n");
    std::vector<svc::InventoryItem> items = kit();
    items.push_back(make_item(Bag, 10, kShield, 401));
    Harness player(cyb_record(std::move(items)));
    player.play();
    Players all;
    auto watcher = join_world(player, all, other_record(other_id(2), "ayla", 4));
    player.run(milliseconds(300));
    const world::WorldActor* pawn = shown_pawn(player);
    CHECK(pawn != nullptr && watcher->s->actor_view() && watcher->s->actor_view()->channel_of(pawn->id) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = watcher->s->actor_view()->channel_of(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const float max_h0 = session_stat(*player.s, content::kGuidMaxHealth);
    const auto shield = player.s->inventory().at(Bag, 10);
    CHECK(shield && shield->item_record_id == kShield, "(the shield is in the bag)");
    if (!shield) return;
    const size_t from = watcher->got.size();
    player.client_sends({player.client.actor(player.pawn_ch, item_used(3, 10, shield->instance_guid), true)});
    player.run(milliseconds(300));
    CHECK(player.s->inventory().at(Worn, 16) && player.s->inventory().at(Worn, 16)->item_record_id == kShield, "(the shield is worn)");
    const float max_h1 = session_stat(*player.s, content::kGuidMaxHealth);
    CHECK(max_h1 > max_h0, "(the shield raised the maximum Health)");
    const auto deltas = stat_deltas_on(*watcher, pch, pawn_copy, from, content::kGuidMaxHealth);
    CHECK(!deltas.empty() && deltas.back().value == max_h1, "the watcher is sent the new maximum Health");
    CHECK(shown_stat(*shown_pawn(player), content::kGuidMaxHealth) == max_h1, "the pawn the watchers hold has it");
}
AOC_REGISTER_TEST(test_gear_that_changes_the_maximum_is_shown_live);

// ---- a rider ---------------------------------------------------------------------------------------------------------------------------

static void test_a_rider_is_still_shown_with_its_health_and_mana() {
    std::printf("a player who rides is still shown with its Health and Mana on the seated pawn, and a hit on the mount does not move them\n");
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
    const repl::Actor pawn_copy = pawn->actor;
    const float max_h = session_stat(*a.s, content::kGuidMaxHealth);
    const float before = session_stat(*a.s, content::kGuidHealth);
    // a hit on the mount: the rider's own Health is untouched, so nothing of it goes to the watcher
    const size_t from_hit = b.got.size();
    a.s->take_damage(a.conn, 40.0, a.t);
    a.run(milliseconds(250));
    CHECK(session_stat(*a.s, content::kGuidHealth) == before, "(the damage went to the mount, not to the rider)");
    CHECK(stat_deltas_on(b, pch, pawn_copy, from_hit, content::kGuidHealth).empty(), "...and the watcher is sent no Health delta for it");
    // the rider's own value changes (a hurt, then a heal to full): the seated pawn's channel carries it and the attachment stays
    CHECK(a.s->set_current_value(content::kGuidHealth, max_h - 30.0f), "(the rider is hurt)");
    a.run(milliseconds(1200));
    const size_t from = b.got.size();
    CHECK(a.s->set_current_value(content::kGuidHealth, max_h), "(a heal fills the rider's Health)");
    a.run(milliseconds(400));
    CHECK(a.s->mounted(), "(the rider still rides)");
    const auto deltas = stat_deltas_on(b, pch, pawn_copy, from, content::kGuidHealth);
    CHECK(deltas.size() == 1 && deltas[0].value == max_h, "the watcher is sent the seated pawn's Health delta");
    const world::WorldActor* now = shown_pawn(a);
    CHECK(now != nullptr && same_object(actor_object(now->actor, "AttachmentReplication.AttachParent"), mount->actor.guid), "...and the pawn is still attached to the mount");
}
AOC_REGISTER_TEST(test_a_rider_is_still_shown_with_its_health_and_mana);

// ---- a relog ---------------------------------------------------------------------------------------------------------------------------

static void test_a_relogged_player_is_shown_with_its_health_and_the_changes_go_on() {
    std::printf("a player who logs out and back in is shown again with its current Health, and its next change is sent too\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    a.s->take_damage(a.conn, 80.0, a.t);
    a.run(milliseconds(300));
    const GoodPosition saved = a.s->last_position();
    const size_t from = b.got.size();
    a.s->finish("the test");
    a.run(milliseconds(250));
    CHECK(shown_pawn(a) == nullptr && closes_on(b, pch, from).size() == 1, "(the pawn left the registry and the watcher's channel was closed)");
    svc::Character again = cyb_record();
    again.has_position = true;
    again.position.x = saved.x;
    again.position.y = saved.y;
    again.position.z = saved.z;
    auto a2 = join_world(a, sc.all, again);
    a.run(milliseconds(600));
    const world::WorldActor* pawn2 = shown_pawn(*a2);
    const uint32_t pch2 = pawn2 ? sc.channel(pawn2->id) : 0;
    const fake::Delivered* open = pch2 != 0 ? open_on(b, pch2, from) : nullptr;
    CHECK(open != nullptr, "(the watcher is sent the returned player's pawn)");
    if (!open) return;
    const auto held = sent_array(*open, pawn2->actor, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
    const float full = session_stat(*a2->s, content::kGuidMaxHealth);
    CHECK(held && stat_in(*held, content::kGuidHealth) == full, "the open carries the returned player's Health (a new life starts full)");
    // a change after the relog reaches the watcher again
    const size_t from2 = b.got.size();
    const repl::Actor pawn_copy = pawn2->actor;
    a2->s->take_damage(a2->conn, 50.0, a2->t);
    a.run(milliseconds(300));
    const float hurt = session_stat(*a2->s, content::kGuidHealth);
    const auto deltas = stat_deltas_on(b, pch2, pawn_copy, from2, content::kGuidHealth);
    CHECK(hurt < full && !deltas.empty() && deltas.back().value == hurt, "a change after the relog is sent once more");
}
AOC_REGISTER_TEST(test_a_relogged_player_is_shown_with_its_health_and_the_changes_go_on);

// ---- two players side by side ---------------------------------------------------------------------------------------------------------

static void test_two_players_health_changes_side_by_side() {
    std::printf("two players who change their Health side by side are each seen by the watcher, on their own pawn's channel, and by each other\n");
    Harness a(cyb_record());
    a.play();
    Players all;
    auto c = join_world(a, all, other_record(other_id(3), "kira", 5));
    auto b = join_world(a, all, other_record(other_id(2), "ayla", 4));
    a.run(milliseconds(300));
    const world::WorldActor* pa = shown_pawn(a);
    const world::WorldActor* pc = shown_pawn(*c);
    CHECK(pa != nullptr && pc != nullptr, "(the two players' pawns are in the world)");
    if (!pa || !pc) return;
    const uint32_t pa_b = b->s->actor_view()->channel_of(pa->id), pc_b = b->s->actor_view()->channel_of(pc->id);
    const uint32_t pa_c = c->s->actor_view()->channel_of(pa->id), pc_a = a.s->actor_view()->channel_of(pc->id);
    CHECK(pa_b != 0 && pc_b != 0 && pa_b != pc_b, "the watcher holds both pawns, each on a channel of its own");
    CHECK(pa_c != 0 && pc_a != 0, "the two players hold each other's pawn");
    if (!pa_b || !pc_b || !pa_c || !pc_a) return;
    const repl::Actor pa_copy = pa->actor, pc_copy = pc->actor;
    const size_t from_b = b->got.size(), from_a = a.got.size(), from_c = c->got.size();
    a.s->take_damage(a.conn, 60.0, a.t);
    c->s->take_damage(c->conn, 30.0, c->t);
    a.run(milliseconds(300));
    const auto b_sees_a = stat_deltas_on(*b, pa_b, pa_copy, from_b, content::kGuidHealth);
    const auto b_sees_c = stat_deltas_on(*b, pc_b, pc_copy, from_b, content::kGuidHealth);
    const auto a_sees_c = stat_deltas_on(a, pc_a, pc_copy, from_a, content::kGuidHealth);
    const auto c_sees_a = stat_deltas_on(*c, pa_c, pa_copy, from_c, content::kGuidHealth);
    CHECK(!b_sees_a.empty() && b_sees_a.back().value == session_stat(*a.s, content::kGuidHealth), "the watcher is sent the first player's Health delta");
    CHECK(!b_sees_c.empty() && b_sees_c.back().value == session_stat(*c->s, content::kGuidHealth), "and the second player's too, on its own channel");
    CHECK(!a_sees_c.empty() && a_sees_c.back().value == session_stat(*c->s, content::kGuidHealth), "the first player is sent the second's change");
    CHECK(!c_sees_a.empty() && c_sees_a.back().value == session_stat(*a.s, content::kGuidHealth), "the second player is sent the first's");
}
AOC_REGISTER_TEST(test_two_players_health_changes_side_by_side);

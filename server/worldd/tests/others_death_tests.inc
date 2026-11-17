// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/others_death_tests.inc: other players see a player's death, his respawn and a resurrection in place (Social, seeing other players: their death)
//
//  A player (cyb by default) and one or two others in one world (tests/mounts_ride_others_rig.h): the player dies, respawns, dies while mounted, comes back after a death and
//  dies next to another dead player; what a watcher's client is sent is read chain by chain (the update of the pawn the watchers hold): the death bit (CombatInfo.bIsDead)
//  with Health 0 and Mana 0 in ONE update, and the alive bit with the new values and the place after a respawn.
//
//  The Resurrect runs on the real content.db (its ability tables and the world's fights are only there, session/ability_resurrect.h): a Cleric casts Resurrection on a dead
//  ally, the client's ServerResurrect confirms it, the dead player is asked (ClientPromptResurrect) and his answer (ServerPromptedResurrectResponse) brings him back in place
//  with the record's 25 % of Health and Mana; the watchers see the death bit clear with the new bars in one update.  Also the refusals: a player who is not a Cleric, a
//  target beyond the cast's reach, a living target, and a declined prompt.
//
//  Modelled: no recording of a resurrect exists, so the flow is the client's calls as the generated schema names them (ServerResurrect, ClientPromptResurrect,
//  ServerPromptedResurrectResponse): the cast is the trigger, the request confirms it.  The ratios are the game's own record's (0.25/0.25 of the maxima, read from the two
//  ResurrectRecordBase rows of the design data); contentc does not export that table, so they are written in session/ability_resurrect.h.
// ============================================================================

/// The Cleric_Resurrect ability and the ResurrectRecordBase its hit names (the game's data; the ability is auto-granted at Cleric level 6).
static constexpr int64_t kResurrectAbility = 6064630847771377729ll;

/// The subobject of class `cls` on the pawn the watchers hold (0: none).
static uint64_t death_component(const world::WorldActor& pawn, const char* cls) {
    const repl::Subobject* sub = spawns::sub_of(pawn, cls);
    return sub ? sub->guid.ObjectId : 0;
}

/// The CombatInfo.bIsDead a chain carries for the pawn (nullopt: the chain does not say).
static std::optional<int64_t> dead_in(const fake::Delivered& d, const repl::Actor& pawn, uint64_t combat_object) {
    if (combat_object == 0) return std::nullopt;
    return number_in(chain_states(d, pawn), combat_object, "bIsDead");
}

/// The death bit the pawn the watchers hold carries now (nullopt: no CombatInfo or no value).
static std::optional<int64_t> pawn_dead(const world::WorldActor& pawn) {
    const repl::Subobject* combat = spawns::sub_of(pawn, "UPlayerCombatInfo");
    if (!combat || !combat->obj.schema) return std::nullopt;
    const repl::Value* v = combat->obj.state.get(*combat->obj.schema, "bIsDead");
    if (!v) return std::nullopt;
    if (const auto* b = v->get_if<bool>()) return *b ? 1 : 0;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    return std::nullopt;
}

/// One chain of the watcher on `channel` that carries CombatInfo.bIsDead = `want`, with the Health and Mana items that same chain also carries.
struct DeathFrame {
    bool  has_health = false, has_mana = false;
    float health = 0, mana = 0;
};
static std::optional<DeathFrame> frame_with_bit(const Harness& watcher, uint32_t channel, const repl::Actor& pawn, uint64_t combat_object, size_t from, int64_t want) {
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close) continue;
        const auto bit = dead_in(d, pawn, combat_object);
        if (!bit || *bit != want) continue;
        DeathFrame out;
        const auto arr = sent_array(d, pawn, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
        if (arr) {
            if (const auto v = stat_in(*arr, content::kGuidHealth)) out.health = *v, out.has_health = true;
            if (const auto v = stat_in(*arr, content::kGuidMana)) out.mana = *v, out.has_mana = true;
        }
        return out;
    }
    return std::nullopt;
}

/// A cast Resurrection at `target` as the Cleric's client sends it: ServerTriggerAbility on the caster's AbilityComponent.
static BunchWriter resurrect_trigger(const emit::FIntrepidNetworkGUID& component, const emit::FIntrepidNetworkGUID& target, int32_t instance) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(kResurrectAbility));
    if (ok && instance != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(instance));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = repl::Value(target);
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) {
        std::printf("  [FAIL] the Resurrection trigger's parameters do not fit\n");
        return BunchWriter();
    }
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, p, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] ServerTriggerAbility does not write\n");
        return BunchWriter();
    }
    BunchWriter out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = component;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// ServerResurrect(CharacterName) on the PlayerController: the resurrector's client confirms the cast.
static BunchWriter resurrect_request(const std::string& name) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerResurrect");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "CharacterName", repl::Value::str(name))) {
        std::printf("  [FAIL] ServerResurrect's parameters do not fit\n");
        return BunchWriter();
    }
    return actor_rpc(pc, "ServerResurrect", p);
}

/// ServerPromptedResurrectResponse(Response) on the dead player's PlayerController: his answer to the prompt.
static BunchWriter resurrect_response(bool yes) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerPromptedResurrectResponse");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "Response", repl::Value::boolean(yes))) {
        std::printf("  [FAIL] ServerPromptedResurrectResponse's parameter does not fit\n");
        return BunchWriter();
    }
    return actor_rpc(pc, "ServerPromptedResurrectResponse", p);
}

/// One ClientPromptResurrect of a chain: whether it is there and the instigator's four words.
struct ResurrectPrompt {
    bool                    present = false;
    std::array<uint32_t, 4> instigator{};
};
static std::optional<ResurrectPrompt> prompt_in(const fake::Delivered& d) {
    for (const repl::RpcCall& c : calls_of(d, "AoCPlayerControllerBP_C")) {
        if (!c.rpc || std::string(c.rpc->name) != "ClientPromptResurrect") continue;
        ResurrectPrompt p;
        p.present = true;
        for (int i = 0; i < 4; ++i) {
            if (const auto v = repl::rpcf::int_or(repl::rpcf::value_of(*c.rpc, c.params, std::string("InstigatorGuid.") + "ABCD"[i]), 0))
                p.instigator[static_cast<size_t>(i)] = static_cast<uint32_t>(*v);
        }
        return p;
    }
    return std::nullopt;
}

/// The resurrection prompts a player's client got from chain `from` on.
static std::vector<ResurrectPrompt> prompts_in(const Harness& h, size_t from) {
    std::vector<ResurrectPrompt> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].close) continue;
        if (const auto p = prompt_in(h.got[i])) out.push_back(*p);
    }
    return out;
}

/// The four words of a session's character GUID.
static std::array<uint32_t, 4> character_words(const PlayerSession& s) {
    const auto& g = s.bootstrap().setup().character_guid;
    return {g[0], g[1], g[2], g[3]};
}

/// A player's record with a class and a level (the party's other players, the Cleric).
static svc::Character death_record(const std::string& id, const std::string& name, int account, int class_id, int level) {
    svc::Character c = other_record(id, name, account);
    c.class_id = class_id;
    c.class_name = class_id == 3 ? "Cleric" : "Bard";
    c.level = level;
    return c;
}

/// The ground and the fights of a real-content world (the Resurrect's target checks and the ability runtime need the world's combat).
static void prepare_combat_ground(PlayerWorld& w) {
    std::string err;
    auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
    if (!nav || !w.adopt_ground(std::move(nav), err)) std::printf("  [FAIL] the ground: %s\n", err.c_str());
}

/// Server SendServerRespawnChoice(RespawnChoice) on the pawn.
static BunchWriter respawn_choice_call(int64_t record) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("SendServerRespawnChoice");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "RespawnChoice", repl::Value::i64(record))) {
        std::printf("  [FAIL] SendServerRespawnChoice's parameter does not fit\n");
        return BunchWriter();
    }
    return actor_rpc(pawn, "SendServerRespawnChoice", p);
}

/// The player who dies (cyb) and a watcher (ayla) at the same place, or `watch_dx` cm east of it.
struct DeathScene {
    Harness                  a;
    Players                  all;
    std::unique_ptr<Harness> b;
    explicit DeathScene(double watch_dx = 0) : a(cyb_record()) {
        a.play();
        std::optional<std::pair<double, double>> at;
        if (watch_dx != 0) {
            const GoodPosition g = a.s->movement().good();
            at = std::make_pair(g.x + watch_dx, g.y);
        }
        b = join_world(a, all, other_record(other_id(2), "ayla", 4, at));
        a.run(milliseconds(300));
    }
    uint32_t channel(uint64_t actor) const { return b->s->actor_view() ? b->s->actor_view()->channel_of(actor) : 0; }
};

// ---- the death the watchers are sent ----------------------------------------------------------------------------------------------------

static void test_other_players_see_a_players_death_in_one_update() {
    std::printf("a player who dies is shown dead to the watcher: one update carries bIsDead with Health 0 and Mana 0, and the pawn stays\n");
    DeathScene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    const uint32_t pch = sc.channel(pawn_id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = death_component(*pawn, "UPlayerCombatInfo");
    CHECK(combat != 0, "(the pawn the watcher holds has its CombatInfo)");
    const float max_h = session_stat(*a.s, content::kGuidMaxHealth), max_m = session_stat(*a.s, content::kGuidMaxMana);
    const size_t from = b.got.size();
    CHECK(a.s->gm_kill(a.conn, a.t, "the test").ok && a.s->dead(), "(the player dies)");
    a.run(milliseconds(400));
    const auto health = stat_deltas_on(b, pch, pawn_copy, from, content::kGuidHealth);
    const auto mana = stat_deltas_on(b, pch, pawn_copy, from, content::kGuidMana);
    CHECK(!health.empty() && health.back().value == 0.0f, "the watcher is sent Health 0");
    CHECK(!mana.empty() && mana.back().value == 0.0f, "...and Mana 0");
    const auto frame = frame_with_bit(b, pch, pawn_copy, combat, from, 1);
    CHECK(frame.has_value(), "one update of the pawn carries bIsDead 1");
    CHECK(frame && frame->has_health && frame->health == 0.0f && frame->has_mana && frame->mana == 0.0f,
          "...and that same update carries Health 0 and Mana 0 (the death frame retail sends)");
    CHECK(pawn_dead(*shown_pawn(a)) == 1, "the pawn the watchers hold says dead");
    CHECK(shown_stat(*shown_pawn(a), content::kGuidMaxHealth) == max_h && shown_stat(*shown_pawn(a), content::kGuidMaxMana) == max_m, "...and keeps its maxima");
    // a dead player stays dead and quiet: no further death-bit update, no bar moves
    const size_t from_dead = b.got.size();
    a.run(milliseconds(2000));
    CHECK(!frame_with_bit(b, pch, pawn_copy, combat, from_dead, 1).has_value(), "a dead player's bit is not sent again while he stays dead");
    CHECK(stat_deltas_on(b, pch, pawn_copy, from_dead, content::kGuidHealth).empty() && stat_deltas_on(b, pch, pawn_copy, from_dead, content::kGuidMana).empty(),
          "...and nothing regenerates while he is dead");
    CHECK(sc.channel(pawn_id) == pch && closes_on(b, pch, from).empty(), "the pawn's channel stays open: the pawn is the corpse");
}
AOC_REGISTER_TEST(test_other_players_see_a_players_death_in_one_update);

static void test_a_player_who_comes_into_view_while_dead_is_seen_dead() {
    std::printf("a watcher who comes into view while the player is dead is shown him dead, with empty bars, in the open\n");
    Scene sc(30000);   // the watcher stands 300 m east of where the player starts
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) == 0, "300 m away the watcher holds nothing of the player");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    CHECK(a.s->gm_kill(a.conn, a.t, "the test").ok, "(the player dies out of the watcher's sight)");
    a.run(milliseconds(400));
    CHECK(sc.channel(pawn_id) == 0, "(the watcher still holds nothing of the player)");
    const GoodPosition at = a.s->movement().good();
    CHECK(b.s->gm_teleport(b.conn, at.x + 50.0, at.y, at.z, b.t, "the test").status == PlayerSession::GmMoveResult::Status::Moved, "(the watcher is put beside the dead player)");
    a.run(milliseconds(500));
    const world::WorldActor* pawn2 = shown_pawn(a);
    const uint32_t pch = pawn2 ? sc.channel(pawn2->id) : 0;
    const fake::Delivered* open = pch != 0 ? open_on(b, pch) : nullptr;
    CHECK(open != nullptr, "(its view opens the dead player's pawn)");
    if (!open) return;
    const uint64_t combat = death_component(*pawn2, "UPlayerCombatInfo");
    CHECK(dead_in(*open, pawn2->actor, combat) == 1, "the open carries bIsDead 1");
    const auto held = sent_array(*open, pawn2->actor, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
    CHECK(held && stat_in(*held, content::kGuidHealth) == 0.0f && stat_in(*held, content::kGuidMana) == 0.0f, "...and the zero Health and Mana the dead player has");
}
AOC_REGISTER_TEST(test_a_player_who_comes_into_view_while_dead_is_seen_dead);

static void test_a_respawn_elsewhere_is_seen_alive_with_full_bars_and_the_new_place() {
    std::printf("a player who dies away from the login spot and respawns there is shown alive again, with full bars, where he respawns; the same pawn\n");
    DeathScene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    // he dies 50 m east of the spot the respawn returns him to (the world has no Emberspring: the nearest point is the login spot)
    const GoodPosition login = a.s->movement().good();
    CHECK(a.s->gm_teleport(a.conn, login.x + 5000.0, login.y, login.z, a.t, "the test").status == PlayerSession::GmMoveResult::Status::Moved, "(the player walks 50 m off the login spot)");
    a.run(milliseconds(300));
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    const uint32_t pch = sc.channel(pawn_id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = death_component(*pawn, "UPlayerCombatInfo");
    const size_t from_death = b.got.size();
    CHECK(a.s->gm_kill(a.conn, a.t, "the test").ok, "(the player dies there)");
    a.run(milliseconds(400));
    const GoodPosition died_at = a.s->movement().good();
    CHECK(frame_with_bit(b, pch, pawn_copy, combat, from_death, 1).has_value(), "(the watcher saw his death)");
    const float max_h = session_stat(*a.s, content::kGuidMaxHealth), max_m = session_stat(*a.s, content::kGuidMaxMana);
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, respawn_choice_call(PlayerSession::kRespawnRecord), /*reliable=*/true)});
    a.run(milliseconds(700));
    CHECK(!a.s->dead() && a.s->respawns() == 1, "(the player respawns, as today)");
    const GoodPosition at = a.s->movement().good();
    const world::WorldActor* now = shown_pawn(a);
    CHECK(now != nullptr && (std::fabs(now->x - died_at.x) > 1.0 || std::fabs(now->y - died_at.y) > 1.0), "(he is not where he died)");
    CHECK(now != nullptr && std::fabs(now->x - at.x) < 2.0 && std::fabs(now->y - at.y) < 2.0, "...the pawn the watchers hold is where he respawned");
    const auto frame = frame_with_bit(b, pch, pawn_copy, combat, from, 0);
    CHECK(frame.has_value(), "one update of the same pawn carries bIsDead 0");
    CHECK(frame && frame->has_health && frame->health == max_h && frame->has_mana && frame->mana == max_m, "...with the new life's full Health and Mana");
    CHECK(pawn_dead(*shown_pawn(a)) == 0 && shown_stat(*shown_pawn(a), content::kGuidHealth) == max_h && shown_stat(*shown_pawn(a), content::kGuidMana) == max_m,
          "the pawn the watchers hold says alive with the full bars");
    CHECK(pawn_id != 0 && sc.channel(pawn_id) == pch, "(the same pawn, on the same channel)");
}
AOC_REGISTER_TEST(test_a_respawn_elsewhere_is_seen_alive_with_full_bars_and_the_new_place);

static void test_a_player_who_dies_while_mounted_is_seen_dead() {
    std::printf("a player who dies while mounted is shown dead and dismounted: the seated pawn gets the death bit and the empty bars\n");
    DeathScene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the player rides)");
    if (!a.s->mounted()) return;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the seated pawn)");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    const uint32_t pch = sc.channel(pawn_id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = death_component(*pawn, "UPlayerCombatInfo");
    const size_t from = b.got.size();
    CHECK(a.s->set_current_value(content::kGuidHealth, 0.0f), "(the rider's Health falls to 0)");
    a.run(milliseconds(400));
    CHECK(a.s->dead() && !a.s->mounted(), "(the player died and the death dismounted him)");
    const auto frame = frame_with_bit(b, pch, pawn_copy, combat, from, 1);
    CHECK(frame.has_value(), "the watcher is sent bIsDead 1 on the pawn he held seated");
    CHECK(frame && frame->has_health && frame->health == 0.0f && frame->has_mana && frame->mana == 0.0f, "...with Health 0 and Mana 0 in that same update");
    CHECK(pawn_dead(*shown_pawn(a)) == 1, "the pawn the watchers hold says dead");
    CHECK(sc.channel(pawn_id) == pch, "(the pawn stays: the corpse is the pawn)");
}
AOC_REGISTER_TEST(test_a_player_who_dies_while_mounted_is_seen_dead);

static void test_a_player_who_comes_back_after_a_death_is_seen_alive_in_a_new_life() {
    std::printf("a player who logs out dead and back in is shown alive again with a new life's bars, on a new pawn\n");
    DeathScene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    CHECK(a.s->gm_kill(a.conn, a.t, "the test").ok, "(the player dies)");
    a.run(milliseconds(400));
    const GoodPosition saved = a.s->last_position();
    const size_t from = b.got.size();
    a.s->finish("the test");
    a.run(milliseconds(300));
    CHECK(shown_pawn(a) == nullptr && closes_on(b, pch, from).size() == 1, "(the dead player's pawn leaves the registry and the watcher's channel closes)");
    svc::Character again = cyb_record();
    again.has_position = true;
    again.position.x = saved.x;
    again.position.y = saved.y;
    again.position.z = saved.z;
    auto a2 = join_world(a, sc.all, again);
    a.run(milliseconds(700));
    const world::WorldActor* pawn2 = shown_pawn(*a2);
    const uint32_t pch2 = pawn2 ? sc.channel(pawn2->id) : 0;
    const fake::Delivered* open = pch2 != 0 ? open_on(b, pch2, from) : nullptr;
    CHECK(open != nullptr, "(the watcher is sent the returned player's pawn)");
    if (!open) return;
    const uint64_t combat = death_component(*pawn2, "UPlayerCombatInfo");
    CHECK(dead_in(*open, pawn2->actor, combat) == 0, "the open says alive: the death is not his new life's");
    const auto held = sent_array(*open, pawn2->actor, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
    const float full = session_stat(*a2->s, content::kGuidMaxHealth);
    CHECK(held && stat_in(*held, content::kGuidHealth) == full, "...with the returned player's full Health");
    const size_t from2 = b.got.size();
    a2->s->take_damage(a2->conn, 50.0, a2->t);
    a.run(milliseconds(300));
    CHECK(!stat_deltas_on(b, pch2, pawn2->actor, from2, content::kGuidHealth).empty() && pawn_dead(*shown_pawn(*a2)) == 0, "...and his changes go on, his bit alive");
}
AOC_REGISTER_TEST(test_a_player_who_comes_back_after_a_death_is_seen_alive_in_a_new_life);

static void test_two_players_dead_side_by_side_are_seen_on_their_own_pawns() {
    std::printf("two players who die side by side are each seen dead on his own pawn, and each lives again on his own\n");
    Harness a(cyb_record());
    a.play();
    Players all;
    auto c = join_world(a, all, other_record(other_id(3), "kira", 5));
    auto b = join_world(a, all, other_record(other_id(2), "ayla", 4));
    a.run(milliseconds(400));
    const world::WorldActor* pa = shown_pawn(a);
    const world::WorldActor* pc = shown_pawn(*c);
    CHECK(pa != nullptr && pc != nullptr, "(the two players' pawns are in the world)");
    if (!pa || !pc) return;
    const uint32_t pa_b = b->s->actor_view()->channel_of(pa->id), pc_b = b->s->actor_view()->channel_of(pc->id);
    CHECK(pa_b != 0 && pc_b != 0 && pa_b != pc_b, "the watcher holds both pawns, each on a channel of its own");
    if (!pa_b || !pc_b) return;
    const repl::Actor pa_copy = pa->actor, pc_copy = pc->actor;
    const uint64_t pa_combat = death_component(*pa, "UPlayerCombatInfo"), pc_combat = death_component(*pc, "UPlayerCombatInfo");
    const size_t from = b->got.size();
    CHECK(a.s->gm_kill(a.conn, a.t, "the test").ok && c->s->gm_kill(c->conn, c->t, "the test").ok, "(both players die)");
    a.run(milliseconds(500));
    const auto fa = frame_with_bit(*b, pa_b, pa_copy, pa_combat, from, 1);
    const auto fc = frame_with_bit(*b, pc_b, pc_copy, pc_combat, from, 1);
    CHECK(fa.has_value() && fa->has_health && fa->health == 0.0f && fa->has_mana && fa->mana == 0.0f, "the watcher is sent the first player's death frame");
    CHECK(fc.has_value() && fc->has_health && fc->health == 0.0f && fc->has_mana && fc->mana == 0.0f, "...and the second's, on its own channel");
    CHECK(pawn_dead(*shown_pawn(a)) == 1 && pawn_dead(*shown_pawn(*c)) == 1, "each pawn the watcher holds says dead");
    a.s->gm_revive(a.conn, a.t, "the test");
    a.run(milliseconds(500));
    CHECK(pawn_dead(*shown_pawn(a)) == 0 && pawn_dead(*shown_pawn(*c)) == 1, "the first is alive again and the second still dead");
    CHECK(shown_stat(*shown_pawn(a), content::kGuidHealth) == session_stat(*a.s, content::kGuidHealth), "...with his new Health");
    CHECK(shown_stat(*shown_pawn(*c), content::kGuidHealth) == 0.0f, "...and the second's pawn keeps its zero");
}
AOC_REGISTER_TEST(test_two_players_dead_side_by_side_are_seen_on_their_own_pawns);

// ---- the Resurrect on a dead ally, on the real content.db ---------------------------------------------------------------

/// Four players in one real-content world: the Cleric (cyb's record as class 3, level 10), the ally who dies, a second Bard and a watcher.
struct ResurrectScene {
    Harness                  cleric;
    Players                  all;
    std::unique_ptr<Harness> dead, bard, watch;
    ResurrectScene()
        : cleric(death_record(other_id(1), "cleric", 6, 3, 10), prepare_combat_ground, nullptr, t0, /*real_content=*/true) {
        cleric.play();
        dead = join_world(cleric, all, death_record(other_id(2), "deadman", 7, 2, 1));
        bard = join_world(cleric, all, death_record(other_id(3), "bardman", 8, 2, 1));
        watch = join_world(cleric, all, death_record(other_id(4), "ayla", 9, 2, 1));
        cleric.run(milliseconds(400));
    }
    bool ok() const { return cleric.fx.world->combat() != nullptr && dead && bard && watch && dead->s && watch->s; }
    uint32_t channel_of(const world::WorldActor& pawn) const { return watch->s->actor_view() ? watch->s->actor_view()->channel_of(pawn.id) : 0; }
};

static void test_a_resurrection_revives_a_dead_ally_in_place_and_the_watchers_see_it() {
    std::printf("the Cleric's Resurrection: the prompt, the answer and the revive in place with the record's bars; the watchers see the death bit clear\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on: the ability runtime has its target port)");
    CHECK(std::find(sc.cleric.s->known_abilities().begin(), sc.cleric.s->known_abilities().end(), kResurrectAbility) != sc.cleric.s->known_abilities().end(),
          "(the level-10 Cleric knows Resurrection: its level grants it)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    Harness& dead = *sc.dead;
    Harness& watch = *sc.watch;
    const world::WorldActor* pawn = shown_pawn(dead);
    const uint32_t pch = pawn ? sc.channel_of(*pawn) : 0;
    CHECK(pawn != nullptr && pch != 0, "(the watcher holds the ally's pawn)");
    if (!pawn || pch == 0) return;
    const uint64_t combat = death_component(*pawn, "UPlayerCombatInfo");
    const repl::Actor pawn_copy = pawn->actor;
    const float max_h = session_stat(*dead.s, content::kGuidMaxHealth), max_m = session_stat(*dead.s, content::kGuidMaxMana);
    // a living player is no target of the Resurrection
    const size_t casts_living = cleric.s->pipeline() ? cleric.s->pipeline()->casts_started() : 0;
    const size_t from_living = watch.got.size();
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_trigger(cleric.s->bootstrap().setup().guids.ability, dead.s->bootstrap().setup().guids.pawn, 1), true)});
    cleric.run(milliseconds(300));
    CHECK(cleric.s->pipeline() && cleric.s->pipeline()->casts_started() == casts_living, "a Resurrection cast at a living player is refused: no cast starts");
    CHECK(!frame_with_bit(watch, pch, pawn_copy, combat, from_living, 1).has_value(), "...and nothing of a death is sent");
    // the death
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(the ally dies)");
    cleric.run(milliseconds(300));
    CHECK(frame_with_bit(watch, pch, pawn_copy, combat, from_living, 1).has_value(), "(the watcher saw his death)");
    // a player who is not a Cleric cannot raise him
    const size_t prompts_before = prompts_in(dead, 0).size();
    sc.bard->client_sends({sc.bard->client.actor(sc.bard->pc_ch, resurrect_request("deadman"), true)});
    cleric.run(milliseconds(200));
    CHECK(sc.bard->s->resurrect_requests() == 1 && sc.bard->s->resurrect_refusals() == 1, "a ServerResurrect from a player who is not a Cleric is refused");
    CHECK(prompts_in(dead, 0).size() == prompts_before && dead.s->dead(), "...and the dead player is asked nothing: he stays dead");
    // beyond the cast's reach
    const GoodPosition at = cleric.s->movement().good();
    const size_t casts_far = cleric.s->pipeline() ? cleric.s->pipeline()->casts_started() : 0;
    CHECK(cleric.s->gm_teleport(cleric.conn, at.x + 8000.0, at.y, at.z, cleric.t, "the test").status == PlayerSession::GmMoveResult::Status::Moved, "(the Cleric moves 80 m away)");
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_trigger(cleric.s->bootstrap().setup().guids.ability, dead.s->bootstrap().setup().guids.pawn, 2), true)});
    cleric.run(milliseconds(300));
    CHECK(cleric.s->pipeline() && cleric.s->pipeline()->casts_started() == casts_far, "a Resurrection cast beyond its reach is refused: no cast starts");
    CHECK(cleric.s->resurrect_refusals() == 0, "...and it is the cast's refusal, not a request's");
    CHECK(cleric.s->gm_teleport(cleric.conn, at.x, at.y, at.z, cleric.t, "the test").status == PlayerSession::GmMoveResult::Status::Moved, "(the Cleric comes back)");
    // the cast at the dead ally pays its Mana and starts
    const size_t casts = cleric.s->pipeline() ? cleric.s->pipeline()->casts_started() : 0;
    const float mana_before = session_stat(*cleric.s, content::kGuidMana);
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_trigger(cleric.s->bootstrap().setup().guids.ability, dead.s->bootstrap().setup().guids.pawn, 3), true)});
    cleric.run(milliseconds(300));
    CHECK(cleric.s->pipeline() && cleric.s->pipeline()->casts_started() == casts + 1, "the Resurrection cast at the dead ally starts");
    CHECK(session_stat(*cleric.s, content::kGuidMana) < mana_before, "...and pays its Mana");
    // the client's confirmation, and the prompt
    const size_t from_prompt = dead.got.size();
    cleric.client_sends({cleric.client.actor(cleric.pc_ch, resurrect_request("deadman"), true)});
    cleric.run(milliseconds(300));
    CHECK(cleric.s->resurrect_requests() == 1 && cleric.s->resurrect_prompts_sent() == 1, "the Cleric's ServerResurrect is taken and the dead player is asked");
    const auto prompts = prompts_in(dead, from_prompt);
    CHECK(prompts.size() == 1, "the dead player's client gets one ClientPromptResurrect");
    const std::array<uint32_t, 4> instigator = character_words(*cleric.s);
    CHECK(!prompts.empty() && prompts[0].instigator == instigator, "...naming the Cleric's character as the instigator");
    CHECK(dead.s->resurrect_prompts() == 1 && dead.s->dead(), "...and he stays dead until he answers");
    // the same request again: the dead player is not asked twice
    cleric.client_sends({cleric.client.actor(cleric.pc_ch, resurrect_request("deadman"), true)});
    cleric.run(milliseconds(200));
    CHECK(cleric.s->resurrect_requests() == 2 && cleric.s->resurrect_prompts_sent() == 1 && prompts_in(dead, from_prompt).size() == 1,
          "a repeated ServerResurrect asks nobody again while the prompt waits");
    // his answer: the revive in place with the record's bars
    const GoodPosition was_at = dead.s->last_position();
    const size_t from_revive = watch.got.size();
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(true), true)});
    cleric.run(milliseconds(500));
    CHECK(!dead.s->dead() && dead.s->resurrections() == 1, "his answer brings him back in place");
    const float health_now = session_stat(*dead.s, content::kGuidHealth), mana_now = session_stat(*dead.s, content::kGuidMana);
    CHECK(std::fabs(health_now - max_h * 0.25f) <= session_stat(*dead.s, content::kGuidHealthRegeneration) + 0.5f, "his Health is the record's 25 percent of its maximum");
    CHECK(std::fabs(mana_now - max_m * 0.25f) <= session_stat(*dead.s, content::kGuidManaRegeneration) + 0.5f, "...and his Mana 25 percent too");
    const GoodPosition now_at = dead.s->last_position();
    CHECK(std::fabs(now_at.x - was_at.x) < 1.0 && std::fabs(now_at.y - was_at.y) < 1.0, "...where he fell: the revive is in place");
    const auto frame = frame_with_bit(watch, pch, pawn_copy, combat, from_revive, 0);
    CHECK(frame.has_value(), "the watcher is sent bIsDead 0 on the same pawn");
    CHECK(frame && frame->has_health && frame->has_mana, "...and that update carries his bars");
    CHECK(pawn_dead(*shown_pawn(dead)) == 0 && shown_stat(*shown_pawn(dead), content::kGuidHealth) == health_now && shown_stat(*shown_pawn(dead), content::kGuidMana) == mana_now,
          "the pawn the watchers hold says alive, with the values the revived player has");
    CHECK(shown_pawn(dead) != nullptr && sc.channel_of(*shown_pawn(dead)) == pch, "(the same pawn, on the same channel)");
    // the same answer again: it belongs to no prompt, and nothing revives twice
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(true), true)});
    cleric.run(milliseconds(300));
    CHECK(dead.s->resurrections() == 1 && !dead.s->resurrect_offer_pending(), "a repeated answer revives nobody a second time");
    CHECK(pawn_dead(*shown_pawn(dead)) == 0, "...and his pawn stays alive");
}
AOC_REGISTER_TEST(test_a_resurrection_revives_a_dead_ally_in_place_and_the_watchers_see_it);

static void test_a_declined_resurrection_prompt_leaves_the_dead_player_dead() {
    std::printf("a dead player who turns the resurrection prompt down stays dead, and the Cleric's cast is spent\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(a real-content world with the Cleric and his ally)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    Harness& dead = *sc.dead;
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(the ally dies)");
    cleric.run(milliseconds(300));
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_trigger(cleric.s->bootstrap().setup().guids.ability, dead.s->bootstrap().setup().guids.pawn, 1), true)});
    cleric.run(milliseconds(300));
    const size_t from_prompt = dead.got.size();
    cleric.client_sends({cleric.client.actor(cleric.pc_ch, resurrect_request("deadman"), true)});
    cleric.run(milliseconds(300));
    CHECK(prompts_in(dead, from_prompt).size() == 1, "(the dead player is asked)");
    const size_t resurrections_before = dead.s->resurrections();
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(false), true)});
    cleric.run(milliseconds(400));
    CHECK(dead.s->dead() && dead.s->resurrect_declines() == 1 && dead.s->resurrections() == resurrections_before,
          "his answer is a decline: he stays dead and nothing revives him");
    CHECK(pawn_dead(*shown_pawn(dead)) == 1, "...and the pawn the watchers hold stays dead");
    // his own client may still choose a respawn, as today
    CHECK(dead.s->gm_revive(dead.conn, dead.t, "the test").ok, "(the GM's revive still brings him back elsewhere)");
    cleric.run(milliseconds(400));
    CHECK(pawn_dead(*shown_pawn(dead)) == 0, "...and the watchers see him alive");
}
AOC_REGISTER_TEST(test_a_declined_resurrection_prompt_leaves_the_dead_player_dead);

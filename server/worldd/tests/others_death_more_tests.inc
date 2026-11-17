// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/others_death_more_tests.inc: the cases that follow the first ones of other players' death and the Resurrection (Social, seeing other players: their death)
//
//  Included after tests/others_death_tests.inc (its helpers: ResurrectScene, resurrect_trigger, resurrect_request, resurrect_response, prompts_in, death_record, pawn_dead).
//  A player raised in place walks on at once (his client's movement clock restarted with the death, so the server must forget the old one); a prompt nobody answered neither
//  outlives its minute nor its life, so the next Cleric can ask again; the Resurrection's hit the client asks for raises the dead player it names, and no one else; a prompt
//  turned down is not asked again by the cast's other call; the Resurrection's calls are rate-limited.
// ============================================================================
#include "protocol/repl/hit_rpc.h"
#include "session/ability_resurrect.h"

/// The dead player's pawn walks nowhere: a move of the client at `ts` on the clock it runs, from where the player stands.
static void stand_at_ts(Harness& h, float ts) {
    const GoodPosition g = h.s->movement().good();
    move_pawn(h, move_at(ts, g.x, g.y, g.z, yaw16_of(g.yaw_degrees)));
}

/// The Cleric's cast of Resurrection at `target` (client instance `instance`) and the client's ServerResurrect naming him: the dead player is asked.
static void cast_and_confirm_resurrect(Harness& cleric, Harness& target, const char* target_name, int32_t instance) {
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_trigger(cleric.s->bootstrap().setup().guids.ability, target.s->bootstrap().setup().guids.pawn, instance), true)});
    cleric.run(milliseconds(300));
    cleric.client_sends({cleric.client.actor(cleric.pc_ch, resurrect_request(target_name), true)});
    cleric.run(milliseconds(300));
}

// ---- a revived player's own movement ----------------------------------------------------------------------------------------------------

static void test_a_player_raised_in_place_walks_on_with_his_clock_restarted() {
    std::printf("a player raised in place moves on at once: his client's clock restarted with the death, and the moves on it are heard, not dropped for as long as the old clock had run\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    Harness& dead = *sc.dead;
    // the player's clock has run for a minute when he dies
    for (int i = 0; i < 4; ++i) {
        stand_at_ts(dead, 60.0f + 0.2f * static_cast<float>(i));
        cleric.run(milliseconds(200));
    }
    CHECK(dead.s->movement().good().has_time_stamp && dead.s->movement().good().time_stamp > 60.0f, "(his moves were heard on a clock past 60 s)");
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(the ally dies)");
    cleric.run(milliseconds(300));
    cast_and_confirm_resurrect(cleric, dead, "deadman", 1);
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(true), true)});
    cleric.run(milliseconds(500));
    CHECK(!dead.s->dead() && dead.s->resurrections() == 1, "(he is raised in place)");
    // the client's pawn was restarted at the death: its moves count from near 0 again
    const size_t acked = dead.s->moves_acknowledged(), stale = dead.s->moves_stale();
    for (int i = 1; i <= 10; ++i) {
        stand_at_ts(dead, 0.1f * static_cast<float>(i));
        cleric.run(milliseconds(100));
    }
    CHECK(dead.s->moves_acknowledged() >= acked + 9 && dead.s->moves_stale() == stale, "the moves after the revive, at the restarted clock's TimeStamps, are acknowledged");
    CHECK(dead.s->movement().good().time_stamp < 10.0f, "...and the last good move is on the new clock (under 10 s, not over 60)");
}
AOC_REGISTER_TEST(test_a_player_raised_in_place_walks_on_with_his_clock_restarted);

// ---- a prompt nobody answered ------------------------------------------------------------------------------------------------------

static void test_a_prompt_nobody_answered_does_not_block_the_next_cleric() {
    std::printf("a dead player who never answers is asked again by another Cleric once the prompt's minute is over\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    Harness& dead = *sc.dead;
    auto healer = join_world(cleric, sc.all, death_record(other_id(5), "healer", 10, 3, 10));
    cleric.run(milliseconds(400));
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(the ally dies)");
    cleric.run(milliseconds(300));
    const size_t from_first = dead.got.size();
    cast_and_confirm_resurrect(cleric, dead, "deadman", 1);
    CHECK(prompts_in(dead, from_first).size() == 1, "(the first Cleric's prompt reaches him)");
    // a second ask while the prompt waits changes nothing
    const size_t from_second = dead.got.size();
    cast_and_confirm_resurrect(*healer, dead, "deadman", 1);
    CHECK(prompts_in(dead, from_second).empty(), "another Cleric's ask while the prompt waits asks him nothing more");
    // the minute is over, nobody answered
    cleric.run(milliseconds(61000));
    const size_t from_third = dead.got.size();
    cast_and_confirm_resurrect(*healer, dead, "deadman", 2);
    CHECK(prompts_in(dead, from_third).size() == 1, "a Cleric who asks after the prompt's minute is over asks him again");
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(true), true)});
    cleric.run(milliseconds(500));
    CHECK(!dead.s->dead() && dead.s->resurrections() == 1, "...and his answer to that prompt raises him");
}
AOC_REGISTER_TEST(test_a_prompt_nobody_answered_does_not_block_the_next_cleric);

static void test_a_prompt_does_not_outlive_the_life_it_was_made_for() {
    std::printf("a prompt a dead player left unanswered and then respawned from is gone: in his next life a Cleric can ask him, and an old answer raises nobody\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    Harness& dead = *sc.dead;
    auto healer = join_world(cleric, sc.all, death_record(other_id(5), "healer", 10, 3, 10));
    cleric.run(milliseconds(400));
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(the ally dies)");
    cleric.run(milliseconds(300));
    cast_and_confirm_resurrect(cleric, dead, "deadman", 1);
    CHECK(dead.s->resurrect_offer_pending() && dead.s->dead(), "(he is asked and does not answer)");
    // he comes back by himself (a respawn), and the prompt of his first life with him
    CHECK(dead.s->gm_revive(dead.conn, dead.t, "the test").ok, "(he respawns)");
    cleric.run(milliseconds(500));
    CHECK(!dead.s->dead() && dead.s->respawns() == 1, "(he is alive again, elsewhere)");
    // the old prompt's answer, late: it belongs to no life he is in
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(true), true)});
    cleric.run(milliseconds(300));
    CHECK(dead.s->resurrections() == 0 && !dead.s->resurrect_offer_pending(), "an answer to the old prompt raises nobody and leaves no prompt behind");
    // his second life ends, and the healer asks
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(he dies again)");
    cleric.run(milliseconds(300));
    const size_t from = dead.got.size();
    cast_and_confirm_resurrect(*healer, dead, "deadman", 1);
    CHECK(prompts_in(dead, from).size() == 1, "a Cleric who asks him in his second life asks him");
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(true), true)});
    cleric.run(milliseconds(500));
    CHECK(!dead.s->dead() && dead.s->resurrections() == 1, "...and his answer raises him in place");
}
AOC_REGISTER_TEST(test_a_prompt_does_not_outlive_the_life_it_was_made_for);

// ---- the Resurrection's hit the client asks for ------------------------------------------------------------------------------------------

/// RequestHitVerification_Server for the Resurrection's hit, naming `target`'s pawn the way the client names an actor whose component it has no GUID for (in place, by its outer).
static BunchWriter resurrect_hit_request(const Harness& cleric, const world::WorldActor& target) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("RequestHitVerification_Server");
    const abilities::Catalog* catalog = cleric.fx.world->ability_catalog();
    const abilities::AbilityDef* def = catalog ? catalog->ability(kResurrectAbility) : nullptr;
    const abilities::HitDef* hit = def ? abilities::resurrect_hit_of(*catalog, *def) : nullptr;
    if (!rpc || !hit) {
        std::printf("  [FAIL] (the Resurrection's hit is in the tables)\n");
        return BunchWriter();
    }
    int64_t key = -1, targeting = 0;
    for (const content::AbilityHitRow& h : def->hits) {
        if (h.hit_guid == hit->row.guid) key = h.key;
    }
    for (const content::TargetingRow& t : def->targetings) {
        if (t.key == key) targeting = t.guid;
    }
    repl::HitRequest req;
    repl::HitComponentRef c;
    repl::InlineObject cylinder;
    cylinder.flags = 5;
    cylinder.outer = target.actor.guid;
    cylinder.path = "CollisionCylinder";
    cylinder.checksum = 0xb876932f;
    c.inline_object = cylinder;
    c.server_timestamp = 1.0f;
    req.components.push_back(c);
    req.ability_guid = kResurrectAbility;
    req.targeting_guid = targeting;
    req.hit_guid = hit->row.guid;
    const auto params = repl::hit_request_params(*rpc, req);
    if (!params) {
        std::printf("  [FAIL] the hit request's parameters do not fit\n");
        return BunchWriter();
    }
    BunchWriter field;
    bool failed = false;
    repl::CodecOptions opts;
    opts.inline_objects = true;
    if (repl::write_rpc_field(cls, *rpc, *params, field, opts, &failed) == 0 || failed) {
        std::printf("  [FAIL] RequestHitVerification_Server does not write\n");
        return BunchWriter();
    }
    BunchWriter out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = cleric.s->bootstrap().setup().guids.ability;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

static void test_the_resurrection_hit_the_client_asks_for_raises_the_dead_player_it_names() {
    std::printf("the Resurrection's hit, as the client asks for it, asks the dead player it names (and no living one) and his answer raises him\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    Harness& dead = *sc.dead;
    Harness& bard = *sc.bard;
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(the ally dies)");
    cleric.run(milliseconds(300));
    const world::WorldActor* dead_pawn = shown_pawn(dead);
    const world::WorldActor* bard_pawn = shown_pawn(bard);
    CHECK(dead_pawn != nullptr && bard_pawn != nullptr, "(both pawns are in the world)");
    if (!dead_pawn || !bard_pawn) return;
    const size_t casts = cleric.s->pipeline() ? cleric.s->pipeline()->casts_started() : 0;
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_trigger(cleric.s->bootstrap().setup().guids.ability, dead.s->bootstrap().setup().guids.pawn, 1), true)});
    cleric.run(milliseconds(200));
    CHECK(cleric.s->pipeline() && cleric.s->pipeline()->casts_started() == casts + 1, "(the Cleric's Resurrection cast at the dead ally starts)");
    // the client's hit request names the living Bard: it lands on nobody
    const float bard_health = session_stat(*bard.s, content::kGuidHealth);
    const size_t from_bard = bard.got.size();
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_hit_request(cleric, *bard_pawn), true)});
    cleric.run(milliseconds(800));
    CHECK(prompts_in(bard, from_bard).empty() && !bard.s->resurrect_offer_pending() && bard.s->resurrections() == 0, "a hit request that names a living player asks and raises nobody");
    CHECK(session_stat(*bard.s, content::kGuidHealth) == bard_health && !bard.s->dead(), "...and leaves his Health alone");
    // the one that names the dead ally
    const size_t from_dead = dead.got.size();
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_hit_request(cleric, *dead_pawn), true)});
    cleric.run(milliseconds(800));
    const auto prompts = prompts_in(dead, from_dead);
    CHECK(prompts.size() == 1, "the hit request that names the dead ally lands: his client gets one ClientPromptResurrect");
    CHECK(!prompts.empty() && prompts[0].instigator == character_words(*cleric.s), "...naming the Cleric's character");
    CHECK(cleric.s->resurrect_requests() == 0 && dead.s->dead(), "(no ServerResurrect came: the hit alone asked him) and he stays dead until he answers");
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(true), true)});
    cleric.run(milliseconds(500));
    CHECK(!dead.s->dead() && dead.s->resurrections() == 1, "his answer raises him in place");
    CHECK(pawn_dead(*shown_pawn(dead)) == 0, "...and the pawn the watchers hold says alive");
}
AOC_REGISTER_TEST(test_the_resurrection_hit_the_client_asks_for_raises_the_dead_player_it_names);

static void test_a_prompt_turned_down_is_not_asked_again_by_the_same_cast() {
    std::printf("a dead player who turns the prompt down is not asked again by the same cast's hit\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    Harness& dead = *sc.dead;
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(the ally dies)");
    cleric.run(milliseconds(300));
    const world::WorldActor* dead_pawn = shown_pawn(dead);
    if (!dead_pawn) return;
    const size_t from = dead.got.size();
    cast_and_confirm_resurrect(cleric, dead, "deadman", 1);
    CHECK(prompts_in(dead, from).size() == 1, "(the Cleric's cast asks him)");
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(false), true)});
    cleric.run(milliseconds(200));
    CHECK(dead.s->dead() && dead.s->resurrect_declines() == 1, "(he turns it down)");
    // the same cast's hit, asked for by the client a moment later
    const size_t from_hit = dead.got.size();
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_hit_request(cleric, *dead_pawn), true)});
    cleric.run(milliseconds(800));
    CHECK(prompts_in(dead, from_hit).empty() && !dead.s->resurrect_offer_pending(), "the hit of the cast he turned down does not ask him again");
    CHECK(dead.s->dead() && dead.s->resurrections() == 0, "...and he stays dead");
}
AOC_REGISTER_TEST(test_a_prompt_turned_down_is_not_asked_again_by_the_same_cast);

static void test_the_resurrection_calls_are_rate_limited() {
    std::printf("a client that floods ServerResurrect is heard up to its budget and no further\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& bard = *sc.bard;
    const size_t before = bard.s->resurrect_requests();
    for (int i = 0; i < 40; ++i) bard.client_sends({bard.client.actor(bard.pc_ch, resurrect_request("deadman"), true)});
    const size_t heard = bard.s->resurrect_requests() - before;
    CHECK(heard >= 1 && heard <= 20, "40 requests in one moment: only the burst the budget allows is taken");
    sc.cleric.run(milliseconds(1500));
    bard.client_sends({bard.client.actor(bard.pc_ch, resurrect_request("deadman"), true)});
    CHECK(bard.s->resurrect_requests() == before + heard + 1, "...and the budget refills with time");
}
AOC_REGISTER_TEST(test_the_resurrection_calls_are_rate_limited);

// ---- the revived player's own client ----------------------------------------------------------------------------------------------------

/// The CombatInfo.bIsDead the player's OWN client was sent on his pawn's channel from chain `from` on: the last value a chain says (nullopt: none does).
static std::optional<int64_t> own_dead_bit(const Harness& h, size_t from) {
    const repl::Actor& pawn = h.s->bootstrap().pawn();
    const uint64_t combat = h.s->bootstrap().setup().guids.combat_info.ObjectId;
    std::optional<int64_t> last;
    for (size_t i = from; i < h.got.size(); ++i) {
        const fake::Delivered& d = h.got[i];
        if (d.channel != h.pawn_ch || d.close) continue;
        if (const auto bit = dead_in(d, pawn, combat)) last = bit;
    }
    return last;
}

static void test_the_player_raised_in_place_is_told_by_his_own_client_pawn_that_he_lives() {
    std::printf("the player a Resurrection raises is sent bIsDead 0 on his own pawn, so his client plays the revive\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    Harness& dead = *sc.dead;
    const size_t from_death = dead.got.size();
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(the ally dies)");
    cleric.run(milliseconds(300));
    CHECK(own_dead_bit(dead, from_death) == 1, "his own client is sent bIsDead 1 with his death");
    cast_and_confirm_resurrect(cleric, dead, "deadman", 1);
    const size_t from_revive = dead.got.size();
    dead.client_sends({dead.client.actor(dead.pc_ch, resurrect_response(true), true)});
    cleric.run(milliseconds(500));
    CHECK(!dead.s->dead(), "(he is raised in place)");
    CHECK(own_dead_bit(dead, from_revive) == 0, "his own client is sent bIsDead 0 with the revive");
}
AOC_REGISTER_TEST(test_the_player_raised_in_place_is_told_by_his_own_client_pawn_that_he_lives);

// ---- the other abilities keep to the world's mobs ------------------------------------------------------------------------------------------

/// ServerTriggerAbility(`ability`, [`target`]) on the caster's AbilityComponent: a targeted cast as the client sends it.
static BunchWriter targeted_trigger(const emit::FIntrepidNetworkGUID& component, int64_t ability, const emit::FIntrepidNetworkGUID& target, int32_t instance) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(ability));
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
        std::printf("  [FAIL] the trigger's parameters do not fit\n");
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

static void test_an_ability_that_raises_nobody_is_still_not_cast_at_another_player() {
    std::printf("a heal can now be cast at another player (abilities between players): the cast starts and spends its Mana\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    constexpr int64_t kMend = 6064629375766374578ll;   // Cleric_Mend: a heal for the caster and his allies, granted at level 1
    CHECK(std::find(cleric.s->known_abilities().begin(), cleric.s->known_abilities().end(), kMend) != cleric.s->known_abilities().end(), "(the Cleric knows Mend)");
    const size_t casts = cleric.s->pipeline() ? cleric.s->pipeline()->casts_started() : 0;
    const float mana = session_stat(*cleric.s, content::kGuidMana);
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, targeted_trigger(cleric.s->bootstrap().setup().guids.ability, kMend, sc.bard->s->bootstrap().setup().guids.pawn, 1), true)});
    cleric.run(milliseconds(400));
    CHECK(cleric.s->pipeline() && cleric.s->pipeline()->casts_started() == casts + 1, "a heal cast at another player starts its cast");
    CHECK(session_stat(*cleric.s, content::kGuidMana) < mana, "...and spends its Mana");
}
AOC_REGISTER_TEST(test_an_ability_that_raises_nobody_is_still_not_cast_at_another_player);

static void test_a_confirmation_from_a_cleric_who_moved_out_of_reach_raises_nobody() {
    std::printf("a ServerResurrect from a Cleric who has walked out of the cast's reach since the cast asks the dead player nothing\n");
    ResurrectScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& cleric = sc.cleric;
    Harness& dead = *sc.dead;
    CHECK(dead.s->gm_kill(dead.conn, dead.t, "the test").ok, "(the ally dies)");
    cleric.run(milliseconds(300));
    cleric.client_sends({cleric.client.actor(cleric.pawn_ch, resurrect_trigger(cleric.s->bootstrap().setup().guids.ability, dead.s->bootstrap().setup().guids.pawn, 1), true)});
    cleric.run(milliseconds(200));
    const GoodPosition at = cleric.s->movement().good();
    CHECK(cleric.s->gm_teleport(cleric.conn, at.x + 8000.0, at.y, at.z, cleric.t, "the test").status == PlayerSession::GmMoveResult::Status::Moved, "(the Cleric is moved 80 m away)");
    const size_t from = dead.got.size();
    cleric.client_sends({cleric.client.actor(cleric.pc_ch, resurrect_request("deadman"), true)});
    cleric.run(milliseconds(300));
    CHECK(cleric.s->resurrect_requests() == 1 && cleric.s->resurrect_refusals() == 1, "the confirmation is refused");
    CHECK(prompts_in(dead, from).empty() && dead.s->dead() && !dead.s->resurrect_offer_pending(), "...the dead player is asked nothing and stays dead");
}
AOC_REGISTER_TEST(test_a_confirmation_from_a_cleric_who_moved_out_of_reach_raises_nobody);

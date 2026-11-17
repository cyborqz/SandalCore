// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/session_party_resurrect_tests.inc: the party shows a member dead, and the Cleric's Resurrection brings him back (Social, seeing other players: their death)
//
//  Registered tests of test_worldd_session, after tests/session_party_stats_more_tests.inc (its helpers).  A party of two on the real content.db: the second member dies
//  (the frame and the live item say dead, the milestone 3 shape), the first - a Cleric - casts Resurrection on him, the client's ServerResurrect confirms it, the dead member
//  answers the prompt, and the frame and the live item say alive again with the record's 25 % of his maximum Health and Mana.
//
//  Modelled: no recording of a resurrect exists; the flow is the generated schema's three calls (the cast is the trigger, ServerResurrect confirms it), and the ratios are
//  the game's ResurrectRecordBase's (session/ability_resurrect.h).
// ============================================================================
#include "nav_fixture.h"

/// The Cleric_Resurrect ability (auto-granted at Cleric level 6).
static constexpr int64_t kPartyResurrect = 6064630847771377729ll;

/// True when two numbers are the same within `tolerance` (this file does not include <cmath>).
static bool party_close(double a, double b, double tolerance) { return a <= b + tolerance && b <= a + tolerance; }

/// The real content.db the party's resurrect tests run on: AOC_CONTENT_DB, else the working tree's file (the tests run from the repository's root).
static std::string party_real_db() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? std::string(env) : std::string("dist/content/content.db");
}

/// A party character of a class and a level (a Cleric needs one at level 6 and up; a Bard is the case that must not raise anyone).
static svc::Character party_class_record(const char* name, const std::array<uint32_t, 4>& words, int class_id, int level) {
    svc::Character c = party_record(name, words);
    c.class_id = class_id;
    c.class_name = class_id == 3 ? "Cleric" : "Bard";
    c.level = level;
    return c;
}

/// ServerTriggerAbility(Resurrection, [the dead member's pawn]) on the Cleric's AbilityComponent.
static BunchWriter party_resurrect_trigger(const bs::PlayerSetup& setup, const emit::FIntrepidNetworkGUID& target, int32_t instance) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(kPartyResurrect));
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
    header.object = setup.guids.ability;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// The parameters of ServerResurrect(CharacterName).
static repl::RpcParams party_resurrect_request(const std::string& name) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerResurrect");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "CharacterName", repl::Value::str(name))) {
        std::printf("  [FAIL] ServerResurrect's parameters do not fit\n");
        return repl::RpcParams{};
    }
    return p;
}

/// The parameters of ServerPromptedResurrectResponse(Response).
static repl::RpcParams party_resurrect_answer(bool yes) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerPromptedResurrectResponse");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "Response", repl::Value::boolean(yes))) {
        std::printf("  [FAIL] ServerPromptedResurrectResponse's parameter does not fit\n");
        return repl::RpcParams{};
    }
    return p;
}

/// The ClientPromptResurrect a member's client got from chain `from` on (nullopt: none).
static std::optional<std::array<uint32_t, 4>> party_prompt_in(const std::vector<fake::Delivered>& got, const schema::ClassSchema& pc_cls, size_t from) {
    for (size_t i = from; i < got.size(); ++i) {
        if (got[i].close) continue;
        const auto call = party_single_rpc(got[i], pc_cls, "ClientPromptResurrect");
        if (!call) continue;
        return party_words(*call, "InstigatorGuid");
    }
    return std::nullopt;
}

static void test_the_party_shows_a_member_dead_and_the_resurrection_brings_him_back() {
    std::printf("a party member dies (frame and live item dead, empty bars) and the Cleric's Resurrection brings him back in place with 25 percent of his bars\n");
    const std::string db = party_real_db();
    CHECK(fs::exists(db), ("the real content.db is there: " + db).c_str());
    if (!fs::exists(db)) return;
    PartyWorld w(db);
    // the world's fights: the Resurrection's target checks and the ability runtime need them
    {
        std::string err;
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        CHECK(nav && w.fx.world->adopt_ground(std::move(nav), err), ("the world is ready for a fight: " + err).c_str());
        if (!w.fx.world->combat()) return;
    }
    const schema::ClassSchema& pc_cls = class_named("AoCPlayerControllerBP_C");
    CHECK(w.enter(0, party_class_record("p1", std::array<uint32_t, 4>{0x51a00000u, 0x1000u, 0x2000u, 0x3000u}, 3, 10), identity()), "(the Cleric enters the world)");
    CHECK(w.enter(1, party_record("p2", std::array<uint32_t, 4>{0x51a00001u, 0x1001u, 0x2001u, 0x3001u}), identity()), "(his ally enters)");
    if (!w[0].s || !w[1].s) return;
    CHECK(std::find(w[0].s->known_abilities().begin(), w[0].s->known_abilities().end(), kPartyResurrect) != w[0].s->known_abilities().end(),
          "(the level-10 Cleric knows Resurrection)");
    CHECK(party_pair(w, 0, 1, pc_cls), "(the two form a party)");
    party_run(w, 1200);
    // the death: the frame and the live item say dead with empty bars
    const float max_h = party_stat(w, 1, content::kGuidMaxHealth), max_m = party_stat(w, 1, content::kGuidMaxMana);
    const size_t before = w[0].got.size();
    CHECK(w[1].s->gm_kill(w[1].conn, w.t, "the test").ok && w[1].s->dead(), "(the ally dies)");
    party_run(w, 1300);
    {
        const auto frames = party_frames_in(w[0].got, pc_cls, before);
        CHECK(!frames.empty() && frames.back().members.size() == 2 && frames.back().members[1].alive_status == 1 && frames.back().members[1].health_percent == 0.0f,
              "the party frame says the member dead, with an empty bar");
        const PartyStatsItem* item = item_of(last_party_stats(w, 0, before), w.guid_of(1));
        CHECK(item && item->alive == 1 && item->health_percent == 0.0f, "...and so does his live item");
    }
    // the Cleric's Resurrection cast at the dead member
    const size_t casts = w[0].s->pipeline() ? w[0].s->pipeline()->casts_started() : 0;
    w.client_sends(w[0], {w[0].client.actor(w[0].s->channel_of(bs::Role::Pawn), party_resurrect_trigger(w[0].s->bootstrap().setup(), w[1].s->bootstrap().setup().guids.pawn, 1),
                                             true)});
    party_run(w, 300);
    CHECK(w[0].s->pipeline() && w[0].s->pipeline()->casts_started() == casts + 1, "the cast starts");
    // the client's confirmation, and the dead member's prompt
    const size_t from_prompt = w[1].got.size();
    CHECK(party_send(w, 0, "ServerResurrect", party_resurrect_request("p2")), "(the Cleric's client confirms it: ServerResurrect)");
    party_run(w, 300);
    const auto prompt = party_prompt_in(w[1].got, pc_cls, from_prompt);
    CHECK(prompt.has_value(), "the dead member's client gets ClientPromptResurrect");
    CHECK(prompt && *prompt == w.guid_of(0), "...naming the Cleric's character");
    CHECK(w[1].s->dead(), "...and he stays dead until he answers");
    // his answer: he comes back in place with the record's bars
    const GoodPosition was_at = w[1].s->last_position();
    const size_t from_revive = w[0].got.size();
    CHECK(party_send(w, 1, "ServerPromptedResurrectResponse", party_resurrect_answer(true)), "(the dead member answers: yes)");
    party_run(w, 1300);
    CHECK(!w[1].s->dead() && w[1].s->resurrections() == 1, "the answer brings him back in place");
    const float health_now = party_stat(w, 1, content::kGuidHealth), mana_now = party_stat(w, 1, content::kGuidMana);
    CHECK(party_close(health_now, max_h * 0.25f, party_stat(w, 1, content::kGuidHealthRegeneration) + 0.5f), "his Health is the record's 25 percent of its maximum");
    CHECK(party_close(mana_now, max_m * 0.25f, party_stat(w, 1, content::kGuidManaRegeneration) + 0.5f), "...and his Mana 25 percent too");
    const GoodPosition now_at = w[1].s->last_position();
    CHECK(party_close(now_at.x, was_at.x, 1.0) && party_close(now_at.y, was_at.y, 1.0), "...where he fell: in place");
    const auto frames = party_frames_in(w[0].got, pc_cls, from_revive);
    CHECK(!frames.empty() && frames.back().members.size() == 2 && frames.back().members[1].alive_status == 0, "the party frame says him alive again");
    if (!frames.empty() && frames.back().members.size() == 2) {
        const PartyMemberFrame& row = frames.back().members[1];
        CHECK(party_near(row.health_percent, health_now / max_h, 0.02f) && party_near(row.mana_percent, mana_now / max_m, 0.02f), "...with his revived bars' shares");
    }
    const PartyStatsItem* item = item_of(last_party_stats(w, 0, from_revive), w.guid_of(1));
    CHECK(item && item->alive == 0 && party_near(item->health_percent, health_now / max_h, 0.02f), "...and his live item says alive with his share");
}
AOC_REGISTER_TEST(test_the_party_shows_a_member_dead_and_the_resurrection_brings_him_back);

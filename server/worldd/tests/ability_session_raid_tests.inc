// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_raid_tests.inc
//
//  Abilities between players in a raid: the combat code's two tiers (the same squad 2, another squad of the raid 1, no group 0),
//  the Party mask (8) reaching the caster's squad only and the Raid mask (16) the whole raid; the ally candidate lists follow the
//  same masks (the Cleric's Communal Restoration reaches the caster's squad in a raid, and the Saga's Pantheon candidates are the
//  caster's squad for its Party mask).  The sessions are the real ones (players_tests in ability_session_players_tests.inc).
// ============================================================================

namespace raid_ability_tests {

/// A client call on the PlayerController of a session (the same form as the party helpers).
static BunchWriter raid_call_bunch(const char* name, const repl::RpcParams& params) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc(name);
    if (!rpc) return BunchWriter();
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(pc, *rpc, params, field, client_options(), &failed) == 0 || failed) return BunchWriter();
    BunchWriter out;
    repl::BlockHeader header;
    if (repl::write_content_block(out, header, field, /*server_form=*/false) == 0) return BunchWriter();
    return out;
}

static BunchWriter raid_convert_bunch() { return raid_call_bunch("ServerConvertPartyToRaid", repl::RpcParams{}); }

static BunchWriter raid_move_bunch(const std::string& name, int64_t squad) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerMoveRaidMember");
    repl::RpcParams params;
    if (!rpc || !repl::set_rpc_param(*rpc, params, "CharacterName", repl::Value::str(name)) ||
        !repl::set_rpc_param(*rpc, params, "NewSquadNum", repl::Value::i64(squad))) return BunchWriter();
    return raid_call_bunch("ServerMoveRaidMember", params);
}

/// A party of three (the first the leader), the convert button and p3 moved to squad 2: a raid of three with two squads.
static bool raid_of_three(players_tests::Players& p) {
    using namespace players_tests;
    send_on_pc(p[0], party_invite_bunch(p.records[1].name));
    p.all(0.5);
    const auto group = invited_group(p[1]);
    if (group == std::array<uint32_t, 4>{}) return false;
    send_on_pc(p[1], party_accept_bunch(group, p.records[0].name));
    p.all(0.5);
    send_on_pc(p[0], party_invite_bunch(p.records[2].name));
    p.all(0.5);
    send_on_pc(p[2], party_accept_bunch(group, p.records[0].name));
    p.all(0.5);
    send_on_pc(p[0], raid_convert_bunch());
    p.all(0.5);
    send_on_pc(p[0], raid_move_bunch(p.records[2].name, 2));
    p.all(0.5);
    return true;
}

}  // namespace raid_ability_tests

static void test_raid_combat_group_tiers() {
    using namespace players_tests;
    std::printf("a raid's tiers: the same squad 2, another squad of the raid 1, a player in no group 0; the Party mask reaches the squad, the Raid mask the whole raid\n");
    Players p;
    CHECK(p.start(4), "(four players in one world with ground and fights)");
    if (p.runs.size() != 4 || !p.world().combat()) return;
    CHECK(raid_ability_tests::raid_of_three(p), "a raid of three with p3 moved to squad 2");
    combat::CombatWorld& cw = *p.world().combat();
    const bs::PlayerSetup sa = p.setup(0), sb = p.setup(1), sc = p.setup(2), sd = p.setup(3);
    combat::PlayerPort* from = cw.find_player(sa.guids.pawn.ObjectId);
    CHECK(from != nullptr, "(the caster's port is in the fight)");
    if (!from) return;
    const combat::TargetInfo same = cw.inspect(sb.guids.pawn.ObjectId, from->fighter());
    const combat::TargetInfo other = cw.inspect(sc.guids.pawn.ObjectId, from->fighter());
    const combat::TargetInfo stranger = cw.inspect(sd.guids.pawn.ObjectId, from->fighter());
    CHECK(same.other_player && same.group == 2, "the same squad's member is tier 2");
    CHECK(other.other_player && other.group == 1, "the other squad's member is tier 1");
    CHECK(stranger.other_player && stranger.group == 0, "a player in no group is tier 0");
    CHECK(combat::mask_reaches(8, same) && !combat::mask_reaches(8, other), "the Party mask (8) reaches the caster's squad only");
    CHECK(combat::mask_reaches(16, same) && combat::mask_reaches(16, other), "the Raid mask (16) reaches both squads");
    CHECK(!combat::mask_reaches(8, stranger) && !combat::mask_reaches(16, stranger), "neither mask reaches the player in no group");
}

AOC_REGISTER_TEST(test_raid_combat_group_tiers);

static void test_raid_party_mask_reaches_the_squad() {
    using namespace players_tests;
    std::printf("Communal Restoration (Self and Party in its mask) in a raid reaches the caster's squad member, not the other squad's\n");
    Players p;
    CHECK(p.start(3), "(three players in one world with ground and fights)");
    if (p.runs.size() != 3 || !p.world().combat()) return;
    CHECK(raid_ability_tests::raid_of_three(p), "a raid of three with p3 moved to squad 2");
    Run& a = p[0];
    Run& b = p[1];
    Run& c = p[2];
    PlayerWorld& world = p.world();
    combat::CombatWorld& cw = *world.combat();
    const bs::PlayerSetup sa = p.setup(0), sb = p.setup(1), sc = p.setup(2);
    const abilities::AbilityDef* cr = world.ability_catalog()->ability_named("Cleric_CommunalRestoration");
    CHECK(cr != nullptr && !cr->hits.empty(), "(Communal Restoration is in the tables)");
    if (!cr || cr->hits.empty()) return;
    a.h.s->teach_ability(cr->row.guid);
    int64_t hit = 0, targeting = 0;
    if (const abilities::HitDef* first = world.ability_catalog()->hit_named("Cleric_CommunalRestoration_1")) hit = first->row.guid;
    if (!hit) hit = cr->hits[0].hit_guid;
    for (const content::TargetingRow& t : cr->targetings) {
        for (const auto& h : cr->hits) {
            if (h.hit_guid == hit && t.key == h.key) targeting = t.guid;
        }
    }
    const double max_b = b.m.stats[content::kGuidMaxHealth], max_c = c.m.stats[content::kGuidMaxHealth];
    wound(cw, sb, p.wolves[0], max_b * 0.5, a.now());
    wound(cw, sc, p.wolves[0], max_c * 0.5, a.now());
    p.all(0.3);
    const double hurt_b = b.m.stats[content::kGuidHealth], hurt_c = c.m.stats[content::kGuidHealth];
    CHECK(hurt_b < max_b * 0.6 && hurt_c < max_c * 0.6, "(both are wounded)");
    const int32_t id = a.next_id++;
    a.send_trigger(cr->row.guid, id);
    p.all(0.3);
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("RequestHitVerification_Server");
    repl::HitRequest req;
    for (const emit::FIntrepidNetworkGUID& o : {sa.guids.collision_cylinder, sb.guids.collision_cylinder, sc.guids.collision_cylinder}) {
        repl::HitComponentRef ref;
        ref.object = o;
        ref.server_timestamp = 1.0f;
        req.components.push_back(ref);
    }
    req.ability_guid = cr->row.guid;
    req.targeting_guid = targeting;
    req.hit_guid = hit;
    const auto params = rpc ? repl::hit_request_params(*rpc, req) : std::nullopt;
    CHECK(params.has_value(), "(the request's parameters fit)");
    if (!params) return;
    a.send_reliable(ability_rpc(sa.guids.ability, "RequestHitVerification_Server", *params));
    p.all(6.0);
    const abilities::EffectDef* restoration = world.ability_catalog()->effect_named("Cleric_CommunalRestoration_Effect1");
    const double gain_b = b.m.stats[content::kGuidHealth] - hurt_b, gain_c = c.m.stats[content::kGuidHealth] - hurt_c;
    CHECKF(gain_b > gain_c + 1, "the squad member's Health rises by more than the other squad's regeneration (%.1f against %.1f)", gain_b, gain_c);
    CHECK(restoration && b.m.has_effect(restoration->row.guid), "the squad member holds Communal Restoration's effect");
    CHECK(restoration && !c.m.has_effect(restoration->row.guid), "the other squad's member does not");
    CHECK(!a.h.s->abilities_failed() && !b.h.s->abilities_failed() && !c.h.s->abilities_failed(), "(nothing failed)");
}

AOC_REGISTER_TEST(test_raid_party_mask_reaches_the_squad);

static void test_raid_saga_ally_takes_the_squad() {
    using namespace players_tests;
    std::printf("the Saga's Pantheon candidates in a raid: its Party mask keeps the caster's squad only, the Raid mask keeps both\n");
    Players p;
    CHECK(p.start(3), "(three players in one world with ground and fights)");
    if (p.runs.size() != 3 || !p.world().combat()) return;
    CHECK(raid_ability_tests::raid_of_three(p), "a raid of three with p3 moved to squad 2");
    const AbilityPipeline* pipeline = p[0].h.s->pipeline();
    CHECK(pipeline != nullptr, "(the leader's pipeline is there)");
    if (!pipeline) return;
    // The test-only accessor runs the Saga's own candidate filter (ally_targets_of) with the mask the ability carries.
    const auto party_targets = const_cast<AbilityPipeline*>(pipeline)->ally_targets_for_test("Bard_Saga_Pantheon", 8, 9);
    bool has_self = false, has_squad = false, has_other = false;
    for (const uint64_t actor : party_targets) {
        has_self = has_self || actor == p.setup(0).guids.pawn.ObjectId;
        has_squad = has_squad || actor == p.setup(1).guids.pawn.ObjectId;
        has_other = has_other || actor == p.setup(2).guids.pawn.ObjectId;
    }
    CHECK(has_self && has_squad && !has_other, "with the Party mask the Saga keeps the caster and the squad, not the other squad");
    const auto raid_targets = const_cast<AbilityPipeline*>(pipeline)->ally_targets_for_test("Bard_Saga_Pantheon", 8, 16);
    bool raid_other = false;
    for (const uint64_t actor : raid_targets) raid_other = raid_other || actor == p.setup(2).guids.pawn.ObjectId;
    CHECK(raid_other, "with the Raid mask it keeps the other squad too");
}

AOC_REGISTER_TEST(test_raid_saga_ally_takes_the_squad);

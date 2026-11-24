// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Abilities between players: contributed by MrJambix in #23.
// ============================================================================
//  tests/ability_session_players_tests.inc: abilities between players, through two (or three) real sessions in one world (combat/combat_players.cpp)
//
//  A Cleric's Mend cast at another player heals him and not the caster, and the caster's client is told of the hit; his health bar on the pawn the others are
//  shown follows him; a harmful hit does not reach another player; and Communal Restoration (Self and Party in its mask) reaches the caster's party member and
//  not a stranger.  (The Resurrect's own path is lane 13's: tests/others_death_tests.inc and others_death_more_tests.inc.)
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

namespace players_tests {

size_t count_of(const std::vector<std::string>& calls, const char* name) { return static_cast<size_t>(std::count(calls.begin(), calls.end(), std::string(name))); }

/// The registry's Player actor that is `r`'s pawn as the other players are shown it (null before the phase-in).
const world::WorldActor* shown_of(Run& r, PlayerWorld& world) {
    const uint64_t id = static_cast<uint64_t>(r.h.s->bootstrap().setup().player_id);
    const world::WorldActor* found = nullptr;
    world.registry().for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Player && a.owner == id) found = &a;
    });
    return found;
}

/// Whether the pawn the others are shown carries the player's four live stats (Health, MaxHealth, Mana, MaxMana) and his Corruption as they are now.  The
/// other EveryoneProxy stats follow gear and level, not a heal: lane 13's live copy names the records it keeps current (session/player_shown.cpp's kLiveStats).
bool shown_stats_follow(Run& r, const world::WorldActor& shown) {
    const bs::PlayerSetup& setup = r.h.s->bootstrap().setup();
    const repl::Subobject* own = r.h.s->bootstrap().pawn().find(setup.guids.stats);
    const repl::Subobject* theirs = shown.actor.find(setup.guids.stats);
    if (!own || !own->obj.schema || !theirs) return false;
    static constexpr int64_t kStats[] = {content::kGuidHealth, content::kGuidMaxHealth, content::kGuidMana, content::kGuidMaxMana, combat::pvp::kStatCorruption};
    size_t total = 0;
    for (const int64_t record : kStats) {
        const repl::FastArrayItem* mine = nullptr;
        const repl::FastArrayItem* there = nullptr;
        for (const char* name : {"StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}) {
            const schema::NetField* f = own->obj.schema->field(name);
            const repl::FastArrayState* a = f ? own->obj.arrays.find(f->index) : nullptr;
            const repl::FastArrayState* b = f ? theirs->obj.arrays.find(f->index) : nullptr;
            if (!a || !b) continue;
            for (const repl::FastArrayItem& it : a->items()) {
                const auto v = it.values.values.find(1);
                const auto* t = v != it.values.values.end() ? v->second.get_if<repl::StatInt32Inst>() : nullptr;
                if (t && t->record == record) mine = &it;
            }
            for (const repl::FastArrayItem& it : b->items()) {
                const auto v = it.values.values.find(1);
                const auto* t = v != it.values.values.end() ? v->second.get_if<repl::StatInt32Inst>() : nullptr;
                if (t && t->record == record) there = &it;
            }
            if (mine) break;
        }
        if (!mine) continue;   // (a stat this character has no item for)
        if (!there || !(there->values == mine->values)) return false;
        ++total;
    }
    return total > 0;
}

/// A creature's blow on a player, through his port (the world's way in).
void wound(combat::CombatWorld& cw, const bs::PlayerSetup& who, uint64_t by, double amount, double now) {
    combat::IncomingHit h;
    h.instigator = by;
    h.hit_guid = 1;
    h.mods.push_back({1, content::kGuidHealth, -amount});
    h.at = now;
    if (combat::PlayerPort* p = cw.find_player(who.guids.pawn.ObjectId)) p->receive_hit(h, now);
}

/// The GroupGuid of the last ClientRequestJoinGroup a session's client was sent (zero: none).
std::array<uint32_t, 4> invited_group(Run& r) {
    std::array<uint32_t, 4> out{};
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    for (const fake::Delivered& d : r.h.got) {
        if (d.channel != r.h.pc_ch) continue;
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            size_t pos = b.payload_pos;
            const auto calls = repl::read_rpc_fields(pc, d.content.data(), pos + b.payload_bits, pos);
            if (!calls) continue;
            for (const repl::RpcCall& c : *calls) {
                if (!c.decoded || std::string(c.rpc->name) != "ClientRequestJoinGroup") continue;
                const char* const words[] = {"GroupGuid.A", "GroupGuid.B", "GroupGuid.C", "GroupGuid.D"};
                for (size_t i = 0; i < 4; ++i) {
                    if (const auto v = repl::rpcf::int_or(repl::rpcf::value_of(*c.rpc, c.params, words[i]), 0)) out[i] = static_cast<uint32_t>(*v);
                }
            }
        }
    }
    return out;
}

/// ServerInviteToGroup(CharacterName) / ServerAcceptJoinGroup(GroupGuid, InviterName) on a session's PlayerController.
BunchWriter party_invite_bunch(const std::string& name) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerInviteToGroup");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "CharacterName", repl::Value::str(name))) return BunchWriter();
    return actor_bunch(pc, "ServerInviteToGroup", p);
}
BunchWriter party_accept_bunch(const std::array<uint32_t, 4>& group, const std::string& inviter) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerAcceptJoinGroup");
    repl::RpcParams p;
    bool ok = rpc != nullptr;
    const char* const words[] = {"GroupGuid.A", "GroupGuid.B", "GroupGuid.C", "GroupGuid.D"};
    for (size_t i = 0; ok && i < 4; ++i) ok = repl::set_rpc_param(*rpc, p, words[i], repl::Value::i64(static_cast<int32_t>(group[i])));
    ok = ok && repl::set_rpc_param(*rpc, p, "InviterName", repl::Value::str(inviter));
    return ok ? actor_bunch(pc, "ServerAcceptJoinGroup", p) : BunchWriter();
}

/// Players of level 25 in one world with ground and two wolves far off, phased in.  The first is a Cleric.
struct Players {
    std::vector<svc::Character> records;
    std::vector<std::unique_ptr<Run>> runs;
    std::vector<uint64_t> wolves;
    uint64_t far_wolf = 0, next_actor_id = 5000000;
    bool ground = false;

    Players() = default;
    Players(const Players&) = delete;
    Players& operator=(const Players&) = delete;
    /// The joined sessions go before the first Run, whose world they are in.
    ~Players() {
        while (!runs.empty()) runs.pop_back();
    }

    bool start(size_t count) {
        static const char* const ids[] = {"bbec5c86eb101fe5f38c87f0fa22e7d1", "cbec5c86eb101fe5f38c87f0fa22e7d2", "dbec5c86eb101fe5f38c87f0fa22e7d3"};
        static const char* const names[] = {"AshesPatient", "AshesStranger", "AshesThird"};
        for (size_t i = 0; i < count; ++i) {
            svc::Character c = bard25_bagged();
            if (i == 0) c.class_id = 3;   // the Cleric
            else {
                c.id = ids[i - 1];
                c.name = names[i - 1];
            }
            records.push_back(c);
        }
        runs.push_back(std::make_unique<Run>(0, true, &records[0], [&](PlayerWorld& w) { spawn_wolves(w, wolves, far_wolf, 2, 24, 800, ground, next_actor_id); }));
        if (!ground || !world().combat()) return false;
        for (size_t i = 1; i < count; ++i) runs.push_back(std::make_unique<Run>(0, true, &records[i], std::function<void(PlayerWorld&)>{}, &world()));
        for (auto& r : runs) r->play(0.5, false);
        for (auto& r : runs) phase_in(*r);
        all(1.0);
        return true;
    }
    PlayerWorld& world() { return *runs[0]->h.fx.world; }
    Run& operator[](size_t i) { return *runs[i]; }
    bs::PlayerSetup setup(size_t i) { return runs[i]->h.s->bootstrap().setup(); }
    void all(double seconds) {
        for (int i = 0; i < static_cast<int>(std::llround(seconds * 10)); ++i) {
            for (auto& r : runs) r->play(0.1, false);
            world().tick_world(runs[0]->now());
        }
    }
};

}  // namespace players_tests

static void test_players_a_cleric_helps_another_player() {
    using namespace players_tests;
    std::printf("abilities between players: Mend heals another player, his health bar follows, a harmful hit does not reach him %s\n", kAftermathNote);
    Players p;
    const bool started = p.start(2);
    CHECK(started, "(two players in one world with ground and fights)");
    if (!started) return;
    Run& a = p[0];
    Run& b = p[1];
    PlayerWorld& world = p.world();
    combat::CombatWorld& cw = *world.combat();
    const bs::PlayerSetup sa = p.setup(0), sb = p.setup(1);
    CHECK(cw.player_pawn(sb.guids.pawn.ObjectId) != nullptr && cw.player_pawn(sa.guids.pawn.ObjectId) != nullptr, "each player is the other's target: his pawn is in the world and in the fights");
    CHECK(cw.actor_of_object(sb.guids.collision_cylinder.ObjectId) == sb.guids.pawn.ObjectId, "the cylinder a client names is the pawn's");
    const abilities::Catalog& cat = *world.ability_catalog();
    const abilities::AbilityDef* mend = cat.ability_named("Cleric_Mend");
    const abilities::AbilityDef* axe = cat.ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(mend && axe, "(the skills are in the tables)");
    if (!mend || !axe) return;
    a.h.s->teach_ability(mend->row.guid);
    a.h.s->teach_ability(axe->row.guid);

    // Mend, cast at him: the projectile flies and heals him, not the caster
    const double max_hp = b.m.stats[content::kGuidMaxHealth];
    wound(cw, sb, p.wolves[0], max_hp * 0.5, a.now());
    p.all(0.5);
    const double hurt = b.m.stats[content::kGuidHealth];
    CHECK(hurt < max_hp * 0.6, "(he is wounded)");
    const world::WorldActor* shown = shown_of(b, world);
    CHECKF(shown && shown_stats_follow(b, *shown), "the pawn the others are shown carries his Health as it is now (%.0f of %.0f)", hurt, max_hp);
    const int32_t id = a.next_id++;
    const BunchWriter trig = trigger_at_object(sa, mend->row.guid, id, sb.guids.collision_cylinder);
    a.send_trigger(mend->row.guid, id, &trig);
    p.all(1.5);
    const std::vector<int32_t> ends = a.m.end_ids();
    CHECK(a.m.has_ability(mend->row.guid) || std::find(ends.begin(), ends.end(), id) == ends.end(), "the cast at him is accepted");
    CHECKF(b.m.stats[content::kGuidHealth] > hurt + 1, "his Health rises (%.1f to %.1f)", hurt, b.m.stats[content::kGuidHealth]);
    const repl::HitInstance* conf = nullptr;
    for (const repl::HitInstance& h : a.m.hits) {
        if (!h.targets.empty()) conf = &h;
    }
    CHECK(conf != nullptr && conf->targets.size() == 1, "the caster's client is told of the hit on him, and on nobody else");
    p.all(0.5);
    shown = shown_of(b, world);
    CHECKF(shown && shown_stats_follow(b, *shown), "his health bar on the pawn the others are shown follows the heal (%.0f)", b.m.stats[content::kGuidHealth]);

    // a harmful hit does not reach another player (no fight between players is served here)
    const double before_swing = b.m.stats[content::kGuidHealth];
    int64_t axe_targeting = 0;
    for (const content::TargetingRow& t : axe->targetings) {
        if (!axe->hits.empty() && t.key == axe->hits[0].key) axe_targeting = t.guid;
    }
    const int32_t swing = a.next_id++;
    const BunchWriter trig_swing = trigger_at_object(sa, axe->row.guid, swing, sb.guids.collision_cylinder);
    a.send_trigger(axe->row.guid, swing, &trig_swing);
    p.all(0.6);
    if (!axe->hits.empty()) a.send_reliable(hit_request_at_object(sa, axe->row.guid, axe_targeting, axe->hits[0].hit_guid, sb.guids.collision_cylinder));
    p.all(0.8);
    CHECKF(b.m.stats[content::kGuidHealth] >= before_swing - 0.5, "the axe's swing at him hurts nobody (%.0f, then %.0f)", before_swing, b.m.stats[content::kGuidHealth]);

    CHECK(!a.h.s->abilities_failed() && !b.h.s->abilities_failed(), "(nothing failed)");
}

AOC_REGISTER_TEST(test_players_a_cleric_helps_another_player);

static void test_players_a_party_heal_reaches_the_party() {
    using namespace players_tests;
    std::printf("Communal Restoration (Self and Party in its mask) reaches the Cleric's party member, not a player outside the party %s\n", kAftermathNote);
    Players p;
    const bool started = p.start(3);
    CHECK(started, "(three players in one world with ground and fights)");
    if (!started) return;
    Run& a = p[0];
    Run& b = p[1];
    Run& c = p[2];
    PlayerWorld& world = p.world();
    combat::CombatWorld& cw = *world.combat();
    const bs::PlayerSetup sa = p.setup(0), sb = p.setup(1), sc = p.setup(2);
    // the Cleric invites the member, who accepts (the party's own calls)
    send_on_pc(a, party_invite_bunch(p.records[1].name));
    p.all(0.5);
    const std::array<uint32_t, 4> group = invited_group(b);
    const bool invited = group != std::array<uint32_t, 4>{};
    CHECK(invited, "(the member is invited)");
    send_on_pc(b, party_accept_bunch(group, p.records[0].name));
    p.all(0.5);
    CHECK(a.h.s->in_party() && b.h.s->in_party() && !c.h.s->in_party(), "the Cleric and the member share a party, the stranger is in none");
    CHECK(cw.inspect(sb.guids.pawn.ObjectId, cw.find_player(sa.guids.pawn.ObjectId)->fighter()).group == 2 &&
              cw.inspect(sc.guids.pawn.ObjectId, cw.find_player(sa.guids.pawn.ObjectId)->fighter()).group == 0,
          "the member is in the Cleric's squad, the stranger in none");
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
    CHECKF(gain_b > gain_c + 1, "the party member's Health rises by more than the stranger's own regeneration (%.1f against %.1f)", gain_b, gain_c);
    CHECK(restoration && b.m.has_effect(restoration->row.guid), "the party member holds Communal Restoration's effect");
    CHECK(restoration && !c.m.has_effect(restoration->row.guid), "the stranger does not");
    // the effect's TempHealth mod is a shield: the caster's numbers give it to the party member (the ally path keeps it as the player-hit path does), the stranger has none
    CHECKF(b.m.shields.size() == 1 && b.m.shields[0].first > 0.0f, "the party member holds Communal Restoration's shield (%zu shield(s))", b.m.shields.size());
    CHECK(c.m.shields.empty(), "the stranger holds no shield");
    CHECK(!a.h.s->abilities_failed() && !b.h.s->abilities_failed() && !c.h.s->abilities_failed(), "(nothing failed)");
}

AOC_REGISTER_TEST(test_players_a_party_heal_reaches_the_party);

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/others_stance_tests.inc: other players see a player's combat stance (Social, seeing other players: their combat stances)
//
//  A player (cyb) and a watcher (ayla), or two players and a watcher, in one world (tests/mounts_ride_others_rig.h).  The player draws (ServerSetTargeting), is in a fight
//  (bInCombat: up with the first exchange, down six seconds after the last, down at death), turns the PvP flag on and off, flags himself by a harmful action on the other player,
//  dies in a fight and rides; what a watcher's client is sent is read chain by chain (the update of the pawn the watchers hold): the five stance bits of CombatInfo
//  (bIsTargeting, bInCombat, bPvPForceFlag, InPvpSuppressionArea and bInPVPCombat) and the AlignmentComponent's Alignments (the name colour: the Combatant from the first
//  flagging action, the NonCombatant before it and after its 60 s).
//
//  Also: a player who comes into view in the middle of a fight gets the bits in the open, the same change twice sends nothing, a draw request over the budget is dropped, a
//  mounted player's seated pawn carries his stance, a player who logs out and back in starts with no stance, and two players' stances stay on their own pawn.
//
//  Modelled: no recording shows another player's pawn receive the stance (nothing records an observer).  The data's own sequence gives the Combatant alignment at the first
//  harmful action, not at the flag bit, and that is what session/player_pvp.cpp runs now; bInPVPCombat comes with a harmful hit between two players, and InPvpSuppressionArea
//  stays 0 (this world has no suppression area).
// ============================================================================

/// The two alignment records the PvP flag swaps on a player (the design data): Combatant is purple with a 60 s removal timer, NonCombatant white; both override the
/// nameplate colour.
static constexpr uint64_t kStanceCombatant = 6064629443707731976ull;
static constexpr uint64_t kStanceNonCombatant = 6064629443712188425ull;

/// The AlignmentContainer an object's `Alignments` holds (nullopt: the object carries none).
static std::optional<repl::AlignmentContainer> container_of(const repl::ObjectState& o) {
    if (!o.schema) return std::nullopt;
    const repl::Value* v = o.state.get(*o.schema, "Alignments");
    const auto* c = v ? v->get_if<repl::AlignmentContainer>() : nullptr;
    if (!c) return std::nullopt;
    return *c;
}

/// The AlignmentComponent's Alignments on the pawn the watchers hold (nullopt: none).
static std::optional<repl::AlignmentContainer> shown_alignments(const world::WorldActor& pawn) {
    for (const repl::Subobject& sub : pawn.actor.subobjects) {
        if (sub.obj.schema && std::string(sub.obj.schema->name) == "UAlignmentComponent") return container_of(sub.obj);
    }
    return std::nullopt;
}

/// ... and on the player's own pawn.
static std::optional<repl::AlignmentContainer> own_alignments(const Harness& h) {
    for (const repl::Subobject& sub : h.s->bootstrap().pawn().subobjects) {
        if (sub.obj.schema && std::string(sub.obj.schema->name) == "UAlignmentComponent") return container_of(sub.obj);
    }
    return std::nullopt;
}

static bool names_alignment(const repl::AlignmentContainer& c, uint64_t key) {
    for (const repl::AlignmentContainer::Instance& i : c.instances) {
        if (i.key == key) return true;
    }
    return false;
}

/// A bit of the player's own pawn's UPlayerCombatInfo (nullopt: the pawn does not carry it).
static std::optional<int64_t> own_pawn_bit(const Harness& h, const char* path) {
    for (const repl::Subobject& sub : h.s->bootstrap().pawn().subobjects) {
        if (sub.obj.schema && std::string(sub.obj.schema->name) == "UPlayerCombatInfo") return int_of(sub.obj, path);
    }
    return std::nullopt;
}

/// A bit of the pawn the watchers hold (nullopt: the pawn does not carry it).
static std::optional<int64_t> shown_bit(const world::WorldActor& pawn, const char* path) {
    for (const repl::Subobject& sub : pawn.actor.subobjects) {
        if (sub.obj.schema && std::string(sub.obj.schema->name) == "UPlayerCombatInfo") return int_of(sub.obj, path);
    }
    return std::nullopt;
}

/// The stance one chain of a watcher carries: the bits of the pawn's CombatInfo it names, and the Alignment it names in the same chain.
struct StanceFrame {
    Clock::time_point                       at{};
    std::optional<int64_t>                  targeting, in_combat, pvp_flag, suppression, pvp_combat, dead;
    std::optional<repl::AlignmentContainer> alignments;
};

static std::optional<int64_t> frame_bit(const StanceFrame& f, const char* path) {
    const std::string p = path;
    if (p == "bIsTargeting") return f.targeting;
    if (p == "bInCombat") return f.in_combat;
    if (p == "bPvPForceFlag") return f.pvp_flag;
    if (p == "InPvpSuppressionArea") return f.suppression;
    if (p == "bInPVPCombat") return f.pvp_combat;
    if (p == "bIsDead") return f.dead;
    return std::nullopt;
}

/// The stance `d` carries for the pawn (nullopt: the chain carries no CombatInfo block of it).
static std::optional<StanceFrame> stance_in(const fake::Delivered& d, const repl::Actor& pawn, uint64_t combat, uint64_t alignment, Clock::time_point at) {
    const States s = parse_states(d, pawn);
    if (s.find(combat) == s.end()) return std::nullopt;
    StanceFrame f;
    f.at = at;
    f.targeting = number_in(s, combat, "bIsTargeting");
    f.in_combat = number_in(s, combat, "bInCombat");
    f.pvp_flag = number_in(s, combat, "bPvPForceFlag");
    f.suppression = number_in(s, combat, "InPvpSuppressionArea");
    f.pvp_combat = number_in(s, combat, "bInPVPCombat");
    f.dead = number_in(s, combat, "bIsDead");
    if (const repl::Value* v = value_in(s, alignment, "Alignments")) {
        if (const auto* c = v->get_if<repl::AlignmentContainer>()) f.alignments = *c;
    }
    return f;
}

/// The first chain on `channel` from `from` on that carries the CombatInfo bit `path` as `want`, with the rest of the stance in that same chain.
static std::optional<StanceFrame> stance_chain(const Harness& watcher, uint32_t channel, const repl::Actor& pawn, uint64_t combat, uint64_t alignment, size_t from,
                                               const char* path, int64_t want) {
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close || d.open) continue;
        const auto f = stance_in(d, pawn, combat, alignment, watcher.got_at[i]);
        if (!f) continue;
        if (frame_bit(*f, path) == want) return f;
    }
    return std::nullopt;
}

/// The stance the open of the watcher's channel carries (nullopt: no open, or it carries no CombatInfo).
static std::optional<StanceFrame> stance_open(const Harness& watcher, uint32_t channel, const repl::Actor& pawn, uint64_t combat, uint64_t alignment, size_t from = 0) {
    const fake::Delivered* open = open_on(watcher, channel, from);
    return open ? stance_in(*open, pawn, combat, alignment, Clock::time_point{}) : std::nullopt;
}

/// ServerSetTargeting(bNewTargeting) on the pawn: the client's draw.
static BunchWriter targeting_call(bool on) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerSetTargeting");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "bNewTargeting", repl::Value::boolean(on))) {
        std::printf("  [FAIL] ServerSetTargeting's parameter does not fit\n");
        return BunchWriter();
    }
    return actor_rpc(pawn, "ServerSetTargeting", p);
}

/// ServerSetPvpFlag(Flag) on the PlayerController: the client's PvP flag.
static BunchWriter pvp_flag_call(bool on) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerSetPvpFlag");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "Flag", repl::Value::boolean(on))) {
        std::printf("  [FAIL] ServerSetPvpFlag's parameter does not fit\n");
        return BunchWriter();
    }
    return actor_rpc(pc, "ServerSetPvpFlag", p);
}

/// A player (cyb) and a watcher (ayla) in one world whose fights are on (the real content.db and the ground it needs), or the watcher `watch_dx` cm east of the player.
struct StanceScene {
    Harness                  a;
    Players                  all;
    std::unique_ptr<Harness> b;
    explicit StanceScene(double watch_dx = 0) : a(cyb_record(), prepare_combat_ground, nullptr, t0, /*real_world=*/true) {
        a.play();
        std::optional<std::pair<double, double>> at;
        if (watch_dx != 0) {
            const GoodPosition g = a.s->movement().good();
            at = std::make_pair(g.x + watch_dx, g.y);
        }
        b = join_world(a, all, other_record(other_id(2), "ayla", 4, at));
        a.run(milliseconds(400));
    }
    bool ok() const { return a.fx.world->combat() != nullptr && b && b->s; }
    uint32_t channel(uint64_t actor) const { return b->s->actor_view() ? b->s->actor_view()->channel_of(actor) : 0; }
};

/// An exchange in the player's fight, as the world counts it: the combat flag goes up, and the world ends it six seconds after the last exchange.
static void exchange(Harness& h) { h.fx.world->combat()->player_exchange(h.s->bootstrap().setup().guids.pawn.ObjectId, world_time_at(h.t)); }

// ---- the bits and the name colour ---------------------------------------------------------------------------------------------------------

static void test_other_players_see_a_players_stance_change() {
    std::printf("other players see a player's stance: the draw, the fight, the PvP flag (which does not colour the name) and the flagging action's Combatant name\n");
    StanceScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a.s->bootstrap().setup().guids.alignment.ObjectId;
    // the draw
    const size_t from_draw = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.run(milliseconds(250));
    CHECK(a.s->targeting() && own_pawn_bit(a, "bIsTargeting") == 1, "the player's own pawn takes bIsTargeting 1 from ServerSetTargeting");
    const auto draw = stance_chain(b, pch, pawn_copy, combat, alignment, from_draw, "bIsTargeting", 1);
    CHECK(draw.has_value(), "the watcher is sent bIsTargeting 1 on the pawn he holds");
    CHECK(shown_bit(*shown_pawn(a), "bIsTargeting") == 1, "...and the pawn the watchers hold carries it");
    // the same request again: nothing goes out
    const size_t from_draw_again = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.run(milliseconds(250));
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from_draw_again, "bIsTargeting", 1).has_value(), "the same targeting again sends nothing");
    // the PvP flag: the bit alone, and no colour with it (the Combatant comes from a harmful action)
    const size_t from_flag = b.got.size();
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    a.run(milliseconds(300));
    CHECK(a.s->pvp_flag() && own_pawn_bit(a, "bPvPForceFlag") == 1, "the player's own pawn takes bPvPForceFlag 1");
    const auto flag = stance_chain(b, pch, pawn_copy, combat, alignment, from_flag, "bPvPForceFlag", 1);
    CHECK(flag.has_value(), "the watcher is sent bPvPForceFlag 1");
    CHECK(flag && !(flag->alignments && names_alignment(*flag->alignments, kStanceCombatant)),
          "...and the flag alone carries no Combatant: the data's sequence colours a player at his first harmful action, not at the key");
    const auto shown_flag = shown_alignments(*shown_pawn(a));
    const auto own_flag = own_alignments(a);
    CHECK(shown_flag && names_alignment(*shown_flag, kStanceNonCombatant), "the pawn the watchers hold keeps the NonCombatant name");
    CHECK(own_flag && names_alignment(*own_flag, kStanceNonCombatant), "...and so does the player's own pawn");
    // a harmful action with the key on: the first flagging action, the Combatant name and bInPVPCombat
    const size_t from_harm = b.got.size();
    combat::Resolved blow;
    blow.mods.push_back({1, content::kGuidHealth, -20.0});
    a.fx.world->combat()->apply_hit_to_player(b.s->bootstrap().setup().guids.pawn.ObjectId, a.s->bootstrap().setup().guids.pawn.ObjectId, blow, world_time_at(a.t), nullptr);
    a.run(milliseconds(400));
    CHECK(a.s->pvp_flag_state() == combat::pvp::Flag::Combatant && a.s->pvp_flaggings() == 1, "a harmful action of his on another player flags him (Combatant)");
    CHECK(stance_chain(b, pch, pawn_copy, combat, alignment, from_harm, "bInPVPCombat", 1).has_value(), "the watcher is sent bInPVPCombat 1: the two trade a harmful hit");
    const auto shown_harm = shown_alignments(*shown_pawn(a));
    const auto own_harm = own_alignments(a);
    CHECK(shown_harm && names_alignment(*shown_harm, kStanceCombatant) && !names_alignment(*shown_harm, kStanceNonCombatant), "the pawn the watchers hold takes the Combatant name (the purple name)");
    CHECK(own_harm && names_alignment(*own_harm, kStanceCombatant), "...and so does the player's own pawn");
    // the same flag again: nothing goes out
    const size_t from_flag_again = b.got.size();
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    a.run(milliseconds(250));
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from_flag_again, "bPvPForceFlag", 1).has_value(), "the same flag again sends nothing");
    // the flag off: the bit only; the Combatant stays its 60 s after the flagging action
    const size_t from_off = b.got.size();
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(false), true)});
    a.run(milliseconds(300));
    CHECK(!a.s->pvp_flag() && own_pawn_bit(a, "bPvPForceFlag") == 0, "the PvP flag goes off on the player's own pawn");
    const auto off_flag = stance_chain(b, pch, pawn_copy, combat, alignment, from_off, "bPvPForceFlag", 0);
    CHECK(off_flag.has_value(), "the watcher is sent bPvPForceFlag 0");
    const auto shown_off = shown_alignments(*shown_pawn(a));
    CHECK(shown_off && names_alignment(*shown_off, kStanceCombatant), "...and the Combatant name stays (the key does not take it off; the record's 60 s timer does)");
    a.run(milliseconds(61000));
    const auto shown_end = shown_alignments(*shown_pawn(a));
    CHECK(shown_end && names_alignment(*shown_end, kStanceNonCombatant) && !names_alignment(*shown_end, kStanceCombatant), "60 s after the last flagging action the Combatant name is gone");
    // a fight: bInCombat up, and down six seconds after the last exchange
    const size_t from_fight = b.got.size();
    const Clock::time_point exchanged_at = a.t;
    exchange(a);
    a.run(milliseconds(300));
    const auto fight = stance_chain(b, pch, pawn_copy, combat, alignment, from_fight, "bInCombat", 1);
    CHECK(fight.has_value(), "a fight puts bInCombat 1 on the watcher's copy");
    CHECK(fight && fight->at - exchanged_at <= milliseconds(500), "...within a tick of the exchange");
    CHECK(shown_bit(*shown_pawn(a), "bInCombat") == 1, "the pawn the watchers hold says in combat");
    const size_t from_after_fight = b.got.size();
    a.run(milliseconds(4700));
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from_after_fight, "bInCombat", 0).has_value(), "five seconds after the exchange the player is still in combat");
    a.run(milliseconds(2000));
    const auto fight_off = stance_chain(b, pch, pawn_copy, combat, alignment, from_after_fight, "bInCombat", 0);
    CHECK(fight_off.has_value(), "six seconds after the last exchange the combat flag goes 0");
    CHECK(fight_off && fight_off->at - exchanged_at >= milliseconds(5700) && fight_off->at - exchanged_at <= milliseconds(7200),
          "...at the data's six seconds (within a tick), not at the next tick");
    CHECK(shown_bit(*shown_pawn(a), "bInCombat") == 0, "the pawn the watchers hold says out of combat");
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from_fight, "InPvpSuppressionArea", 1).has_value(), "InPvpSuppressionArea stays 0: this world has no suppression area");
}
AOC_REGISTER_TEST(test_other_players_see_a_players_stance_change);

// ---- the open of a player who comes into view in a fight ------------------------------------------------------------------------------------

static void test_a_player_who_comes_into_view_mid_fight_gets_his_stance_in_the_open() {
    std::printf("a watcher who comes into view in the middle of a fight with another player is shown the player's draw, combat, PvP flag and Combatant colour when its view opens\n");
    StanceScene sc(30000);   // the watcher stands 300 m east of where the player starts
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr, "(the player's pawn is in the world)");
    if (!pawn) return;
    const uint64_t pawn_id = pawn->id;
    CHECK(sc.channel(pawn_id) == 0, "300 m away the watcher holds nothing of the player");
    // a third player beside him: the harmful action is between the two of them, so the far watcher is sent nothing of it
    auto victim = join_world(a, sc.all, other_record(other_id(3), "kira", 5));
    a.run(milliseconds(200));
    CHECK(victim && victim->s && victim->s->bootstrap().setup().guids.pawn.ObjectId != 0, "(a third player stands beside him)");
    if (!victim || !victim->s) return;
    const uint64_t victim_pawn = victim->s->bootstrap().setup().guids.pawn.ObjectId;
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    exchange(a);
    a.run(milliseconds(300));
    // a harmful action with the key on: the first flagging action, which colours him and puts bInPVPCombat up on both
    combat::Resolved blow;
    blow.mods.push_back({1, content::kGuidHealth, -20.0});
    a.fx.world->combat()->apply_hit_to_player(victim_pawn, a.s->bootstrap().setup().guids.pawn.ObjectId, blow, world_time_at(a.t), nullptr);
    a.run(milliseconds(300));
    CHECK(a.s->targeting() && a.s->pvp_flag() && shown_bit(*shown_pawn(a), "bInCombat") == 1, "(the player draws, flags and fights out of the watcher's sight)");
    CHECK(a.s->pvp_flag_state() == combat::pvp::Flag::Combatant && shown_bit(*shown_pawn(a), "bInPVPCombat") == 1, "...and his harmful action flags him and puts the PvP combat bit up");
    CHECK(sc.channel(pawn_id) == 0, "(the watcher still holds nothing of the player)");
    const GoodPosition at = a.s->movement().good();
    CHECK(b.s->gm_teleport(b.conn, at.x + 50.0, at.y, at.z, b.t, "the test").status == PlayerSession::GmMoveResult::Status::Moved, "(the watcher is put beside the player)");
    a.run(milliseconds(500));
    const world::WorldActor* pawn2 = shown_pawn(a);
    const uint32_t pch = pawn2 ? sc.channel(pawn2->id) : 0;
    CHECK(pawn2 != nullptr && pch != 0, "(its view opens the player's pawn)");
    if (!pawn2 || pch == 0) return;
    const uint64_t combat = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a.s->bootstrap().setup().guids.alignment.ObjectId;
    const auto frame = stance_open(b, pch, pawn2->actor, combat, alignment);
    CHECK(frame.has_value(), "(the open carries the pawn's CombatInfo)");
    CHECK(frame && frame->targeting == 1, "the open carries bIsTargeting 1");
    CHECK(frame && frame->in_combat == 1, "...bInCombat 1");
    CHECK(frame && frame->pvp_flag == 1, "...bPvPForceFlag 1");
    CHECK(frame && frame->pvp_combat == 1 && frame->suppression == 0, "...bInPVPCombat 1 (his harmful action on the third player) and InPvpSuppressionArea 0");
    CHECK(frame && frame->dead == 0, "...the player is alive");
    CHECK(frame && frame->alignments && names_alignment(*frame->alignments, kStanceCombatant), "the open carries the Combatant alignment (the flagged name)");
    CHECK(shown_bit(*pawn2, "bIsTargeting") == 1 && shown_bit(*pawn2, "bInCombat") == 1 && shown_bit(*pawn2, "bPvPForceFlag") == 1 && shown_bit(*pawn2, "bInPVPCombat") == 1,
          "the pawn the watcher opens carries the draw, the fight, the key and the PvP combat");
    const auto opened = shown_alignments(*pawn2);
    CHECK(opened && names_alignment(*opened, kStanceCombatant), "the pawn the watcher opens carries the Combatant name");
}

AOC_REGISTER_TEST(test_a_player_who_comes_into_view_mid_fight_gets_his_stance_in_the_open);

// ---- the rate limit and the same request twice ----------------------------------------------------------------------------------------------

static void test_the_draw_request_is_rate_limited() {
    std::printf("a client that floods ServerSetTargeting is heard up to its budget and no further, and the same bit changes nothing\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a.s->bootstrap().setup().guids.alignment.ObjectId;
    const size_t from = b.got.size();
    for (int i = 0; i < 20; ++i) a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.run(milliseconds(200));
    CHECK(a.s->targeting_calls() == 20, "the session hears all twenty draw requests");
    CHECK(a.s->targeting_changes() == 1, "the first one changes the draw; the same bit again changes nothing");
    CHECK(a.s->targeting_refused() == 4, "the four requests over the budget are dropped");
    CHECK(a.s->targeting() && own_pawn_bit(a, "bIsTargeting") == 1 && shown_bit(*shown_pawn(a), "bIsTargeting") == 1, "the one change reaches the player's own pawn and the watcher's copy");
    CHECK(stance_chain(b, pch, pawn_copy, combat, alignment, from, "bIsTargeting", 1).has_value(), "the watcher is sent the draw on");
    // the opposite bit after the budget refilled: heard, and shown
    a.run(milliseconds(100));
    const size_t from_off = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(false), true)});
    a.run(milliseconds(200));
    CHECK(!a.s->targeting() && own_pawn_bit(a, "bIsTargeting") == 0 && a.s->targeting_refused() == 4, "after the refill the next request is heard and turns the draw off");
    CHECK(stance_chain(b, pch, pawn_copy, combat, alignment, from_off, "bIsTargeting", 0).has_value(), "the watcher is sent the draw off");
}
AOC_REGISTER_TEST(test_the_draw_request_is_rate_limited);

// ---- a death in the middle of a fight -----------------------------------------------------------------------------------------------------

static void test_a_death_mid_fight_clears_the_combat_and_draw_bits_in_the_death_update() {
    std::printf("a player who dies in a fight is shown dead with bInCombat 0 and bIsTargeting 0 in the same update, his own pawn too; a late draw changes nothing\n");
    StanceScene sc;
    CHECK(sc.ok(), "(the world's fights are on)");
    if (!sc.ok()) return;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a.s->bootstrap().setup().guids.alignment.ObjectId;
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    exchange(a);
    a.run(milliseconds(300));
    CHECK(a.s->targeting() && shown_bit(*pawn, "bInCombat") == 1, "(the player draws and fights)");
    const size_t from = b.got.size();
    CHECK(a.s->gm_kill(a.conn, a.t, "the test").ok, "(the player dies in the fight)");
    a.run(milliseconds(500));
    const auto death = stance_chain(b, pch, pawn_copy, combat, alignment, from, "bIsDead", 1);
    CHECK(death.has_value(), "the watcher is sent bIsDead 1");
    CHECK(death && death->in_combat == 0 && death->targeting == 0, "...and that same update carries bInCombat 0 and bIsTargeting 0 (the death frame)");
    CHECK(own_pawn_bit(a, "bIsDead") == 1 && own_pawn_bit(a, "bInCombat") == 0 && own_pawn_bit(a, "bIsTargeting") == 0,
          "the player's own pawn says dead with no combat and no draw");
    CHECK(!a.s->targeting(), "(the draw is forgotten with the death)");
    CHECK(shown_bit(*shown_pawn(a), "bPvPForceFlag") == 1, "the PvP flag stays on the death (the death clears the combat and the draw only)");
    // a draw request after the death: the bit stays 0
    const size_t from_late = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.run(milliseconds(250));
    CHECK(!a.s->targeting() && own_pawn_bit(a, "bIsTargeting") == 0 && shown_bit(*shown_pawn(a), "bIsTargeting") == 0, "a draw request after the death changes nothing");
    CHECK(!stance_chain(b, pch, pawn_copy, combat, alignment, from_late, "bIsTargeting", 1).has_value(), "...and sends the watcher nothing");
}
AOC_REGISTER_TEST(test_a_death_mid_fight_clears_the_combat_and_draw_bits_in_the_death_update);

// ---- a mounted player ---------------------------------------------------------------------------------------------------------------------------

static void test_a_mounted_players_stance_is_shown_on_the_seated_pawn() {
    std::printf("a player who rides is still shown with his stance: the seated pawn carries the draw and the flag (and the key alone does not colour the name)\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the player rides)");
    if (!a.s->mounted()) return;
    const world::WorldActor* pawn = shown_pawn(a);
    const world::WorldActor* mount = a.mount();
    CHECK(pawn != nullptr && mount != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the seated pawn and its mount)");
    if (!pawn || !mount) return;
    const uint32_t pch = sc.channel(pawn->id);
    const repl::Actor pawn_copy = pawn->actor;
    const uint64_t combat = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a.s->bootstrap().setup().guids.alignment.ObjectId;
    const size_t from = b.got.size();
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    a.run(milliseconds(300));
    const auto draw = stance_chain(b, pch, pawn_copy, combat, alignment, from, "bIsTargeting", 1);
    CHECK(draw.has_value(), "the watcher is sent the seated pawn's bIsTargeting 1");
    const auto flag = stance_chain(b, pch, pawn_copy, combat, alignment, from, "bPvPForceFlag", 1);
    CHECK(flag.has_value(), "and its bPvPForceFlag 1");
    CHECK(flag && !(flag->alignments && names_alignment(*flag->alignments, kStanceCombatant)), "...with no Combatant on the seated pawn (the key alone is no flagging action)");
    CHECK(a.s->mounted(), "(the player still rides)");
    const world::WorldActor* now = shown_pawn(a);
    CHECK(now != nullptr && same_object(actor_object(now->actor, "AttachmentReplication.AttachParent"), mount->actor.guid), "...and the pawn the watchers hold is still attached to the mount");
}
AOC_REGISTER_TEST(test_a_mounted_players_stance_is_shown_on_the_seated_pawn);

// ---- a relog ---------------------------------------------------------------------------------------------------------------------------------------

static void test_a_relogged_player_comes_back_with_no_stance() {
    std::printf("a player who logs out with a stance and back in starts with no draw, no fight and no flag; the watcher sees the new pawn plain and the next change goes out\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pawn != nullptr && sc.channel(pawn ? pawn->id : 0) != 0, "(the watcher holds the player's pawn)");
    if (!pawn) return;
    const uint32_t pch = sc.channel(pawn->id);
    a.client_sends({a.client.actor(a.pawn_ch, targeting_call(true), true)});
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    a.run(milliseconds(300));
    CHECK(a.s->targeting() && a.s->pvp_flag(), "(the player draws and flags)");
    const GoodPosition saved = a.s->last_position();
    const size_t from = b.got.size();
    a.s->finish("the test");
    a.run(milliseconds(250));
    CHECK(shown_pawn(a) == nullptr && !closes_on(b, pch, from).empty(), "(the pawn left the registry and the watcher's channel was closed)");
    svc::Character again = cyb_record();
    again.has_position = true;
    again.position.x = saved.x;
    again.position.y = saved.y;
    again.position.z = saved.z;
    auto a2 = join_world(a, sc.all, again);
    a.run(milliseconds(600));
    const world::WorldActor* pawn2 = shown_pawn(*a2);
    const uint32_t pch2 = pawn2 ? sc.channel(pawn2->id) : 0;
    CHECK(pawn2 != nullptr && pch2 != 0, "(the watcher is sent the returned player's pawn)");
    if (!pawn2 || pch2 == 0) return;
    const uint64_t combat = a2->s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment = a2->s->bootstrap().setup().guids.alignment.ObjectId;
    const auto frame = stance_open(b, pch2, pawn2->actor, combat, alignment, from);
    CHECK(frame.has_value(), "(the open carries the pawn's CombatInfo)");
    CHECK(frame && frame->targeting == 0 && frame->in_combat == 0 && frame->pvp_flag == 0 && frame->pvp_combat == 0 && frame->suppression == 0,
          "a new session starts with every stance bit 0 (the flag is not saved)");
    CHECK(frame && frame->alignments && names_alignment(*frame->alignments, kStanceNonCombatant) && !names_alignment(*frame->alignments, kStanceCombatant),
          "...and the NonCombatant alignment (the plain name)");
    // a change after the relog reaches the watcher again, on the new pawn
    const size_t from2 = b.got.size();
    a2->client_sends({a2->client.actor(a2->pc_ch, pvp_flag_call(true), true)});
    a.run(milliseconds(300));
    const auto flag = stance_chain(b, pch2, pawn2->actor, combat, alignment, from2, "bPvPForceFlag", 1);
    CHECK(flag.has_value() && !(flag->alignments && names_alignment(*flag->alignments, kStanceCombatant)),
          "a flag turned on after the relog reaches the watcher, with no Combatant (the key alone)");
}
AOC_REGISTER_TEST(test_a_relogged_player_comes_back_with_no_stance);

// ---- another player's harm on a mounted player ---------------------------------------------------------------------------------------------
// Not tested here: the mounts_ride rig can ride only in its non-real world (there is no mount in the real-content world: `ride_on` fails), and a
// non-real world has no fights (`PlayerWorld::combat()` is null), so another player's harmful hit on a mounted player cannot be driven.  The code
// path (take_hit's `to_mount`: a player's harmful hit lands on the mount and is not judged against the rider's Health) is the fix of finding 5;
// it is reported as not tested.

// ---- two players side by side -------------------------------------------------------------------------------------------------------------------

static void test_two_players_stances_side_by_side() {
    std::printf("two players who stand side by side keep their stances on their own pawns: the keyed one and the drawing one are told apart by the watcher\n");
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
    const uint32_t pc_a = a.s->actor_view()->channel_of(pc->id);
    CHECK(pa_b != 0 && pc_b != 0 && pa_b != pc_b, "the watcher holds both pawns, each on a channel of its own");
    CHECK(pc_a != 0, "the first player holds the second's pawn");
    if (!pa_b || !pc_b || !pc_a) return;
    const uint64_t combat_a = a.s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment_a = a.s->bootstrap().setup().guids.alignment.ObjectId;
    const uint64_t combat_c = c->s->bootstrap().setup().guids.combat_info.ObjectId;
    const uint64_t alignment_c = c->s->bootstrap().setup().guids.alignment.ObjectId;
    const size_t from_b = b->got.size();
    const size_t from_a = a.got.size();
    a.client_sends({a.client.actor(a.pc_ch, pvp_flag_call(true), true)});
    c->client_sends({c->client.actor(c->pawn_ch, targeting_call(true), true)});
    a.run(milliseconds(300));
    const auto flag_a = stance_chain(*b, pa_b, pa->actor, combat_a, alignment_a, from_b, "bPvPForceFlag", 1);
    const auto draw_c = stance_chain(*b, pc_b, pc->actor, combat_c, alignment_c, from_b, "bIsTargeting", 1);
    CHECK(flag_a.has_value(), "the watcher is sent the first player's PvP flag");
    CHECK(draw_c.has_value(), "and the second player's draw, on its own channel");
    CHECK(!stance_chain(*b, pc_b, pc->actor, combat_c, alignment_c, from_b, "bPvPForceFlag", 1).has_value(), "the second player is not flagged");
    CHECK(flag_a && !(flag_a->alignments && names_alignment(*flag_a->alignments, kStanceCombatant)),
          "the first player's chain carries no Combatant (his key alone does not colour it)");
    const auto name_c = shown_alignments(*pc);
    CHECK(name_c && names_alignment(*name_c, kStanceNonCombatant) && !names_alignment(*name_c, kStanceCombatant), "...and the second player's the NonCombatant's (his draw did not change the name)");
    CHECK(stance_chain(a, pc_a, pc->actor, combat_c, alignment_c, from_a, "bIsTargeting", 1).has_value(), "the first player sees the second's draw on his own client");
    CHECK(a.s->pvp_flag() && !c->s->pvp_flag() && c->s->targeting() && !a.s->targeting(), "(each session keeps its own stance)");
}
AOC_REGISTER_TEST(test_two_players_stances_side_by_side);

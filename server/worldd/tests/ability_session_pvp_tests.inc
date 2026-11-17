// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_pvp_tests.inc: PvP, slice 1, through two real sessions in one world: who may hit whom (combat/pvp.h), the attacker's flag
//  (the pawn's Alignments, the removal timer, NotifyFlaggingReason), bInPVPCombat, the flag's end, a corrupted target, the kill and a combatant's death debt,
//  a party member, whom nothing harms, (step 2) corruption: from the record, a creature's kill, a non-combatant's kill, a corrupted death, the save, and
//  (step 3) a duel: the request, the accept, the countdown, the fight without a flag, the blow that would kill, a forfeit, and (step 4) a respawn's
//  PvP protection
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

namespace pvp_tests {

/// ServerSetPvpFlag(Flag): the client's PvP key.
BunchWriter pvp_key(bool on) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerSetPvpFlag");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "Flag", repl::Value::boolean(on))) {
        std::printf("  [FAIL] ServerSetPvpFlag's parameters do not fit\n");
        return BunchWriter();
    }
    return actor_bunch(pc, "ServerSetPvpFlag", p);
}

/// ServerTriggerAbility at an object, carrying the force flag (bPvpForceFlag) as the client's PvP key held at the cast sets it.
BunchWriter trigger_forced(const bs::PlayerSetup& setup, int64_t guid, int32_t id, const emit::FIntrepidNetworkGUID& object, bool force) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    ok = ok && repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "bPvpForceFlag", repl::Value::boolean(force));
    if (ok) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = repl::Value(object);
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) {
        std::printf("  [FAIL] the forced trigger's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "ServerTriggerAbility", p);
}

/// The Alignments a pawn holds (the owner's bootstrap, or what the registry shows the others).
const repl::AlignmentContainer* alignments_of(const repl::Actor& pawn, const emit::FIntrepidNetworkGUID& component) {
    const repl::Subobject* al = pawn.find(component);
    const repl::Value* v = al ? al->obj.state.get(*al->obj.schema, "Alignments") : nullptr;
    return v ? v->get_if<repl::AlignmentContainer>() : nullptr;
}

bool holds(const repl::AlignmentContainer* c, int64_t alignment) {
    if (!c) return false;
    for (const auto& i : c->instances) {
        if (i.key == static_cast<uint64_t>(alignment)) return true;
    }
    return false;
}

size_t count_calls(const std::vector<std::string>& calls, const char* name) { return static_cast<size_t>(std::count(calls.begin(), calls.end(), std::string(name))); }

/// Two Bards of level 25 in one world with ground (and no creature near them), phased in; both know the 1H axe combo.
struct Duo {
    svc::Character ra = bard25_bagged(), rb = bard25_bagged();
    std::vector<uint64_t> wolves;
    uint64_t far_wolf = 0, next_actor_id = 5000000;
    bool ground = false;
    std::unique_ptr<Run> a, b;
    int64_t ability = 0, hit = 0, targeting = 0;
    bs::PlayerSetup sa, sb;
    PositionSink* sink = nullptr;   // the world's saves (step 2: the progress a session hands over)

    bool start() {
        rb.id = "bbec5c86eb101fe5f38c87f0fa22e7d1";
        rb.name = "AshesRival";
        a = std::make_unique<Run>(0, true, &ra, [&](PlayerWorld& w) {
            if (sink) w.set_position_sink(sink);
            spawn_wolves(w, wolves, far_wolf, 0, 24, 1.0, ground, next_actor_id);
        });
        if (!ground || !a->h.fx.world->combat()) return false;
        b = std::make_unique<Run>(0, true, &rb, std::function<void(PlayerWorld&)>{}, a->h.fx.world.get());
        a->play(0.5, false);
        b->play(0.5, false);
        phase_in(*a);
        phase_in(*b);
        both(1.0);
        sa = a->h.s->bootstrap().setup();
        sb = b->h.s->bootstrap().setup();
        const abilities::AbilityDef* axe = a->h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
        if (!axe || axe->hits.empty()) return false;
        ability = axe->row.guid;
        hit = axe->hits[0].hit_guid;
        for (const content::TargetingRow& t : axe->targetings) {
            if (t.key == axe->hits[0].key) targeting = t.guid;
        }
        a->h.s->teach_ability(ability);
        b->h.s->teach_ability(ability);
        return true;
    }
    void both(double seconds) {
        PlayerWorld& world = *a->h.fx.world;
        for (int i = 0; i < static_cast<int>(std::llround(seconds * 10)); ++i) {
            a->play(0.1, false);
            b->play(0.1, false);
            world.tick_world(a->now());
        }
    }
    /// `from` swings the axe at `to` (the trigger at his cylinder, then the hit's verification), with the force flag the trigger carries.
    void swing(Run& from, const bs::PlayerSetup& me, const bs::PlayerSetup& at, bool force) {
        const int32_t id = from.next_id++;
        const BunchWriter t = trigger_forced(me, ability, id, at.guids.collision_cylinder, force);
        from.send_trigger(ability, id, &t);
        both(0.6);
        from.send_reliable(hit_request_at_object(me, ability, targeting, hit, at.guids.collision_cylinder));
        both(0.8);
    }
};

/// The progress each save of the world's sessions carried (step 2: the corruption's).
struct ProgressSink : PositionSink {
    std::vector<std::pair<std::string, svc::Progress>> progress;
    void save(const std::string&, const svc::Position&) override {}
    void save_progress(const std::string& id, svc::Progress p) override { progress.emplace_back(id, std::move(p)); }
};

/// The worn items a corrupted death may drop (the armour, jewellery and weapons: Helmet 1 to RangedOffHand 18).
size_t worn_count(const PlayerSession& s) {
    size_t n = 0;
    for (const svc::InventoryItem& it : s.inventory().items()) {
        if (it.container == svc::ItemContainer::Equipment && it.slot >= items::equip::Helmet && it.slot <= items::equip::RangedOffHand && it.quantity > 0) ++n;
    }
    return n;
}

void wear(svc::Character& c, int64_t record, int32_t slot, uint32_t serial) {
    svc::InventoryItem it = stack_of(record, slot, 1, serial);
    it.container = svc::ItemContainer::Equipment;
    c.items.push_back(it);
}

bool about(double a, double b, double eps = 1e-3) { return std::fabs(a - b) < eps; }


/// ServerRequestDuel(InCharacterName) on the pawn: the /duel command.
BunchWriter duel_request(const std::string& name) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerRequestDuel");
    repl::RpcParams p;
    if (!rpc || !repl::set_rpc_param(*rpc, p, "InCharacterName", repl::Value::str(name))) {
        std::printf("  [FAIL] ServerRequestDuel's parameters do not fit\n");
        return BunchWriter();
    }
    return actor_bunch(pawn, "ServerRequestDuel", p);
}

/// ServerAcceptDuel / ServerDeclineDuel / ServerForfeitDuel(DuelGuid) on the pawn.
BunchWriter duel_call(const char* name, const std::array<uint32_t, 4>& g) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc(name);
    repl::RpcParams p;
    bool ok = rpc != nullptr;
    const char* const words[] = {"DuelGuid.A", "DuelGuid.B", "DuelGuid.C", "DuelGuid.D"};
    for (size_t i = 0; ok && i < 4; ++i) ok = repl::set_rpc_param(*rpc, p, words[i], repl::Value::i64(static_cast<int32_t>(g[i])));
    if (!ok) {
        std::printf("  [FAIL] %s's parameters do not fit\n", name);
        return BunchWriter();
    }
    return actor_bunch(pawn, name, p);
}

}  // namespace pvp_tests

void test_pvp_flagging_between_players() {
    using namespace pvp_tests;
    namespace pvp = combat::pvp;
    std::printf("PvP, slice 1: the force flag, the attacker's flag (purple, with its timer), the strike back, the flag's end, a corrupted target, the kill %s\n", kAftermathNote);
    Duo d;
    const bool started = d.start();
    CHECK(started, "two players in one world with ground, both with the 1H axe combo");
    if (!started) return;
    Run& a = *d.a;
    Run& b = *d.b;
    PlayerWorld& world = *a.h.fx.world;
    const float full = b.m.stats[content::kGuidMaxHealth];
    CHECK(a.h.s->pvp_flag_state() == pvp::Flag::NonCombatant && b.h.s->pvp_flag_state() == pvp::Flag::NonCombatant, "both start as non-combatants");

    // 1. without the force flag a non-combatant cannot be harmed: the swing hurts nobody and flags nobody
    d.swing(a, d.sa, d.sb, false);
    CHECKF(b.m.stats[content::kGuidHealth] >= full - 0.5f, "without the force flag the swing at a non-combatant hurts nobody (%.0f of %.0f)", b.m.stats[content::kGuidHealth], full);
    CHECK(a.h.s->pvp_flag_state() == pvp::Flag::NonCombatant && a.h.s->pvp_flaggings() == 0, "... and flags nobody");

    // 2. the PvP key held: the swing lands, and the attacker is a combatant
    send_on_pc(a, pvp_key(true));
    d.both(0.2);
    d.swing(a, d.sa, d.sb, false);
    const float hurt = b.m.stats[content::kGuidHealth];
    CHECKF(hurt < full - 1.0f, "with the PvP key the swing lands: his Health falls (%.0f of %.0f)", hurt, full);
    CHECK(a.h.s->pvp_flag_state() == pvp::Flag::Combatant, "the attacker is a combatant now");
    CHECK(b.h.s->pvp_flag_state() == pvp::Flag::NonCombatant, "the one he hit is not (taking a hit flags nobody)");
    CHECKF(count_calls(a.m.pc_calls, "NotifyFlaggingReason") == 1, "the attacker's client is told why (NotifyFlaggingReason, %zu)", count_calls(a.m.pc_calls, "NotifyFlaggingReason"));
    const repl::AlignmentContainer* own = alignments_of(a.h.s->bootstrap().pawn(), d.sa.guids.alignment);
    CHECK(holds(own, pvp::kCombatant) && !holds(own, pvp::kNonCombatant), "his pawn holds Combatant in NonCombatant's place");
    CHECK(own && own->timers.size() == 1 && own->timers[0].key == pvp::timer_key(pvp::kCombatant), "... with the removal timer the client counts down (key 1905362248)");
    const world::WorldActor* seen = world.registry().find(d.sa.guids.pawn.ObjectId);
    CHECK(seen && holds(alignments_of(seen->actor, d.sa.guids.alignment), pvp::kCombatant), "the other players are shown the same: his nameplate turns purple");
    CHECK(a.h.s->in_pvp_combat() && b.h.s->in_pvp_combat(), "both are in PvP combat (CombatInfo.bInPVPCombat)");
    send_on_pc(a, pvp_key(false));
    d.both(0.2);

    // 3. a combatant may be struck back without the force flag, and the one who strikes is flagged in turn; the attacker's second hit tells him nothing new
    const float a_full = a.m.stats[content::kGuidHealth];
    d.swing(b, d.sb, d.sa, false);
    CHECKF(a.m.stats[content::kGuidHealth] < a_full - 1.0f, "the one he hit strikes back without the force flag: a combatant may be harmed (%.0f of %.0f)", a.m.stats[content::kGuidHealth], a_full);
    CHECK(b.h.s->pvp_flag_state() == pvp::Flag::Combatant, "... and he is a combatant too (harming a combatant flags)");
    d.swing(a, d.sa, d.sb, false);
    CHECK(count_calls(a.m.pc_calls, "NotifyFlaggingReason") == 1, "a second hit of the combatant's refreshes his flag and tells him nothing new");

    // 4. the flags end 60 s after the last action that flagged
    d.both(62.0);
    CHECK(a.h.s->pvp_flag_state() == pvp::Flag::NonCombatant && b.h.s->pvp_flag_state() == pvp::Flag::NonCombatant, "a minute later both are non-combatants again");
    own = alignments_of(a.h.s->bootstrap().pawn(), d.sa.guids.alignment);
    CHECK(holds(own, pvp::kNonCombatant) && own->timers.empty(), "his pawn holds NonCombatant again, with no timer");
    CHECK(!a.h.s->in_pvp_combat() && !b.h.s->in_pvp_combat(), "and neither is in PvP combat");

    // 4b. a non-combatant is immune to another player's crowd control (NonCombatant.bGrantNonCombatantCrowdControlImmunity); a combatant is not
    const abilities::EffectDef* rooted = world.ability_catalog()->effect_named("Status_Rooted");
    const auto rooted_stacks = [&] { return b.h.s->pipeline() && rooted ? b.h.s->pipeline()->stacks_of(rooted->row.guid) : -1; };
    CHECK(rooted != nullptr && rooted->row.crowd_control_guid != 0 && b.h.s->pipeline() != nullptr, "(the data's root is a crowd control)");
    if (rooted && b.h.s->pipeline()) {
        effects::Applied root;   // (as the caster's pipeline works it out: HitResolver::resolve_effects_with)
        root.def = rooted;
        root.duration = 3.0;
        const std::vector<effects::Applied> roots{root};
        const size_t immune0 = world.combat()->pvp_cc_immunities();
        world.combat()->apply_effects_to_player(d.sb.guids.pawn.ObjectId, d.sa.guids.pawn.ObjectId, roots, a.now());
        d.both(0.3);
        CHECKF(world.combat()->pvp_cc_immunities() == immune0 + 1 && rooted_stacks() == 0, "another player's root does not hold a non-combatant (%d; immune %zu)", rooted_stacks(),
               world.combat()->pvp_cc_immunities() - immune0);
        CHECK(b.h.s->gm_pvp(b.h.conn, "combatant", 0, b.h.t, "the test").ok, "(a GM makes him a combatant)");
        world.combat()->apply_effects_to_player(d.sb.guids.pawn.ObjectId, d.sa.guids.pawn.ObjectId, roots, a.now());
        d.both(0.1);
        // (checked at once, well within the root's 3 s)
        CHECKF(world.combat()->pvp_cc_immunities() == immune0 + 1 && rooted_stacks() == 1, "... a combatant is not immune: the root lands on him (%d)", rooted_stacks());
        d.both(3.5);
        CHECK(b.h.s->gm_pvp(b.h.conn, "noncombatant", 0, b.h.t, "the test").ok, "(and a non-combatant again)");
    }

    // 5. a corrupted player may be harmed without the force flag, and harming him flags nobody
    CHECK(b.h.s->gm_pvp(b.h.conn, "corruption", 4.0, b.h.t, "the test").ok, "(a GM makes him corrupted: 4 corruption)");
    d.both(1.0);
    const world::WorldActor* red = world.registry().find(d.sb.guids.pawn.ObjectId);
    CHECK(red && holds(alignments_of(red->actor, d.sb.guids.alignment), pvp::kCorrupted), "his pawn holds Corrupted: the others see him red");
    const float before_red = b.m.stats[content::kGuidHealth];
    const uint64_t flaggings_before = a.h.s->pvp_flaggings();
    d.swing(a, d.sa, d.sb, false);
    CHECKF(b.m.stats[content::kGuidHealth] < before_red - 1.0f, "a corrupted player is harmed without the force flag (%.0f to %.0f)", before_red, b.m.stats[content::kGuidHealth]);
    CHECKF(a.h.s->pvp_flaggings() == flaggings_before, "... and harming him flags nobody (%llu flagging(s) before, %llu after; the root of 4b may have flagged him already)",
           static_cast<unsigned long long>(flaggings_before), static_cast<unsigned long long>(a.h.s->pvp_flaggings()));

    // 6. the kill: the victim, a combatant, takes half the death debt; the killer is told of the kill
    CHECK(b.h.s->gm_pvp(b.h.conn, "noncombatant", 0, b.h.t, "the test").ok && b.h.s->gm_pvp(b.h.conn, "combatant", 0, b.h.t, "the test").ok, "(a GM makes him a combatant)");
    send_on_pc(a, pvp_key(true));
    d.both(0.3);
    const leveling::Progress progress_before = b.h.s->progress();
    combat::Resolved blow;
    blow.mods.push_back({1, content::kGuidHealth, -1.0e9});
    world.combat()->apply_hit_to_player(d.sb.guids.pawn.ObjectId, d.sa.guids.pawn.ObjectId, blow, a.now(), nullptr);
    d.both(0.5);
    CHECK(b.h.s->dead(), "the blow kills him");
    CHECKF(a.h.s->pvp_kills() == 1, "the killer is told of the kill (%llu)", static_cast<unsigned long long>(a.h.s->pvp_kills()));
    const leveling::LevelCurve* curve = world.level_curve();
    if (curve) {
        const int64_t half = leveling::with_death_debt(*curve, progress_before, 0.01).debt - progress_before.debt;
        const int64_t added = b.h.s->progress().debt - progress_before.debt;
        CHECKF(half > 0 && added == half, "a combatant's death debt is half (+%lld, the half share %lld)", static_cast<long long>(added), static_cast<long long>(half));
    }
    CHECK(!a.h.s->abilities_failed() && !b.h.s->abilities_failed(), "(nothing failed)");
}
AOC_REGISTER_TEST(test_pvp_flagging_between_players);

void test_pvp_party_member_never_harmed() {
    using namespace pvp_tests;
    std::printf("PvP, slice 1: a party member cannot be harmed, force flag or not %s\n", kAftermathNote);
    Duo d;
    const bool started = d.start();
    CHECK(started, "two players in one world with ground, both with the 1H axe combo");
    if (!started) return;
    Run& a = *d.a;
    Run& b = *d.b;
    // the two form a party (the party's own calls: an invitation, then its acceptance)
    send_on_pc(a, players_tests::party_invite_bunch(d.rb.name));
    d.both(0.5);
    send_on_pc(b, players_tests::party_accept_bunch(players_tests::invited_group(b), d.ra.name));
    d.both(0.5);
    CHECK(a.h.s->in_party() && b.h.s->in_party(), "the two form a party");
    const float full = b.m.stats[content::kGuidHealth];
    send_on_pc(a, pvp_key(true));
    d.both(0.2);
    d.swing(a, d.sa, d.sb, true);
    CHECKF(b.m.stats[content::kGuidHealth] >= full - 0.5f, "with the force flag the swing at a party member hurts nobody (%.0f of %.0f)", b.m.stats[content::kGuidHealth], full);
    CHECK(a.h.s->pvp_flag_state() == combat::pvp::Flag::NonCombatant, "... and flags nobody");
}
AOC_REGISTER_TEST(test_pvp_party_member_never_harmed);

void test_pvp_corruption() {
    using namespace pvp_tests;
    namespace pvp = combat::pvp;
    std::printf("PvP, slice 1, step 2: corruption from the record, burned by a creature's kill, gained by a non-combatant's kill; a corrupted death (gear, debt, burn); the save %s\n",
                kAftermathNote);
    ProgressSink sink;
    Duo d;
    d.sink = &sink;
    d.rb.corruption = 7.0;   // he left corrupted (the record's, migration 0025): tier 3
    d.rb.blight = 0.5;
    wear(d.rb, content_fixture::item::kHelmet, items::equip::Helmet, 91001);
    wear(d.rb, content_fixture::item::kLegs, items::equip::Legs, 91002);
    wear(d.rb, content_fixture::item::kFeet, items::equip::Feet, 91003);
    wear(d.rb, content_fixture::item::kScepter, items::equip::Primary, 91004);
    const bool started = d.start();
    CHECK(started, "two players in one world with ground, both with the 1H axe combo");
    if (!started) return;
    Run& a = *d.a;
    Run& b = *d.b;
    PlayerWorld& world = *a.h.fx.world;
    d.both(1.0);

    // 1. the join: the record's corruption makes him red, with the stat and his tier's debuff
    CHECK(b.h.s->pvp_flag_state() == pvp::Flag::Corrupted && about(b.h.s->corruption(), 7.0) && about(b.h.s->blight(), 0.5),
          "the record's 7 corruption and 0.5 blight come back: he is corrupted");
    const world::WorldActor* red = world.registry().find(d.sb.guids.pawn.ObjectId);
    CHECK(red && holds(alignments_of(red->actor, d.sb.guids.alignment), pvp::kCorrupted), "... and the others see him red (Corrupted)");
    // (the join's own chains carry the stat as it is then; the test's client mirror starts after them, from the setup, so it is checked after a change below)
    const stats::StatValue* held = b.h.s->character_stats() ? b.h.s->character_stats()->find(pvp::kStatCorruption) : nullptr;
    CHECKF(held && about(held->value, 7.0) && b.h.s->bootstrap().has_stat_item(pvp::kStatCorruption), "the Corruption stat is 7, with its item in the pawn's stats (%.3f)",
           held ? held->value : -1.0);
    CHECKF(b.h.s->corruption_debuff() == pvp::tier_effect(3), "he holds tier 3's debuff, Corruption_3 (%lld; content.db of contentc version 50)",
           static_cast<long long>(b.h.s->corruption_debuff()));
    CHECK(worn_count(*b.h.s) == 4, "(he wears four items)");

    // 2. a creature's kill burns a little off (CorruptionReductionOnKillNPC), and his client is told
    const size_t kills0 = b.h.s->kills_rewarded();
    combat::Resolved wolf_kill;   // (on the shared world: b's own fixture world has no ground and no combat)
    wolf_kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    world.combat()->apply_hit_to_npc(d.far_wolf, d.sb.guids.pawn.ObjectId, wolf_kill, a.now());
    d.both(0.5);
    const double after_wolf = b.h.s->corruption();
    CHECK(b.h.s->kills_rewarded() == kills0 + 1, "(the creature's kill is his)");
    CHECKF(after_wolf < 7.0 && after_wolf > 6.9, "a level-24 Wolf's kill burns a fortieth of a point or so (7 -> %.4f)", after_wolf);
    CHECK(count_calls(b.m.pc_calls, "NotifyCorruptionDeltaForKill") == 1, "... and his client is told (NotifyCorruptionDeltaForKill)");
    CHECKF(about(b.m.stats[pvp::kStatCorruption], after_wolf), "... and the Corruption stat his client holds follows (%.4f)", b.m.stats[pvp::kStatCorruption]);

    // 3. he kills a non-combatant (with the PvP key: a non-combatant cannot be harmed without it): +1 corruption and half his blight, +1 blight;
    //    a corrupted killer stays corrupted (he is not made a combatant)
    send_on_pc(b, pvp_key(true));
    d.both(0.3);
    combat::Resolved blow;
    blow.mods.push_back({1, content::kGuidHealth, -1.0e9});
    world.combat()->apply_hit_to_player(d.sa.guids.pawn.ObjectId, d.sb.guids.pawn.ObjectId, blow, a.now(), nullptr);
    d.both(0.5);
    send_on_pc(b, pvp_key(false));
    d.both(0.2);
    CHECK(a.h.s->dead() && b.h.s->pvp_kills() == 1, "the blow kills the non-combatant, and the kill is his");
    const double gained = pvp::corruption_for_kill(25, 25, 0.5);
    CHECKF(about(b.h.s->corruption(), after_wolf + gained) && about(gained, 1.25), "the kill adds 1 + 0.5 / 2 = 1.25 corruption (%.4f -> %.4f)", after_wolf, b.h.s->corruption());
    CHECKF(about(b.h.s->blight(), 1.5) && about(b.m.stats[pvp::kStatBlight], 1.5), "... and 1 blight, which his client is told (Corruption_Blight %.3f)",
           b.m.stats[pvp::kStatBlight]);
    CHECK(b.h.s->pvp_flag_state() == pvp::Flag::Corrupted, "... and he is still corrupted, not a combatant");
    CHECK(count_calls(b.m.pc_calls, "NotifyCorruptionDeltaForKill") == 2, "... his client is told (NotifyCorruptionDeltaForKill)");
    CHECK(a.h.s->corruption() == 0 && a.h.s->corruption_debuff() == 0, "the victim gains none");

    // 4. his corrupted death (tier 3): 3 to 5 worn items into the pile, 2 % x 7 of a level's experience as debt, half his corruption burned
    const double before = b.h.s->corruption();
    const int tier = pvp::corruption_tier(before);
    CHECKF(tier == 3, "(he dies at tier 3: %.3f)", before);
    const leveling::Progress progress_before = b.h.s->progress();
    const size_t saves = sink.progress.size();
    world.combat()->apply_hit_to_player(d.sb.guids.pawn.ObjectId, 0, blow, a.now(), nullptr);
    d.both(0.5);
    CHECK(b.h.s->dead(), "a blow from no one kills him");
    const size_t dropped = b.h.s->gear_dropped_last();
    CHECKF(dropped >= 3 && dropped <= 4 && worn_count(*b.h.s) == 4 - dropped, "his death drops 3 to 5 of his worn items, here %zu of 4 (%zu left worn)", dropped,
           worn_count(*b.h.s));
    const leveling::LevelCurve* curve = world.level_curve();
    if (curve) {
        const int64_t want = leveling::with_death_debt(*curve, progress_before, 0.02 * pvp::xp_debt_mult(tier)).debt - progress_before.debt;
        const int64_t added = b.h.s->progress().debt - progress_before.debt;
        CHECKF(want > 0 && added == want, "the debt is 2 %% x 7 (CorruptionTier->XpDebtMult): +%lld, the share's %lld", static_cast<long long>(added), static_cast<long long>(want));
    }
    const double left = before - pvp::corruption_lost_dying(before);
    CHECKF(about(b.h.s->corruption(), left) && about(left, before / 2), "the death burns half his corruption (%.3f -> %.3f)", before, b.h.s->corruption());
    CHECK(count_calls(b.m.pc_calls, "NotifyCorruptionLostForDying") == 1, "... and his client is told (NotifyCorruptionLostForDying)");
    CHECKF(b.h.s->corruption_debuff() == pvp::tier_effect(pvp::corruption_tier(left)), "the debuff follows the tier (tier %d: %lld)", pvp::corruption_tier(left),
           static_cast<long long>(b.h.s->corruption_debuff()));

    // 5. the death's save carries what is left (migration 0025)
    bool saved = false;
    for (size_t i = saves; i < sink.progress.size(); ++i) {
        const auto& [id, p] = sink.progress[i];
        saved = saved || (id == d.rb.id && p.replace_pvp && about(p.corruption, left) && about(p.blight, 1.5));
    }
    CHECK(saved, "the death's save carries his corruption after the burn and his blight (replace_pvp)");
    bool victim_kept = true;
    for (const auto& [id, p] : sink.progress) victim_kept = victim_kept && !(id == d.ra.id && p.replace_pvp);
    CHECK(victim_kept, "the victim's saves leave his PvP row alone (nothing changed)");
    CHECK(!a.h.s->abilities_failed() && !b.h.s->abilities_failed(), "(nothing failed)");
}
AOC_REGISTER_TEST(test_pvp_corruption);

void test_pvp_duel() {
    using namespace pvp_tests;
    namespace pvp = combat::pvp;
    std::printf("PvP, slice 1, step 3: a duel: the request on the pawn, the banner, the accept, the countdown, the fight (the Duel alignment, no flag), the blow that "
                "would kill, the end %s\n", kAftermathNote);
    Duo d;
    const bool started = d.start();
    CHECK(started, "two players in one world with ground, both with the 1H axe combo");
    if (!started) return;
    Run& a = *d.a;
    Run& b = *d.b;
    PlayerWorld& world = *a.h.fx.world;
    const world::DuelBook* book = a.h.s->duel_book_for_test();
    CHECK(book && book == b.h.s->duel_book_for_test() && book->connected(d.sa.character_guid) && book->connected(d.sb.character_guid),
          "both sessions are in the world's duel book");
    if (!book) return;

    // 1. the request (the two stand together: in range)
    send_on_pawn(a, duel_request("ashesrival"));
    d.both(0.5);
    const world::DuelBook::Duel* duel = book->duel_of(d.sa.character_guid);
    CHECK(duel && duel->phase == world::DuelBook::Phase::Asked && duel->b == d.sb.character_guid && duel->id == d.sa.character_guid,
          "ServerRequestDuel(ashesrival): a request to AshesRival, the duel's guid the instigator's");
    CHECK(world.registry().count(world::ActorKind::DuelBanner) == 1, "the banner is in the world (its Duel opens the recipient's window)");
    send_on_pawn(b, duel_request(d.ra.name));
    d.both(0.3);
    CHECK(book->duels() == 1, "his own request back is refused: he is in that one");

    // 2. the accept, the countdown, the fight
    send_on_pawn(b, duel_call("ServerAcceptDuel", d.sa.character_guid));
    d.both(0.5);
    duel = book->duel_of(d.sa.character_guid);
    CHECK(duel && duel->phase == world::DuelBook::Phase::Counting, "ServerAcceptDuel(the duel's guid): the countdown runs");
    d.both(5.0);
    CHECK(book->fighting_key(d.sa.character_guid) != 0, "5 s later the fight begins");
    const repl::AlignmentContainer* own = alignments_of(a.h.s->bootstrap().pawn(), d.sa.guids.alignment);
    const world::WorldActor* seen = world.registry().find(d.sb.guids.pawn.ObjectId);
    CHECK(holds(own, pvp::kDuel) && seen && holds(alignments_of(seen->actor, d.sb.guids.alignment), pvp::kDuel), "both pawns hold the Duel alignment (the others see it)");

    // 3. the partner is harmed without the force flag, and nobody is flagged
    const float full = b.m.stats[content::kGuidHealth];
    d.swing(a, d.sa, d.sb, false);
    CHECKF(b.m.stats[content::kGuidHealth] < full - 1.0f, "his duel partner is harmed without the force flag (%.0f of %.0f)", b.m.stats[content::kGuidHealth], full);
    CHECK(a.h.s->pvp_flag_state() == pvp::Flag::NonCombatant && a.h.s->pvp_flaggings() == 0, "... and nobody is flagged (Duel FlaggingActions 0)");

    // 4. a blow that would kill: 1 Health, the duel is lost, the end gives both their Health back
    combat::Resolved blow;
    blow.mods.push_back({1, content::kGuidHealth, -1.0e9});
    world.combat()->apply_hit_to_player(d.sb.guids.pawn.ObjectId, d.sa.guids.pawn.ObjectId, blow, a.now(), nullptr);
    d.both(0.5);
    CHECK(!b.h.s->dead(), "a blow of his partner's that would kill him leaves him alive");
    CHECK(book->duels() == 0 && world.registry().count(world::ActorKind::DuelBanner) == 0, "the duel is over, the banner gone");
    CHECK(a.h.s->duels_won() == 1 && b.h.s->duels_won() == 0 && a.h.s->duels_fought() == 1 && b.h.s->duels_fought() == 1, "AshesCyb won, AshesRival lost");
    d.both(0.5);
    CHECKF(b.m.stats[content::kGuidHealth] >= b.m.stats[content::kGuidMaxHealth] - 0.5f, "both get their Health back (%.0f of %.0f)", b.m.stats[content::kGuidHealth],
           b.m.stats[content::kGuidMaxHealth]);
    own = alignments_of(a.h.s->bootstrap().pawn(), d.sa.guids.alignment);
    CHECK(!holds(own, pvp::kDuel), "the Duel alignment is gone");
    CHECK(a.h.s->corruption() == 0 && b.h.s->corruption() == 0 && a.h.s->pvp_kills() == 0, "no kill, no corruption");

    // 5. a second duel, forfeited
    send_on_pawn(b, duel_request(d.ra.name));
    d.both(0.3);
    send_on_pawn(a, duel_call("ServerAcceptDuel", d.sb.character_guid));
    d.both(5.5);
    CHECK(book->fighting_key(d.sb.character_guid) != 0, "AshesRival challenges, AshesCyb accepts: a second fight");
    send_on_pawn(a, duel_call("ServerForfeitDuel", d.sb.character_guid));
    d.both(0.5);
    CHECK(book->duels() == 0 && b.h.s->duels_won() == 1, "ServerForfeitDuel (the instigator's guid): AshesCyb gives it up, AshesRival wins");
    CHECK(!a.h.s->abilities_failed() && !b.h.s->abilities_failed(), "(nothing failed)");
}
AOC_REGISTER_TEST(test_pvp_duel);

void test_pvp_respawn_protection() {
    using namespace pvp_tests;
    namespace pvp = combat::pvp;
    std::printf("PvP, slice 1, step 4: a respawn's PvP protection (Emberspring_PVPOptInProtection): no harm from another player, force flag or not; his own attack "
                "ends it; none for a corrupted player; 60 s %s\n", kAftermathNote);
    Duo d;
    const bool started = d.start();
    CHECK(started, "two players in one world with ground, both with the 1H axe combo");
    if (!started) return;
    Run& a = *d.a;
    Run& b = *d.b;
    PlayerWorld& world = *a.h.fx.world;
    const auto die_and_respawn = [&](Run& r, const bs::PlayerSetup& s) {
        combat::Resolved blow;
        blow.mods.push_back({1, content::kGuidHealth, -1.0e9});
        world.combat()->apply_hit_to_player(s.guids.pawn.ObjectId, 0, blow, a.now(), nullptr);
        d.both(0.5);
        const bool died = r.h.s->dead();
        send_on_pawn(r, actor_bunch(class_named("PlayerPawn_C"), "Respawn", {}));
        d.both(1.0);
        return died && !r.h.s->dead();
    };
    const auto poke = [&](const bs::PlayerSetup& target, const bs::PlayerSetup& by) {
        combat::Resolved hit;
        hit.mods.push_back({1, content::kGuidHealth, -50.0});
        world.combat()->apply_hit_to_player(target.guids.pawn.ObjectId, by.guids.pawn.ObjectId, hit, a.now(), nullptr);
        d.both(0.5);
    };

    // 1. the respawn protects him
    CHECK(die_and_respawn(b, d.sb), "(he dies and respawns)");
    CHECK(b.h.s->pvp_protected() && b.h.s->pvp_protections() == 1 && b.h.s->pipeline()->stacks_of(pvp::kEffectEmberspringProtection) == 1,
          "the respawn gives him the PvP protection (Emberspring_PVPOptInProtection, a stack on his pipeline: his client shows it)");
    send_on_pc(a, pvp_key(true));
    d.both(0.2);
    const float hp = b.m.stats[content::kGuidHealth];
    poke(d.sb, d.sa);
    CHECKF(b.m.stats[content::kGuidHealth] >= hp - 0.5f && a.h.s->pvp_flaggings() == 0, "another player's harm does not reach him, even with the force flag, and flags nobody (%.0f of %.0f)",
           b.m.stats[content::kGuidHealth], hp);
    send_on_pc(a, pvp_key(false));
    d.both(0.2);

    // 2. his own attack ends it
    send_on_pc(b, pvp_key(true));
    d.both(0.2);
    poke(d.sa, d.sb);
    send_on_pc(b, pvp_key(false));
    d.both(0.3);
    CHECK(b.h.s->pvp_flag_state() == pvp::Flag::Combatant && !b.h.s->pvp_protected(), "his own attack flags him, and his protection is over");
    const float hp2 = b.m.stats[content::kGuidHealth];
    poke(d.sb, d.sa);
    CHECKF(b.m.stats[content::kGuidHealth] < hp2 - 1.0f, "now he is harmed (a combatant, without the force flag: %.0f of %.0f)", b.m.stats[content::kGuidHealth], hp2);

    // 3. a corrupted player comes back unprotected (the requirement's !IsCorrupted)
    CHECK(b.h.s->gm_pvp(b.h.conn, "noncombatant", 0, b.h.t, "the test").ok && b.h.s->gm_pvp(b.h.conn, "corruption", 10.0, b.h.t, "the test").ok, "(a GM makes him corrupted: 10, so the death leaves 5)");
    d.both(0.5);
    CHECK(die_and_respawn(b, d.sb), "(he dies and respawns)");
    CHECK(!b.h.s->pvp_protected() && b.h.s->pvp_protections() == 1, "corrupted, he gets no protection");

    // 4. it lasts its 60 s
    CHECK(die_and_respawn(a, d.sa) && a.h.s->pvp_protected(), "(the other dies and respawns: protected)");
    d.both(58.0);
    CHECK(a.h.s->pvp_protected(), "58 s later: still protected");
    d.both(3.0);
    CHECK(!a.h.s->pvp_protected(), "after 60 s it is gone");
    CHECK(!a.h.s->abilities_failed() && !b.h.s->abilities_failed(), "(nothing failed)");
}
AOC_REGISTER_TEST(test_pvp_respawn_protection);

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_session_dodge_cancel_tests.inc: a Dodge cancels an ability that has not gone out yet and gives back what it had paid; once the ability has gone out the Dodge changes nothing
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_session.cpp runs it through aoc_tests::run; it is included from ability_session_registered.inc)
// ============================================================================

/// One cast in a real session with a Dodge pressed around it. All times are seconds on the test's own clock, counted from the first press.
struct DodgeCast {
    const char* ability = "Mage_Frostbolt";
    const char* hit = "Mage_Frostbolt_Hit";   // the hit whose confirmations are counted
    bool        at_wolf = true;               // the call names the Wolf (else it names nobody)
    int64_t     branch = 0;                   // a cast bar's branch: the client asks for it `bar` seconds after the trigger (0: it never asks)
    double      bar = 0;
    double      dodge_at = -1;                // a Dodge pressed this long after the trigger (negative: none)
    double      dodge_before = -1;            // a Dodge pressed this long before the trigger (negative: none), to put the Dodge into its cooldown
    double      probe = -1;                   // when to look at the cast's montage on the client, after the trigger (negative: never)
    double      seconds = 6.5;                // how long the test plays after the trigger
};
struct DodgeEnd {
    bool                 ok = false;
    int                  landed = 0;          // confirmations of the hit the cast was to land
    int32_t              cast_id = -1;
    int32_t              dodge_id = -1;       // the Dodge pressed after the trigger
    std::vector<int32_t> ends;                // every ClientEndAbility the client got
    bool                 dodge_item = false;  // the Dodge's item was on the client right after it was pressed (the server took the Dodge)
    bool                 montage_on = false;  // at the probe: the cast's item showed a montage
    float                mana_before = 0, mana_after = 0;   // the client's Mana at the trigger and at the end
    float                health = 0;                        // the client's Health at the end
    bool                 cooldown_on = false;               // the client's list holds the cast's cooldown at the end
    int                  charges_left = -1;                 // the server's cooldown group of the ability (-1: none running)
};
static int ends_of(const DodgeEnd& e, int32_t id) { return static_cast<int>(std::count(e.ends.begin(), e.ends.end(), id)); }
static bool run_dodge_cast(const svc::Character& rec, const DodgeCast& dc, DodgeEnd& out) {
    WolfWorld ww;
    ww.level = 25;
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return false;
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor* wa = r.h.fx.world->registry().find(ww.wolf);
    const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
    const abilities::AbilityDef* def = cat ? cat->ability_named(dc.ability) : nullptr;
    if (!wa || !def) return false;
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
    r.h.s->teach_ability(def->row.guid);
    r.h.fx.world->tick_world(r.now());
    const size_t before = r.m.hits.size();
    const double cast_at = dc.dodge_before >= 0 ? dc.dodge_before : 0.0;
    const auto step = [&] {
        r.tick(false, std::chrono::milliseconds(50));
        r.h.fx.world->tick_world(r.now());
    };
    const auto montage_shown = [&] {
        for (const auto& [id, a] : r.m.abilities)
            if (a.ability == def->row.guid && a.has_montage) return true;
        return false;
    };
    bool pre = dc.dodge_before < 0, cast = false, dodge = dc.dodge_at < 0, requested = dc.branch == 0, probed = dc.probe < 0;
    const int steps = static_cast<int>((cast_at + dc.seconds) / 0.05);
    for (int i = 0; i < steps; ++i) {
        const double t = i * 0.05 + 1e-9;
        if (!pre && t >= 0) {
            pre = true;
            r.trigger(kDodge);
        }
        if (!cast && t >= cast_at) {
            cast = true;
            out.cast_id = r.next_id++;
            out.mana_before = r.m.stats[kManaStat];
            if (dc.at_wolf) {
                const BunchWriter trig = trigger_at_value(setup, def->row.guid, out.cast_id, repl::Value(cyl), 0);
                r.send_trigger(def->row.guid, out.cast_id, &trig);
            } else {
                r.send_trigger(def->row.guid, out.cast_id);
            }
        }
        if (cast && !dodge && t >= cast_at + dc.dodge_at) {
            dodge = true;
            out.dodge_id = r.trigger(kDodge);
            out.dodge_item = r.m.has_ability(kDodge);
        }
        if (cast && !requested && t >= cast_at + dc.bar) {
            requested = true;
            const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerRequestBranchVerification");
            repl::RpcParams bp;
            bool ok = rpc && repl::set_rpc_param(*rpc, bp, "Request.Branch.BranchGuid", repl::Value::i64(dc.branch));
            ok = ok && repl::set_rpc_param(*rpc, bp, "Request.Branch.MontageIndex", repl::Value::i64(0));
            ok = ok && repl::set_rpc_param(*rpc, bp, "Request.Branch.MeshYaw", repl::Value::u64(0));
            ok = ok && repl::set_rpc_param(*rpc, bp, "Request.Branch.Instigator", repl::Value(setup.guids.pawn));
            ok = ok && repl::set_rpc_param(*rpc, bp, "ClientInstanceId", repl::Value::i64(out.cast_id));
            if (ok) r.send_reliable(ability_rpc(setup.guids.ability, "ServerRequestBranchVerification", bp));
        }
        if (cast && !probed && t >= cast_at + dc.probe) {
            probed = true;
            out.montage_on = montage_shown();
        }
        step();
    }
    for (size_t k = before; k < r.m.hits.size(); ++k) {
        const abilities::HitDef* hd = cat->hit(r.m.hits[k].record_guid);
        if (hd && hd->row.name == dc.hit) ++out.landed;
    }
    out.ends = r.m.end_ids();
    out.mana_after = r.m.stats[kManaStat];
    out.health = r.m.stats[content::kGuidHealth];
    for (const auto& [id, guid] : r.m.cooldowns) out.cooldown_on = out.cooldown_on || guid == def->row.guid;
    if (r.h.s->pipeline()) out.charges_left = r.h.s->pipeline()->charges_left(def->row.guid);
    out.ok = true;
    return true;
}

// The game's rule: a Dodge ends the cast bar the character is in. Nothing is paid (the mana, the cooldown), nothing fires and the montage ends; the client is told the cast is over.
// A played run: a Frostbolt pressed, a Dodge 0.9 s later, no end from the client, and the bolt landed when the cast bar's deadline ran out.
void test_dodge_ends_the_cast_bar() {
    std::printf("a Dodge ends the cast bar the character is in: no hit lands, nothing is paid, the cast is ended on the client; a refused Dodge or one after the bar ended ends nothing\n");
    const svc::Character mage = class25(8, "Mage", 17750);
    constexpr int64_t kFrostbolt = 6064628890262700041ll;   // the branch of the Frostbolt's cast bar
    // the control: no Dodge, the cast bar's deadline runs out: the bolt lands, the mana is spent, the cooldown runs, and at 1.2 s the montage is on
    {
        DodgeCast dc;
        dc.probe = 1.2;
        DodgeEnd e;
        const bool ok = run_dodge_cast(mage, dc, e);
        CHECK(ok, "a world with ground and a Wolf for the Frostbolt");
        if (!ok) return;
        std::printf("  (control: %d landed, Mana %.1f -> %.1f, cooldown %d, charges %d, health %.0f)\n", e.landed, e.mana_before, e.mana_after, e.cooldown_on, e.charges_left, e.health);
        CHECK(e.health > 0.0f, "(the player lives through the run)");
        CHECK(e.landed == 1 && e.mana_before - e.mana_after > 50.0f && e.cooldown_on && e.charges_left == 0, "control, no Dodge: the bolt lands once, the Frostbolt's mana is spent and its cooldown runs");
        CHECK(e.montage_on, "control: the Frostbolt's montage is on the client at 1.2 s");
    }
    // a Dodge 0.9 s after the trigger, the client never asks for the bar's end (as in a played run): the cast is over at once
    {
        DodgeCast dc;
        dc.dodge_at = 0.9;
        dc.probe = 1.2;
        DodgeEnd e;
        const bool ok = run_dodge_cast(mage, dc, e);
        CHECK(ok, "a world with ground and a Wolf for the Frostbolt and a Dodge");
        if (!ok) return;
        std::printf("  (Dodge at 0.9 s: %d landed, Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s) for the cast, montage %d)\n", e.landed, e.mana_before, e.mana_after, e.cooldown_on,
                    e.charges_left, ends_of(e, e.cast_id), e.montage_on);
        CHECK(e.health > 0.0f, "(the player lives through the run)");
        CHECK(e.dodge_item, "the server took the Dodge (its item is on the client)");
        CHECK(e.landed == 0, "after a Dodge in the cast bar no bolt lands, not at the cast bar's deadline either");
        CHECK(e.mana_after >= e.mana_before - 0.5f, "... the Frostbolt's mana is not spent (a regeneration tick may have given some)");
        CHECK(!e.cooldown_on && e.charges_left == -1, "... and no cooldown runs for it, on the server or on the client");
        CHECK(ends_of(e, e.cast_id) == 1, "... the client is told the cast is over: one ClientEndAbility for its instance");
        CHECK(!e.montage_on, "... and the Frostbolt's montage is off the client at 1.2 s");
    }
    // a Dodge after the bar's end was asked for: the spell is released, the Dodge cannot take it back
    {
        DodgeCast dc;
        dc.branch = kFrostbolt;
        dc.bar = 2.2;
        dc.dodge_at = 2.35;
        DodgeEnd e;
        const bool ok = run_dodge_cast(mage, dc, e);
        CHECK(ok, "a world with ground and a Wolf for the Frostbolt, its bar's end and a Dodge");
        if (!ok) return;
        std::printf("  (Dodge after the bar's end: %d landed, Mana %.1f -> %.1f, cooldown %d)\n", e.landed, e.mana_before, e.mana_after, e.cooldown_on);
        CHECK(e.dodge_item, "the server took the Dodge");
        CHECK(e.landed == 1 && e.mana_before - e.mana_after > 50.0f && e.cooldown_on, "a Dodge after the cast bar ended ends nothing: the bolt lands, the mana is spent, the cooldown runs");
        CHECK(ends_of(e, e.cast_id) == 0, "... and the client is not told the cast ended");
    }
    // a Dodge the server refuses (the first one's cooldown runs, 0.3 s on) ends nothing
    {
        DodgeCast dc;
        dc.dodge_before = 0.1;
        dc.dodge_at = 0.2;
        DodgeEnd e;
        const bool ok = run_dodge_cast(mage, dc, e);
        CHECK(ok, "a world with ground and a Wolf for the Frostbolt and two Dodges");
        if (!ok) return;
        std::printf("  (refused Dodge: %d landed, Mana %.1f -> %.1f, cooldown %d, %d end(s) for the refused Dodge)\n", e.landed, e.mana_before, e.mana_after, e.cooldown_on, ends_of(e, e.dodge_id));
        CHECK(ends_of(e, e.dodge_id) == 1, "the second Dodge, 0.3 s after the first, is refused (ClientEndAbility for its instance)");
        CHECK(e.landed == 1 && e.mana_before - e.mana_after > 50.0f && e.cooldown_on && ends_of(e, e.cast_id) == 0, "a refused Dodge ends nothing: the bolt lands, the mana is spent, the cooldown runs, the cast is not ended");
    }
    // a cast that paid before its bar's end (the Arcane Circle takes its Mana and starts its cooldown at once): the Dodge gives both back
    {
        DodgeCast dc;
        dc.ability = "Mage_ArcaneCircle";
        dc.hit = "";
        dc.at_wolf = false;
        dc.seconds = 1.5;
        DodgeEnd paid;
        bool ok = run_dodge_cast(mage, dc, paid);
        CHECK(ok, "a world with ground for the Arcane Circle");
        if (!ok) return;
        std::printf("  (Arcane Circle, control: Mana %.1f -> %.1f, cooldown %d, charges %d)\n", paid.mana_before, paid.mana_after, paid.cooldown_on, paid.charges_left);
        CHECK(paid.mana_before - paid.mana_after > 40.0f && paid.cooldown_on && paid.charges_left == 0, "control, no Dodge: the Arcane Circle's Mana is spent and its cooldown runs");
        dc.dodge_at = 0.9;
        DodgeEnd back;
        ok = run_dodge_cast(mage, dc, back);
        CHECK(ok, "a world with ground for the Arcane Circle and a Dodge");
        if (!ok) return;
        std::printf("  (Arcane Circle, Dodge at 0.9 s: Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", back.mana_before, back.mana_after, back.cooldown_on, back.charges_left,
                    ends_of(back, back.cast_id));
        CHECK(back.dodge_item, "the server took the Dodge");
        CHECK(back.mana_after >= back.mana_before - 0.5f, "a Dodge in the cast bar of a cast that had paid gives the Mana back");
        CHECK(!back.cooldown_on && back.charges_left == -1, "... and takes its cooldown off, on the server and on the client");
        CHECK(ends_of(back, back.cast_id) == 1, "... and the client is told the cast is over");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_the_cast_bar);

// A summon's cast bar (8 s) is a cast bar too: a Dodge in it ends the summon, no pet comes and no Mana is spent.
void test_dodge_ends_the_summon() {
    std::printf("a Dodge ends a summon's cast bar: no pet comes, no Mana is spent, the client is told the cast is over\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    const auto prepare = [](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    };
    Run r(0, true, &rec, prepare);
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, false);
    const pets::Definition& d = pets::kDefinitions[0];
    r.h.s->teach_ability(d.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);   // (a summon costs more Mana than a Summoner has left after one)
    r.play(0.2, false);
    const float mana = r.m.stats[kManaStat];
    const int32_t cast_id = r.trigger(d.ability);
    r.play(3.0, false);
    const int32_t dodge_id = r.trigger(kDodge);
    CHECK(r.m.has_ability(kDodge) && dodge_id != cast_id, "the server took the Dodge, 3 s into the summon");
    r.play(7.0, false);
    CHECK(r.h.s->summoned_pet() == 0, "no pet comes at the end of the 8 s: the summon was ended");
    CHECK(r.m.stats[kManaStat] >= mana - 0.5f, "... and no Mana was spent");
    CHECK(r.m.end_ids() == std::vector<int32_t>{cast_id},
          "... and the client is told the summon is over (one ClientEndAbility, for its instance)");
}
AOC_REGISTER_TEST(test_dodge_ends_the_summon);

// ---- the rest of what a Dodge cancels ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The game's rule: a Dodge cancels an ability that has not gone out yet and gives back what it paid. Once something of it has gone out (a hit asked for or landed, an arrow or a volley launched, an effect started) the
// Dodge changes nothing: no cancel, no refund, no end message, and what is in the air still hits. A Dodge the server refuses changes nothing. Besides the cast bars (above) this covers the Ranger's Salvo and Barrage, the
// weapon swings and the held stances (Guard); the Air Strike, the Raining Death, the Whirlwind, the Resplendent Beam, the Deliverance and the Combust follow below. Songs and the Sprint are left alone.

/// The confirmations of `hit` that reached a target, and when the last came (seconds after the trigger).
static int count_hits(const std::vector<TimedHit>& hits, const char* hit, double* last = nullptr) {
    int n = 0;
    for (const TimedHit& h : hits) {
        if (h.hit != hit || h.raw.targets.empty()) continue;
        ++n;
        if (last) *last = std::max(*last, h.at);
    }
    return n;
}
static int count_ends(const FlightEnd& e, int32_t id) { return static_cast<int>(std::count(e.ends.begin(), e.ends.end(), id)); }

void test_dodge_ends_the_barrage() {
    std::printf("a Dodge before the first arrow of a Barrage cancels it and gives back what it paid; once an arrow has left a Dodge changes nothing; a refused Dodge changes nothing\n");
    const svc::Character ranger = class25(1, "Ranger", 17751);
    FlightCast fc;
    fc.ability = "Ranger_Barrage";
    fc.seconds = 3.2;
    std::vector<TimedHit> control;
    FlightEnd ce;
    bool ok = run_flight_cast(ranger, fc, control, &ce);
    CHECK(ok, "a world with ground and a Wolf for the Barrage");
    if (!ok) return;
    double control_last = 0;
    const int all = count_hits(control, "Ranger_Barrage", &control_last);
    const float price = ce.mana_before - ce.mana_after;
    std::printf("  (control: %d arrows landed, the last %.2f s after the trigger; Mana %.1f -> %.1f, charges %d)\n", all, control_last, ce.mana_before, ce.mana_after, ce.charges_left);
    CHECK(all >= 15 && control_last > 1.5 && price > 10.0f && ce.charges_left >= 0, "control, no Dodge: the Barrage lands its arrows, the last of them after 1.5 s, and its Mana is spent and its cooldown runs");
    CHECK(count_ends(ce, ce.cast_id) == 0, "... and the client is told nothing about the end of the cast");
    // a Dodge before the first arrow (it leaves 0.23 s after the trigger): nothing has gone out, the Barrage is cancelled and what it paid comes back
    fc.dodge_at = 0.2;
    {
        std::vector<TimedHit> hits;
        FlightEnd de;
        ok = run_flight_cast(ranger, fc, hits, &de);
        CHECK(ok, "a world with ground and a Wolf for the Barrage and a Dodge before its first arrow");
        if (!ok) return;
        const int n = count_hits(hits, "Ranger_Barrage");
        std::printf("  (Dodge before the first arrow: %d arrows landed; Mana %.1f -> %.1f, charges %d, %d end(s) for the cast)\n", n, de.mana_before, de.mana_after, de.charges_left, count_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(n == 0, "a Dodge before the first arrow cancels the Barrage: no arrow leaves");
        CHECK(count_ends(de, de.cast_id) == 1, "... and the client is told it is over: one ClientEndAbility for its instance");
        CHECK(de.mana_before - de.mana_after < 3.0f && de.charges_left == -1, "... what it paid is given back: its Mana (a regeneration tick may have added some) and its cooldown");
    }
    // a Dodge after the first arrow left (0.7 s: seven arrows had left): the Barrage is out, the Dodge changes nothing
    fc.dodge_at = 0.7;
    {
        std::vector<TimedHit> hits;
        FlightEnd de;
        ok = run_flight_cast(ranger, fc, hits, &de);
        CHECK(ok, "a world with ground and a Wolf for the Barrage and a Dodge after its first arrow");
        if (!ok) return;
        const int n = count_hits(hits, "Ranger_Barrage");
        std::printf("  (Dodge at 0.7 s: %d arrows landed; Mana %.1f -> %.1f, charges %d, %d end(s) for the cast)\n", n, de.mana_before, de.mana_after, de.charges_left, count_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(n == all, "a Dodge after the first arrow changes nothing: every arrow of the Barrage lands");
        CHECK(count_ends(de, de.cast_id) == 0, "... the client is not told the cast is over");
        CHECK(de.mana_before - de.mana_after > 0.8f * price && de.charges_left >= 0, "... and nothing is given back: the Mana stays spent and the cooldown runs");
    }
    // a Dodge the server refuses ends nothing, also before the first arrow
    fc.dodge_first = true;
    fc.dodge_at = 0.2;
    {
        std::vector<TimedHit> hits;
        FlightEnd re;
        ok = run_flight_cast(ranger, fc, hits, &re);
        CHECK(ok, "a world with ground and a Wolf for the Barrage and two Dodges");
        if (!ok) return;
        CHECK(count_ends(re, re.dodge_id) == 1, "the second Dodge, 0.15 s after the first, is refused (ClientEndAbility for its instance)");
        CHECK(count_hits(hits, "Ranger_Barrage") == all && count_ends(re, re.cast_id) == 0 && re.mana_before - re.mana_after > 0.8f * price && re.charges_left >= 0,
              "a refused Dodge changes nothing: every arrow lands, the cast is not ended, it stays paid");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_the_barrage);

void test_dodge_ends_the_salvo() {
    std::printf("a Dodge before the first arrow of a Salvo cancels it and gives back the shot it paid; once an arrow has left a Dodge changes nothing; a refused Dodge changes nothing\n");
    const svc::Character ranger = class25(1, "Ranger", 17751);
    FlightCast fc;
    fc.ability = "Ranger_Salvo";
    fc.seconds = 3.0;
    std::vector<TimedHit> control;
    FlightEnd ce;
    bool ok = run_flight_cast(ranger, fc, control, &ce);
    CHECK(ok, "a world with ground and a Wolf for the Salvo");
    if (!ok) return;
    const std::vector<double> arrows = flight_leaves(ce.log, "Ranger_Salvo: arrow ");
    const int all = count_hits(control, "Ranger_Salvo");
    const float shot = (ce.mana_before - ce.mana_after) / 6.0f;
    std::printf("  (control: %zu arrows, the first at %.3f s, %d hits, %d charges left; one shot cost %.1f Mana)\n", arrows.size(), arrows.empty() ? 0.0 : arrows[0], all, ce.charges_left, shot);
    CHECK(arrows.size() == 6 && all == 6 && ce.charges_left == 0 && shot > 10.0f, "control, no Dodge: six shots paid, six arrows left, six hits landed");
    CHECK(count_ends(ce, ce.cast_id) == 0, "... and the client is told nothing about the end of the cast");
    if (arrows.size() != 6) return;
    // a Dodge after the first shot's cost and before its arrow leaves: nothing has gone out, the shot is given back
    fc.dodge_at = arrows[0] - 0.02;
    {
        std::vector<TimedHit> hits;
        FlightEnd e;
        ok = run_flight_cast(ranger, fc, hits, &e);
        CHECK(ok, "a world with ground and a Wolf for the Salvo and a Dodge before its first arrow");
        if (!ok) return;
        const std::vector<double> left = flight_leaves(e.log, "Ranger_Salvo: arrow ");
        std::printf("  (Dodge at %.2f s, before the first arrow: %zu arrows, %d hits, %d charges left, Mana spent %.1f, %d end(s))\n", fc.dodge_at, left.size(), count_hits(hits, "Ranger_Salvo"), e.charges_left,
                    e.mana_before - e.mana_after, count_ends(e, e.cast_id));
        CHECK(e.dodge_item, "the server took the Dodge");
        CHECK(left.empty() && count_hits(hits, "Ranger_Salvo") == 0, "a Dodge before the first arrow cancels the Salvo: no arrow leaves");
        CHECK(count_ends(e, e.cast_id) == 1, "... and the client is told it is over: one ClientEndAbility for its instance");
        CHECK(e.mana_before - e.mana_after < 3.0f && e.charges_left == -1, "... the shot it paid is given back: its Mana (a regeneration tick may have added some) and its charge");
    }
    // a Dodge after the first arrow left: the Salvo is out and goes on to its last shot
    fc.dodge_at = arrows[0] + 0.1;
    {
        std::vector<TimedHit> hits;
        FlightEnd e;
        ok = run_flight_cast(ranger, fc, hits, &e);
        CHECK(ok, "a world with ground and a Wolf for the Salvo and a Dodge after its first arrow");
        if (!ok) return;
        const std::vector<double> left = flight_leaves(e.log, "Ranger_Salvo: arrow ");
        const float spent = e.mana_before - e.mana_after;
        std::printf("  (Dodge at %.2f s, after the first arrow: %zu arrows, %d hits, %d charges left, Mana spent %.1f, %d end(s))\n", fc.dodge_at, left.size(), count_hits(hits, "Ranger_Salvo"), e.charges_left, spent,
                    count_ends(e, e.cast_id));
        CHECK(e.dodge_item, "the server took the Dodge");
        CHECK(left.size() == 6 && count_hits(hits, "Ranger_Salvo") == 6, "a Dodge after the first arrow changes nothing: all six arrows leave and land");
        CHECK(count_ends(e, e.cast_id) == 0 && e.charges_left == 0 && spent > 5.5f * shot, "... the client is not told the cast is over and the six shots stay paid");
    }
    // a Dodge the server refuses ends nothing, also before the first arrow
    fc.dodge_first = true;
    fc.dodge_at = arrows[0] - 0.02;
    {
        std::vector<TimedHit> hits;
        FlightEnd e;
        ok = run_flight_cast(ranger, fc, hits, &e);
        CHECK(ok, "a world with ground and a Wolf for the Salvo and two Dodges");
        if (!ok) return;
        CHECK(count_ends(e, e.dodge_id) == 1, "the second Dodge, 0.3 s after the first, is refused (ClientEndAbility for its instance)");
        CHECK(count_hits(hits, "Ranger_Salvo") == 6 && e.charges_left == 0 && count_ends(e, e.cast_id) == 0, "a refused Dodge changes nothing: six shots paid, six hits landed, the cast is not ended");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_the_salvo);

/// A melee swing in a real session: the client asks for its hit `request_at` seconds after the trigger, with a Dodge pressed `dodge_at` after it.
struct AxeSwing {
    double dodge_at = -1;
    bool   dodge_first = false;
    double request_at = 0.6;
};
struct AxeEnd {
    int     landed = 0;           // confirmations of the axe's hit that reached a target
    bool    dodge_item = false;
    int32_t swing_id = -1, dodge_id = -1;
    std::vector<int32_t> ends;
};
static bool run_axe_swing(const svc::Character& rec, const AxeSwing& as, AxeEnd& out) {
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return false;
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor* wa = r.h.fx.world->registry().find(ww.wolf);
    const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
    const abilities::AbilityDef* axe = cat ? cat->ability_named("WeaponCombo_Melee_1H_Axe") : nullptr;
    if (!wa || !axe || axe->hits.empty()) return false;
    int64_t targeting = 0;
    for (const content::TargetingRow& t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
    r.h.s->teach_ability(axe->row.guid);
    r.h.fx.world->tick_world(r.now());
    const size_t before = r.m.hits.size();
    if (as.dodge_first) r.trigger(kDodge);
    out.swing_id = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, axe->row.guid, out.swing_id, repl::Value(cyl), 0);
    r.send_trigger(axe->row.guid, out.swing_id, &trig);
    bool dodged = as.dodge_at < 0, asked = false;
    for (int i = 0; i < 30; ++i) {
        const double t = i * 0.05 + 1e-9;
        if (!dodged && t >= as.dodge_at) {
            dodged = true;
            out.dodge_id = r.trigger(kDodge);
            out.dodge_item = r.m.has_ability(kDodge);
        }
        if (!asked && t >= as.request_at) {
            asked = true;
            repl::HitComponentRef comp;
            comp.inline_object = cyl;
            r.send_reliable(hit_request_with(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, comp));
        }
        r.tick(false, std::chrono::milliseconds(50));
        r.h.fx.world->tick_world(r.now());
    }
    for (size_t k = before; k < r.m.hits.size(); ++k) {
        if (r.m.hits[k].record_guid == axe->hits[0].hit_guid && !r.m.hits[k].targets.empty()) ++out.landed;
    }
    out.ends = r.m.end_ids();
    return true;
}

void test_dodge_ends_a_weapon_swing() {
    std::printf("a Dodge before the first arrow or hit of a weapon swing ends the swing; once an arrow has left or a hit was asked for the Dodge changes nothing; a refused Dodge changes nothing\n");
    // the book: one page leaves 0.43 s after the swing starts
    {
        const svc::Character bard = bard25();
        FlightCast fc;
        fc.ability = "WeaponCombo_Ranged_Book";
        fc.ahead = 500;
        fc.seconds = 1.8;
        std::vector<TimedHit> control;
        FlightEnd ce;
        bool ok = run_flight_cast(bard, fc, control, &ce);
        CHECK(ok, "a world with ground and a Wolf for the book's swing");
        if (!ok) return;
        const int page = count_hits(control, "Book_Projectile_Split_Hit_1");
        std::printf("  (book, control: %d page hit(s), %d end(s) for the swing)\n", page, count_ends(ce, ce.cast_id));
        CHECK(page == 1 && count_ends(ce, ce.cast_id) == 0, "control, no Dodge: the swing's page lands once and the client is told nothing");
        fc.dodge_at = 0.25;
        std::vector<TimedHit> dodged;
        FlightEnd de;
        ok = run_flight_cast(bard, fc, dodged, &de);
        CHECK(ok, "a world with ground and a Wolf for the book's swing and a Dodge");
        if (!ok) return;
        std::printf("  (book, Dodge at 0.25 s: %d page hit(s), %d end(s) for the swing)\n", count_hits(dodged, "Book_Projectile_Split_Hit_1"), count_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(count_hits(dodged, "Book_Projectile_Split_Hit_1") == 0 && count_hits(dodged, "Weapon_Book_Hit_1") == 0, "a Dodge before the page left: it never leaves and no hit of the swing lands");
        CHECK(count_ends(de, de.cast_id) == 1, "... and the client is told the swing is over: one ClientEndAbility for its instance");
        fc.dodge_at = 0.6;   // the page has left
        std::vector<TimedHit> late;
        FlightEnd le;
        ok = run_flight_cast(bard, fc, late, &le);
        CHECK(ok, "a world with ground and a Wolf for the book's swing and a Dodge after the page left");
        if (!ok) return;
        CHECK(count_hits(late, "Book_Projectile_Split_Hit_1") == 1 && count_ends(le, le.cast_id) == 0, "a Dodge after the page left changes nothing: the page lands and the client is told nothing");
        fc.dodge_first = true;
        fc.dodge_at = 0.25;
        std::vector<TimedHit> refused;
        FlightEnd re;
        ok = run_flight_cast(bard, fc, refused, &re);
        CHECK(ok, "a world with ground and a Wolf for the book's swing and two Dodges");
        if (!ok) return;
        CHECK(count_ends(re, re.dodge_id) == 1, "the second Dodge, 0.25 s after the first, is refused (ClientEndAbility for its instance)");
        CHECK(count_hits(refused, "Book_Projectile_Split_Hit_1") == 1 && count_ends(re, re.cast_id) == 0, "a refused Dodge changes nothing: the page lands and the swing is not ended");
    }
    // the shortbow's third swing sends three arrows (0.21, 0.37 and 0.49 s)
    {
        const svc::Character ranger = class25(1, "Ranger", 17751);
        FlightCast fc;
        fc.ability = "WeaponCombo_Ranged_Shortbow";
        fc.anim = 4;
        fc.ahead = 500;
        fc.seconds = 1.8;
        std::vector<TimedHit> control;
        FlightEnd ce;
        bool ok = run_flight_cast(ranger, fc, control, &ce);
        CHECK(ok, "a world with ground and a Wolf for the shortbow's swing");
        if (!ok) return;
        const std::vector<double> all = flight_leaves(ce.log, "WeaponCombo_Ranged_Shortbow: arrow ");
        std::printf("  (shortbow, control: %zu arrows", all.size());
        for (const double a : all) std::printf(" %.3f", a);
        std::printf(")\n");
        CHECK(all.size() == 3, "control, no Dodge: the swing sends three arrows");
        if (all.size() != 3) return;
        fc.dodge_at = all[0] - 0.03;   // before the first arrow
        {
            std::vector<TimedHit> dodged;
            FlightEnd de;
            ok = run_flight_cast(ranger, fc, dodged, &de);
            CHECK(ok, "a world with ground and a Wolf for the shortbow's swing and a Dodge before its first arrow");
            if (!ok) return;
            const std::vector<double> left = flight_leaves(de.log, "WeaponCombo_Ranged_Shortbow: arrow ");
            std::printf("  (shortbow, Dodge at %.2f s: %zu arrow(s) left, %d end(s) for the swing)\n", fc.dodge_at, left.size(), count_ends(de, de.cast_id));
            CHECK(de.dodge_item, "the server took the Dodge");
            CHECK(left.empty() && count_ends(de, de.cast_id) == 1, "a Dodge before the first arrow of a swing cancels it: no arrow leaves and the client is told the swing is over");
        }
        fc.dodge_at = all[0] + 0.05;   // after the first arrow, before the second
        {
            std::vector<TimedHit> dodged;
            FlightEnd de;
            ok = run_flight_cast(ranger, fc, dodged, &de);
            CHECK(ok, "a world with ground and a Wolf for the shortbow's swing and a Dodge after its first arrow");
            if (!ok) return;
            const std::vector<double> left = flight_leaves(de.log, "WeaponCombo_Ranged_Shortbow: arrow ");
            std::printf("  (shortbow, Dodge at %.2f s: %zu arrow(s) left, %d end(s) for the swing)\n", fc.dodge_at, left.size(), count_ends(de, de.cast_id));
            CHECK(de.dodge_item, "the server took the Dodge");
            CHECK(left.size() == 3 && count_ends(de, de.cast_id) == 0, "a Dodge after the first arrow of a swing changes nothing: all three arrows leave and the client is told nothing");
        }
    }
    // the axe: its hit is the client's request; a Dodge before the request finds no hit to take, one after it changes nothing
    {
        const svc::Character bard = bard25();
        AxeSwing as;
        AxeEnd control;
        bool ok = run_axe_swing(bard, as, control);
        CHECK(ok, "a world with ground and a Wolf for the axe's swing");
        if (!ok) return;
        std::printf("  (axe, control: %d hit(s) landed)\n", control.landed);
        CHECK(control.landed >= 1, "control, no Dodge: the hit the client asks for 0.6 s into the swing is confirmed on the Wolf");
        as.dodge_at = 0.3;
        AxeEnd dodged;
        ok = run_axe_swing(bard, as, dodged);
        CHECK(ok, "a world with ground and a Wolf for the axe's swing and a Dodge");
        if (!ok) return;
        const int dodge_ends = static_cast<int>(std::count(dodged.ends.begin(), dodged.ends.end(), dodged.swing_id));
        std::printf("  (axe, Dodge at 0.3 s: %d hit(s) landed, %d end(s) for the swing)\n", dodged.landed, dodge_ends);
        CHECK(dodged.dodge_item, "the server took the Dodge");
        CHECK(dodged.landed == 0, "a Dodge in the axe's swing before the client asks for its hit: the hit the client asks for after it is not taken");
        CHECK(dodge_ends == 1, "... and the client is told the swing is over");
        as.dodge_at = 0.7;   // the client asked for the hit at 0.6 s
        AxeEnd late;
        ok = run_axe_swing(bard, as, late);
        CHECK(ok, "a world with ground and a Wolf for the axe's swing and a Dodge after the client's request");
        if (!ok) return;
        const int late_ends = static_cast<int>(std::count(late.ends.begin(), late.ends.end(), late.swing_id));
        CHECK(late.landed >= 1 && late_ends == 0, "a Dodge after the client asked for the hit changes nothing: the hit lands and the client is told nothing");
        as.dodge_first = true;
        as.dodge_at = 0.25;
        AxeEnd refused;
        ok = run_axe_swing(bard, as, refused);
        CHECK(ok, "a world with ground and a Wolf for the axe's swing and two Dodges");
        if (!ok) return;
        const int refused_ends = static_cast<int>(std::count(refused.ends.begin(), refused.ends.end(), refused.swing_id));
        CHECK(refused.landed >= 1 && refused_ends == 0, "a refused Dodge changes nothing: the hit lands and the swing is not ended");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_a_weapon_swing);

/// General_Guard in a real session, held from the press on, with a Dodge pressed `dodge_at` seconds after it (negative: none).
struct GuardRun {
    double dodge_at = -1;
    bool   dodge_first = false;
};
struct GuardEnd {
    bool    on_early = false, on_late = false;   // the Guard's effect is on the client 0.6 s after the press and at the end
    bool    dodge_item = false;
    int32_t guard_id = -1, dodge_id = -1;
    std::vector<int32_t> ends;
};
static bool run_guard(const svc::Character& rec, const GuardRun& gr, GuardEnd& out) {
    constexpr int64_t kGuardEffect = 6064631667969556480ll;   // General_Guard's own effect
    Run r(0, true, &rec);
    r.play(0.5, false);
    r.h.s->teach_ability(content::kGuidGuard);
    if (gr.dodge_first) r.trigger(kDodge);
    out.guard_id = r.trigger(content::kGuidGuard);
    bool dodged = gr.dodge_at < 0;
    for (int i = 0; i < 24; ++i) {
        const double t = i * 0.1 + 1e-9;
        if (!dodged && t >= gr.dodge_at) {
            dodged = true;
            out.dodge_id = r.trigger(kDodge);
            out.dodge_item = r.m.has_ability(kDodge);
        }
        if (i == 6) out.on_early = r.m.has_effect(kGuardEffect);
        r.play(0.1, false);
    }
    out.on_late = r.m.has_effect(kGuardEffect);
    out.ends = r.m.end_ids();
    return true;
}

void test_dodge_ends_a_held_stance() {
    std::printf("a Dodge before a held stance's effect starts cancels the stance; once the effect is on the Dodge changes nothing; a refused Dodge changes nothing\n");
    const svc::Character mage = class25(8, "Mage", 17750);
    GuardRun gr;
    GuardEnd control;
    run_guard(mage, gr, control);
    std::printf("  (Guard, control: effect on at 0.6 s %d, at the end %d, %zu end(s))\n", control.on_early, control.on_late, control.ends.size());
    CHECK(control.on_early && control.on_late && control.ends.empty(), "control, no Dodge: the Guard's effect comes and stays while the key is held, and the client is told nothing");
    // a Dodge before the effect starts (it starts 0.09 s after the press: the item comes at 0.06 s and the effect 0.035 s after the cost step): the Guard never starts
    gr.dodge_at = 0.0;
    {
        GuardEnd dodged;
        run_guard(mage, gr, dodged);
        const int guard_ends = static_cast<int>(std::count(dodged.ends.begin(), dodged.ends.end(), dodged.guard_id));
        std::printf("  (Guard, Dodge with the press: effect on at 0.6 s %d, at the end %d, %d end(s) for the Guard)\n", dodged.on_early, dodged.on_late, guard_ends);
        CHECK(dodged.dodge_item, "the server took the Dodge");
        CHECK(!dodged.on_early && !dodged.on_late, "a Dodge before the Guard's effect starts cancels it: the effect never comes");
        CHECK(guard_ends == 1, "... and the client is told the Guard is over: one ClientEndAbility for its instance");
    }
    // a Dodge once the effect is on: the stance is out, the Dodge changes nothing
    gr.dodge_at = 1.0;
    {
        GuardEnd dodged;
        run_guard(mage, gr, dodged);
        const int guard_ends = static_cast<int>(std::count(dodged.ends.begin(), dodged.ends.end(), dodged.guard_id));
        std::printf("  (Guard, Dodge at 1.0 s: effect on at 0.6 s %d, at the end %d, %d end(s) for the Guard)\n", dodged.on_early, dodged.on_late, guard_ends);
        CHECK(dodged.dodge_item && dodged.on_early, "the Guard's effect is on, and the server took the Dodge");
        CHECK(dodged.on_late && guard_ends == 0, "a Dodge after the Guard's effect started changes nothing: the effect stays and the client is told nothing");
    }
    gr.dodge_first = true;
    gr.dodge_at = 0.0;
    {
        GuardEnd refused;
        run_guard(mage, gr, refused);
        const int dodge_ends = static_cast<int>(std::count(refused.ends.begin(), refused.ends.end(), refused.dodge_id));
        const int refused_guard_ends = static_cast<int>(std::count(refused.ends.begin(), refused.ends.end(), refused.guard_id));
        CHECK(dodge_ends == 1, "the second Dodge, right after the first, is refused (ClientEndAbility for its instance)");
        CHECK(refused.on_late && refused_guard_ends == 0, "a refused Dodge changes nothing: the Guard's effect comes and stays and the client is not told it ended");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_a_held_stance);

// ---- the Air Strike, the Raining Death and the held casts ----------------------------------------------------------------------------------------------------------------------------------
// The same rule for the Air Strike, the Raining Death, the Whirlwind, the Resplendent Beam, the Deliverance and the Combust. Songs and the Sprint are left alone.

/// One cast in a real session with the hits the client asks for, the key's release and a Dodge, all in seconds after the trigger.
struct ChanCast {
    const char*              ability = "";
    std::vector<std::string> ask;                // the hits (by name) the client asks for, from `ask_from` on, every `ask_gap`
    double                   ask_from = 0.5, ask_gap = 0.5;
    bool                     aim_wolf = false;   // the call names the Wolf
    bool                     aim_self = false;   // ... or the caster (an ally-on-self cast); else it names nobody
    bool                     ask_wolf = true;    // the requests name the Wolf (else the caster)
    double                   ahead = 150;        // the Wolf stands this far east of the player, cm
    double                   north = 0;          // ... and this far along the way the player faces (an ability that gathers the enemies in a cone ahead of him)
    double                   release_at = -1;    // the key goes up
    double                   dodge_at = -1;      // a Dodge pressed (negative: none)
    bool                     dodge_first = false;   // a Dodge pressed before the trigger, so that the one at dodge_at is refused
    double                   seconds = 6.0;
    float                    fill = 300.0f;      // the class resource the cast takes, set before it
    float                    hurt = -1;          // Health set to this before the cast (a heal then shows in the Health; negative: left)
};
struct ChanEnd {
    struct Hit {
        double      at;
        std::string name;
        bool        target;
    };
    std::vector<Hit>     hits;                    // every confirmation the client got: seconds after the trigger, the hit's name, whether it reached a target
    int32_t              cast_id = -1, dodge_id = -1;
    bool                 dodge_item = false;
    double               dodged_at = -1;
    std::vector<int32_t> ends;
    float                mana_before = 0, mana_after = 0;
    float                health_before = 0, health_after = 0;
    double               healed_at = -1;          // when the Health first stood 80 over where the cast began (a heal that is confirmed to nobody shows only here; the Health regenerates 20 a tick)
    bool                 cooldown_on = false;
    int                  charges_left = -1;
    std::string          log;
    std::string          probe;                   // where the caster stood, which way he faced and what he saw of the Wolf as the cast began
};
static int chan_ends(const ChanEnd& e, int32_t id) { return static_cast<int>(std::count(e.ends.begin(), e.ends.end(), id)); }
/// The confirmations of `name` that came after `from` seconds (and reached a target when `target`).
static int chan_hits(const ChanEnd& e, const char* name, double from = -1.0e9, bool target = false) {
    int n = 0;
    for (const ChanEnd::Hit& h : e.hits) {
        if (h.name == name && h.at > from && (!target || h.target)) ++n;
    }
    return n;
}
static int log_lines(const std::string& log, const char* a, const char* b = nullptr) {
    int n = 0;
    std::istringstream in(log);
    for (std::string line; std::getline(in, line);) {
        if (line.find(a) != std::string::npos && (!b || line.find(b) != std::string::npos)) ++n;
    }
    return n;
}
/// What went wrong, for a failing check: the log's lines about `who` (a refusal, a cast that was cut short).
static void log_excerpt(const std::string& log, const char* who) {
    int shown = 0;
    std::istringstream in(log);
    for (std::string line; std::getline(in, line) && shown < 8;) {
        if (line.find(who) == std::string::npos) continue;
        std::printf("    | %s\n", line.substr(0, 220).c_str());
        ++shown;
    }
}
static bool run_chan_cast(const svc::Character& rec, const ChanCast& cc, ChanEnd& out) {
    g_log.str("");
    WolfWorld ww;
    ww.level = 25;
    ww.ahead = cc.ahead;
    ww.north = cc.north;
    Run r(0, true, &rec, [&](PlayerWorld& w) { ww.prepare(w); });
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return false;
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor* wa = r.h.fx.world->registry().find(ww.wolf);
    const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
    const abilities::AbilityDef* def = cat ? cat->ability_named(cc.ability) : nullptr;
    if (!wa || !def) return false;
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
    r.h.s->teach_ability(def->row.guid);
    for (const content::CostRow& c : def->costs) {   // the class resource the cost takes (the Cleric's Divine Power): full
        if (c.stat_guid != 0 && c.stat_guid != kManaStat && c.stat_guid != kStamina) r.h.s->set_current_value(c.stat_guid, cc.fill);
    }
    if (cc.hurt >= 0) r.h.s->set_current_value(kHealthStat, cc.hurt);
    r.play(0.2, false);
    r.h.fx.world->tick_world(r.now());
    // the hits the client asks for: the hit, its targeting
    struct Ask {
        int64_t hit, targeting;
    };
    std::vector<Ask> asks;
    for (const std::string& name : cc.ask) {
        for (const content::AbilityHitRow& h : def->hits) {
            const abilities::HitDef* hd = h.source == "hit" ? cat->hit(h.hit_guid) : nullptr;
            if (!hd || hd->row.name != name) continue;
            int64_t targeting = 0;
            for (const content::TargetingRow& t : def->targetings) {
                if (t.key == h.key) targeting = t.guid;
            }
            asks.push_back({h.hit_guid, targeting});
            break;
        }
    }
    if (asks.size() != cc.ask.size()) return false;
    const size_t before = r.m.hits.size();
    if (combat::CombatWorld* cw = r.h.fx.world->combat(); cw && !cw->players().empty()) {
        const combat::Fighter& self = cw->players().front()->fighter();
        const combat::TargetInfo ti = cw->inspect(ww.wolf, self);
        const combat::Vec3 me = self.position();
        char buf[300];
        std::snprintf(buf, sizeof buf, "the caster at (%.0f, %.0f) faces %.0f degrees; the Wolf at (%.0f, %.0f), %.0f cm away, hostile %d, in sight %d", me.x, me.y, self.facing_degrees(), ti.position.x,
                      ti.position.y, ti.surface_distance_cm, ti.relation == combat::Relation::Hostile, ti.in_line_of_sight);
        out.probe = buf;
    }
    if (cc.dodge_first) r.trigger(kDodge);
    out.mana_before = r.m.stats[kManaStat];
    out.health_before = r.m.stats[kHealthStat];
    out.cast_id = r.next_id++;
    const double base = r.now();
    if (cc.aim_wolf) {
        const BunchWriter trig = trigger_at_value(setup, def->row.guid, out.cast_id, repl::Value(cyl), 0);
        r.send_trigger(def->row.guid, out.cast_id, &trig);
    } else if (cc.aim_self) {
        const BunchWriter trig = trigger_at_object(setup, def->row.guid, out.cast_id, setup.guids.collision_cylinder);
        r.send_trigger(def->row.guid, out.cast_id, &trig);
    } else {
        r.send_trigger(def->row.guid, out.cast_id);
    }
    bool dodged = cc.dodge_at < 0, released = cc.release_at < 0;
    double next_ask = asks.empty() ? 1.0e9 : cc.ask_from;
    size_t reported = 0;
    const int steps = static_cast<int>(cc.seconds / 0.05);
    for (int i = 0; i < steps; ++i) {
        const double t = i * 0.05 + 1e-9;
        if (!released && t >= cc.release_at) {
            released = true;
            r.cancel(out.cast_id);
        }
        if (t >= next_ask) {
            next_ask += cc.ask_gap;
            for (const Ask& a : asks) {
                repl::HitComponentRef comp;
                if (cc.ask_wolf) comp.inline_object = cyl;
                else comp.object = setup.guids.collision_cylinder;
                r.send_reliable(hit_request_with(setup, def->row.guid, a.targeting, a.hit, comp));
            }
        }
        if (!dodged && t >= cc.dodge_at) {
            dodged = true;
            out.dodged_at = r.now() - base;
            out.dodge_id = r.trigger(kDodge);
            out.dodge_item = r.m.has_ability(kDodge);
        }
        r.tick(false, std::chrono::milliseconds(50));
        r.h.fx.world->tick_world(r.now());
        if (out.healed_at < 0 && r.m.stats[kHealthStat] >= out.health_before + 80.0f) out.healed_at = r.now() - base;
        for (; before + reported < r.m.hits.size(); ++reported) {
            const repl::HitInstance& h = r.m.hits[before + reported];
            const abilities::HitDef* hd = cat->hit(h.record_guid);
            out.hits.push_back({r.now() - base, hd ? hd->row.name : std::string(), !h.targets.empty()});
        }
    }
    spdlog::default_logger()->flush();
    out.log = g_log.str();
    out.ends = r.m.end_ids();
    out.mana_after = r.m.stats[kManaStat];
    out.health_after = r.m.stats[kHealthStat];
    for (const auto& [id, guid] : r.m.cooldowns) out.cooldown_on = out.cooldown_on || guid == def->row.guid;
    if (r.h.s->pipeline()) out.charges_left = r.h.s->pipeline()->charges_left(def->row.guid);
    return true;
}
/// What a cast paid, in Mana (the Divine Power of the Cleric's casts is not read: it decays on its own over the run).
static float spent_mana(const ChanEnd& e) { return e.mana_before - e.mana_after; }

void test_dodge_ends_the_air_strike() {
    std::printf("a Dodge before the first arrow of an Air Strike cancels it and gives back what it paid; once an arrow has left a Dodge changes nothing; a refused Dodge changes nothing\n");
    const svc::Character ranger = class25(1, "Ranger", 17751);
    ChanCast cc;
    cc.ability = "Ranger_AirStrike";
    cc.ahead = 200;
    cc.seconds = 3.5;
    ChanEnd ce;
    bool ok = run_chan_cast(ranger, cc, ce);
    CHECK(ok, "a world with ground and a Wolf for the Air Strike");
    if (!ok) return;
    const int shots = log_lines(ce.log, "Ranger_AirStrike (arrow ", "it leaves"), landings = log_lines(ce.log, "Ranger_AirStrike (arrow ", "lands at");
    const float price = spent_mana(ce);
    std::printf("  (control: %d arrows left, %d landed, %d area hit(s) on the Wolf, Mana %.1f -> %.1f, charges %d, %zu end(s))\n", shots, landings, chan_hits(ce, "Ranger_AirStrike_AoE", -1e9, true), ce.mana_before,
                ce.mana_after, ce.charges_left, ce.ends.size());
    CHECK(shots == 3 && landings == 3 && price > 10.0f && ce.charges_left == 0, "control, no Dodge: three arrows leave and land, the Mana is spent and the cooldown runs");
    CHECK(chan_ends(ce, ce.cast_id) == 0, "... and the client is told nothing about the end of the cast");
    if (shots != 3) {
        log_excerpt(ce.log, "AirStrike");
        return;
    }
    // a Dodge before the first arrow (it leaves 1.03 s after the trigger): nothing has gone out, the Air Strike is cancelled and what it paid comes back
    cc.dodge_at = 0.5;
    {
        ChanEnd de;
        ok = run_chan_cast(ranger, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Air Strike and a Dodge before its first arrow");
        if (!ok) return;
        std::printf("  (Dodge at %.2f s: %d arrow(s) left, Mana %.1f -> %.1f, charges %d, %d end(s) for the cast)\n", de.dodged_at, log_lines(de.log, "Ranger_AirStrike (arrow ", "it leaves"), de.mana_before,
                    de.mana_after, de.charges_left, chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(log_lines(de.log, "Ranger_AirStrike (arrow ", "it leaves") == 0 && chan_hits(de, "Ranger_AirStrike_AoE", -1e9, true) == 0, "a Dodge before the first arrow cancels the Air Strike: no arrow leaves");
        CHECK(chan_ends(de, de.cast_id) == 1, "... the client is told the Air Strike is over: one ClientEndAbility for its instance");
        CHECK(spent_mana(de) < 3.0f && !de.cooldown_on && de.charges_left == -1, "... and what it paid is given back: the Mana (a regeneration tick may have added some) and the cooldown");
    }
    // a Dodge between the first and the second arrow (they leave 1.03 and 1.12 s after the trigger): the Air Strike is out, the Dodge changes nothing
    cc.dodge_at = 1.05;
    {
        ChanEnd de;
        ok = run_chan_cast(ranger, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Air Strike and a Dodge after its first arrow");
        if (!ok) return;
        const int left = log_lines(de.log, "Ranger_AirStrike (arrow ", "it leaves"), landed = log_lines(de.log, "Ranger_AirStrike (arrow ", "lands at");
        std::printf("  (Dodge at %.2f s: %d arrow(s) left, %d landed, Mana %.1f -> %.1f, charges %d, %d end(s) for the cast)\n", de.dodged_at, left, landed, de.mana_before, de.mana_after, de.charges_left,
                    chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(left == 3 && landed == 3, "a Dodge after the first arrow changes nothing: all three arrows leave and land");
        CHECK(chan_ends(de, de.cast_id) == 0 && spent_mana(de) > 0.8f * price && de.cooldown_on && de.charges_left == 0, "... the client is told nothing and the Air Strike stays paid: the Mana is spent and the cooldown runs");
    }
    // a Dodge after the Air Strike is over
    cc.dodge_at = 2.8;
    {
        ChanEnd de;
        ok = run_chan_cast(ranger, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Air Strike and a late Dodge");
        if (!ok) return;
        CHECK(de.dodge_item && chan_ends(de, de.cast_id) == 0, "a Dodge after the Air Strike is over ends nothing");
        CHECK(spent_mana(de) > 0.8f * price && de.cooldown_on && de.charges_left == 0, "... and gives nothing back: the Mana stays spent and the cooldown runs");
    }
    // a Dodge the server refuses, also before the first arrow
    cc.dodge_first = true;
    cc.dodge_at = 0.5;
    {
        ChanEnd de;
        ok = run_chan_cast(ranger, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Air Strike and two Dodges");
        if (!ok) return;
        CHECK(chan_ends(de, de.dodge_id) == 1, "the second Dodge, 0.5 s after the first, is refused (ClientEndAbility for its instance)");
        CHECK(log_lines(de.log, "Ranger_AirStrike (arrow ", "it leaves") == 3 && chan_ends(de, de.cast_id) == 0 && spent_mana(de) > 0.8f * price && de.cooldown_on,
              "a refused Dodge changes nothing: three arrows leave, the cast is not ended, it stays paid");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_the_air_strike);

void test_dodge_ends_the_raining_death() {
    std::printf("a Dodge before the first volley of a Raining Death cancels it and gives back what it paid; once a volley has left a Dodge changes nothing; a refused Dodge changes nothing\n");
    const svc::Character ranger = class25(1, "Ranger", 17751);
    ChanCast cc;
    cc.ability = "Ranger_RainingDeath";
    cc.ahead = 0;
    cc.north = 700;   // (the caster faces +Y)
    cc.seconds = 3.0;
    ChanEnd ce;
    bool ok = run_chan_cast(ranger, cc, ce);
    CHECK(ok, "a world with ground and a Wolf for the Raining Death");
    if (!ok) return;
    const int volleys = log_lines(ce.log, "Ranger_RainingDeath (volley");
    const int landed = chan_hits(ce, "Ranger_RainingDeath_Hit", -1e9, true);
    const float price = spent_mana(ce);
    std::printf("  (control: %d volleys, %d hits, Mana %.1f -> %.1f, charges %d, %zu end(s))\n", volleys, landed, ce.mana_before, ce.mana_after, ce.charges_left, ce.ends.size());
    CHECK(volleys == 2 && landed == 2 && price > 10.0f && ce.charges_left == 0, "control, no Dodge: two volleys leave, the Wolf is hit twice, the Mana is spent and the cooldown runs");
    CHECK(chan_ends(ce, ce.cast_id) == 0, "... and the client is told nothing about the end of the cast");
    if (volleys != 2 || landed != 2) {
        std::printf("    | %s\n", ce.probe.c_str());
        log_excerpt(ce.log, "RainingDeath");
        return;
    }
    // a Dodge before the first volley (it leaves 0.65 s after the trigger): nothing has gone out, the Raining Death is cancelled and what it paid comes back
    cc.dodge_at = 0.3;
    {
        ChanEnd de;
        ok = run_chan_cast(ranger, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Raining Death and a Dodge before its first volley");
        if (!ok) return;
        std::printf("  (Dodge at %.2f s: %d volley(s), %d hit(s), Mana %.1f -> %.1f, charges %d, %d end(s) for the cast)\n", de.dodged_at, log_lines(de.log, "Ranger_RainingDeath (volley"),
                    chan_hits(de, "Ranger_RainingDeath_Hit", -1e9, true), de.mana_before, de.mana_after, de.charges_left, chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(log_lines(de.log, "Ranger_RainingDeath (volley") == 0 && chan_hits(de, "Ranger_RainingDeath_Hit", -1e9, true) == 0, "a Dodge before the first volley cancels the Raining Death: no volley leaves");
        CHECK(chan_ends(de, de.cast_id) == 1, "... the client is told the Raining Death is over: one ClientEndAbility for its instance");
        CHECK(spent_mana(de) < 3.0f && !de.cooldown_on && de.charges_left == -1, "... and what it paid is given back: the Mana (a regeneration tick may have added some) and the cooldown");
    }
    // a Dodge between the two volleys (the second leaves 0.93 s after the trigger): the Raining Death is out, the Dodge changes nothing
    cc.dodge_at = 0.75;
    {
        ChanEnd de;
        ok = run_chan_cast(ranger, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Raining Death and a Dodge after its first volley");
        if (!ok) return;
        const int left = log_lines(de.log, "Ranger_RainingDeath (volley"), n = chan_hits(de, "Ranger_RainingDeath_Hit", -1e9, true);
        std::printf("  (Dodge at %.2f s: %d volley(s), %d hit(s), Mana %.1f -> %.1f, charges %d, %d end(s) for the cast)\n", de.dodged_at, left, n, de.mana_before, de.mana_after, de.charges_left,
                    chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(left == 2 && n == 2, "a Dodge after the first volley changes nothing: both volleys leave and land");
        CHECK(chan_ends(de, de.cast_id) == 0 && spent_mana(de) > 0.8f * price && de.cooldown_on && de.charges_left == 0, "... the client is told nothing and the Raining Death stays paid: the Mana is spent and the cooldown runs");
    }
    cc.dodge_at = 2.0;   // both volleys left and landed
    {
        ChanEnd de;
        ok = run_chan_cast(ranger, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Raining Death and a late Dodge");
        if (!ok) return;
        CHECK(de.dodge_item && chan_ends(de, de.cast_id) == 0, "a Dodge after the Raining Death is over ends nothing");
        CHECK(spent_mana(de) > 0.8f * price && de.cooldown_on && de.charges_left == 0, "... and gives nothing back: the Mana stays spent and the cooldown runs");
    }
    cc.dodge_first = true;
    cc.dodge_at = 0.3;
    {
        ChanEnd de;
        ok = run_chan_cast(ranger, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Raining Death and two Dodges");
        if (!ok) return;
        CHECK(chan_ends(de, de.dodge_id) == 1, "the second Dodge, 0.3 s after the first, is refused (ClientEndAbility for its instance)");
        CHECK(chan_hits(de, "Ranger_RainingDeath_Hit", -1e9, true) == 2 && chan_ends(de, de.cast_id) == 0 && spent_mana(de) > 0.8f * price && de.cooldown_on,
              "a refused Dodge changes nothing: both volleys land, the cast is not ended, it stays paid");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_the_raining_death);

void test_dodge_ends_the_whirlwind() {
    std::printf("a Dodge before a Whirlwind's effects start cancels it; once it is spinning the Dodge changes nothing; a refused Dodge changes nothing\n");
    const svc::Character fighter = class25(4, "Fighter", 17749);
    ChanCast cc;
    cc.ability = "Fighter_Whirlwind";
    cc.ask = {"Fighter_Whirlwind", "Fighter_Whirlwind_2"};
    cc.ask_from = 0.5;
    cc.ask_gap = 0.5;
    cc.ahead = 120;
    cc.seconds = 5.0;
    ChanEnd ce;
    bool ok = run_chan_cast(fighter, cc, ce);
    CHECK(ok, "a world with ground and a Wolf for the Whirlwind");
    if (!ok) return;
    const auto on_wolf = [](const ChanEnd& e, double from) { return chan_hits(e, "Fighter_Whirlwind", from, true) + chan_hits(e, "Fighter_Whirlwind_2", from, true); };
    const int all = on_wolf(ce, -1e9);
    double last = 0;
    for (const ChanEnd::Hit& h : ce.hits) last = std::max(last, h.at);
    std::printf("  (control: %d hit(s) on the Wolf, the last %.2f s after the trigger, Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", all, last, ce.mana_before, ce.mana_after, ce.cooldown_on,
                ce.charges_left, chan_ends(ce, ce.cast_id));
    CHECK(all >= 3 && last > 2.5, "control, no Dodge: the Whirlwind's hits keep landing on the Wolf while it spins");
    CHECK(spent_mana(ce) > 10.0f && ce.cooldown_on, "... it spent its Mana and its cooldown runs");
    CHECK(chan_ends(ce, ce.cast_id) == 0, "... and the client is told nothing about the end of the cast");
    if (all < 3) return;
    // a Dodge before the Whirlwind's effects start (0.34 s after the press): nothing has gone out, it is cancelled and no hit is ever taken
    cc.dodge_at = 0.15;
    {
        ChanEnd de;
        ok = run_chan_cast(fighter, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Whirlwind and a Dodge before it spins");
        if (!ok) return;
        std::printf("  (Dodge at %.2f s: %d hit(s), Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", de.dodged_at, on_wolf(de, -1e9), de.mana_before, de.mana_after, de.cooldown_on, de.charges_left,
                    chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(on_wolf(de, -1e9) == 0, "a Dodge before the Whirlwind spins cancels it: the client's requests for its hits are not taken");
        CHECK(chan_ends(de, de.cast_id) == 1, "... the client is told the Whirlwind is over: one ClientEndAbility for its instance");
        CHECK(spent_mana(de) < 3.0f && !de.cooldown_on && de.charges_left == -1, "... and nothing is spent: no Mana (a regeneration tick may have added some) and no cooldown");
    }
    // a Dodge while it spins: the Whirlwind is out, the Dodge changes nothing
    cc.dodge_at = 1.6;
    {
        ChanEnd de;
        ok = run_chan_cast(fighter, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Whirlwind and a Dodge while it spins");
        if (!ok) return;
        std::printf("  (Dodge at %.2f s: %d hit(s), %d of them 0.3 s or more after it, Mana %.1f -> %.1f, cooldown %d, %d end(s))\n", de.dodged_at, on_wolf(de, -1e9), on_wolf(de, de.dodged_at + 0.3), de.mana_before,
                    de.mana_after, de.cooldown_on, chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(on_wolf(de, -1e9) == all && on_wolf(de, de.dodged_at + 0.3) >= 1, "a Dodge while the Whirlwind spins changes nothing: its hits go on landing");
        CHECK(chan_ends(de, de.cast_id) == 0 && spent_mana(de) > 10.0f && de.cooldown_on, "... the client is told nothing and the Whirlwind stays paid: the Mana is spent and the cooldown runs");
    }
    cc.dodge_first = true;
    cc.dodge_at = 0.15;
    {
        ChanEnd re;
        ok = run_chan_cast(fighter, cc, re);
        CHECK(ok, "a world with ground and a Wolf for the Whirlwind and two Dodges");
        if (!ok) return;
        CHECK(chan_ends(re, re.dodge_id) == 1, "the second Dodge, 0.15 s after the first, is refused (ClientEndAbility for its instance)");
        CHECK(on_wolf(re, 2.5) >= 1 && chan_ends(re, re.cast_id) == 0 && spent_mana(re) > 10.0f && re.cooldown_on, "a refused Dodge changes nothing: the hits go on, the cast is not ended, it stays paid");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_the_whirlwind);

void test_dodge_ends_the_resplendent_beam() {
    std::printf("a Dodge in the bar of a Resplendent Beam cancels it; once the key is up and the beam is fired the Dodge changes nothing; a refused Dodge changes nothing\n");
    const svc::Character cleric = class25(3, "Cleric", 17748);
    ChanCast cc;
    cc.ability = "Cleric_ResplendentBeam";
    cc.ahead = 4000;
    cc.aim_self = true;
    cc.ask_wolf = false;
    cc.ask = {"Cleric_ResplendentBeam_Hit"};
    cc.ask_from = 1.4;
    cc.ask_gap = 0.4;
    cc.release_at = 1.0;
    cc.seconds = 5.0;
    ChanEnd ce;
    bool ok = run_chan_cast(cleric, cc, ce);
    CHECK(ok, "a world with ground for the Resplendent Beam");
    if (!ok) return;
    const int all = chan_hits(ce, "Cleric_ResplendentBeam_Hit");
    std::printf("  (control: %d hit(s), Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", all, ce.mana_before, ce.mana_after, ce.cooldown_on, ce.charges_left, chan_ends(ce, ce.cast_id));
    CHECK(spent_mana(ce) > 30.0f && ce.cooldown_on, "control, no Dodge: the beam is fired, it spent its Mana and its cooldown runs");
    CHECK(all >= 1, "... and its hit is confirmed");
    CHECK(chan_ends(ce, ce.cast_id) == 0, "... and the client is told nothing about the end of the cast");
    if (spent_mana(ce) < 30.0f) return;
    // a Dodge in the bar (the key is held until 1.0 s): the beam never fires
    cc.dodge_at = 0.6;
    {
        ChanEnd de;
        ok = run_chan_cast(cleric, cc, de);
        CHECK(ok, "a world with ground for the Resplendent Beam and a Dodge in its bar");
        if (!ok) return;
        std::printf("  (Dodge at %.2f s: %d hit(s), Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", de.dodged_at, chan_hits(de, "Cleric_ResplendentBeam_Hit"), de.mana_before, de.mana_after,
                    de.cooldown_on, de.charges_left, chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(chan_hits(de, "Cleric_ResplendentBeam_Hit") == 0, "a Dodge in the bar cancels the beam: no hit of it lands");
        CHECK(chan_ends(de, de.cast_id) == 1, "... the client is told the beam is over: one ClientEndAbility for its instance");
        CHECK(spent_mana(de) < 3.0f && !de.cooldown_on && de.charges_left == -1, "... and nothing is spent: no Mana (a regeneration tick may have added some) and no cooldown");
    }
    // a Dodge after the key went up (1.0 s): the beam is out, the Dodge changes nothing
    cc.dodge_at = 1.5;
    {
        ChanEnd de;
        ok = run_chan_cast(cleric, cc, de);
        CHECK(ok, "a world with ground for the Resplendent Beam and a Dodge after the beam is fired");
        if (!ok) return;
        std::printf("  (Dodge at %.2f s: %d hit(s), Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", de.dodged_at, chan_hits(de, "Cleric_ResplendentBeam_Hit"), de.mana_before, de.mana_after,
                    de.cooldown_on, de.charges_left, chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(chan_hits(de, "Cleric_ResplendentBeam_Hit") == all, "a Dodge after the beam is fired changes nothing: its hits land as they would have");
        CHECK(chan_ends(de, de.cast_id) == 0 && spent_mana(de) > 30.0f && de.cooldown_on, "... the client is told nothing and the beam stays paid: the Mana is spent and the cooldown runs");
    }
    cc.dodge_at = 4.0;   // the beam is over
    {
        ChanEnd de;
        ok = run_chan_cast(cleric, cc, de);
        CHECK(ok, "a world with ground for the Resplendent Beam and a late Dodge");
        if (!ok) return;
        CHECK(de.dodge_item && chan_ends(de, de.cast_id) == 0, "a Dodge after the beam is over ends nothing");
        CHECK(spent_mana(de) > 30.0f && de.cooldown_on && chan_hits(de, "Cleric_ResplendentBeam_Hit") >= 1, "... and gives nothing back: it stays paid and its hit landed");
    }
    cc.dodge_first = true;
    cc.dodge_at = 0.5;
    {
        ChanEnd re;
        ok = run_chan_cast(cleric, cc, re);
        CHECK(ok, "a world with ground for the Resplendent Beam and two Dodges");
        if (!ok) return;
        CHECK(chan_ends(re, re.dodge_id) == 1, "the second Dodge, 0.5 s after the first, is refused (ClientEndAbility for its instance)");
        CHECK(chan_ends(re, re.cast_id) == 0 && spent_mana(re) > 30.0f && re.cooldown_on && chan_hits(re, "Cleric_ResplendentBeam_Hit") >= 1, "a refused Dodge changes nothing: the beam is fired, the cast is not ended, it stays paid");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_the_resplendent_beam);

// The Deliverance is a held cast that heals the Cleric and the allies around him when the client asks for its hit; its heal at the caster is confirmed to nobody, so it is read in the Cleric's Health.
void test_dodge_ends_the_deliverance() {
    std::printf("a Dodge before a Deliverance's heal goes out cancels it and gives back what it paid; once the heal has landed the Dodge changes nothing; a refused Dodge changes nothing\n");
    const svc::Character cleric = class25(3, "Cleric", 17748);
    ChanCast cc;
    cc.ability = "Cleric_Deliverance";
    cc.ahead = 4000;
    cc.aim_self = true;
    cc.ask_wolf = false;
    cc.ask = {"Cleric_Deliverance_Hit"};   // the client asks for the heal once, as its key goes up (the Deliverance's montage runs 3.3 s)
    cc.ask_from = 2.5;
    cc.ask_gap = 100;
    cc.hurt = 300.0f;
    cc.seconds = 6.0;
    ChanEnd ce;
    bool ok = run_chan_cast(cleric, cc, ce);
    CHECK(ok, "a world with ground for the Deliverance");
    if (!ok) return;
    std::printf("  (control: Health %.0f -> %.0f, healed at %.2f s, Mana %.1f -> %.1f, %d end(s))\n", ce.health_before, ce.health_after, ce.healed_at, ce.mana_before, ce.mana_after, chan_ends(ce, ce.cast_id));
    CHECK(spent_mana(ce) > 10.0f, "control, no Dodge: it spent its Mana");
    CHECK(ce.healed_at > 0, "... and it healed the Cleric");
    CHECK(chan_ends(ce, ce.cast_id) == 0, "... and the client is told nothing about the end of the cast");
    if (spent_mana(ce) < 10.0f || ce.healed_at < 0) {
        log_excerpt(ce.log, "Deliverance");
        return;
    }
    // a Dodge just after the cost step, and one just before the heal: nothing has gone out, the Deliverance is cancelled and what it paid comes back
    for (const double at : {0.6, ce.healed_at - 0.3}) {
        cc.dodge_at = at;
        ChanEnd de;
        ok = run_chan_cast(cleric, cc, de);
        CHECK(ok, "a world with ground for the Deliverance and a Dodge before its heal");
        if (!ok) return;
        std::printf("  (Dodge at %.2f s: Health %.0f -> %.0f, healed at %.2f s, Mana %.1f -> %.1f, %d end(s))\n", de.dodged_at, de.health_before, de.health_after, de.healed_at, de.mana_before, de.mana_after,
                    chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(de.healed_at < 0, "a Dodge before the heal cancels the Deliverance: its heal never lands");
        CHECK(chan_ends(de, de.cast_id) == 1, "... the client is told the Deliverance is over: one ClientEndAbility for its instance");
        CHECK(spent_mana(de) < 3.0f, "... and what it paid is given back: the Mana (a regeneration tick may have added some)");
    }
    // a Dodge just after the heal landed, while the montage still runs (it ends 3.3 s after the press): the heal is out, it costs full
    cc.dodge_at = ce.healed_at + 0.25;
    {
        ChanEnd de;
        ok = run_chan_cast(cleric, cc, de);
        CHECK(ok, "a world with ground for the Deliverance and a Dodge after its heal");
        if (!ok) return;
        std::printf("  (Dodge at %.2f s, after the heal: Health %.0f -> %.0f, healed at %.2f s, Mana %.1f -> %.1f, %d end(s))\n", de.dodged_at, de.health_before, de.health_after, de.healed_at, de.mana_before, de.mana_after,
                    chan_ends(de, de.cast_id));
        CHECK(de.dodge_item, "the server took the Dodge");
        CHECK(de.healed_at > 0 && de.health_after > de.health_before + 80.0f, "a Dodge after the heal landed changes nothing: the Cleric stays healed");
        CHECK(chan_ends(de, de.cast_id) == 0 && spent_mana(de) > 10.0f, "... the client is told nothing and the heal costs full: the Mana stays spent (no free heal)");
    }
    cc.dodge_at = 5.0;   // the Deliverance is over
    {
        ChanEnd de;
        ok = run_chan_cast(cleric, cc, de);
        CHECK(ok, "a world with ground for the Deliverance and a late Dodge");
        if (!ok) return;
        CHECK(de.dodge_item && chan_ends(de, de.cast_id) == 0, "a Dodge after the Deliverance is over ends nothing");
        CHECK(de.healed_at > 0 && spent_mana(de) > 10.0f, "... its heal stays landed and it stays paid");
    }
    cc.dodge_first = true;
    cc.dodge_at = 0.5;
    {
        ChanEnd re;
        ok = run_chan_cast(cleric, cc, re);
        CHECK(ok, "a world with ground for the Deliverance and two Dodges");
        if (!ok) return;
        CHECK(chan_ends(re, re.dodge_id) == 1, "the second Dodge, 0.5 s after the first, is refused (ClientEndAbility for its instance)");
        CHECK(re.healed_at > 0 && chan_ends(re, re.cast_id) == 0 && spent_mana(re) > 10.0f, "a refused Dodge changes nothing: its heal lands, the cast is not ended, it stays paid");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_the_deliverance);

// The Mage's Combust has no bar and no effect of its own: its hit is asked for by the client, and that is when it goes out.
void test_dodge_ends_the_combust() {
    std::printf("a Dodge before a Combust's hit is asked for cancels it and gives back what it paid; once the hit is asked for the Dodge changes nothing\n");
    const svc::Character mage = class25(8, "Mage", 17750);
    ChanCast cc;
    cc.ability = "Mage_Combust";
    cc.aim_wolf = true;
    cc.ahead = 600;
    cc.ask = {"Mage_Combust_Hit"};
    cc.ask_from = 1.0;
    cc.ask_gap = 100;
    cc.seconds = 3.0;
    ChanEnd ce;
    bool ok = run_chan_cast(mage, cc, ce);
    CHECK(ok, "a world with ground and a Wolf for the Combust");
    if (!ok) return;
    const int all = chan_hits(ce, "Mage_Combust_Hit", -1e9, true);
    std::printf("  (control: %d hit(s), Mana %.1f -> %.1f, cooldown %d, %d end(s))\n", all, ce.mana_before, ce.mana_after, ce.cooldown_on, chan_ends(ce, ce.cast_id));
    CHECK(all >= 1 && spent_mana(ce) > 10.0f && ce.cooldown_on && chan_ends(ce, ce.cast_id) == 0, "control, no Dodge: the hit lands, the Mana is spent and the cooldown runs");
    if (all < 1) {
        log_excerpt(ce.log, "Combust");
        return;
    }
    cc.dodge_at = 0.5;
    {
        ChanEnd de;
        ok = run_chan_cast(mage, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Combust and a Dodge before the hit");
        if (!ok) return;
        CHECK(de.dodge_item && chan_hits(de, "Mage_Combust_Hit", -1e9, true) == 0 && chan_ends(de, de.cast_id) == 1, "a Dodge before the hit is asked for cancels the Combust: no hit, and the client is told it is over");
        CHECK(spent_mana(de) < 3.0f && !de.cooldown_on, "... and what it paid is given back: the Mana (a regeneration tick may have added some) and the cooldown");
    }
    cc.dodge_at = 1.3;
    {
        ChanEnd de;
        ok = run_chan_cast(mage, cc, de);
        CHECK(ok, "a world with ground and a Wolf for the Combust and a Dodge after the hit");
        if (!ok) return;
        CHECK(de.dodge_item && chan_hits(de, "Mage_Combust_Hit", -1e9, true) >= 1 && chan_ends(de, de.cast_id) == 0, "a Dodge after the hit was asked for changes nothing: the hit lands and the client is told nothing");
        CHECK(spent_mana(de) > 10.0f && de.cooldown_on, "... and the Combust stays paid");
    }
}
AOC_REGISTER_TEST(test_dodge_ends_the_combust);

// ---- the Dodge on every ordinary ability ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The game's two rules: a Dodge cancels an ability and gives back what it paid until the ability goes out, and once it has gone out the Dodge changes nothing. For the kinds those rules leave open: the item of a
// consumable is used at the press and its buff applies in full whatever the Dodge does; a pet command, a leap, a charge, a teleport and a dash count as gone out at the press; only the five Melodies are songs (the ten
// one-shot Songs are ordinary abilities); an ability with no hit of its own is gone out once it is paid.

/// The windows of the Dodge rule for one ability, in seconds after the press.
struct DodgeWindows {
    double before = 0.1;       // a Dodge before the cost step: nothing was paid
    double between = 0.5;      // ... after the cost step and before the ability goes out: the cost comes back (negative: the ability has no such window)
    double after = 1.0;        // ... after the ability went out
    double refused = -1;       // the Dodge the server refuses, inside the cooldown of the one pressed before the trigger (negative: the time of `before`)
    double ask_from = 0.8;     // when the client asks for the hit (negative: it asks for none)
    double seconds = 3.0;      // how long each run plays
    float  min_price = 10.0f;  // the Mana the cast must spend for the control to count (0: the cast spends no Mana: its cooldown is the sign that it is paid)
    bool   on_target = true;   // the hit's confirmations reach a target (a buff on the caster does not)
    bool   lands = true;       // the ability has a hit that lands (false: no hit is counted)
};
static void prepare_dodge_cast(ChanCast& cc, const char* hit, const DodgeWindows& w) {
    if (w.ask_from >= 0) {
        cc.ask = {hit};
        cc.ask_from = w.ask_from;
        cc.ask_gap = 100;
    }
    cc.seconds = w.seconds;
}
static int landed_hits(const ChanEnd& e, const char* hit, const DodgeWindows& w) { return w.lands ? chan_hits(e, hit, -1e9, w.on_target) : 0; }
/// Nothing is paid: no Mana (a regeneration tick may have added some) and no cooldown.
static bool dodge_unpaid(const ChanEnd& e, const DodgeWindows& w) { return (w.min_price <= 0 || spent_mana(e) < 3.0f) && !e.cooldown_on && e.charges_left == -1; }
/// It stays paid: the Mana spent (when it spends any) and the cooldown running.
static bool dodge_paid(const ChanEnd& e, const DodgeWindows& w, float price) { return (w.min_price <= 0 || spent_mana(e) > 0.8f * price) && e.cooldown_on; }

/// The control of an ability: the cast with no Dodge goes out (its hit lands, when it has one), is paid, starts its cooldown and the client is told nothing. False, after the reason, when it does not.
static bool run_dodge_control(const svc::Character& rec, const ChanCast& cc, const char* hit, const char* what, const DodgeWindows& w, int& landed, float& price) {
    ChanEnd ce;
    const bool ok = run_chan_cast(rec, cc, ce);
    CHECKF(ok, "%s: a world with ground and a Wolf", what);
    if (!ok) return false;
    landed = landed_hits(ce, hit, w);
    price = spent_mana(ce);
    const bool paid = w.min_price > 0 ? price > w.min_price : ce.cooldown_on;
    std::printf("  (%s, control: %d hit(s), Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", what, landed, ce.mana_before, ce.mana_after, ce.cooldown_on, ce.charges_left, chan_ends(ce, ce.cast_id));
    const bool good = (!w.lands || landed >= 1) && paid && ce.cooldown_on && chan_ends(ce, ce.cast_id) == 0;
    CHECKF(good, "%s, control, no Dodge: the ability goes out, it is paid, its cooldown runs and the client is told nothing", what);
    if (!good) {
        std::printf("    | %s\n", ce.probe.c_str());
        log_excerpt(ce.log, cc.ability);
    }
    return good;
}

// An ordinary instant ability has a short wind up: its item comes at once, its cost step at about 0.3 s and its hit is asked for by the client after that. A Dodge in the wind up cancels it (before the
// cost step nothing was paid, after it the cost comes back); once the hit was asked for or landed the Dodge changes nothing. `w` holds the times of the windows for the ability's own timeline.
static void check_ordinary_ability_dodge(const svc::Character& rec, ChanCast cc, const char* hit, const char* what, const DodgeWindows& w = DodgeWindows()) {
    prepare_dodge_cast(cc, hit, w);
    int control_landed = 0;
    float price = 0;
    if (!run_dodge_control(rec, cc, hit, what, w, control_landed, price)) return;
    // a Dodge before the cost step: nothing was paid, the cast is cancelled
    cc.dodge_at = w.before;
    {
        ChanEnd de;
        const bool ok = run_chan_cast(rec, cc, de);
        CHECKF(ok, "%s: a world with ground and a Wolf and a Dodge before the cost step", what);
        if (!ok) return;
        std::printf("  (%s, Dodge at %.2f s: %d hit(s), Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", what, de.dodged_at, landed_hits(de, hit, w), de.mana_before, de.mana_after, de.cooldown_on,
                    de.charges_left, chan_ends(de, de.cast_id));
        CHECKF(de.dodge_item && landed_hits(de, hit, w) == 0, "%s: a Dodge before the cost step cancels it: nothing of it goes out", what);
        CHECKF(chan_ends(de, de.cast_id) == 1, "%s: ... and the client is told it is over: one ClientEndAbility for its instance", what);
        CHECKF(dodge_unpaid(de, w), "%s: ... nothing was paid: no Mana (a regeneration tick may have added some) and no cooldown", what);
    }
    // a Dodge after the cost step and before the ability goes out: the cost comes back
    if (w.between >= 0) {
        cc.dodge_at = w.between;
        ChanEnd de;
        const bool ok = run_chan_cast(rec, cc, de);
        CHECKF(ok, "%s: a world with ground and a Wolf and a Dodge between the cost step and the launch", what);
        if (!ok) return;
        std::printf("  (%s, Dodge at %.2f s: %d hit(s), Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", what, de.dodged_at, landed_hits(de, hit, w), de.mana_before, de.mana_after, de.cooldown_on,
                    de.charges_left, chan_ends(de, de.cast_id));
        CHECKF(de.dodge_item && landed_hits(de, hit, w) == 0, "%s: a Dodge between the cost step and the launch cancels it: nothing of it goes out", what);
        CHECKF(chan_ends(de, de.cast_id) == 1, "%s: ... and the client is told it is over: one ClientEndAbility for its instance", what);
        CHECKF(dodge_unpaid(de, w), "%s: ... and the cost is given back: the Mana (a regeneration tick may have added some) and the cooldown", what);
    }
    // a Dodge after the ability went out: the Dodge changes nothing
    cc.dodge_at = w.after;
    {
        ChanEnd de;
        const bool ok = run_chan_cast(rec, cc, de);
        CHECKF(ok, "%s: a world with ground and a Wolf and a Dodge after it went out", what);
        if (!ok) return;
        std::printf("  (%s, Dodge at %.2f s: %d hit(s), Mana %.1f -> %.1f, cooldown %d, %d end(s))\n", what, de.dodged_at, landed_hits(de, hit, w), de.mana_before, de.mana_after, de.cooldown_on,
                    chan_ends(de, de.cast_id));
        CHECKF(de.dodge_item && (!w.lands || landed_hits(de, hit, w) >= 1), "%s: a Dodge after it went out changes nothing: its hit stays landed (when it has one)", what);
        CHECKF(chan_ends(de, de.cast_id) == 0 && dodge_paid(de, w, price), "%s: ... the client is told nothing and it stays paid: the Mana is spent (when it spends any) and the cooldown runs", what);
    }
    // a Dodge the server refuses, also before the cost step
    cc.dodge_first = true;
    cc.dodge_at = w.refused >= 0 ? w.refused : w.before;
    {
        ChanEnd re;
        const bool ok = run_chan_cast(rec, cc, re);
        CHECKF(ok, "%s: a world with ground and a Wolf and two Dodges", what);
        if (!ok) return;
        CHECKF(chan_ends(re, re.dodge_id) == 1, "%s: the second Dodge, inside the cooldown of the first, is refused (ClientEndAbility for its instance)", what);
        CHECKF((!w.lands || landed_hits(re, hit, w) >= 1) && chan_ends(re, re.cast_id) == 0 && dodge_paid(re, w, price), "%s: a refused Dodge changes nothing: the ability goes out, the cast is not ended, it stays paid", what);
    }
}

/// An ability a Dodge leaves alone: the same cast with a Dodge at each of `dodges` is not ended, stays paid and goes out as it did with no Dodge.
static void check_dodge_leaves_alone(const svc::Character& rec, ChanCast cc, const char* hit, const char* what, const DodgeWindows& w, std::initializer_list<double> dodges) {
    prepare_dodge_cast(cc, hit, w);
    int control_landed = 0;
    float price = 0;
    if (!run_dodge_control(rec, cc, hit, what, w, control_landed, price)) return;
    for (const double at : dodges) {
        cc.dodge_at = at;
        ChanEnd de;
        const bool ok = run_chan_cast(rec, cc, de);
        CHECKF(ok, "%s: a world with ground and a Wolf and a Dodge at %.2f s", what, at);
        if (!ok) return;
        std::printf("  (%s, Dodge at %.2f s: %d hit(s), Mana %.1f -> %.1f, cooldown %d, charges %d, %d end(s))\n", what, de.dodged_at, landed_hits(de, hit, w), de.mana_before, de.mana_after, de.cooldown_on,
                    de.charges_left, chan_ends(de, de.cast_id));
        CHECKF(de.dodge_item && chan_ends(de, de.cast_id) == 0, "%s: a Dodge at %.2f s ends nothing: the client is told nothing about the cast", what, at);
        CHECKF((!w.lands || landed_hits(de, hit, w) == control_landed) && dodge_paid(de, w, price), "%s: ... it stays paid and goes out as it did with no Dodge", what);
    }
}

void test_dodge_cancels_an_ordinary_ability() {
    std::printf("a Dodge in the wind up of an ordinary instant ability cancels it (the cost comes back once it was paid); after the hit was asked for the Dodge changes nothing\n");
    {   // a physical instant: the Fighter's Lethal Blow at the Wolf (its cost step comes at 0.32 s)
        ChanCast cc;
        cc.ability = "Fighter_LethalBlow";
        cc.aim_wolf = true;
        cc.ahead = 120;
        check_ordinary_ability_dodge(class25(4, "Fighter", 17749), cc, "Fighter_LethalBlow", "Fighter_LethalBlow");
    }
    {   // a magical instant: the Summoner's Spirit Shred at the Wolf (its cost step comes at 0.22 s; it has no bar, no projectile and no zone)
        ChanCast cc;
        cc.ability = "Summoner_SpiritShred";
        cc.aim_wolf = true;
        cc.ahead = 600;
        check_ordinary_ability_dodge(class25(6, "Summoner", 17753), cc, "Summoner_SpiritShred", "Summoner_SpiritShred");
    }
}
AOC_REGISTER_TEST(test_dodge_cancels_an_ordinary_ability);

// Songs and the Sprint are not ended by a Dodge.
void test_dodge_leaves_songs_and_the_sprint() {
    std::printf("a Dodge leaves a Bard song and the Sprint going\n");
    {
        const svc::Character rec = bard25();
        Run r(0, true, &rec);
        r.play(0.5, true);
        r.h.s->teach_ability(kCheerful);
        const int32_t song = trigger_aimed_now(r, kCheerful, false);
        r.play(3.5, false);
        CHECK(r.m.has_effect(kCheerfulSelf), "(the song's effect is on 3.5 s after the press)");
        const int32_t dodge = r.trigger(kDodge);
        CHECK(r.m.has_ability(kDodge) && dodge != song, "the server took the Dodge");
        r.play(4.0, false);
        const std::vector<int32_t> ends = r.m.end_ids();
        CHECK(r.m.has_effect(kCheerfulSelf), "a Dodge does not end a song: its effect is still on");
        CHECK(std::count(ends.begin(), ends.end(), song) == 0, "... and the client is not told the song is over");
    }
    {
        const svc::Character mage = class25(8, "Mage", 17750);
        Run r(0, true, &mage);
        r.play(0.5, true);
        const int32_t sprint = r.trigger(kSprint);
        r.play(1.0, true);
        CHECK(r.rt().sprinting(), "(the Sprint is on)");
        const int32_t dodge = r.trigger(kDodge);
        CHECK(r.m.has_ability(kDodge) && dodge != sprint, "the server took the Dodge");
        r.play(1.0, true);
        const std::vector<int32_t> ends = r.m.end_ids();
        CHECK(r.rt().sprinting() && r.m.has_effect(kSprintEffect), "a Dodge does not end the Sprint");
        CHECK(std::count(ends.begin(), ends.end(), sprint) == 0, "... and the client is not told the Sprint is over");
    }
}
AOC_REGISTER_TEST(test_dodge_leaves_songs_and_the_sprint);

// ---- the other kinds of ability: where each one goes out, and what a Dodge leaves alone ----------------------------------------------------------------------------------------------------------

void test_dodge_cancels_each_kind_of_ability() {
    std::printf("a Dodge cancels an ability that has not gone out, whatever its kind: a hit the client asks for, a Song, a hit the server makes, a projectile, a zone, an effect on the caster, an ability with no hit\n");
    const svc::Character fighter = class25(4, "Fighter", 17749);
    const svc::Character mage = class25(8, "Mage", 17750);
    {   // a hit the client asks for, with no montage end in the recordings (cost step at 0.3 s): it stays open for 1.4 s after the press, or until the hit is asked for
        ChanCast cc;
        cc.ability = "Fighter_Overpower";
        cc.aim_wolf = true;
        cc.ahead = 120;
        check_ordinary_ability_dodge(fighter, cc, "Fighter_Overpower", "Fighter_Overpower");
    }
    {   // ... and one whose hit the client never asks for: a Dodge inside the 1.4 s cancels it, one after it does not
        ChanCast cc;
        cc.ability = "Fighter_Overpower";
        cc.aim_wolf = true;
        cc.ahead = 120;
        DodgeWindows w;
        w.ask_from = -1;
        w.lands = false;
        w.between = 1.2;
        w.after = 1.7;
        check_ordinary_ability_dodge(fighter, cc, nullptr, "Fighter_Overpower with no hit asked for", w);
    }
    {   // one of the ten one-shot Songs (not a Melody), aimed at the caster: its cost step comes at 1.9 s, so there is a long wind up before it
        ChanCast cc;
        cc.ability = "Bard_Song_Anthem";
        cc.aim_self = true;
        cc.ask_wolf = false;
        DodgeWindows w;
        w.before = 1.0;
        w.between = 2.05;
        w.after = 2.6;
        w.refused = 0.3;
        w.ask_from = 2.2;
        w.seconds = 4.0;
        w.on_target = false;
        check_ordinary_ability_dodge(bard25(), cc, "Bard_Song_Anthem_ST", "Bard_Song_Anthem", w);
    }
    {   // a hit the server makes by itself, 0.035 s after the cost step (0.53 s): a Dodge before it cancels, one after it changes nothing
        ChanCast cc;
        cc.ability = "Fighter_Wallop";
        cc.aim_wolf = true;
        cc.ahead = 120;
        DodgeWindows w;
        w.before = 0.3;
        w.between = -1;
        w.after = 0.9;
        w.ask_from = -1;
        w.seconds = 2.0;
        check_ordinary_ability_dodge(fighter, cc, "Fighter_Wallop", "Fighter_Wallop", w);
    }
    {   // a projectile: the Firebolt leaves 0.3 s after the press and lands a flight later; a Dodge before it leaves cancels it, one after it left cannot stop it
        ChanCast cc;
        cc.ability = "Mage_Firebolt";
        cc.aim_wolf = true;
        cc.ahead = 600;
        DodgeWindows w;
        w.before = 0.15;
        w.between = -1;
        w.after = 0.9;
        w.ask_from = -1;
        w.seconds = 2.5;
        check_ordinary_ability_dodge(mage, cc, "Mage_FireballCombo_Projectile_Hit", "Mage_Firebolt", w);
    }
    {   // a zone: it goes out at its cost step (0.16 s), when its first hit is made
        ChanCast cc;
        cc.ability = "Mage_ArcaneEye";
        DodgeWindows w;
        w.before = 0.1;
        w.between = -1;
        w.after = 0.6;
        w.ask_from = -1;
        w.seconds = 1.5;
        w.lands = false;
        check_ordinary_ability_dodge(mage, cc, nullptr, "Mage_ArcaneEye", w);
    }
    {   // an ability that puts an effect on the caster itself: it goes out when the effect starts, 0.035 s after the cost step (0.28 s); it spends no Mana, so its cooldown shows that it is paid
        ChanCast cc;
        cc.ability = "Mage_Elemental_Empowerment";
        DodgeWindows w;
        w.before = 0.1;
        w.between = -1;
        w.after = 0.6;
        w.ask_from = -1;
        w.seconds = 1.0;
        w.lands = false;
        w.min_price = 0;
        check_ordinary_ability_dodge(mage, cc, nullptr, "Mage_Elemental_Empowerment", w);
    }
    {   // an ability with no hit of its own: nothing of it goes out for the server, so its payment is its launch (cost step at 0.35 s, the montage ends at 1.0 s)
        ChanCast cc;
        cc.ability = "Tank_Fortify";
        DodgeWindows w;
        w.before = 0.2;
        w.between = -1;
        w.after = 0.7;
        w.ask_from = -1;
        w.seconds = 2.0;
        w.lands = false;
        w.min_price = 0;
        check_ordinary_ability_dodge(class25(7, "Tank", 17754), cc, nullptr, "Tank_Fortify", w);
    }
}
AOC_REGISTER_TEST(test_dodge_cancels_each_kind_of_ability);

// A pet command is obeyed at the press, and a leap, a charge, a teleport and a dash move the character at the press: they count as gone out then, so a Dodge cancels and gives back nothing.
/// A Summoner with the Guardian out, one pet command pressed and a Dodge `dodge_ms` after it (the cost step comes 0.3 s after the press, Dismiss's 0.07 s): the Dodge cancels nothing, and the cooldown stays.
static void check_dodge_after_a_pet_command(const char* what, int64_t command, int dodge_ms, bool stays) {
    const svc::Character rec = class25(6, "Summoner", 17753);
    Run r(0, true, &rec, [&](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    });
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, false);
    const pets::Definition& guardian = pets::kDefinitions[0];
    r.h.s->teach_ability(guardian.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(guardian.ability);
    r.play(8.8, false);
    CHECKF(r.h.s->summoned_pet() != 0 && r.h.s->pet_following(), "%s: (the Guardian comes, and follows)", what);
    if (!r.h.s->summoned_pet()) return;
    const int32_t id = r.trigger(command);
    r.tick(false, std::chrono::milliseconds(dodge_ms));
    r.trigger(kDodge);
    CHECKF(r.m.has_ability(kDodge), "%s: (the server took the Dodge)", what);
    r.play(0.4, false);
    bool cooldown = false;
    for (const auto& [cooldown_id, guid] : r.m.cooldowns) cooldown = cooldown || guid == command;
    CHECKF(cooldown, "%s: the command's cooldown runs: the Dodge gave nothing back", what);
    if (stays) CHECKF(!r.h.s->pet_following(), "%s: the pet obeyed the command at the press, whatever the Dodge did", what);
    r.play(1.5, false);
    const std::vector<int32_t> ends = r.m.end_ids();
    CHECKF(std::count(ends.begin(), ends.end(), id) == 0, "%s: the client is told nothing about the end of the command", what);
}

void test_dodge_leaves_pet_commands_alone() {
    std::printf("a Dodge leaves a pet command alone: the pet obeys at the press, so the command has gone out\n");
    check_dodge_after_a_pet_command("Summoner_PetCommand_Stay", 6064633576956297816ll, 100, true);
    check_dodge_after_a_pet_command("Summoner_PetCommand_Dismiss", 6064633780469564013ll, 50, false);
}
AOC_REGISTER_TEST(test_dodge_leaves_pet_commands_alone);

void test_dodge_leaves_leaps_charges_and_teleports_alone() {
    std::printf("a Dodge leaves a leap, a charge, a teleport and a dash alone: they count as gone out at the press\n");
    const svc::Character fighter = class25(4, "Fighter", 17749);
    {   // a leap the data gives a speed (cost step at 0.26 s)
        ChanCast cc;
        cc.ability = "Fighter_Blitz";
        cc.aim_wolf = true;
        cc.ahead = 120;
        DodgeWindows w;
        w.ask_from = -1;
        w.seconds = 1.5;
        w.lands = false;
        check_dodge_leaves_alone(fighter, cc, nullptr, "Fighter_Blitz", w, {0.1});
    }
    {   // a leap the data gives no speed (cost step at 0.3 s)
        ChanCast cc;
        cc.ability = "Fighter_Leapstrike";
        DodgeWindows w;
        w.ask_from = -1;
        w.seconds = 1.5;
        w.lands = false;
        check_dodge_leaves_alone(fighter, cc, nullptr, "Fighter_Leapstrike", w, {0.1});
    }
    {   // a teleport (cost step at 0.08 s)
        ChanCast cc;
        cc.ability = "Mage_Blink";
        DodgeWindows w;
        w.ask_from = -1;
        w.seconds = 1.0;
        w.lands = false;
        check_dodge_leaves_alone(class25(8, "Mage", 17750), cc, nullptr, "Mage_Blink", w, {0.05});
    }
    {   // a charge: it was cancelled by a Dodge until now; the Dodge comes well after its cost step (0.07 s)
        ChanCast cc;
        cc.ability = "Tank_Charge";
        DodgeWindows w;
        w.ask_from = -1;
        w.seconds = 2.0;
        w.lands = false;
        check_dodge_leaves_alone(class25(7, "Tank", 17754), cc, nullptr, "Tank_Charge", w, {0.5});
    }
}
AOC_REGISTER_TEST(test_dodge_leaves_leaps_charges_and_teleports_alone);

/// ServerTriggerAbility as the client sends it for a consumable: the ability of the item, the item's slot in the bag, its whole instance and its place (3: the bag).
static BunchWriter trigger_with_bag_item(const bs::PlayerSetup& setup, int64_t guid, int32_t id, int slot, const svc::InventoryItem& item) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(items::to_instance(item)));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(3));
    if (!ok) {
        std::printf("  [FAIL] the consumable trigger's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "ServerTriggerAbility", p);
}

// A potion, a scroll or an elixir is used at the press: its item leaves the bag and its buff is applied, whatever the Dodge does. A Dodge never gives an item back.
void test_dodge_leaves_a_consumable_alone() {
    std::printf("a Dodge leaves a consumable alone: its item is used at the press, its buff applies in full and nothing is given back\n");
    constexpr int64_t kElixir = 6064632106057072682ll;   // the item: Elixir of Constitution
    for (const double dodge_at : {-1.0, 0.03, 0.3}) {
        svc::Character rec = class25(4, "Fighter", 17749);
        rec.items.push_back(stack_of(kElixir, 0, 3, 93001));
        Run r(0, true, &rec);
        r.play(0.5, false);
        const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
        const abilities::Catalog* cat = r.h.fx.world->ability_catalog();
        const abilities::AbilityDef* def = cat ? cat->ability_named("Consumable_Elixir_Constitution") : nullptr;
        const svc::InventoryItem* item = r.h.s->inventory().at(svc::ItemContainer::Backpack, 0);
        CHECK(def && item && !def->hits.empty() && !def->effects.empty() && held_of(*r.h.s, kElixir) == 3, "(three Elixirs of Constitution lie in the bag, and the ability, its hit and its effect are in the tables)");
        if (!def || !item || def->hits.empty() || def->effects.empty()) return;
        const svc::InventoryItem stack = *item;
        const int64_t effect = def->effects.begin()->second;
        const int32_t id = r.next_id++;
        const BunchWriter trig = trigger_with_bag_item(setup, def->row.guid, id, 0, stack);
        r.send_trigger(def->row.guid, id, &trig);
        bool dodged = dodge_at < 0, asked = false;
        for (int i = 0; i < 40; ++i) {
            const double t = i * 0.05 + 1e-9;
            if (!asked && t >= 0.4) {   // the drink's animation reaches the hit about now
                asked = true;
                r.send_reliable(hit_request_bunch(setup, def->row.guid, kTargetingSelf, def->hits.front().hit_guid));
            }
            if (!dodged && t >= dodge_at) {
                dodged = true;
                r.trigger(kDodge);
                CHECK(r.m.has_ability(kDodge), "(the server took the Dodge)");
            }
            r.tick(false, std::chrono::milliseconds(50));
        }
        const std::vector<int32_t> ends = r.m.end_ids();
        std::printf("  (Dodge at %.2f s: %lld Elixirs left in the bag, the buff %s, %d end(s) for the cast)\n", dodge_at, static_cast<long long>(held_of(*r.h.s, kElixir)), r.m.has_effect(effect) ? "on" : "off",
                    static_cast<int>(std::count(ends.begin(), ends.end(), id)));
        CHECK(held_of(*r.h.s, kElixir) == 2, "the item was used at the press and nothing gave it back: two Elixirs are left");
        CHECK(r.m.has_effect(effect), "the buff applies in full: its effect is on the character");
        CHECK(std::count(ends.begin(), ends.end(), id) == 0, "the client is told nothing about the end of the cast");
    }
}
AOC_REGISTER_TEST(test_dodge_leaves_a_consumable_alone);

// Drop Crate: the crate leaves the back 0.7 s after the press, whatever the pipeline did with the cast. A Dodge before that keeps the crate on the back (the client's own cancel does the same).
void test_dodge_keeps_the_crate_on_the_back() {
    std::printf("a Dodge before the crate leaves the back keeps it there; with no Dodge Drop Crate puts it into the item bag 0.7 s after the press\n");
    constexpr int64_t kCrate = 6064634015066030099ll;   // Crate_Automata_Personal_Parts: a small commodity crate carried on the back
    for (const double dodge_at : {-1.0, 0.1, 0.5}) {
        svc::Character rec = class25(4, "Fighter", 17749);
        svc::InventoryItem crate = stack_of(kCrate, 0, 1, 94001);
        crate.container = svc::ItemContainer::Equipment;
        crate.slot = items::equip::CargoHold;
        rec.items.push_back(crate);
        Run r(0, true, &rec);
        r.play(0.5, false);
        phase_in(r);   // (the crate leaves the back only after the phase-in)
        r.play(1.0, false);
        r.h.s->teach_ability(content::kGuidDropCrate);
        CHECK(r.h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::CargoHold) != nullptr, "(the crate is on the back)");
        g_log.str("");
        r.trigger(content::kGuidDropCrate);
        bool dodged = dodge_at < 0;
        for (int i = 0; i < 24; ++i) {
            const double t = i * 0.05 + 1e-9;
            if (!dodged && t >= dodge_at) {
                dodged = true;
                r.trigger(kDodge);
                CHECK(r.m.has_ability(kDodge), "(the server took the Dodge)");
            }
            r.tick(false, std::chrono::milliseconds(50));
        }
        const bool on_back = r.h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::CargoHold) != nullptr;
        std::printf("  (Dodge at %.2f s: the crate is %s)\n", dodge_at, on_back ? "on the back" : "in the bag");
        if (dodge_at < 0) {
            const bool left = !on_back && held_of(*r.h.s, kCrate) == 1;
            CHECK(left, "control, no Dodge: the crate left the back and lies in the bag");
            if (!left) {
                spdlog::default_logger()->flush();
                log_excerpt(g_log.str(), "DropCrate");
            }
        } else {
            CHECK(on_back, "a Dodge before the crate left the back keeps it on the back");
        }
    }
}
AOC_REGISTER_TEST(test_dodge_keeps_the_crate_on_the_back);

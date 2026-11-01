// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_forged_requests_tests.inc: what a client that sends calls the game never sends cannot get out of the ability runtime
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
//
//  Each check builds a character of the Ability rig in a world with Wolves (the way a session attaches the runtime to the world), casts one ability and plays the
//  request a modified client could send: a charged shot asked for too soon, a charged shot that cannot be paid for, a cast bar ended after its zone opened, a quick
//  tap that asks like a full bar, a key release that claims a huge hold, a launch asked for long before the montage gets there, a hit asked for again and again.
// ============================================================================

/// A character of `class_enum` at level 25 with a world around him: the ability runtime is attached to a combat world and a fake player.
struct Forge {
    Rig                    rig;
    combat_rig::World      w;
    combat::NpcStatBook    book;
    combat::CombatWorld    world;
    combat_rig::FakePlayer player;
    combat::WorldHitPort   port;
    std::string            wolf_path;

    Forge(Fixture& f, int class_enum, const std::vector<std::string>& skills, uint64_t seed = 7)
        : rig(class_enum, 25, skills, true, seed),
          book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          player(rig, 77, {0, 0, 1098}),
          port(world, w.registry, player.fighter()),
          wolf_path(wolf_archetype(f)) {
        if (!rig.ok()) return;
        world.add_player(&player);
        rig.pipe->set_combat(&port);
    }
    bool ok() const { return rig.ok() && w.problem.empty(); }
    /// A Wolf of level 25 at (x, y) (the player stands at 0, 0 and faces +X; a small aggro radius: it does not come for him).
    uint64_t wolf(double x, double y) { return w.spawn(wolf_path, x, y, 25, 100); }
    /// How much Health these Wolves have lost.
    double lost(const std::vector<uint64_t>& wolves) const {
        double sum = 0;
        for (const uint64_t id : wolves) sum += world.max_health_of(id) - world.health_of(id);
        return sum;
    }
};

/// How many times the zones' actors were opened, faded out or closed in what the rig has seen.
size_t forged_zone_ops(const Rig& rig, bootstrap::ZoneOp::Kind kind) {
    size_t n = 0;
    for (const Seen& s : rig.log) {
        for (const bootstrap::ZoneOp& z : s.zones) n += z.kind == kind ? 1 : 0;
    }
    return n;
}

/// The confirmations of a hit that named a target, from everything the rig has seen.
std::vector<const bootstrap::HitConfirm*> forged_confirmations(const Rig& rig, int64_t hit_guid) {
    std::vector<const bootstrap::HitConfirm*> out;
    for (const Seen& s : rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid == hit_guid && !c.targets.empty()) out.push_back(&c);
        }
    }
    return out;
}

// ---- a held Scatter Shot (the Concentrated talent) --------------------------------------------------------------------------------------------------
// The charged shot is fired when the client asks for the branch of a full charge, which the recordings show 1.11 to 1.15 s after the trigger; the shot pierces every enemy on a line ahead of the
// caster. A request that comes sooner is taken as made when a charge can fill, and a cast that cannot pay its cost when the shot is due fires nothing.
void test_forged_scatter_shot(Fixture& f) {
    std::printf("a held Scatter Shot: the charged shot waits for a charge to fill, and a cast that cannot pay fires nothing\n");
    constexpr double t0 = 1.0;
    const int64_t charged = ability_rig::record_guid(real_db_path(), "AnimBranchRecordBase", "Ranger_Passive_ScatterShot_Concentrated");
    CHECK(charged != 0, "the animation branch of the talent's charged shot is in the data");
    if (charged == 0) return;
    struct Result {
        bool   ok = false, accepted = false, unpaid = false;
        double mana_before = 0, mana_early = 0, mana_late = 0, lost_early = 0, lost_late = 0;
    };
    // one cast of a Ranger who holds the talent, with four Wolves in the line ahead of him (the nearest 1.5 m away: a scatter reaches it in 25 ms, before the cost that follows the jump by 36 ms):
    // the client asks for the charged shot `request` seconds after the trigger (`tap`: lets the key go then instead, which sends the scatter), `drain` takes all his Mana at that moment, and the
    // Wolves are looked at `early` and `late` seconds after the trigger
    const auto cast = [&](double request, bool drain, double early, double late, bool tap = false) {
        Result out;
        Forge g(f, 1, {"Ranger_Passive_ScatterShot_Concentrated"});
        CHECKF(g.ok(), "the world for the Scatter Shot (%s%s)", g.rig.problem.c_str(), g.w.problem.c_str());
        if (!g.ok()) return out;
        const std::vector<uint64_t> wolves = {g.wolf(150, 0), g.wolf(400, 0), g.wolf(700, 0), g.wolf(1000, 0)};
        g.rig.know({"Ranger_ScatterShot"});
        const int32_t id = g.rig.next_client_id;
        const Refusal refusal = g.rig.trigger("Ranger_ScatterShot", t0, false);
        out.accepted = refusal == Refusal::None;
        if (!out.accepted) std::printf("    (the Scatter Shot was refused: %s)\n", refusal_name(refusal));
        out.mana_before = g.rig.mana();
        g.rig.run_to(t0 + request);
        if (tap) g.rig.pipe->cancel(id, t0 + request, 0.2);   // (the key goes up; the call says how long it was held)
        else g.rig.pipe->branch_request(id, charged, t0 + request);
        if (drain) g.rig.set_current(content::kGuidMana, 0.0);
        g.rig.run_to(t0 + early);
        out.lost_early = g.lost(wolves);
        out.mana_early = g.rig.mana();
        g.rig.run_to(t0 + late);
        out.lost_late = g.lost(wolves);
        out.mana_late = g.rig.mana();
        for (const Seen& s : g.rig.log) out.unpaid = out.unpaid || (s.refusal && s.note.find("cannot be paid for any more") != std::string::npos);
        out.ok = g.rig.pawn.broken.empty();
        return out;
    };
    // the request comes 0.3 s after the trigger: nothing is fired or paid before a charge can fill (1.0 s), and then the shot goes out and is paid for
    const Result soon = cast(0.3, false, 0.9, 1.8);
    CHECK(soon.accepted && soon.ok, "(the Scatter Shot is cast, and the pawn takes every change)");
    CHECKF(soon.lost_early == 0.0 && soon.mana_early == soon.mana_before, "a request for the charged shot 0.3 s after the trigger fires nothing at 0.9 s: no Wolf is hurt and no Mana is paid (%.1f Health lost, Mana %.2f of %.2f)",
           soon.lost_early, soon.mana_early, soon.mana_before);
    CHECKF(soon.lost_late > 0.0 && soon.mana_late < soon.mana_before, "...the shot goes out when the charge could be full (1.0 s plus the montage's jump) and is paid for: %.1f Health lost, Mana %.2f of %.2f",
           soon.lost_late, soon.mana_late, soon.mana_before);
    // the request comes when a charge is full, and all the Mana is gone by the time the shot is paid for: the cast is over and nothing is fired
    const Result broke = cast(1.2, true, 1.5, 2.0);
    CHECK(broke.accepted && broke.ok, "(a second Scatter Shot is cast)");
    CHECKF(broke.unpaid, "a charged shot that cannot be paid for ends the cast (\"cannot be paid for any more\")");
    CHECKF(broke.lost_early == 0.0 && broke.lost_late == 0.0, "...and fires nothing: no Wolf is hurt (%.1f at 1.5 s, %.1f at 2.0 s)", broke.lost_early, broke.lost_late);
    // the key goes up 0.5 s after the trigger (a tap): the scatter leaves with the jump 0.1 s later and is paid for 36 ms after that
    const Result tapped = cast(0.5, false, 1.2, 1.8, true);
    CHECK(tapped.accepted && tapped.ok, "(a third Scatter Shot is cast, its key let go at 0.5 s)");
    CHECKF(tapped.lost_late > 0.0 && tapped.mana_late < tapped.mana_before, "a tap sends the scatter and pays for it: %.1f Health lost, Mana %.2f of %.2f", tapped.lost_late, tapped.mana_late, tapped.mana_before);
    // ... with all the Mana gone as the key goes up: the jump comes before the cost step, and a cast that cannot pay sends nothing
    const Result tap_broke = cast(0.5, true, 1.2, 1.8, true);
    CHECK(tap_broke.accepted && tap_broke.ok && tap_broke.unpaid, "(a fourth Scatter Shot is cast, its key let go at 0.5 s with no Mana left: it ends at its cost step)");
    CHECKF(tap_broke.lost_early == 0.0 && tap_broke.lost_late == 0.0, "a tap that cannot pay sends nothing: no Wolf is hurt (%.1f at 1.2 s, %.1f at 1.8 s)", tap_broke.lost_early, tap_broke.lost_late);
}
AOC_REGISTER_TEST(test_forged_scatter_shot);

// ---- a cast bar the client ends ---------------------------------------------------------------------------------------------------------------------
// A cast opens the lingering zones its ability owns (the Bard's dances do) as it starts. When the client ends a cast bar before its end nothing is paid and nothing fires, and the zones the cast
// opened end with it (before this they stayed open for their whole life with no cast behind them). No ability with a cast bar owns a zone in the data today (a Cleric's Chains of Restraint opens
// its zone with a hit), so the check gives the Mage's Magma Field the two zones its record lists, for the length of the check, and puts them back.
struct ForgedOwnZones {
    abilities::AbilityDef* def = nullptr;
    std::vector<int64_t>   before;
    ForgedOwnZones(const abilities::AbilityDef* ability, std::vector<int64_t> zones) : def(const_cast<abilities::AbilityDef*>(ability)) {
        if (!def) return;
        before = def->zones;
        def->zones = std::move(zones);
    }
    ~ForgedOwnZones() {
        if (def) def->zones = before;
    }
};
void test_forged_cast_bar_zone(Fixture&) {
    std::printf("a cast bar the client ends takes its zone with it\n");
    Rig rig(8, 25, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    rig.know({"Mage_MagmaField"});
    const ForgedOwnZones zones(rig.catalog->ability_named("Mage_MagmaField"), {6064631404175951226ll, 6064631522804306548ll});   // (the Pyromania and the Wildfire field)
    constexpr double t0 = 1.0;
    const Refusal refusal = rig.trigger("Mage_MagmaField", t0, false);
    CHECKF(refusal == Refusal::None, "Magma Field is cast (%s)", refusal_name(refusal));
    rig.run_to(t0 + 0.5);
    int64_t instance = -1;
    for (const auto& [key, item] : rig.pawn.abilities) {
        if (item.ability_guid == rig.ability("Mage_MagmaField")) instance = key;
    }
    CHECKF(instance >= 0 && forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Open) == 2, "the cast bar is under way: its item is on the pawn and its two zones have opened (%zu opened)",
           forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Open));
    if (instance < 0) return;
    CHECK(rig.pipe->end_request(static_cast<int32_t>(instance), t0 + 0.6), "the client ends the instance before its bar is over");
    rig.run_to(t0 + 3.0);
    CHECKF(forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Fade) == 2 && forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Close) == 2,
           "both zones fade out and close with the cancelled cast, 2.4 s on (%zu faded, %zu closed)", forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Fade), forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Close));
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_forged_cast_bar_zone);

// ---- a held ability's fill -------------------------------------------------------------------------------------------------------------------------
// The Mage's Lightning Strike hits for 100 % to 300 % of its damage by how long the key was held (the progress ratio of the cast). A bar that ran out asks for its branch at its end: a full
// charge. A tap asks at once (retail's own quick Lightning Strike came as a request 171 ms after the trigger) and a release claims its hold in the call (CancelledTime): the quick tap is a
// charge of nothing, and a hold that is longer than the cast has run cannot be believed.
struct ForgedStrike {
    bool    ok = false;
    double  value = 0;
    uint8_t flags = 0;
};
/// One Lightning Strike at a Wolf 6 m ahead, the fill the cast ended with by `how`: 0 the bar ran out (a request at its end), 1 a tap (a request 171 ms after the trigger), 2 a release 0.3 s after
/// the trigger that claims a hold of a million seconds. The hit's damage on the Wolf, as the client was told.
ForgedStrike forged_strike(Fixture& f, uint64_t seed, int how) {
    ForgedStrike out;
    Forge g(f, 8, {}, seed);
    if (!g.ok()) return out;
    const uint64_t wolf = g.wolf(600, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_LightningStrike");
    const abilities::HitDef* hit = f.catalog->hit_named("Mage_LightningStrike_Hit");
    if (!wolf || !def || !hit) return out;
    g.rig.know({"Mage_LightningStrike"});
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : def->hits) {
        if (h.hit_guid != hit->row.guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : def->targetings) {
            if (tg.key == h.key) targeting = tg.guid;
        }
    }
    constexpr int64_t kCastComplete = 6064628894971461633ll;   // the branch of the cast bar (Mage_Lightning_CastComplete)
    constexpr double t0 = 1.0;
    constexpr int32_t id = 41;
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    if (g.rig.pipe->trigger(def->row.guid, id, t0, info) != Refusal::None) return out;
    double ask = t0 + 1.2;   // when the client asks for the hit, after the cost step
    if (how == 0) {
        g.rig.run_to(t0 + 2.416);
        g.rig.pipe->branch_request(id, kCastComplete, t0 + 2.416);
        ask = t0 + 3.2;
    } else if (how == 1) {
        g.rig.run_to(t0 + 0.171);
        g.rig.pipe->branch_request(id, kCastComplete, t0 + 0.171);
    } else {
        g.rig.run_to(t0 + 0.3);
        g.rig.pipe->cancel(id, t0 + 0.3, 1.0e6);
    }
    g.rig.run_to(ask);
    g.rig.pipe->hit_request(def->row.guid, targeting, hit->row.guid, 1, 0, ask, {wolf});
    g.rig.run_to(ask + 1.0);
    for (const bootstrap::HitConfirm* c : forged_confirmations(g.rig, hit->row.guid)) {
        if (c->targets[0].mods.empty()) continue;
        out.ok = true;
        out.value = c->targets[0].mods[0].value;
        out.flags = c->targets[0].flags;
    }
    return out;
}

void test_forged_held_fill(Fixture& f) {
    std::printf("a held ability's fill: a tap asks like a full bar, a release claims a hold longer than the cast has run\n");
    bool done = false;
    // (a critical hit or a miss changes the number: the first seed whose three casts are plain ones is compared)
    for (uint64_t seed = 1; seed <= 12 && !done; ++seed) {
        const ForgedStrike full = forged_strike(f, seed, 0), tap = forged_strike(f, seed, 1), forged = forged_strike(f, seed, 2);
        if (!full.ok || !tap.ok || !forged.ok || full.flags != 0 || tap.flags != 0 || forged.flags != 0 || full.value == 0.0) continue;
        done = true;
        const double tap_share = tap.value / full.value, forged_share = forged.value / full.value;
        CHECK(full.value < 0.0, "a full bar hits the Wolf for damage");
        CHECKF(tap_share > 0.25 && tap_share < 0.42, "a tap that asked 171 ms after the trigger does a third of a full bar's damage (100 %% against 300 %%): %.3f of it", tap_share);
        CHECKF(forged_share > 0.30 && forged_share < 0.55, "a release 0.3 s after the trigger that claims a hold of a million seconds is cut to the 0.3 s it ran (about 127 %% against 300 %%): %.3f of a full bar", forged_share);
    }
    CHECK(done, "a seed where the full bar, the tap and the forged release are all plain hits (no critical hit, block or miss)");
}
AOC_REGISTER_TEST(test_forged_held_fill);

// ---- a launch asked for too soon --------------------------------------------------------------------------------------------------------------------
// The Ranger's Snipe launches its arrow at a moment of its montage (about 3.8 s after the trigger at a level 25 Ranger's speed), and the client asks for the hit that goes with the launch in the
// very frame of it: that request is the launch. A forged request 0.3 s after the trigger would land the arrow at the cost step; the request moves the launch only a little sooner than the plan.
void test_forged_launch_request(Fixture& f) {
    std::printf("a launch asked for long before the montage gets there is not believed\n");
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the world for the Snipe (%s%s)", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(1200, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named("Ranger_Snipe");
    CHECK(wolf != 0 && def != nullptr, "a Wolf 12 m ahead and the Snipe");
    if (!wolf || !def) return;
    g.rig.know({"Ranger_Snipe"});
    constexpr double t0 = 1.0;
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    const Refusal refusal = g.rig.pipe->trigger(def->row.guid, 50, t0, info);
    CHECKF(refusal == Refusal::None, "the Snipe is cast at the Wolf (%s)", refusal_name(refusal));
    g.rig.take();
    g.rig.hit_request("Ranger_Snipe", "Ranger_Snipe_Hasting", t0 + 0.3);   // the forged request: the montage's own comes at about 3.8 s
    g.rig.run_to(t0 + 2.0);
    const std::vector<uint64_t> wolves = {wolf};
    CHECKF(g.lost(wolves) == 0.0, "the arrow has not left 2.0 s after the trigger, however early the request came (%.1f Health lost)", g.lost(wolves));
    g.rig.run_to(t0 + 6.0);
    CHECKF(g.lost(wolves) > 0.0, "...and it lands when the montage launches it (%.1f Health lost at 6.0 s)", g.lost(wolves));
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}
AOC_REGISTER_TEST(test_forged_launch_request);

// ---- a hit asked for again and again ----------------------------------------------------------------------------------------------------------------
// The Fighter's Maim throws a wave of its own when the client asks for its Slicing hit (the talent): the projectile leaves once a cast, however often the hit is asked for, and never after the caster's
// death.
void test_forged_slicing_requests(Fixture& f) {
    std::printf("a hit that fires a projectile fires it once a cast, and never after the caster died\n");
    const abilities::HitDef* slicing = f.catalog->hit_named("Fighter_Maim_Slicing");
    CHECK(slicing != nullptr, "the Maim's projectile hit is in the data");
    if (!slicing) return;
    for (const bool dies : {false, true}) {
        Forge g(f, 4, {"Fighter_Passive_Maim_Slicing"});
        CHECKF(g.ok(), "the world for the Maim (%s%s)", g.rig.problem.c_str(), g.w.problem.c_str());
        if (!g.ok()) return;
        const uint64_t wolf = g.wolf(800, 0);
        g.rig.know({"Fighter_Maim"});
        constexpr double t0 = 1.0;
        g.rig.asks = false;
        const Refusal refusal = wolf != 0 ? g.rig.trigger("Fighter_Maim", t0, false) : Refusal::Busy;
        CHECKF(wolf != 0 && refusal == Refusal::None, "the Maim is cast with a Wolf in the line ahead (%s)", refusal_name(refusal));
        // the client asks for the Slicing hit again and again (a modified one: every 0.2 s, eight requests are the most a hit takes)
        const int asks = dies ? 1 : 6;
        for (int i = 0; i < asks; ++i) g.rig.hit_request("Fighter_Maim", "Fighter_Maim_Slicing_Fire", t0 + 0.6 + 0.2 * i);
        if (dies) g.rig.pipe->set_dead(true);   // (the request was accepted; the character dies before the confirmation is due)
        g.rig.run_to(t0 + 4.0);
        const size_t waves = forged_confirmations(g.rig, slicing->row.guid).size();
        if (dies) {
            CHECKF(waves == 0, "a request accepted just before the death fires nothing after it (%zu hits of the wave)", waves);
        } else {
            CHECKF(waves == 1, "six requests for the Slicing hit throw one wave: one hit on the Wolf (%zu)", waves);
        }
    }
}
AOC_REGISTER_TEST(test_forged_slicing_requests);

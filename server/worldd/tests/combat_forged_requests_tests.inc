// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_forged_requests_tests.inc: what a client that sends calls the game never sends cannot get out of the ability runtime
//  (registered with AOC_REGISTER_TEST: test_worldd_combat.cpp runs it through aoc_tests::run; it is included from combat_registered.inc)
//
//  Each check builds a character of the Ability rig in a world with Wolves (the way a session attaches the runtime to the world), casts one ability and plays the
//  request a modified client could send: a charged shot asked for too soon, a charged shot that cannot be paid for, a cast bar ended after its zone opened, a quick
//  tap that asks like a full bar, a key release that claims a huge hold, a launch asked for long before the montage gets there, a hit asked for again and again, and the ranged weapon swings' montage, requests and stun.
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

// ---- the ranged weapon swings: what a modified client picks, asks for and keeps in the air ---------------------------------------------------------------
// A ranged weapon swing names its step in the call (the AnimationIndex: the montage of the step) and the server fires the arrows of that step: the Shortbow's swings send one arrow, two, one, three,
// three and five. The client plays the steps in order and starts over after the last; a modified client could name the five-arrow step on every swing, ask for the hits the arrows carry on top of
// the arrows' own landing, let a swing's arrows leave after a stun, and hold a cast for as long as an arrow takes to reach a target that ran off.

/// A Ranger who swings a ranged weapon combo at a Wolf, swing after swing; what landed is counted from the confirmations the client was sent.
struct Swings {
    Forge                        g;
    const abilities::AbilityDef* def = nullptr;
    uint64_t                     wolf = 0;
    double                       t = 1.0;       // when the last swing was triggered
    int32_t                      id = 100;
    std::set<int64_t>            arrow_hits;    // the hits the combo's projectiles carry

    Swings(Fixture& f, const char* combo, double x = 800.0) : g(f, 1, {}), def(f.catalog->ability_named(combo)) {
        if (!g.ok() || !def) return;
        wolf = g.w.spawn(g.wolf_path, x, 0, 60, 100);   // (a Wolf of level 60 outlives every arrow of the checks)
        g.rig.know({combo});
        g.rig.asks = false;   // (the client's requests are played by hand where a check wants one)
        for (const content::AbilityHitRow& h : def->hits) {
            if (h.source == "projectile") arrow_hits.insert(h.hit_guid);
        }
    }
    bool ok() const { return g.ok() && def && wolf != 0; }
    /// The client triggers a swing `gap` seconds after the last one and names this montage.
    Refusal swing(int anim, double gap) {
        t += gap;
        g.rig.run_to(t - 0.001);
        TriggerInfo info;
        info.animation_index = anim;
        info.target_count = 1;
        info.target_actors = {wolf};
        const Refusal r = g.rig.pipe->trigger(def->row.guid, id++, t, info);
        g.rig.take();
        return r;
    }
    void play(double after) { g.rig.run_to(t + after); }
    /// The hits that landed on the Wolf from this source (content::kSourceTypeProjectile: an arrow got there; kSourceTypeAbility: a request of the client was served): all the arrows' hits, or the hit of this name.
    size_t landed(int64_t source, const char* hit = nullptr) const {
        size_t n = 0;
        for (const Seen& s : g.rig.log) {
            for (const bootstrap::HitConfirm& c : s.confirmations) {
                if (c.targets.empty() || c.source_type != source) continue;
                if (hit) {
                    const abilities::HitDef* h = g.rig.catalog->hit(c.record_guid);
                    n += h && h->row.name == hit ? 1 : 0;
                } else {
                    n += arrow_hits.count(c.record_guid) ? 1 : 0;
                }
            }
        }
        return n;
    }
    /// The targeting record the ability's data gives a direct hit (the call's second argument).
    int64_t targeting_of(const abilities::HitDef& hit) const {
        int64_t targeting = content::kTargetingDefaultSelf;
        for (const content::AbilityHitRow& h : def->hits) {
            if (h.hit_guid != hit.row.guid || h.source != "hit") continue;
            for (const content::TargetingRow& tg : def->targetings) {
                if (tg.key == h.key) targeting = tg.guid;
            }
        }
        return targeting;
    }
    /// The client asks for a hit of the combo and names the Wolf, `after` seconds after the last trigger (`own`: the request names the caster's own component too).
    AbilityPipeline::HitVerdict ask(const abilities::HitDef& hit, double after, size_t own = 0) {
        g.rig.run_to(t + after);
        const auto v = g.rig.pipe->hit_request(def->row.guid, targeting_of(hit), hit.row.guid, 1, own, t + after, {wolf});
        g.rig.take();
        return v;
    }
    /// The Wolf stands `x` cm ahead of the player.
    void put_wolf(double x) {
        const world::WorldActor* a = g.w.registry.find(wolf);
        if (a) g.w.registry.move(wolf, x, 0, a->z, a->yaw_degrees);
    }
};

/// The arrows each swing of a Shortbow sent, for the montages named in order.
std::vector<size_t> shortbow_arrows(Fixture& f, const std::vector<int>& anims, bool* accepted) {
    std::vector<size_t> out;
    Swings s(f, "WeaponCombo_Ranged_Shortbow");
    CHECKF(s.ok(), "the world for the Shortbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return out;
    size_t before = 0;
    for (const int anim : anims) {
        if (s.swing(anim, 3.0) != Refusal::None) *accepted = false;
        s.play(2.5);
        const size_t now = s.landed(content::kSourceTypeProjectile);
        out.push_back(now - before);
        before = now;
    }
    return out;
}

void test_forged_swing_step(Fixture& f) {
    std::printf("a ranged weapon swing plays the next step of its combo or starts over, whatever montage the client names\n");
    bool accepted = true;
    // the whole combo in order (the fifth swing may be the finisher or its proc, the sixth is the extended finisher): one arrow, two, one, three, three, five
    const std::vector<size_t> honest = shortbow_arrows(f, {0, 1, 2, 3, 5, 7}, &accepted);
    CHECK(honest == std::vector<size_t>({1, 2, 1, 3, 3, 5}), "the Shortbow's combo in order sends 1, 2, 1, 3, 3 and 5 arrows");
    // starting over at any point
    const std::vector<size_t> restarted = shortbow_arrows(f, {0, 1, 2, 0, 1}, &accepted);
    CHECK(restarted == std::vector<size_t>({1, 2, 1, 1, 2}), "...and a swing that starts the combo over (montage 0) is taken wherever the combo was: 1, 2, 1, 1 and 2 arrows");
    // a modified client that names the five-arrow montage on every swing
    const std::vector<size_t> forged = shortbow_arrows(f, {0, 7, 7, 7, 7, 7}, &accepted);
    CHECK(accepted, "(every swing is accepted)");
    size_t total = 0, most_early = 0;
    for (size_t i = 0; i < forged.size(); ++i) {
        total += forged[i];
        if (i + 1 < forged.size()) most_early = std::max(most_early, forged[i]);
    }
    std::string arrows;
    for (const size_t n : forged) arrows += (arrows.empty() ? "" : ", ") + std::to_string(n);
    CHECKF(total == 15 && most_early <= 3, "naming the five-arrow montage on every swing sends the arrows of the combo's own steps, 15 in six swings, none of the first five more than three (sent: %s)", arrows.c_str());
}
AOC_REGISTER_TEST(test_forged_swing_step);

// ---- a swing the client ends ------------------------------------------------------------------------------------------------------------------------
// A stunned client ends the swing it plays (ServerEndAbility). The arrows that have left land; the ones the montage had still to send never leave.
void test_forged_swing_ended(Fixture& f) {
    std::printf("a swing the client ends sends no more arrows\n");
    const auto volley = [&](bool stun) {
        size_t arrows = 0;
        Swings s(f, "WeaponCombo_Ranged_Shortbow");
        CHECKF(s.ok(), "the world for the Shortbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
        if (!s.ok()) return arrows;
        for (const int anim : {0, 1, 2}) {   // (the combo up to its three-arrow swing)
            s.swing(anim, 3.0);
            s.play(2.5);
        }
        const size_t before = s.landed(content::kSourceTypeProjectile);
        CHECK(s.swing(3, 3.0) == Refusal::None, "the three-arrow swing is accepted");
        s.play(0.30);   // (the item has shown by now, 0.258 s; the first arrow has left, the second and third have not)
        const bootstrap::ActiveAbilityItem* item = s.g.rig.pawn.ability_item();
        CHECK(item != nullptr && item->ability_guid == s.def->row.guid, "the swing's item is the pawn's");
        if (stun && item) CHECK(s.g.rig.pipe->end_request(static_cast<int32_t>(item->server_instance_id), s.t + 0.30), "the client ends the swing's instance");
        s.play(3.0);
        arrows = s.landed(content::kSourceTypeProjectile) - before;
        CHECK(s.g.rig.pawn.broken.empty(), "(the pawn took every change)");
        return arrows;
    };
    const size_t free_swing = volley(false);
    CHECKF(free_swing == 3, "a swing nobody ends sends its three arrows (%zu landed)", free_swing);
    const size_t stunned = volley(true);
    CHECKF(stunned == 1, "a swing the client ends 0.30 s in sends only the arrow that had left (%zu landed)", stunned);
}
AOC_REGISTER_TEST(test_forged_swing_ended);

// ---- a swing the next swing's item replaces ---------------------------------------------------------------------------------------------------------
// The next swing's item takes the pawn's place: the montage of the swing before it is gone, and an arrow that had still to leave never does. The volley's arrows have always been held to that;
// the swings that send one arrow were not.
void test_swing_replaced_by_next(Fixture& f) {
    std::printf("a swing's single arrow does not leave once the next swing's item has taken its place\n");
    Swings s(f, "WeaponCombo_Ranged_Longbow");
    CHECKF(s.ok(), "the world for the Longbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return;
    // the first swing's arrow leaves 0.6306 s into its montage; the second swing comes 0.45 s after it and shows its item 88 ms later
    CHECK(s.swing(0, 3.0) == Refusal::None && s.swing(1, 0.45) == Refusal::None, "two swings 0.45 s apart are accepted");
    s.play(3.0);
    const size_t first = s.landed(content::kSourceTypeProjectile, "WeaponCombo_Longbow_01");
    const size_t second = s.landed(content::kSourceTypeProjectile, "WeaponCombo_Longbow_02");
    CHECKF(first == 0, "the first swing's arrow never leaves: the second swing's item came before its montage sent it (%zu landed)", first);
    CHECKF(second == 1, "...and the second swing's arrow lands (%zu landed)", second);
}
AOC_REGISTER_TEST(test_swing_replaced_by_next);

// ---- an arrow aimed at a target that is far when it leaves --------------------------------------------------------------------------------------------
// The flight of a shot is the distance over the projectile's speed, at most kMaxShotFlight: a target that is pulled far away by the time the arrow leaves does not hold the cast for as long as the arrow
// would take.
void test_swing_far_arrow_flight(Fixture& f) {
    std::printf("an arrow aimed at a target that ran far off is in the air for no more than the longest flight\n");
    Swings s(f, "WeaponCombo_Ranged_Shortbow");
    CHECKF(s.ok(), "the world for the Shortbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
    if (!s.ok()) return;
    for (const int anim : {0, 1, 2}) {
        s.swing(anim, 3.0);
        s.play(2.5);
    }
    const size_t before = s.landed(content::kSourceTypeProjectile);
    CHECK(s.swing(3, 3.0) == Refusal::None, "the three-arrow swing is accepted with the Wolf 8 m ahead");
    s.g.rig.run_to(s.t + 0.05);
    s.put_wolf(90000.0);   // (900 m away by the time the arrows leave: a flight of more than six seconds at the fastest arrow)
    s.g.rig.run_to(s.t + 2.0);
    s.put_wolf(800.0);     // (back in reach long before a flight of that length is over, long after the longest one is)
    s.play(4.5);
    const size_t soon = s.landed(content::kSourceTypeProjectile) - before;
    CHECKF(soon == 3, "all three arrows have landed 4.5 s after the trigger: the longest flight is %.0f s (%zu landed)", AbilityPipeline::kMaxShotFlight, soon);
    s.play(25.0);
    const size_t late = s.landed(content::kSourceTypeProjectile) - before;
    CHECKF(late == 3, "(nothing lands twice, and nothing is lost: %zu landed at 25 s)", late);
}
AOC_REGISTER_TEST(test_swing_far_arrow_flight);

// ---- the hits a client asks for during a swing ----------------------------------------------------------------------------------------------------------
// The server lands a ranged swing's projectile hits when the projectile gets to the target; the retail client asks for none of them. The hits the client does ask for are the Wand's beam steps': the
// beam's ticks and its end, and the step's own hit. A request for any other hit of the combo finds no slot, and each hit of a beam step takes the requests an honest swing sends and a few more.

/// One request of a swing: the hit's name and the seconds after the swing's trigger.
struct BeamAsk {
    const char* hit;
    double      after;
};

/// The Wand's swings in order up to the beam step named (montage 4 is the first beam step, 6 the second); the beam step is triggered and returned accepted or not.
Refusal wand_beam_swing(Swings& s, int beam) {
    for (const int anim : {0, 1, 2, 3, 4}) {
        if (anim == beam) break;
        s.swing(anim, 3.0);
        s.play(2.5);
    }
    return s.swing(beam, 3.0);
}

/// Plays the requests in the order given (their times grow) and counts, by hit name, the ones the swing took. A request the swing took is Scheduled, or Empty where the hit has no enemy to reach.
std::map<std::string, size_t> beam_taken(Fixture& f, Swings& s, const std::vector<BeamAsk>& asks) {
    std::map<std::string, size_t> taken;
    for (const BeamAsk& a : asks) {
        const abilities::HitDef* h = f.catalog->hit_named(a.hit);
        CHECKF(h != nullptr, "%s is in the data", a.hit);
        if (!h) continue;
        const auto v = s.ask(*h, a.after, 1);
        if (v == AbilityPipeline::HitVerdict::Scheduled || v == AbilityPipeline::HitVerdict::Empty) ++taken[a.hit];
    }
    return taken;
}

void test_forged_swing_requests(Fixture& f) {
    std::printf("the hits a ranged weapon swing's arrows carry take no request from the client\n");
    {   // the Shortbow's first swing: the client names the hit its arrow carries
        Swings s(f, "WeaponCombo_Ranged_Shortbow");
        CHECKF(s.ok(), "the world for the Shortbow swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
        if (!s.ok()) return;
        const abilities::HitDef* carried = f.catalog->hit_named("WeaponCombo_Shortbow_Hit1");
        CHECK(carried != nullptr, "the hit the first arrow carries is in the data");
        if (!carried) return;
        CHECK(s.swing(0, 1.0) == Refusal::None, "the first swing is accepted");
        const auto verdict = s.ask(*carried, 0.5);
        s.play(3.0);
        CHECKF(verdict != AbilityPipeline::HitVerdict::Scheduled, "a request for the hit its arrow carries is not served (%s)", AbilityPipeline::verdict_name(verdict));
        CHECKF(s.landed(content::kSourceTypeAbility, "WeaponCombo_Shortbow_Hit1") == 0, "...and lands nothing on the Wolf (%zu landed)", s.landed(content::kSourceTypeAbility, "WeaponCombo_Shortbow_Hit1"));
        CHECKF(s.landed(content::kSourceTypeProjectile, "WeaponCombo_Shortbow_Hit1") == 1, "the arrow itself landed once (%zu)", s.landed(content::kSourceTypeProjectile, "WeaponCombo_Shortbow_Hit1"));
    }
    {   // the Wand: its beam steps ask for the beam's hits, as the retail client does; the hits of its other steps find no slot even then
        Swings s(f, "WeaponCombo_Ranged_Wand");
        CHECKF(s.ok(), "the world for the Wand swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
        if (!s.ok()) return;
        const abilities::HitDef* beam_end = f.catalog->hit_named("Weapon_Wand_Beam_ComboEnd_Hit");
        const abilities::HitDef* step_one = f.catalog->hit_named("Weapon_Wand_Hit_1");
        const abilities::HitDef* step_six = f.catalog->hit_named("Weapon_Wand_Hit_6+");
        CHECK(beam_end && step_one && step_six, "the beam's end, the first step's hit and the extended finisher's hit are in the data");
        if (!beam_end || !step_one || !step_six) return;
        for (const int anim : {0, 1, 2, 3}) {   // (the combo up to its beam)
            s.swing(anim, 3.0);
            s.play(2.5);
        }
        CHECK(s.swing(4, 3.0) == Refusal::None, "the beam step is accepted");
        const auto other = s.ask(*step_one, 0.4);
        const auto end = s.ask(*beam_end, 0.8);
        s.play(1.6);
        CHECKF(end == AbilityPipeline::HitVerdict::Scheduled, "the request for the beam's end is served (%s)", AbilityPipeline::verdict_name(end));
        CHECKF(s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit") == 1, "...and the beam's end lands on the Wolf once (%zu)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit"));
        CHECKF(other != AbilityPipeline::HitVerdict::Scheduled && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_1") == 0,
               "a request for the first step's hit during the beam is not served and lands nothing (%s, %zu landed)", AbilityPipeline::verdict_name(other), s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_1"));
        CHECK(s.swing(6, 3.0) == Refusal::None, "the extended finisher's beam step is accepted");
        const auto six = s.ask(*step_six, 0.8);
        s.play(1.6);
        CHECKF(six == AbilityPipeline::HitVerdict::Scheduled, "the request for the extended finisher's own hit is served (%s)", AbilityPipeline::verdict_name(six));
        CHECKF(s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+") == 1, "...and lands on the Wolf once (%zu)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+"));
        CHECK(s.g.rig.pawn.broken.empty(), "(the pawn took every change)");
    }
    {   // the first beam step (montage 4): every request of an honest swing lands; the hit no honest client asks for and the requests over a hit's share land nothing
        Swings s(f, "WeaponCombo_Ranged_Wand");
        CHECKF(s.ok(), "the world for the Wand swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
        if (!s.ok()) return;
        CHECK(wand_beam_swing(s, 4) == Refusal::None, "the first beam step is accepted");
        // the requests of a recorded swing: the step's hit three times, the four beam effects' audio hits and the beam's end once each
        const std::map<std::string, size_t> honest = beam_taken(f, s, {{"Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY", 0.486}, {"Weapon_Wand_Hit_5+", 0.486}, {"Weapon_Wand_BeamFX_Hit_Lightning_AUDIOONLY", 0.499},
                                                                       {"Weapon_Wand_BeamFX_Hit_Fire_AUDIOONLY", 0.499}, {"Weapon_Wand_BeamFX_Hit_Ice_AUDIOONLY", 0.499}, {"Weapon_Wand_Hit_5+", 0.621},
                                                                       {"Weapon_Wand_Hit_5+", 0.728}, {"Weapon_Wand_Beam_ComboEnd_Hit", 0.808}});
        const auto taken = [](const std::map<std::string, size_t>& m, const char* hit) { const auto it = m.find(hit); return it == m.end() ? size_t{0} : it->second; };
        CHECKF(taken(honest, "Weapon_Wand_Hit_5+") == 3 && taken(honest, "Weapon_Wand_Beam_ComboEnd_Hit") == 1 && taken(honest, "Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY") == 1 &&
                   taken(honest, "Weapon_Wand_BeamFX_Hit_Lightning_AUDIOONLY") == 1 && taken(honest, "Weapon_Wand_BeamFX_Hit_Fire_AUDIOONLY") == 1 && taken(honest, "Weapon_Wand_BeamFX_Hit_Ice_AUDIOONLY") == 1,
               "an honest first beam step's requests are all taken: the step's hit 3 of 3, the end 1 of 1, each beam effect's audio hit 1 of 1 (%zu, %zu, %zu, %zu, %zu, %zu)", taken(honest, "Weapon_Wand_Hit_5+"),
               taken(honest, "Weapon_Wand_Beam_ComboEnd_Hit"), taken(honest, "Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY"), taken(honest, "Weapon_Wand_BeamFX_Hit_Lightning_AUDIOONLY"),
               taken(honest, "Weapon_Wand_BeamFX_Hit_Fire_AUDIOONLY"), taken(honest, "Weapon_Wand_BeamFX_Hit_Ice_AUDIOONLY"));
        s.play(1.0);
        CHECKF(s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_5+") == 3 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit") == 1,
               "...and they land on the Wolf: the step's hit 3 times, the end once (%zu, %zu)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_5+"), s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit"));
        // a modified client: the two procs' hits (no honest swing asks for them), and the step's hit, the end and a beam effect's audio hit over the share of an honest swing
        const std::map<std::string, size_t> forged = beam_taken(f, s, {{"Weapon_Wand_ProcHit", 1.05}, {"Weapon_Wand_ProcHit_2", 1.10}, {"Weapon_Wand_Hit_5+", 1.15}, {"Weapon_Wand_Hit_5+", 1.20},
                                                                       {"Weapon_Wand_Hit_5+", 1.25}, {"Weapon_Wand_Beam_ComboEnd_Hit", 1.30}, {"Weapon_Wand_Beam_ComboEnd_Hit", 1.35},
                                                                       {"Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY", 1.40}, {"Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY", 1.45}});
        s.play(3.0);
        CHECKF(taken(forged, "Weapon_Wand_ProcHit") == 0 && taken(forged, "Weapon_Wand_ProcHit_2") == 0 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit") == 0 &&
                   s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit_2") == 0,
               "a request for a proc's hit is not taken and lands nothing (%zu and %zu taken, %zu and %zu landed)", taken(forged, "Weapon_Wand_ProcHit"), taken(forged, "Weapon_Wand_ProcHit_2"),
               s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit"), s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit_2"));
        CHECKF(taken(forged, "Weapon_Wand_Hit_5+") == 1 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_5+") == 4,
               "the step's hit is taken 4 times in all, one more than an honest swing asks for: the 5th and 6th request land nothing (%zu more taken, %zu landed)", taken(forged, "Weapon_Wand_Hit_5+"),
               s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_5+"));
        CHECKF(taken(forged, "Weapon_Wand_Beam_ComboEnd_Hit") == 1 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit") == 2,
               "the beam's end is taken twice in all, and a third request lands nothing (%zu more taken, %zu landed)", taken(forged, "Weapon_Wand_Beam_ComboEnd_Hit"),
               s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd_Hit"));
        CHECKF(taken(forged, "Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY") == 1, "a beam effect's audio hit is taken twice in all (%zu more taken)", taken(forged, "Weapon_Wand_BeamFX_Hit_Arcane_AUDIOONLY"));
        CHECK(s.g.rig.pawn.broken.empty(), "(the pawn took every change)");
    }
    {   // the second beam step (montage 6) asks for its hit up to 12 times, 68 ms apart at the closest
        Swings s(f, "WeaponCombo_Ranged_Wand");
        CHECKF(s.ok(), "the world for the Wand swings (%s%s)", s.g.rig.problem.c_str(), s.g.w.problem.c_str());
        if (!s.ok()) return;
        CHECK(wand_beam_swing(s, 6) == Refusal::None, "the second beam step is accepted");
        std::vector<BeamAsk> asks = {{"Weapon_Wand_BeamFX_Hit_BeamAmp_AUDIOONLY", 0.122}};
        for (const double at : {0.122, 0.235, 0.330, 0.435, 0.527, 0.633, 0.726, 0.830, 0.917, 1.031, 1.115, 1.210}) asks.push_back({"Weapon_Wand_Hit_6+", at});
        asks.push_back({"Weapon_Wand_Beam_ComboEnd2_Hit", 1.210});
        const std::map<std::string, size_t> honest = beam_taken(f, s, asks);
        const auto taken = [](const std::map<std::string, size_t>& m, const char* hit) { const auto it = m.find(hit); return it == m.end() ? size_t{0} : it->second; };
        CHECKF(taken(honest, "Weapon_Wand_Hit_6+") == 12 && taken(honest, "Weapon_Wand_Beam_ComboEnd2_Hit") == 1 && taken(honest, "Weapon_Wand_BeamFX_Hit_BeamAmp_AUDIOONLY") == 1,
               "an honest second beam step's requests are all taken: the step's hit 12 of 12, the end and the audio hit 1 of 1 (%zu, %zu, %zu)", taken(honest, "Weapon_Wand_Hit_6+"),
               taken(honest, "Weapon_Wand_Beam_ComboEnd2_Hit"), taken(honest, "Weapon_Wand_BeamFX_Hit_BeamAmp_AUDIOONLY"));
        s.play(1.4);
        CHECKF(s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+") == 12, "...and the step's hit lands on the Wolf 12 times (%zu)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+"));
        const std::map<std::string, size_t> forged = beam_taken(f, s, {{"Weapon_Wand_Hit_6+", 1.45}, {"Weapon_Wand_Hit_6+", 1.55}, {"Weapon_Wand_Hit_6+", 1.65}, {"Weapon_Wand_Hit_6+", 1.75},
                                                                       {"Weapon_Wand_ProcHit", 1.85}, {"Weapon_Wand_ProcHit_2", 1.95}, {"Weapon_Wand_Beam_ComboEnd2_Hit", 2.05}, {"Weapon_Wand_Beam_ComboEnd2_Hit", 2.15}});
        s.play(4.0);
        CHECKF(taken(forged, "Weapon_Wand_Hit_6+") == 2 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+") == 14,
               "the step's hit is taken 14 times in all, two more than an honest swing asks for: the 15th and 16th request land nothing (%zu more taken, %zu landed)", taken(forged, "Weapon_Wand_Hit_6+"),
               s.landed(content::kSourceTypeAbility, "Weapon_Wand_Hit_6+"));
        CHECKF(taken(forged, "Weapon_Wand_ProcHit") == 0 && taken(forged, "Weapon_Wand_ProcHit_2") == 0 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit") == 0 &&
                   s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit_2") == 0,
               "...and a request for a proc's hit lands nothing (%zu and %zu landed)", s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit"), s.landed(content::kSourceTypeAbility, "Weapon_Wand_ProcHit_2"));
        CHECKF(taken(forged, "Weapon_Wand_Beam_ComboEnd2_Hit") == 1 && s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd2_Hit") == 2,
               "the second beam's end is taken twice in all, and a third request lands nothing (%zu more taken, %zu landed)", taken(forged, "Weapon_Wand_Beam_ComboEnd2_Hit"),
               s.landed(content::kSourceTypeAbility, "Weapon_Wand_Beam_ComboEnd2_Hit"));
        CHECK(s.g.rig.pawn.broken.empty(), "(the pawn took every change)");
    }
}
AOC_REGISTER_TEST(test_forged_swing_requests);

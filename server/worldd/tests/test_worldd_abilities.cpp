// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_abilities.cpp: the ability runtime and its replication, worldd plan A7
//
//  content.db's ability rules   ContentDb::ability_rules reads the numbers contentc resolved: Sprint 1.25 every 0.25 s
//                               while moving, Dodge 50 once with a 0.65 s cooldown, Rest a free toggle; the Evasion
//                               effect, the 1 s regeneration pause; the refusals (a file of another shape, a Sprint
//                               that is not held, an interval of a few microseconds ...).  With AOC_CONTENT_DB set, the
//                               real file's rules are the same
//  the runtime (a simulated    Sprint: the first charge with the activation, a full bar lasts 20 s (80 charges) and ends
//  clock)                       with ClientEndAbility; MoveSpeedMult 1.2 while it runs; it charges only while the player
//                               moves, a cancel stops it without ClientEndAbility, a Dodge does not end it, a Rest ends it
//                               with one; regeneration is off while sprinting and moving; Stamina comes back at 5 a second,
//                               after a pause of 1 s (the DisableRegen effect item); Dodge: -50, the cooldown, the montage and
//                               MeshYaw of the trigger, a refusal under 50 and in the cooldown (to the tick), regeneration off
//                               for 1.5 s; Rest: its effect after the sit-down, the regeneration it raises, the end; the
//                               ServerInstanceId counter; anything else is answered with ClientEndAbility; one jump of the
//                               clock gives the events of many steps, and the clock never goes back
//  Rest on the wire             PlayerBootstrap::apply_ability_change + ability_update: the six packets of the original
//                               server's General_Rest (pkt#8175-8320) rebuilt, each bit for bit (the montage export
//                               included); the effect clock; ClientEndAbility; what apply_ability_change refuses
//  Sprint and Dodge on the wire the retail server's replies (capture 745: a Bard's Sprint and Dodge) against the items the
//                               runtime sends, field by field; the Cooldowns item; a Sprint out of Stamina
// ============================================================================
#include "bootstrap/bootstrap.h"
#include "bootstrap/captured_setup.h"
#include "capture_blocks.h"
#include "capture_bootstrap.h"
#include "content/content_db.h"
#include "content_db_fixture.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"
#include "protocol/server_guid_allocator.h"
#include "session/abilities.h"
#include "session/player_world.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace bs = aoc::worldd::bootstrap;
namespace content = aoc::worldd::content;
namespace fs = std::filesystem;
using namespace aoc::worldd;
using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
// -- C.Y.B . C-O-A --

// The records the runtime reads (services/internal/content/known.go and the capture's StatsComponent).
static constexpr int64_t kHealthRegen = 109343399171806ll;
static constexpr int64_t kManaRegen = 109343399171796ll;
static constexpr int64_t kMaxHealth = 108986356618873ll;
static constexpr int64_t kMaxMana = 109183578756729ll;
static constexpr int64_t kRestEffect = 109394092054139ll;
static constexpr int64_t kEvasionEffect = 106223481149050ll;
static constexpr int64_t kSprintEffect = 6064631641826394197ll;
static constexpr int64_t kDisableRegenEffect = 6064631668308443138ll;
static constexpr int64_t kMoveSpeedMult = 109228544644729ll;   // StatRepInt32EveryoneProxy item 8
static constexpr int64_t kScepterCombo = 6064631845843960261ll;   // a combat ability worldd does not run
static constexpr int64_t kGuard = 107887054050936ll;

static const float k06 = 0.6f;   // HealthRegeneration and ManaRegeneration at level 1 (0x3f19999a)

// ============================================================================================================================
//  the rules
// ============================================================================================================================

struct Rules {
    fs::path dir;
    std::optional<content::ContentDb> db;
    content::AbilityRules rules;

    Rules() {
        dir = content_fixture::make_temp_dir("worldd_abilities_");
        if (!content_fixture::make_db(dir / "content.db")) std::exit(2);
        std::string error;
        db = content::ContentDb::open((dir / "content.db").string(), error);
        if (!db) {
            std::printf("  [FAIL] the fixture content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        const auto r = db->ability_rules(error);
        if (!r) {
            std::printf("  [FAIL] the fixture's ability rules do not read: %s\n", error.c_str());
            std::exit(2);
        }
        rules = *r;
    }
    ~Rules() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

static std::map<int64_t, double> level1_stats() {
    return {{content::kGuidStamina, 100},       {content::kGuidMaxStamina, 100}, {kHealthRegen, k06}, {kManaRegen, k06},
            {kMaxHealth, 90},                   {kMaxMana, 90},                  {kMoveSpeedMult, 1.0}};
}

static std::vector<int32_t> ints(std::initializer_list<int32_t> l) { return l; }
static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static void test_rules(const Rules& r) {
    std::printf("content.db's ability rules\n");
    const content::AbilityRules& a = r.rules;
    CHECK(a.sprint.guid == content::kGuidSprint && a.sprint.held == content::Held::Smart && a.sprint.min_stamina == 1.0 &&
              a.sprint.cost_amount == 1.25 && a.sprint.charge_interval == 0.25 && a.sprint.cost_requires_moving && a.sprint.cooldown == 0,
          "Sprint: held, needs 1 Stamina, takes 1.25 every 0.25 s while moving (5 a second), no cooldown");
    CHECK(a.dodge.guid == content::kGuidDodge && a.dodge.held == content::Held::Naive && a.dodge.cost_amount == 50 &&
              a.dodge.charge_interval == 0 && a.dodge.cooldown == 0.65 && a.dodge.triggers_cooldown && a.dodge.disable_during_cooldown,
          "Dodge: 50 once, a 0.65 s cooldown the paid cost starts");
    CHECK(a.rest.guid == content::kGuidRest && a.rest.held == content::Held::Toggle && a.rest.requires_out_of_combat && a.rest.cost_stat_guid == 0 &&
              a.rest.targets_self && a.rest.cancel_section == "SitEnd",
          "Rest: a free toggle, out of combat only, on the caster, its end section SitEnd");
    const content::EffectRule* ev = a.effect(a.dodge.effect_guid);
    const content::EffectRule* ender = ev ? a.effect(ev->end_effect_guid) : nullptr;
    CHECK(ev && !ev->infinite && ev->duration == 0.5 && ev->stops_regen_standing && ev->stops_regen_moving && ender &&
              ev->end_effect_duration == 1.0 && ender->stops_regen_standing && ender->stops_regen_moving,
          "the Dodge's Evasion effect lasts 0.5 s and stops regeneration; its End hit pauses it for 1 s more");
    const content::EffectRule* sp = a.effect(a.sprint.effect_guid);
    CHECK(sp && sp->infinite && !sp->stops_regen_standing && sp->stops_regen_moving && sp->end_effect_duration == 1.0,
          "the Sprint's effect is infinite, stops regeneration only while moving, and pauses it for 1 s when it ends");
    CHECK(sp && sp->tick_timer == 1.0 && sp->stat_mods.size() == 1 && sp->stat_mods[0].stat_guid == kMoveSpeedMult && sp->stat_mods[0].source_stat_guid == 0 &&
              sp->stat_mods[0].addend == 0.2 && sp->stat_mods[0].factor == 0,
          "and it ticks every 1 s (retail's TickTime) and adds 0.2 to MoveSpeedMult: 1.2, 720 cm/s against 600 running");
    const content::EffectRule* rest = a.effect(a.rest.effect_guid);
    CHECK(rest && rest->infinite && rest->stat_mods.size() == 2 && rest->stat_mods[0].stat_guid == kHealthRegen &&
              rest->stat_mods[0].source_stat_guid == kMaxHealth && rest->stat_mods[0].factor == 0.01 && rest->stat_mods[0].addend == 1.0,
          "the Rest effect adds 0.01 x MaxHealth + 1 to HealthRegeneration, and the same with Mana");
    CHECK(a.stat_regen_time == 1.0 && a.regen_rate == 5.0, "Stamina regenerates 5 every 1 s");
    CHECK(a.ability(content::kGuidSprint) == &a.sprint && a.ability(content::kGuidDodge) == &a.dodge && a.ability(content::kGuidRest) == &a.rest &&
              a.ability(kScepterCombo) == nullptr && a.ability(0) == nullptr,
          "only Sprint, Dodge and Rest are abilities worldd runs");
// built with C Y B and C O A

    // a content.db whose rows this worldd cannot run is refused with a message that says how to rebuild it
    const auto altered = [&](const char* name, const char* from, const char* to) {
        const fs::path path = r.dir / (std::string(name) + ".db");
        content_fixture::DbOptions o;
        o.ability_from = from;
        o.ability_to = to;
        if (!content_fixture::make_db(path, o)) std::exit(2);
        std::string error;
        auto opened = content::ContentDb::open(path.string(), error);
        if (!opened) return std::string("open: ") + error;
        const auto rules = opened->ability_rules(error);
        return rules ? std::string() : error;
    };
    {
        const std::string e = altered("unchanged", "'Smart'", "'Smart'");
        CHECK(e.empty(), "(a file rebuilt from the same rows reads)");
        const std::string held = altered("notheld", "'Smart'", "'Naive'");
        CHECK(contains(held, "Sprint") && contains(held, "contentc"), "a Sprint that is not held is refused, naming the Sprint and how to rebuild the file");
        const std::string unknown = altered("unknown", "'Smart'", "'Sticky'");
        CHECK(contains(unknown, "General_Sprint") && contains(unknown, "Sticky"), "an unknown held behaviour is refused by name");
        const std::string nocost = altered("nocost", "1.25, 1, 0.25", "0.0, 1, 0.25");
        CHECK(contains(nocost, "Sprint"), "a Sprint that costs nothing is refused");
        const std::string nocd = altered("nocd", "50.0, 0, 0.0, 0.65, 1, 1", "50.0, 0, 0.0, 0.0, 1, 1");
        CHECK(contains(nocd, "Dodge"), "a Dodge with no cooldown is refused");
        // an interval of a few microseconds would run millions of timers in one tick of the runtime's clock
        const std::string fast = altered("fastcharge", "1.25, 1, 0.25", "1.25, 1, 0.001");
        CHECK(contains(fast, "Sprint") && contains(fast, "interval") && contains(fast, "contentc"), "a Sprint that charges every millisecond is refused");
        const std::string edge = altered("edgecharge", "1.25, 1, 0.25", "1.25, 1, 0.01");
        CHECK(edge.empty(), "(0.01 s is the shortest charge interval accepted)");
        content_fixture::DbOptions slow_regen;
        slow_regen.stat_regen_time = "0.001";
        const fs::path regen_path = r.dir / "fastregen.db";
        if (!content_fixture::make_db(regen_path, slow_regen)) std::exit(2);
        std::string regen_error;
        auto regen_db = content::ContentDb::open(regen_path.string(), regen_error);
        const auto regen_rules = regen_db ? regen_db->ability_rules(regen_error) : std::nullopt;
        CHECK(regen_db && !regen_rules && contains(regen_error, "regeneration") && contains(regen_error, "interval"),
              "so is a Stamina regeneration tick of a millisecond");
    }
    {
        // the world is not ready with rules it cannot run, and says why; with good ones it hands them to the sessions
        const fs::path path = r.dir / "world_notheld.db";
        content_fixture::DbOptions o;
        o.ability_from = "'Smart'";
        o.ability_to = "'Naive'";
        if (!content_fixture::make_db(path, o)) std::exit(2);
        std::string error;
        auto bad = content::ContentDb::open(path.string(), error);
        aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
        if (bad) {
            PlayerWorld refused(&*bad, "", bs::GameStateMode::Tier2, 60, guids);
            CHECK(!refused.ready() && refused.ability_rules() == nullptr && contains(refused.content_problem(), "Sprint"),
                  "a PlayerWorld given such a content.db is not ready (no login) and its problem names the Sprint");
        }
        PlayerWorld ready(&*r.db, "", bs::GameStateMode::Tier2, 60, guids);
        CHECK(ready.ready() && ready.ability_rules() && ready.ability_rules()->sprint.cost_amount == 1.25, "with the right rows it is ready and holds the rules for its sessions");
    }
    {
        content_fixture::DbOptions o;
        o.ability_tables = false;
        const fs::path path = r.dir / "old.db";
        if (!content_fixture::make_db(path, o)) std::exit(2);
        std::string error;
        const auto opened = content::ContentDb::open(path.string(), error);
        CHECK(!opened && contains(error, "contentc") && contains(error, "ability_runtime"),
              "a content.db from before plan A7 (no ability tables) fails at open and says to rebuild it with this checkout's contentc");
    }
}

// ============================================================================================================================
//  the runtime, on a simulated clock
// ============================================================================================================================

struct Sim {
    AbilityRuntime rt;
    std::vector<AbilityEvent> log;   // every event so far
    double now = 0;
    bool moving = false;
    int64_t us = 0;                  // the clock in microseconds: every step lands exactly on the decimal it names

    explicit Sim(const content::AbilityRules& rules, std::map<int64_t, double> stats = level1_stats(), double world_time = 1000)
        : rt(rules, std::move(stats), world_time) {}

    /// Runs the clock to `t` in steps of `step` seconds; while `moving` the client reports a move at every step.
    void run_to(double t, double step = 0.05) {
        const int64_t end = std::llround(t * 1e6), by = std::llround(step * 1e6);
        while (us + by <= end) {
            us += by;
            now = static_cast<double>(us) / 1e6;
            if (moving) rt.set_moving(true, now);
            rt.tick(now);
            take();
        }
    }
    void take() {
        for (AbilityEvent& e : rt.take_events()) log.push_back(std::move(e));
    }
    Refusal trigger(int64_t guid, int32_t id, const TriggerInfo& info = {}) {
        const Refusal r = rt.trigger(guid, id, now, info);
        take();
        return r;
    }
    bool cancel(int32_t id) {
        const bool b = rt.cancel(id, now);
        take();
        return b;
    }
    void move(bool m) {
        moving = m;
        rt.set_moving(m, now);
        take();
    }
    /// The events whose note contains `part`.
    std::vector<const AbilityEvent*> with(const char* part) const {
        std::vector<const AbilityEvent*> out;
        for (const AbilityEvent& e : log)
            if (contains(e.note, part)) out.push_back(&e);
        return out;
    }
    std::vector<int32_t> ends() const {
        std::vector<int32_t> out;
        for (const AbilityEvent& e : log)
            if (e.end_ability) out.push_back(*e.end_ability);
        return out;
    }
    /// The values the events set for `record`, in order.
    std::vector<float> series_of(int64_t record) const {
        std::vector<float> out;
        for (const AbilityEvent& e : log)
            for (const bs::StatChange& s : e.change.stats)
                if (s.record == record) out.push_back(s.value);
        return out;
    }
    /// The Stamina values the events set, in order.
    std::vector<float> stamina_series() const {
        std::vector<float> out;
        for (const AbilityEvent& e : log)
            for (const bs::StatChange& s : e.change.stats)
                if (s.record == content::kGuidStamina) out.push_back(s.value);
        return out;
    }
};
// -- C-Y-B & C | O | A --

/// The value `change` sets `record` to, if it sets it.
static std::optional<float> stat_of(const bs::AbilityChange& change, int64_t record) {
    for (const bs::StatChange& s : change.stats)
        if (s.record == record) return s.value;
    return std::nullopt;
}

static bool near_d(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static void test_sprint(const Rules& r) {
    std::printf("Sprint\n");
    {
        Sim s(r.rules);
        s.move(true);
        CHECK(s.trigger(content::kGuidSprint, 0) == Refusal::None && s.rt.sprinting(), "a Sprint starts with full Stamina");
        const auto starts = s.with("the Sprint starts");
        CHECK(starts.size() == 1 && starts[0]->change.abilities_added.size() == 1 && starts[0]->change.abilities_added[0].ability_guid == content::kGuidSprint &&
                  starts[0]->change.abilities_added[0].server_instance_id == 0 && starts[0]->change.abilities_added[0].montage_group == "Sprint" &&
                  starts[0]->change.abilities_added[0].montage_path == bs::kSprintMontagePath && starts[0]->change.effects_added.size() == 1 &&
                  starts[0]->change.effects_added[0].effect_guid == kSprintEffect && starts[0]->change.effects_added[0].duration > 1e38f &&
                  starts[0]->change.effects_added[0].tick_time == 1.0f,
              "it adds its ability item (instance 0, MontageGroup Sprint, the Sprint montage) and its effect: infinite, ticking every 1 s");
        CHECK(stat_of(starts[0]->change, kMoveSpeedMult) == 1.2f && s.rt.stat(kMoveSpeedMult) == 1.2,
              "in the same step MoveSpeedMult goes from 1.0 to 1.2 (the retail server replicates it with the Sprint: the client moves at 600 x it)");
        CHECK(stat_of(starts[0]->change, content::kGuidStamina) == 98.75f && s.rt.stamina() == 98.75,
              "and the first charge is taken with the activation: 98.75, in the same step");
        s.run_to(1.0);
        const auto series = s.stamina_series();
        CHECK(series.size() == 5 && series[0] == 98.75f && series[1] == 97.5f && series[2] == 96.25f && series[4] == 93.75f,
              "then every 0.25 s it takes 1.25 (98.75, 97.5 ... 93.75 after a second)");
        CHECK(s.rt.stamina() == 93.75, "5 Stamina a second");
        s.run_to(19.5);
        CHECK(s.rt.sprinting() && s.rt.stamina() == 1.25, "after 19.5 s it still runs, with 1.25 left (79 charges)");
        s.run_to(19.75);
        CHECK(!s.rt.sprinting() && s.rt.stamina() == 0.0 && s.stamina_series().size() == 80,
              "the 80th charge, at 19.75 s, leaves 0 and the Sprint is over: a full bar lasts 20 s from the first charge");
        const auto ends = s.with("out of Stamina");
        CHECK(ends.size() == 1 && !ends[0]->end_ability && ends[0]->at == 19.75 && ends[0]->change.abilities_removed == std::vector<int64_t>{0} &&
                  ends[0]->change.effects_removed.size() == 1 && stat_of(ends[0]->change, kMoveSpeedMult) == 1.0f,
              "that step removes the ability item and the effect and puts MoveSpeedMult back to 1.0 ...");
        CHECK(ends.size() == 1 && ends[0]->change.effects_added.size() == 1 && ends[0]->change.effects_added[0].effect_guid == kDisableRegenEffect &&
                  ends[0]->change.effects_added[0].duration == 1.0f && s.rt.regen_pause_until() == 20.75,
              "... and adds the 1 s DisableRegen effect (retail: all in one packet)");
        CHECK(s.ends().empty(), "ClientEndAbility has not gone yet");
        s.run_to(20.0);
        const auto told = s.with("the client is told");
        CHECK(told.size() == 1 && told[0]->end_ability == 0 && near_d(told[0]->at, 19.75 + AbilityRuntime::kEndAbilityDelay, 1e-9) && told[0]->change.empty() &&
                  s.ends() == std::vector<int32_t>{0},
              "ClientEndAbility(0) follows 163 ms later, after the update (retail capture 745), on its own");
        // a Sprint over empty
        CHECK(s.trigger(content::kGuidSprint, 1) == Refusal::Stamina && s.ends().back() == 1 && !s.rt.sprinting() && s.rt.refused() == 1,
              "a new Sprint with no Stamina is refused with ClientEndAbility(1)");
        CHECK(s.log.back().change.empty() && s.log.back().end_ability == 1, "(and nothing else changes: the refusal carries no state change)");
    }
    {
        // a Sprint with the player standing takes no first charge; one that starts with 1.25 Stamina ends at once, as retail's did
        Sim stand(r.rules);
        stand.trigger(content::kGuidSprint, 0);
        CHECK(stand.rt.stamina() == 100.0 && !stat_of(stand.log[0].change, content::kGuidStamina), "a Sprint started standing still takes no charge with the activation");
        Sim last(r.rules, [] { auto m = level1_stats(); m[content::kGuidStamina] = 1.25; return m; }());
        last.move(true);
        last.trigger(content::kGuidSprint, 6);
        CHECK(!last.rt.sprinting() && last.rt.stamina() == 0.0 && last.with("out of Stamina").size() == 1 && last.ends().empty(),
              "with 1.25 Stamina (enough to start) the activation's charge leaves 0 and the Sprint ends in the same instant");
        last.run_to(0.15);
        CHECK(last.ends().empty(), "its ClientEndAbility has not gone at 0.15 s ...");
        last.run_to(0.2);
        CHECK(last.ends() == std::vector<int32_t>{6}, "... and goes at 0.163 s, with the client's own id");
    }
    {
        // a cancel stops it, without ClientEndAbility (the client ended its own), and starts the regeneration pause
        Sim s(r.rules);
        s.move(true);
        s.trigger(content::kGuidSprint, 0);
        s.run_to(2.0);
        CHECK(s.rt.stamina() == 88.75, "(2 s of Sprint, nine charges with the first: 88.75)");
        CHECK(s.cancel(0) && !s.rt.sprinting(), "ServerNotifyHeldAbilityCancelled(0) releases the Sprint");
        const auto released = s.with("released");
        CHECK(released.size() == 1 && !released[0]->end_ability && released[0]->change.abilities_removed.size() == 1 && released[0]->change.effects_removed.size() == 1 &&
                  stat_of(released[0]->change, kMoveSpeedMult) == 1.0f,
              "its items are removed, MoveSpeedMult is back to 1.0 and no ClientEndAbility is sent");
        s.run_to(2.99);
        CHECK(s.rt.stamina() == 88.75, "the drain stopped, and regeneration waits out the 1 s pause");
        s.run_to(3.0);
        CHECK(s.rt.stamina() == 93.75, "the first regeneration tick after the pause (t = 3 s) gives 5");
        CHECK(!s.cancel(0) && !s.cancel(7), "a cancel of a finished or an unknown instance is ignored");
        const auto pause_end = s.with("regeneration pause ends");
        CHECK(pause_end.size() == 1 && pause_end[0]->at == 3.0 && pause_end[0]->change.effects_removed.size() == 1,
              "the DisableRegen effect is removed when the pause is over (retail: 1.0 s after it was added)");
    }
    {
        // it charges only while the player moves, and the client's last word on moving goes stale
        Sim s(r.rules);
        s.trigger(content::kGuidSprint, 0);   // standing
        s.run_to(2.0);
        CHECK(s.rt.sprinting() && s.rt.stamina() == 100.0 && s.with("charges").empty(), "a Sprint with the player standing charges nothing");
        s.move(true);
        s.run_to(3.0);
        CHECK(s.rt.stamina() == 95.0, "once the player moves it takes 5 a second");
        s.moving = false;   // no more reports: the last one, at 3 s, goes stale after kMovingStale
        s.run_to(6.0);
        double last_charge = 0;
        for (const AbilityEvent* e : s.with("charges")) last_charge = e->at;
        CHECK(last_charge == 3.75 && !s.rt.moving(6.0), "a player no move has been heard from for 0.75 s counts as standing: the last charge is at 3.75 s");
    }
    {
        // regeneration is off while sprinting and moving, and on while sprinting and standing
        Sim s(r.rules);
        s.move(true);
        s.trigger(content::kGuidSprint, 0);
        s.run_to(1.5);
        CHECK(s.rt.regeneration_blocked(1.5), "sprinting and moving: regeneration is blocked");
        s.moving = false;
        s.rt.set_moving(false, s.now);
        CHECK(!s.rt.regeneration_blocked(s.now), "sprinting and standing: it is not");
    }
    {
        // a bar that is not a multiple of the charge never goes below 0 (101 - 80 x 1.25 = 1: one more charge, and 0.25 short)
        Sim odd(r.rules, [] { auto m = level1_stats(); m[content::kGuidStamina] = 101; m[content::kGuidMaxStamina] = 101; return m; }());
        odd.move(true);
        odd.trigger(content::kGuidSprint, 0);
        odd.run_to(19.75);
        CHECK(odd.rt.sprinting() && odd.rt.stamina() == 1.0, "a bar of 101: after 80 charges 1 is left, which is still enough to Sprint");
        odd.run_to(20.0);
        float lowest = 1000;
        for (const float v : odd.stamina_series()) lowest = std::min(lowest, v);
        CHECK(!odd.rt.sprinting() && odd.rt.stamina() == 0.0 && lowest == 0.0f, "the 81st charge stops at 0, not at -0.25: Stamina is never negative");
    }
    {
        // below the minimum; a Dodge does not end it, a Rest does
        Sim low(r.rules, [] { auto m = level1_stats(); m[content::kGuidStamina] = 0.5; return m; }());
        CHECK(low.trigger(content::kGuidSprint, 0) == Refusal::Stamina && low.ends() == std::vector<int32_t>{0}, "with 0.5 Stamina a Sprint is refused (it needs 1)");
        Sim d(r.rules);
        d.move(true);
        d.trigger(content::kGuidSprint, 0);
        d.run_to(1.0);
        d.trigger(content::kGuidDodge, 1);
        CHECK(d.rt.sprinting() && d.rt.evading() && d.ends().empty() && !stat_of(d.with("the Dodge starts")[0]->change, kMoveSpeedMult),
              "a Dodge does not end the Sprint (retail capture 745: the Sprint, MoveSpeedMult and the drain run on through it): no ClientEndAbility, no change to MoveSpeedMult");
        const double before = d.rt.stamina();
        d.run_to(2.0);
        CHECK(d.rt.sprinting() && d.rt.stamina() < before && d.series_of(kMoveSpeedMult) == std::vector<float>{1.2f},
              "the drain goes on, and MoveSpeedMult was set once, by the Sprint");
        Sim rest(r.rules);
        rest.move(true);
        rest.trigger(content::kGuidSprint, 0);
        rest.trigger(content::kGuidRest, 1);
        CHECK(!rest.rt.sprinting() && rest.rt.resting() && rest.ends() == std::vector<int32_t>{0} && rest.series_of(kMoveSpeedMult) == std::vector<float>({1.2f, 1.0f}),
              "a Rest ends the Sprint with ClientEndAbility (no retail capture has a Rest, and no instance may linger on the client), MoveSpeedMult back to 1.0");
        // a second trigger of a running instance is not started twice and not ended either
        Sim dup(r.rules);
        dup.trigger(content::kGuidSprint, 0);
        const size_t events = dup.log.size();
        CHECK(dup.trigger(content::kGuidSprint, 0) == Refusal::Duplicate && dup.log.size() == events && dup.rt.sprinting(),
              "the same ClientInstanceId again while it runs: dropped, nothing answered (ClientEndAbility would end the real one)");
        // a new Sprint replaces a running one
        dup.trigger(content::kGuidSprint, 3);
        CHECK(dup.rt.sprinting() && dup.with("replaced").size() == 1, "a Sprint while another runs replaces it");
    }
}

static void test_regeneration(const Rules& r) {
    std::printf("Stamina regeneration\n");
    {
        // a full bar after a Sprint: a pause of 1 s, then 5 a second, so 20 s from the end of the Sprint
        Sim s(r.rules);
        s.move(true);
        s.trigger(content::kGuidSprint, 0);
        s.run_to(20.0);
        CHECK(s.rt.stamina() == 0.0, "(the bar is empty at t = 20 s: it was at 19.75)");
        s.move(false);
        s.run_to(20.99);
        CHECK(s.rt.stamina() == 0.0, "the End hit's pause (to 20.75 s) and then the tick at 21 s: nothing before it");
        s.run_to(21.0);
        CHECK(s.rt.stamina() == 5.0 && s.rt.regen_pause_until() == 20.75, "then +5 at the first tick after the pause");
        s.run_to(30.0);
        CHECK(s.rt.stamina() == 50.0, "50 after 10 s of regeneration");
        s.run_to(39.95);
        CHECK(s.rt.stamina() == 95.0, "95 after 19 ticks");
        s.run_to(40.0);
        CHECK(s.rt.stamina() == 100.0, "back to 100 at t = 40 s: 20 ticks of 5 after the Sprint ended at 19.75 s");
        const size_t events = s.log.size();
        s.run_to(60.0);
        CHECK(s.rt.stamina() == 100.0 && s.log.size() == events, "a full bar sends nothing more (no change, no event)");
        const auto series = s.stamina_series();
        CHECK(series.size() == 80 + 20 && series.back() == 100.0f, "(the whole run: 80 charges and 20 regeneration ticks)");
    }
    {
        // regeneration never goes above the maximum
        Sim s(r.rules, [] { auto m = level1_stats(); m[content::kGuidStamina] = 98; return m; }());
        s.run_to(3.0);
        const auto series = s.stamina_series();
        CHECK(series.size() == 1 && series[0] == 100.0f && s.rt.stamina() == 100.0, "98 and +5 is 100, not 103");
    }
    {
        // one jump of the clock gives the events of many small steps: each timer runs at its own time
        Sim fine(r.rules), jump(r.rules);
        fine.trigger(content::kGuidDodge, 0);
        jump.trigger(content::kGuidDodge, 0);
        fine.run_to(30.0);
        jump.rt.tick(30.0);
        jump.take();
        bool same = fine.log.size() == jump.log.size() && fine.log.size() > 10;
        for (size_t i = 0; same && i < fine.log.size(); ++i) {
            const AbilityEvent& a = fine.log[i];
            const AbilityEvent& b = jump.log[i];
            same = a.note == b.note && a.at == b.at && a.change.stats.size() == b.change.stats.size() &&
                   (a.change.stats.empty() || a.change.stats[0].value == b.change.stats[0].value);
        }
        CHECK(same && fine.rt.stamina() == 100.0 && jump.rt.stamina() == 100.0,
              "a Dodge and 30 s in 600 steps, or in one tick: the same events, each at its own time, and a full bar in both");
    }
}
// C-Y-B x C O A

static void test_dodge(const Rules& r) {
    std::printf("Dodge\n");
    Sim s(r.rules);
    s.run_to(0.5);
    CHECK(s.trigger(content::kGuidDodge, 0) == Refusal::None && s.rt.stamina() == 50.0, "a Dodge costs 50, at once");
    const auto start = s.with("the Dodge starts");
    CHECK(start.size() == 1 && start[0]->at == 0.5 && start[0]->change.stats.size() == 1 && start[0]->change.stats[0].value == 50.0f &&
              start[0]->change.abilities_added.size() == 1 && start[0]->change.abilities_added[0].ability_guid == content::kGuidDodge &&
              start[0]->change.cooldowns_set.size() == 1 && start[0]->change.effects_added.size() == 1 &&
              start[0]->change.effects_added[0].effect_guid == kEvasionEffect && start[0]->change.effects_added[0].duration == 0.5f,
          "one step: Stamina 50, the ability item, the Cooldowns item and the 0.5 s Evasion effect");
    const bs::CooldownItem& cd = start[0]->change.cooldowns_set[0];
    CHECK(cd.ability_guid == content::kGuidDodge && cd.triggered && cd.initial_cooldown == 0.65f && cd.charges == 1 && cd.end_time - cd.start_time > 0.6499 &&
              cd.end_time - cd.start_time < 0.6501 && std::fabs(cd.start_time - bs::effect_clock(1000.5)) < 1e-6 &&
              start[0]->change.effects_added[0].start_time == cd.start_time,
          "the cooldown: triggered, 0.65 s, one charge, from now to now + 0.65 on the synchronized clock (the Evasion's StartTime is its start, as retail's)");
    CHECK(s.rt.evading() && s.rt.dodge_cooldown_until() == 0.5 + 0.65, "the Evasion runs; the cooldown ends at 1.15 s");

    // a Dodge in the cooldown is refused, with Stamina to spare
    s.run_to(1.0);
    const auto evasion_end = s.with("the Evasion ends");
    CHECK(!s.rt.evading() && evasion_end.size() == 1 && evasion_end[0]->change.effects_removed.size() == 1 && evasion_end[0]->change.effects_added.size() == 1 &&
              evasion_end[0]->change.effects_added[0].effect_guid == kDisableRegenEffect && evasion_end[0]->change.effects_added[0].duration == 1.0f,
          "the Evasion effect ends after 0.5 s, and the 1 s DisableRegen effect takes its place in the same step (retail)");
    CHECK(s.trigger(content::kGuidDodge, 1) == Refusal::Cooldown && s.ends() == std::vector<int32_t>{1} && s.rt.stamina() == 50.0,
          "a second Dodge at 0.5 s after the first, in the cooldown: refused with ClientEndAbility(1), Stamina unchanged");
    s.run_to(1.2);
    CHECK(s.with("cooldown is over").size() == 1 && s.rt.stamina() == 50.0, "the cooldown ends at 1.15 s and its items go");
    const auto over = s.with("cooldown is over");
    CHECK(over.size() == 1 && over[0]->change.abilities_removed == std::vector<int64_t>{0} && over[0]->change.cooldowns_removed == std::vector<int64_t>{content::kGuidDodge},
          "the ability item and the Cooldowns item are removed");
    CHECK(s.trigger(content::kGuidDodge, 2) == Refusal::None && s.rt.stamina() == 0.0, "after the cooldown a Dodge is allowed: 50 left, so 0");
    s.run_to(2.5);
    CHECK(s.trigger(content::kGuidDodge, 3) == Refusal::Stamina && s.ends() == ints({1, 3}) && s.rt.stamina() < 50.0,
          "a Dodge with less than 50 Stamina is refused with ClientEndAbility(3)");
    // regeneration after the second Dodge (at 1.2): Evasion to 1.7, pause to 2.7, the first tick at 3 s
    s.run_to(2.99);
    CHECK(s.rt.stamina() == 0.0, "regeneration stays off for 1.5 s after a Dodge (the Evasion, then the pause): still 0 at 2.99 s");
    s.run_to(3.0);
    CHECK(s.rt.stamina() == 5.0, "and resumes at the next tick (t = 3 s): 5");

    // a Dodge exactly at the cost
    Sim exact(r.rules, [] { auto m = level1_stats(); m[content::kGuidStamina] = 50; return m; }());
    CHECK(exact.trigger(content::kGuidDodge, 0) == Refusal::None && exact.rt.stamina() == 0.0, "50 Stamina is enough for a Dodge");
    Sim short_(r.rules, [] { auto m = level1_stats(); m[content::kGuidStamina] = 49.9; return m; }());
    CHECK(short_.trigger(content::kGuidDodge, 0) == Refusal::Stamina && short_.rt.stamina() == 49.9, "49.9 is not");
}

/// The cooldown's edge, to the tick: the cooldown (0.65 s) is over at exactly t + 0.65, not a moment before.  The first Dodge is
/// at 0.25 s, where 0.25 + 0.65 and the clock's 0.9 are the same double.
static void test_dodge_cooldown_edge(const Rules& r) {
    std::printf("Dodge: the cooldown to the tick\n");
    Sim at_edge(r.rules);
    at_edge.run_to(0.25);
    at_edge.trigger(content::kGuidDodge, 0);
    at_edge.run_to(0.9);
    CHECK(at_edge.rt.dodge_cooldown_until() == 0.9, "(the first Dodge at 0.25 s: its cooldown ends at 0.9 s)");
    CHECK(at_edge.trigger(content::kGuidDodge, 1) == Refusal::None && at_edge.rt.stamina() == 0.0 && at_edge.rt.evading(),
          "a Dodge at exactly 0.65 s after the first is accepted");
    Sim early(r.rules);
    early.run_to(0.25);
    early.trigger(content::kGuidDodge, 0);
    early.run_to(0.85);
    CHECK(early.rt.trigger(content::kGuidDodge, 1, 0.899) == Refusal::Cooldown, "one at 0.649 s after it is refused: in the cooldown");
    early.take();
    CHECK(early.ends() == std::vector<int32_t>{1} && early.rt.stamina() == 50.0, "with ClientEndAbility and no cost");
}

/// The clock never goes back: a late or reordered caller cannot rewind the timers.
static void test_clock_never_goes_back(const Rules& r) {
    std::printf("the clock never goes back\n");
    Sim s(r.rules);
    s.run_to(5.0);
    s.rt.tick(3.0);
    s.take();
    CHECK(s.log.empty(), "a tick with an earlier time changes nothing");
    CHECK(s.rt.trigger(content::kGuidDodge, 0, 3.0) == Refusal::None, "(a Dodge with an earlier time is a Dodge)");
    s.take();
    const auto start = s.with("the Dodge starts");
    CHECK(start.size() == 1 && start[0]->at == 5.0 && s.rt.dodge_cooldown_until() == 5.0 + 0.65,
          "... made at the runtime's own time, 5 s, not the caller's 3 s: its cooldown ends at 5.65 s");
    s.rt.set_moving(true, 1.0);   // a move heard late
    s.rt.tick(5.4);
    s.take();
    CHECK(s.rt.evading() && s.with("the Evasion ends").empty(), "and the Evasion (0.5 s from 5 s) runs to 5.5 s, as if the clock had been 5 s all along");
    s.rt.tick(5.5);
    s.take();
    CHECK(!s.rt.evading() && s.with("the Evasion ends").size() == 1, "(it ends at 5.5 s)");
}

/// The Dodge's items: the montage the client's AnimationIndex names and MeshYaw (retail: 31 Dodges in 16 captures).
static void test_dodge_items(const Rules& r) {
    std::printf("Dodge: the montage and MeshYaw of the trigger\n");
    const auto dodge = [&](int32_t index, double x, double y) {
        Sim s(r.rules);
        s.trigger(content::kGuidDodge, 0, TriggerInfo{index, x, y});
        const auto start = s.with("the Dodge starts");
        return start.size() == 1 && start[0]->change.abilities_added.size() == 1 ? start[0]->change.abilities_added[0] : bs::ActiveAbilityItem{};
    };
    const auto id_of = [](const bs::ActiveAbilityItem& a) { const auto m = bs::montage_for(a.montage_path); return m ? m->guid.ObjectId : 0ull; };
    const auto package_of = [](const bs::ActiveAbilityItem& a) {
        const auto m = bs::montage_for(a.montage_path);
        return m && m->chain.size() == 2 ? m->chain[1].guid.ObjectId : 0ull;
    };
    const bs::ActiveAbilityItem fwd = dodge(5, 1, 0);
    CHECK(id_of(fwd) == 3289872098355792151ull && fwd.mesh_yaw == 0 && fwd.montage_group == "DefaultGroup" && fwd.targets_self == false,
          "AnimationIndex 5, input (1, 0): the forward montage (3289872098355792151), MeshYaw 0, group DefaultGroup");
    CHECK(id_of(dodge(5, 1, -1)) == 3289872098355792151ull && dodge(5, 1, -1).mesh_yaw == 57344, "AnimationIndex 5 with the diagonal (1, -1): the same montage, MeshYaw 57344 (315 degrees)");
    CHECK(id_of(dodge(7, 0, 1)) == 1215243220362629129ull && dodge(7, 0, 1).mesh_yaw == 0, "AnimationIndex 7, input (0, 1): the right montage (1215243220362629129), MeshYaw 0");
    CHECK(id_of(dodge(8, -1, 0)) == 858903896054330709ull && dodge(8, -1, 0).mesh_yaw == 0, "AnimationIndex 8, input (-1, 0): the back montage (858903896054330709), MeshYaw 0");
    const bs::ActiveAbilityItem none = dodge(9, 0, 0);
    CHECK(id_of(none) == 11479915692035936417ull && package_of(none) == 5290943011317832207ull && none.mesh_yaw == 32768,
          "AnimationIndex 9, no input: the no-input montage (11479915692035936417, package 5290943011317832207), MeshYaw 32768");
    CHECK(id_of(dodge(2, 0, 1)) == 10081100002258589133ull && dodge(2, 0, 1).mesh_yaw == 0, "AnimationIndex 2: the Rogue's dash to the right (10081100002258589133), MeshYaw 0");
    const bs::ActiveAbilityItem unknown = dodge(6, 0, -1);
    CHECK(unknown.montage_path.empty() && unknown.mesh_yaw == 0 && unknown.ability_guid == content::kGuidDodge,
          "an index no capture shows (6): the Dodge still happens, with no Montage");
    CHECK(dodge(5, std::nan(""), 1).mesh_yaw == 0 && dodge(5, 1e308, -1e308).mesh_yaw == 57344, "an input that is not a number gives MeshYaw 0; a huge one is only its direction");
    CHECK(dodge(0, 1, 0).montage_path.empty() && dodge(-3, 1, 0).montage_path.empty() && dodge(2147483647, 1, 0).montage_path.empty(), "and so does any other index, whatever it is");
    // the Dodge's item has no montage of its own in the rules (the first of the ability's list is the Rogue's forward dash)
    CHECK(r.rules.dodge.montage_path.find("RogueDash_Fwd") != std::string::npos && fwd.montage_path != r.rules.dodge.montage_path,
          "the rules' own montage for the Dodge (the Rogue's forward dash) is not what is sent");

    // the dash window
    Sim s(r.rules);
    s.run_to(0.5);
    CHECK(!s.rt.dash_allowed(0.5), "before a Dodge the A6 checker gets no dash allowance");
    s.trigger(content::kGuidDodge, 0);
    CHECK(s.rt.dash_allowed(0.5) && s.rt.dash_allowed(1.0) && s.rt.dash_allowed(1.29) && !s.rt.dash_allowed(1.31),
          "a Dodge allows the dash from its trigger to the end of its Evasion plus 0.3 s (0.8 s: the dash covers 559-810 cm within 0.8 s)");
}

static void test_rest(const Rules& r) {
    std::printf("Rest\n");
    Sim s(r.rules);
    CHECK(s.trigger(content::kGuidRest, 0) == Refusal::None && s.rt.resting(), "Rest starts (a toggle, out of combat)");
    const auto start = s.with("the Rest starts");
    CHECK(start.size() == 1 && start[0]->change.abilities_added.size() == 1 && start[0]->change.abilities_added[0].targets_self &&
              start[0]->change.abilities_added[0].animation_counter == 0 && start[0]->change.abilities_added[0].montage_path == r.rules.rest.montage_path &&
              start[0]->change.effects_added.empty() && start[0]->change.stats.empty(),
          "first only the ability item: the caster as target, the Rest montage, AnimationCounter 0");
    s.run_to(1.9);
    CHECK(!s.rt.rest_effect_on(), "the effect is not there during the sit-down");
    s.run_to(2.0);
    const auto effect = s.with("the Rest effect starts");
    CHECK(s.rt.rest_effect_on() && effect.size() == 1 && effect[0]->change.effects_added.size() == 1 &&
              effect[0]->change.effects_added[0].effect_guid == kRestEffect && effect[0]->change.effects_added[0].effect_id == 0,
          "after kRestEffectDelay (2 s, inside the capture's 1.77 and 2.04) the Rest effect is added (EffectId 0)");
    CHECK(effect[0]->change.stats.size() == 2 && effect[0]->change.stats[0].record == kHealthRegen && effect[0]->change.stats[0].value == 2.5f &&
              effect[0]->change.stats[1].record == kManaRegen && effect[0]->change.stats[1].value == 2.5f,
          "with it HealthRegeneration and ManaRegeneration go from 0.6 to 2.5 (0.6 + 0.01 x 90 + 1), as pkt#8228");
    CHECK(effect[0]->change.effects_added[0].start_time == bs::effect_clock(1000.0 + 2.0) && effect[0]->change.effects_added[0].tick_time == 0.0f,
          "its StartTime is the synchronized clock at that moment");
// [C.O.A :: C Y B]

    // regeneration of Stamina is untouched by Rest
    // a cancel jumps the montage to its end section and removes the effect a moment later
    CHECK(s.cancel(0) && !s.rt.resting(), "ServerNotifyHeldAbilityCancelled(0) releases the Rest");
    const auto jump = s.with("the Rest is released");
    CHECK(jump.size() == 1 && jump[0]->change.abilities_changed.size() == 1 && jump[0]->change.abilities_changed[0].may_jump_to_position &&
              jump[0]->change.abilities_changed[0].montage_position == 5.82606792f && jump[0]->change.abilities_changed[0].animation_counter == 1 &&
              jump[0]->change.abilities_removed.empty(),
          "the ability item changes: MontagePosition 5.826 (SitEnd), bMayJumpToPosition, AnimationCounter 1; it stays, as in the capture");
    CHECK(s.rt.rest_effect_on(), "the effect is still on");
    s.run_to(2.1);
    const auto gone = s.with("the Rest effect ends");
    CHECK(!s.rt.rest_effect_on() && gone.size() == 1 && gone[0]->change.effects_removed == std::vector<int64_t>{0} && gone[0]->change.stats.size() == 2 &&
              gone[0]->change.stats[0].value == k06 && gone[0]->change.stats[1].value == k06,
          "0.067 s later the effect is removed and the regeneration is back to 0.6 (pkt#8318 -> #8320)");

    // a second Rest replaces the first in one step (pkt#8240), the item with a new AnimationCounter
    Sim again(r.rules);
    again.trigger(content::kGuidRest, 0);
    again.run_to(2.5);
    again.trigger(content::kGuidRest, 1);
    const auto replaced = again.with("the Rest starts");
    CHECK(replaced.size() == 2 && replaced[1]->change.abilities_removed == std::vector<int64_t>{0} && replaced[1]->change.abilities_added.size() == 1 &&
              replaced[1]->change.abilities_added[0].server_instance_id == 1 && replaced[1]->change.abilities_added[0].animation_counter == 2 &&
              replaced[1]->change.effects_removed == std::vector<int64_t>{0} && replaced[1]->change.stats.size() == 2 && replaced[1]->change.stats[0].value == k06,
          "one step: instance 0 removed, instance 1 added with AnimationCounter 2, the effect removed, the regeneration back to 0.6");
    again.run_to(4.6);
    const auto second = again.with("the Rest effect starts");
    CHECK(second.size() == 2 && second[1]->change.effects_added[0].effect_id == 1 && second[1]->change.stats[0].value == 2.5f,
          "the second effect (EffectId 1, ids are never reused) comes 2 s after it");

    // out of combat only
    Sim fight(r.rules);
    fight.rt.set_in_combat(true, 0);
    CHECK(fight.trigger(content::kGuidRest, 0) == Refusal::InCombat && fight.ends() == std::vector<int32_t>{0}, "in combat a Rest is refused with ClientEndAbility");
    // Rest and a Dodge
    Sim d(r.rules);
    d.trigger(content::kGuidRest, 0);
    d.run_to(3.0);
    d.trigger(content::kGuidDodge, 1);
    CHECK(!d.rt.resting() && d.with("the Rest ends").size() == 1, "a Dodge ends a Rest (the item jumps to its end section)");
}

/// The mounts milestone, stage 3 fix 2: a mounted rider has no Sprint and no Stamina use (the user's run of 2026-10-08: a Sprint started before the mount-up ran on while mounted
/// and took the Stamina to 0).
static void test_mounted(const Rules& r) {
    std::printf("mounted: no Sprint, no Stamina use\n");
    {
        Sim s(r.rules);
        s.move(true);
        CHECK(s.trigger(content::kGuidSprint, 31) == Refusal::None && s.rt.sprinting(), "(a Sprint runs, the player moves)");
        s.run_to(2.0);
        const double stamina_before = s.rt.stamina();
        CHECK(stamina_before < 100.0 && s.rt.stat(kMoveSpeedMult) == 1.2, "(it has taken Stamina, MoveSpeedMult is 1.2)");
        const size_t events_before = s.log.size();
        s.rt.set_mounted(true, s.now);
        s.take();
        CHECK(s.rt.mounted() && !s.rt.sprinting(), "getting on ends the Sprint");
        CHECK(s.ends() == ints({31}), "with ClientEndAbility(the client's instance) at once (not 163 ms later)");
        const auto ending = s.with("gets on a mount");
        CHECK(ending.size() == 1 && ending[0]->change.abilities_removed.size() == 1 && ending[0]->change.effects_removed.size() == 1 && ending[0]->change.effects_added.empty() &&
                  stat_of(ending[0]->change, kMoveSpeedMult) == 1.0f && s.rt.stat(kMoveSpeedMult) == 1.0,
              "the way a Sprint ends: its ability item and effect removed, MoveSpeedMult back to 1.0, and no DisableRegen effect (no regeneration pause for a rider)");
        CHECK(s.log.size() > events_before && s.rt.regen_pause_until() < 0, "(the exertion pause did not start)");
        s.move(true);   // the rider's moves are its mount's: the pawn counts as standing
        s.run_to(6.0);
        CHECK(!s.rt.moving(s.now), "a mounted rider never counts as moving");
        CHECK(s.rt.stamina() >= stamina_before, "four more seconds mounted take no Stamina (they give it back, standing)");
        const size_t before = s.log.size();
        const Refusal sprint = s.trigger(content::kGuidSprint, 32);
        const Refusal dodge = s.trigger(content::kGuidDodge, 33);
        const Refusal rest = s.trigger(content::kGuidRest, 34);
        CHECK(sprint == Refusal::Mounted && dodge == Refusal::Mounted && rest == Refusal::Mounted && !s.rt.sprinting() && !s.rt.evading() && !s.rt.resting(),
              "a Sprint, a Dodge and a Rest asked for while mounted are refused (Mounted)");
        CHECK(s.ends() == ints({31, 32, 33, 34}) && s.log.size() == before + 3, "each is answered with ClientEndAbility and nothing else");
        CHECK(s.rt.stamina() == s.rt.stat(content::kGuidStamina) && s.rt.stat(kMoveSpeedMult) == 1.0, "(nothing changed on the pawn)");
        // getting off: the pawn's abilities are its own again; the Sprint does not come back by itself
        s.rt.set_mounted(false, s.now);
        s.take();
        CHECK(!s.rt.mounted() && !s.rt.sprinting(), "getting off does not bring the Sprint back");
        CHECK(s.trigger(content::kGuidSprint, 35) == Refusal::None && s.rt.sprinting(), "but a new Sprint starts");
    }
    {
        // a Rest ends at mount-up too
        Sim s(r.rules);
        CHECK(s.trigger(content::kGuidRest, 40) == Refusal::None && s.rt.resting(), "(a Rest runs)");
        s.run_to(3.0);
        s.rt.set_mounted(true, s.now);
        s.take();
        CHECK(!s.rt.resting(), "getting on ends a Rest");
    }
    {
        // asking twice changes nothing
        Sim s(r.rules);
        s.rt.set_mounted(true, s.now);
        s.rt.set_mounted(true, s.now);
        s.take();
        CHECK(s.log.empty() && s.rt.mounted(), "getting on twice is the same as once");
    }
}

static void test_other_abilities(const Rules& r) {
    std::printf("anything else is refused\n");
    Sim s(r.rules);
    CHECK(s.trigger(kScepterCombo, 0) == Refusal::NotEnabled && s.ends() == std::vector<int32_t>{0} && s.log.size() == 1 && s.log[0].change.empty(),
          "a combat ability: ClientEndAbility(ClientInstanceId) and nothing else");
    CHECK(s.trigger(kGuard, 4) == Refusal::NotEnabled && s.ends() == ints({0, 4}), "General_Guard too");
    CHECK(s.trigger(0, 5) == Refusal::NotEnabled && s.trigger(-1, 6) == Refusal::NotEnabled && s.ends() == ints({0, 4, 5, 6}),
          "and an ability that does not exist");
    CHECK(s.rt.refused() == 4 && s.rt.stamina() == 100.0 && !s.rt.sprinting() && !s.rt.resting() && !s.rt.evading(), "none of them changes anything");
    CHECK(!s.rt.enabled(kScepterCombo) && s.rt.enabled(content::kGuidSprint) && s.rt.enabled(content::kGuidDodge) && s.rt.enabled(content::kGuidRest),
          "enabled(): the whitelist is Sprint, Dodge and Rest");
// ~ Cyb :: C | O | A ~

    // a runtime whose character lacks a stat it needs does not start
    auto missing = level1_stats();
    missing.erase(content::kGuidMaxStamina);
    AbilityRuntime bad(r.rules, missing, 0);
    CHECK(!bad.ok() && contains(bad.error(), "MaxStamina"), "a character with no MaxStamina: ok() is false and says which stat");
    CHECK(bad.trigger(content::kGuidSprint, 0, 0) == Refusal::NotEnabled && bad.take_events().empty(), "(and it runs nothing)");
    auto no_regen = level1_stats();
    no_regen.erase(kHealthRegen);
    AbilityRuntime bad2(r.rules, no_regen, 0);
    CHECK(!bad2.ok() && contains(bad2.error(), "regeneration"), "so with no HealthRegeneration (the Rest effect raises it)");
    auto no_speed = level1_stats();
    no_speed.erase(kMoveSpeedMult);
    AbilityRuntime bad3(r.rules, no_speed, 0);
    CHECK(!bad3.ok() && contains(bad3.error(), "MoveSpeedMult"), "and with no MoveSpeedMult (the Sprint's effect raises it)");
}

/// ServerInstanceId is the server's own counter: the client's ids go wherever they like.
static void test_instances(const Rules& r) {
    std::printf("ServerInstanceId: the server's own counter\n");
    Sim s(r.rules, [] { auto m = level1_stats(); m[content::kGuidStamina] = 300; m[content::kGuidMaxStamina] = 300; return m; }());
    s.move(true);
    s.trigger(content::kGuidSprint, 40);
    s.trigger(kScepterCombo, 41);
    s.trigger(content::kGuidDodge, 42, TriggerInfo{5, 1, 0});
    const auto sprint = s.with("the Sprint starts");
    const auto dodge = s.with("the Dodge starts");
    CHECK(sprint.size() == 1 && sprint[0]->change.abilities_added[0].server_instance_id == 0 && dodge.size() == 1 &&
              dodge[0]->change.abilities_added[0].server_instance_id == 1,
          "the Sprint (ClientInstanceId 40) is instance 0 and the Dodge (42) instance 1; the refused trigger (41) took none");
    CHECK(s.rt.instance_of(40) == 0 && s.rt.instance_of(42) == 1 && !s.rt.instance_of(41) && !s.rt.instance_of(0) && !s.rt.instance_of(1),
          "the runtime knows each running instance by the client's id, never by the server's");
    s.run_to(1.5);   // the Dodge's items are gone
    s.trigger(content::kGuidDodge, 43, TriggerInfo{5, 1, 0});
    const auto dodges = s.with("the Dodge starts");
    CHECK(dodges.size() == 2 && dodges[1]->change.abilities_added[0].server_instance_id == 2, "the next instance is 2: the counter never goes back or repeats");
    CHECK(!s.cancel(0) && !s.cancel(1) && s.rt.sprinting(), "a cancel names the client's id: 0 and 1, instance numbers, release nothing");
    CHECK(s.cancel(40) && !s.rt.sprinting(), "a cancel of the Sprint's ClientInstanceId (40) does");
    // the client's id is what ClientEndAbility carries, whatever the instance
    Sim e(r.rules, [] { auto m = level1_stats(); m[content::kGuidStamina] = 1.25; return m; }());
    e.move(true);
    e.trigger(kScepterCombo, 9);
    e.trigger(content::kGuidSprint, 17);
    e.run_to(0.5);
    CHECK(e.ends() == ints({9, 17}) && e.with("the Sprint starts")[0]->change.abilities_added[0].server_instance_id == 0,
          "ClientEndAbility carries the client's id (17 for the Sprint that ran out, instance 0), after the refusal's (9)");
}

// ============================================================================================================================
//  on the wire
// ============================================================================================================================

struct Ref {
    const char* name;
    size_t      bits;
    const char* hex;
};
template <class B>
static Ref ref(const B& b) { return {b.name, b.bits, b.hex}; }

static bool same_as(const BunchWriter& w, const Ref& b) {
    const auto want = capture_blocks::bytes_of(b.hex);
    size_t diff = 0;
    if (w.bit_pos() != b.bits) {
        std::printf("    wrote %zu bits, expected %zu (%s)", w.bit_pos(), b.bits, b.name);
        const size_t n = w.bit_pos() < b.bits ? w.bit_pos() : b.bits;
        if (!capture_blocks::same_bit_range(w.data(), want.data(), n, &diff)) std::printf("; first differing bit %zu", diff);
        std::printf("\n");
        return false;
    }
    if (!capture_blocks::same_bit_range(w.data(), want.data(), b.bits, &diff)) {
        std::printf("    first differing bit %zu (%s)\n", diff, b.name);
        return false;
    }
    return true;
}

static CodecOptions stat_options() {
    CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

static const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

/// The fast-array fields of the content block of `object` in `payload`.
static std::optional<std::vector<FastArrayField>> fast_fields(const BunchWriter& payload, const FIntrepidNetworkGUID& object, const char* cls_name) {
    const auto views = read_content_blocks(payload.data(), payload.bit_pos(), 0);
    if (!views) return std::nullopt;
    for (const BlockView& v : *views) {
        if (v.header.is_actor || !(v.header.object == object)) continue;
        size_t p = v.payload_pos;
        return read_fast_array_fields(class_named(cls_name), payload.data(), v.payload_pos + v.payload_bits, p, stat_options());
    }
    return std::nullopt;
}

static const FastArrayField* field_named(const std::vector<FastArrayField>& fields, const char* cls_name, const char* name) {
    const schema::NetField* f = class_named(cls_name).field(name);
    if (!f) return nullptr;
    for (const FastArrayField& x : fields)
        if (x.field == f->index) return &x;
    return nullptr;
}

static bs::PlayerSetup captured_setup_with_owner_key(int32_t owner_key) {
    auto setup = bs::captured_character_setup();
    if (!setup) {
        std::printf("  [FAIL] the captured character's setup does not build\n");
        std::exit(2);
    }
    // The original's first Stats delta of the player names base 690 while the possession state was key 417: the 273 steps
    // between are in no packet (test_repl_fast_array starts its chain there too).
    setup->stats.owner.array_key = owner_key;
    return std::move(*setup);
}
// C | O | A with C_Y_B

/// The StartTime of the one effect the captured block adds.
static double captured_start_time(const capture_bootstrap::Bits& b, const bs::PlayerSetup& setup) {
    BunchWriter w;
    const auto bytes = capture_blocks::bytes_of(b.hex);
    w.write_bit_range(bytes.data(), 0, b.bits);
    const auto fields = fast_fields(w, setup.guids.stats, "UAoCStatsComponent");
    const FastArrayField* fx = fields ? field_named(*fields, "UAoCStatsComponent", "CurrentEffects") : nullptr;
    if (!fx || fx->delta.changed.size() != 1) return 0;
    const auto it = fx->delta.changed[0].values.values.find(3);
    return it != fx->delta.changed[0].values.values.end() && it->second.get_if<double>() ? *it->second.get_if<double>() : 0;
}

static bs::ActiveAbilityItem rest_item(int64_t instance, int32_t counter, float position = 0, bool jump = false) {
    bs::ActiveAbilityItem a;
    a.ability_guid = content::kGuidRest;
    a.server_instance_id = instance;
    a.targets_self = true;
    a.montage_path = std::string(bs::kRestMontagePath);
    a.montage_position = position;
    a.may_jump_to_position = jump;
    a.animation_counter = counter;
    return a;
}

static bs::EffectItem rest_effect(int64_t id, double start) {
    bs::EffectItem e;
    e.effect_id = id;
    e.effect_guid = kRestEffect;
    e.start_time = start;
    e.duration = AbilityRuntime::kInfiniteDuration;
    e.tick_time = 0;
    e.stacks = 1;
    return e;
}

static void regen_to(bs::AbilityChange& c, float v) {
    c.stats.push_back({kHealthRegen, v});
    c.stats.push_back({kManaRegen, v});
}

/// A joined pawn of the captured character (its Stats Owner array at key 690), ready for ability updates.
struct Pawn {
    bs::PlayerBootstrap pb;
    explicit Pawn(int32_t owner_key = 690) : pb(captured_setup_with_owner_key(owner_key), bs::GameStateMode::Tier2) {
        const auto join = pb.ok() ? pb.join_sequence() : std::nullopt;
        if (!join) {
            std::printf("  [FAIL] the captured character does not join: %s\n", pb.error().c_str());
            std::exit(2);
        }
        // the client has acknowledged the join's exports (the session marks them when their packets are acked)
        for (const bs::Send& s : *join)
            for (const auto& guids : s.emission.exported_by_bunch) pb.mark_exported(guids);
    }
    std::optional<bs::Send> update(const bs::AbilityChange& c) {
        if (!pb.apply_ability_change(c)) return std::nullopt;
        return pb.ability_update();
    }
};

static void test_rest_golden() {
    std::printf("General_Rest on the wire (pkt#8175-8320)\n");
    Pawn p;
    const bs::PlayerSetup& setup = p.pb.setup();
    const double start1 = captured_start_time(capture_bootstrap::kRestStats8228, setup);
    const double start2 = captured_start_time(capture_bootstrap::kRestStats8301, setup);
    CHECK(start1 > 15061867.6 && start1 < 15061867.8 && start2 > 15061870.0 && start2 < 15061870.2, "(the captured effects' StartTimes: 15,061,867.7 and 15,061,870.1)");

    {   // pkt#8175: the ability item and the montage's export
        bs::AbilityChange c;
        c.abilities_added.push_back(rest_item(0, 0));
        const auto s = p.update(c);
        CHECK(s && s->role == bs::Role::Pawn && !s->opens_channel && s->reliable(), "pkt#8175: an ability update is one reliable chain on the pawn's channel");
        CHECK(s && s->emission.exports.size() == 1 && same_as(s->emission.exports[0], ref(capture_bootstrap::kRestAbilityAddExports0)),
              "the montage's export bunch (CM_M_ABLT_General_Rest_Montage and its package, 1,345 bits) equals the original's bit for bit");
        CHECK(s && same_as(s->emission.payload, ref(capture_bootstrap::kRestAbilityAddContent)),
              "ReplicatedActiveAbilities key 1 base 0, instance 0 (the caster as target, the montage, Instigator the pawn): 1,253 bits, bit for bit");
        if (s) p.pb.mark_exported(s->emission.exported_by_bunch[0]);
    }
    {   // pkt#8228: the regeneration up, the effect added
        bs::AbilityChange c;
        regen_to(c, 2.5f);
        c.effects_added.push_back(rest_effect(0, start1));
        const auto s = p.update(c);
        CHECK(s && s->emission.exports.empty() && same_as(s->emission.payload, ref(capture_bootstrap::kRestStats8228)),
              "pkt#8228: Owner 692 against 690 (ids 15 and 19 to 2.5) and CurrentEffects key 1 base 0 (EffectId 0): 1,343 bits, bit for bit");
    }
    {   // pkt#8240: instance 0 replaced by instance 1, the effect removed, the regeneration back
        bs::AbilityChange c;
        regen_to(c, k06);
        c.effects_removed.push_back(0);
        c.abilities_removed.push_back(0);
        c.abilities_added.push_back(rest_item(1, 2));
        const auto s = p.update(c);
        CHECK(s && s->emission.exports.empty() && same_as(s->emission.payload, ref(capture_bootstrap::kRestBoth8240)),
              "pkt#8240: the ability delete and add (key 1 -> 4), the regeneration back (694 against 692), the effect delete (key 3 against 1): both blocks, 2,019 bits, bit for bit");
    }
    {   // pkt#8301: the regeneration up again, the second effect
        bs::AbilityChange c;
        regen_to(c, 2.5f);
        c.effects_added.push_back(rest_effect(1, start2));
        const auto s = p.update(c);
        CHECK(s && same_as(s->emission.payload, ref(capture_bootstrap::kRestStats8301)), "pkt#8301: the second effect (EffectId 1, element ID 2): 1,343 bits, bit for bit");
    }
    {   // pkt#8318: the montage jumps to its end section
        bs::AbilityChange c;
        c.abilities_changed.push_back(rest_item(1, 3, 5.82606792f, true));
        const auto s = p.update(c);
        CHECK(s && s->emission.exports.empty() && same_as(s->emission.payload, ref(capture_bootstrap::kRestAbility8318)),
              "pkt#8318: MontagePosition 5.826, bMayJumpToPosition, AnimationCounter 3 (key 5 against 4): 1,253 bits, bit for bit (the export is acknowledged)");
    }
    {   // pkt#8320: the effect removed, the regeneration back
        bs::AbilityChange c;
        regen_to(c, k06);
        c.effects_removed.push_back(1);
        const auto s = p.update(c);
        CHECK(s && same_as(s->emission.payload, ref(capture_bootstrap::kRestStats8320)), "pkt#8320: the effect delete (key 6 against 4) and the regeneration back: 734 bits, bit for bit");
        CHECK(!p.pb.ability_update(), "and with nothing changed there is no update");
    }
}

static void test_stamina_delta() {
    std::printf("a Stamina delta\n");
    Pawn p;
    bs::AbilityChange c;
    c.stats.push_back({content::kGuidStamina, 98.75f});
    const auto s = p.update(c);
    CHECK(s && s->emission.exports.empty() && s->reliable() && s->role == bs::Role::Pawn, "a Stamina step is one reliable chain, with no export");
    const auto fields = s ? fast_fields(s->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
    const FastArrayField* owner = fields ? field_named(*fields, "UAoCStatsComponent", "StatRepInt32Owner") : nullptr;
    CHECK(fields && fields->size() == 1 && owner && owner->delta.base_key == 690 && owner->delta.array_key == 691 && owner->delta.deleted.empty() && owner->delta.changed.size() == 1,
          "it is the StatsComponent's StatRepInt32Owner alone: key 691 against 690, one item changed");
    const StatInt32Inst* item = owner && owner->delta.changed.size() == 1 && owner->delta.changed[0].values.values.count(1)
                                    ? owner->delta.changed[0].values.values.at(1).get_if<StatInt32Inst>()
                                    : nullptr;
    CHECK(item && item->record == content::kGuidStamina && item->value == std::bit_cast<uint32_t>(98.75f) && owner->delta.changed[0].id == 176,
          "the item is Stamina (element ID 176) with the new value as float bits");
    CHECK(s && s->emission.payload.bit_pos() > 0, "(and the chain's payload is the block alone)");
// (CoA | C Y B)

    // MaxStamina and StaminaRegeneration are in the same array and untouched
    c = {};
    c.stats.push_back({content::kGuidStamina, 98.75f});
    CHECK(p.pb.apply_ability_change(c) && !p.pb.ability_update(), "the same value again is no change: nothing is sent");
    c = {};
    c.stats.push_back({content::kGuidStamina, 97.5f});
    c.stats.push_back({content::kGuidStamina, 96.25f});
    const auto two = p.update(c);
    const auto f2 = two ? fast_fields(two->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
    const FastArrayField* o2 = f2 ? field_named(*f2, "UAoCStatsComponent", "StatRepInt32Owner") : nullptr;
    CHECK(o2 && o2->delta.changed.size() == 1 && o2->delta.base_key == 691 && o2->delta.array_key == 693 &&
              o2->delta.changed[0].values.values.at(1).get_if<StatInt32Inst>()->value == std::bit_cast<uint32_t>(96.25f),
          "two changes of one stat in a step go out as the last value (the key moved by two)");
    // a stat in the OwnerProxy array (the Evasion effect's stats are in the proxy arrays): found wherever it is
    c = {};
    c.stats.push_back({109343399171812ll, 1.0f});   // MagicalEvasion
    const auto prox = p.update(c);
    const auto f3 = prox ? fast_fields(prox->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
    CHECK(f3 && field_named(*f3, "UAoCStatsComponent", "StatRepInt32OwnerProxy") && !field_named(*f3, "UAoCStatsComponent", "StatRepInt32Owner"),
          "MagicalEvasion is in StatRepInt32OwnerProxy and goes out there");
}

/// A stat that replicates base and equipment (the fishing stats' items are extended) is a change when the BASE alone differs, or the equipment
/// alone, and the value stays: the pawn must write the numbers it was given (the gear's own change of a bucket can move any of the three).
static void test_bucket_stat_changes() {
    std::printf("a stat item with base and equipment: each of the three numbers alone is a change\n");
    constexpr int64_t kHook = 6064634285519601665ll;   // Artisan_Gathering_Fishing_Sport_Hook_Rate
    Pawn p;
    auto change = [&](float value, float base, float equipment, bool buckets = true) {
        bs::AbilityChange c;
        bs::StatChange sc;
        sc.record = kHook;
        sc.value = value;
        sc.buckets = buckets;
        sc.base = base;
        sc.equipment = equipment;
        c.stats.push_back(sc);
        return c;
    };
    // what the update carries for the stat: the item's three numbers
    struct Seen { bool item = false, extended = false; uint32_t value = 0, base = 0, equipment = 0; };
    auto seen = [&](const std::optional<bs::Send>& s) {
        Seen out;
        const auto fields = s ? fast_fields(s->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
        const FastArrayField* owner = fields ? field_named(*fields, "UAoCStatsComponent", "StatRepInt32Owner") : nullptr;
        if (!owner) return out;
        for (const auto& el : owner->delta.changed) {
            const auto found = el.values.values.find(1);
            const StatInt32Inst* it = found != el.values.values.end() ? found->second.get_if<StatInt32Inst>() : nullptr;
            if (it && it->record == kHook) out = {true, it->extended, it->value, it->base, it->equipment};
        }
        return out;
    };
    auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
    const auto first = p.update(change(100.0f, 10.0f, 150.0f));
    Seen a = seen(first);
    CHECK(first && a.item && a.extended && a.value == bits(100.0f) && a.base == bits(10.0f) && a.equipment == bits(150.0f),
          "(Hook_Rate is an extended item: value 100, base 10, equipment 150 are written)");
    CHECK(p.pb.apply_ability_change(change(100.0f, 10.0f, 150.0f)) && !p.pb.ability_update(), "the same three numbers again: no change, nothing sent");
    // the base alone
    const auto base_only = p.update(change(100.0f, 20.0f, 150.0f));
    a = seen(base_only);
    CHECK(base_only && a.item && a.value == bits(100.0f) && a.base == bits(20.0f) && a.equipment == bits(150.0f),
          "a change of the BASE alone (value and equipment as they were) is sent, and the item's base is the new one");
    CHECK(p.pb.apply_ability_change(change(100.0f, 20.0f, 150.0f)) && !p.pb.ability_update(), "(and then it is no change)");
    // the equipment alone
    const auto equip_only = p.update(change(100.0f, 20.0f, 350.0f));
    a = seen(equip_only);
    CHECK(equip_only && a.item && a.value == bits(100.0f) && a.base == bits(20.0f) && a.equipment == bits(350.0f), "a change of the EQUIPMENT alone is sent, with the base kept");
    // the sign of a zero is a difference of the bits
    const auto zero = p.update(change(100.0f, 0.0f, 350.0f));
    CHECK(zero && seen(zero).base == bits(0.0f), "(a base of +0.0)");
    const auto neg_zero = p.update(change(100.0f, -0.0f, 350.0f));
    CHECK(neg_zero && seen(neg_zero).base == bits(-0.0f), "a base of -0.0 after +0.0 is a change (the float bits differ)");
    // the value alone
    const auto value_only = p.update(change(90.0f, -0.0f, 350.0f));
    a = seen(value_only);
    CHECK(value_only && a.item && a.value == bits(90.0f) && a.base == bits(-0.0f) && a.equipment == bits(350.0f), "a change of the value alone is sent, base and equipment kept");
    // a change that does not say it carries the buckets leaves them alone, whatever it holds
    const auto plain = p.update(change(80.0f, 5.0f, 5.0f, false));
    a = seen(plain);
    CHECK(plain && a.item && a.value == bits(80.0f) && a.base == bits(-0.0f) && a.equipment == bits(350.0f), "a change without buckets writes the value and not the base and equipment");
}
// [C.Y.B + C-O-A]

static void test_cooldown_item() {
    std::printf("the Cooldowns item (the retail Dodge reply's layout)\n");
    Pawn p;
    bs::AbilityChange c;
    bs::CooldownItem cd;
    cd.ability_guid = content::kGuidDodge;
    cd.triggered = true;
    cd.initial_cooldown = 0.65f;
    cd.start_time = 13809071.207829274;   // retail capture 745, the Dodge at 6.3 s
    cd.end_time = 13809071.85782925;
    cd.charges = 1;
    c.cooldowns_set.push_back(cd);
    const auto s = p.update(c);
    CHECK(s && s->emission.exports.empty(), "a Cooldowns item goes out on its own, with no export");
    const auto fields = s ? fast_fields(s->emission.payload, p.pb.setup().guids.ability, "UAoCAbilityComponent") : std::nullopt;
    const FastArrayField* f = fields ? field_named(*fields, "UAoCAbilityComponent", "Cooldowns") : nullptr;
    CHECK(fields && fields->size() == 1 && f && f->delta.array_key == 1 && f->delta.base_key == 0 && f->delta.changed.size() == 1 && f->delta.deleted.empty(),
          "Cooldowns key 1 against 0 with one new item");
    const ElementState* e = f && !f->delta.changed.empty() ? &f->delta.changed[0].values : nullptr;
    const auto val = [&](uint16_t h) -> const Value* {
        if (!e) return nullptr;
        const auto it = e->values.find(h);
        return it == e->values.end() ? nullptr : &it->second;
    };
    CHECK(e && val(1) && *val(1) == Value::i64(content::kGuidDodge) && val(2) && *val(2) == Value::boolean(true) && val(3) && val(3)->get_if<float>() &&
              *val(3)->get_if<float>() == 0.65f && val(4) && val(4)->get_if<double>() && *val(4)->get_if<double>() == 13809071.207829274 && val(5) &&
              val(5)->get_if<double>() && *val(5)->get_if<double>() == 13809071.85782925 && val(6) && *val(6) == Value::i64(1),
          "it reads back: AbilityGuid, bCooldownTriggered, InitialCooldown, the synchronized start and end times (doubles), Charges 1 (retail's)");
    // an end: the item goes, the key moves by two
    c = {};
    c.cooldowns_removed.push_back(content::kGuidDodge);
    const auto gone = p.update(c);
    const auto f2 = gone ? fast_fields(gone->emission.payload, p.pb.setup().guids.ability, "UAoCAbilityComponent") : std::nullopt;
    const FastArrayField* g = f2 ? field_named(*f2, "UAoCAbilityComponent", "Cooldowns") : nullptr;
    CHECK(g && g->delta.base_key == 1 && g->delta.array_key == 3 && g->delta.deleted.size() == 1 && g->delta.changed.empty(), "its removal is a delete (key 3 against 1)");
    // started again: a new element ID
    c = {};
    c.cooldowns_set.push_back(cd);
    const auto again = p.update(c);
    const auto f3 = again ? fast_fields(again->emission.payload, p.pb.setup().guids.ability, "UAoCAbilityComponent") : std::nullopt;
    const FastArrayField* h = f3 ? field_named(*f3, "UAoCAbilityComponent", "Cooldowns") : nullptr;
    CHECK(h && h->delta.changed.size() == 1 && h->delta.changed[0].id == 2, "a new cooldown is a new item (element ID 2: IDs are not reused)");
}

static void test_end_ability() {
    std::printf("ClientEndAbility\n");
    Pawn p;
    const auto s = p.pb.end_ability(7);
    CHECK(s && s->role == bs::Role::Pawn && !s->opens_channel && s->reliable() && s->emission.exports.empty(), "ClientEndAbility is reliable, on the pawn's channel, with no export");
    if (!s) return;
    const auto blocks = read_content_blocks(s->emission.payload.data(), s->emission.payload.bit_pos(), 0);
    CHECK(blocks && blocks->size() == 1 && !(*blocks)[0].header.is_actor && (*blocks)[0].header.object == p.pb.setup().guids.ability, "one block, on the AbilityComponent");
    if (!blocks || blocks->size() != 1) return;
    const BlockView& v = (*blocks)[0];
    size_t pos = v.payload_pos;
    const auto calls = read_rpc_fields(class_named("UAoCAbilityComponent"), s->emission.payload.data(), v.payload_pos + v.payload_bits, pos);
    CHECK(calls && calls->size() == 1 && (*calls)[0].decoded && std::string((*calls)[0].rpc->name) == "ClientEndAbility", "it reads back as ClientEndAbility");
    const schema::RepCommand* cmd = calls && !calls->empty() ? rpc_command(*(*calls)[0].rpc, "ClientInstanceId") : nullptr;
    const Value* id = nullptr;
    if (cmd && calls && !calls->empty()) {
        const auto it = (*calls)[0].params.values.find(cmd->handle);
        if (it != (*calls)[0].params.values.end()) id = &it->second;
    }
    CHECK(id && id->get_if<int64_t>() && *id->get_if<int64_t>() == 7, "with ClientInstanceId 7");
    CHECK(p.pb.end_ability(7) && !p.pb.ability_update(), "(it changes no shadow: an update after it has nothing to send)");
    // before the join's second state there is no pawn component to call on
    auto setup = bs::captured_character_setup();
    bs::PlayerBootstrap fresh(std::move(*setup), bs::GameStateMode::Tier2);
    CHECK(fresh.ok() && !fresh.end_ability(0) && !fresh.ability_update(), "before the pawn's second state there is nothing to call or update");
    bs::AbilityChange c;
    c.stats.push_back({content::kGuidStamina, 1.0f});
    CHECK(!fresh.apply_ability_change(c), "and no change to apply");
}

static void test_apply_refusals() {
    std::printf("apply_ability_change refuses what does not fit\n");
    Pawn p;
    bs::AbilityChange none;
    CHECK(p.pb.apply_ability_change(none) && !p.pb.ability_update(), "an empty change is applied and sends nothing");
    bs::AbilityChange c;
    c.stats.push_back({12345, 1.0f});
    CHECK(!p.pb.apply_ability_change(c), "a stat the character does not have is refused");
    c = {};
    c.effects_removed.push_back(9);
    CHECK(!p.pb.apply_ability_change(c), "an effect to remove that was never added is refused");
    c = {};
    c.abilities_removed.push_back(9);
    CHECK(!p.pb.apply_ability_change(c), "so is an ability instance");
    c = {};
    c.abilities_changed.push_back(rest_item(9, 1));
    CHECK(!p.pb.apply_ability_change(c), "an ability item to change that is not there");
    c = {};
    c.cooldowns_removed.push_back(content::kGuidDodge);
    CHECK(!p.pb.apply_ability_change(c), "a cooldown to remove that is not there");
    CHECK(!p.pb.ability_update(), "(none of them changed anything)");
    c = {};
    c.effects_added.push_back(rest_effect(0, 1.0));
    CHECK(p.pb.apply_ability_change(c), "an effect is added");
    c = {};
    c.stats.push_back({content::kGuidStamina, 50.0f});
    c.effects_added.push_back(rest_effect(0, 2.0));
    CHECK(!p.pb.apply_ability_change(c), "the same EffectId again is refused...");
    const auto after = p.pb.ability_update();
    const auto fields = after ? fast_fields(after->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
    CHECK(fields && fields->size() == 1 && field_named(*fields, "UAoCStatsComponent", "CurrentEffects") && !field_named(*fields, "UAoCStatsComponent", "StatRepInt32Owner"),
          "...as a whole: its Stamina step was not applied either");
    // a change that removes and adds the same key in one step is fine
    c = {};
    c.effects_removed.push_back(0);
    c.effects_added.push_back(rest_effect(0, 3.0));
    CHECK(p.pb.apply_ability_change(c), "removing an effect and adding one with the same id in one step is allowed");
}

// ============================================================================================================================
//  Sprint and Dodge on the wire, against the retail server's replies
// ============================================================================================================================
// (CoA + C_Y_B)

/// The stats the session hands the runtime: every item of the setup's three stat arrays, as floats.
static std::map<int64_t, double> stats_of(const bs::PlayerSetup& setup) {
    std::map<int64_t, double> stats;
    for (const bs::StatArray* a : {&setup.stats.owner, &setup.stats.owner_proxy, &setup.stats.everyone_proxy}) {
        for (const bs::StatItem& it : a->items) stats[it.record] = std::bit_cast<float>(it.value);
    }
    return stats;
}

static const Value* item_value(const ElementState& e, uint16_t handle) {
    const auto it = e.values.find(handle);
    return it == e.values.end() ? nullptr : &it->second;
}
/// A member of a fast-array item, `def` when it was left at its default (not on the wire).
template <class T>
static T item_get(const ElementState& e, uint16_t handle, T def = T{}) {
    const Value* v = item_value(e, handle);
    const T* p = v ? v->get_if<T>() : nullptr;
    return p ? *p : def;
}

/// The items of one fast-array field of the chain `s`, by element ID.
static std::vector<FastArrayDelta::Changed> items_of(const std::optional<bs::Send>& s, const FIntrepidNetworkGUID& object, const char* cls, const char* field) {
    const auto fields = s ? fast_fields(s->emission.payload, object, cls) : std::nullopt;
    const FastArrayField* f = fields ? field_named(*fields, cls, field) : nullptr;
    return f ? f->delta.changed : std::vector<FastArrayDelta::Changed>{};
}

static bool same_guid(const Value* v, uint64_t id) {
    const FIntrepidNetworkGUID* g = v ? v->get_if<FIntrepidNetworkGUID>() : nullptr;
    return g && g->ObjectId == id;
}

/// What the retail server's ReplicatedActiveAbilities item has in every Sprint and Dodge reply, whatever the session: nothing
/// targeted, a PvP flag of 0, no progress, predicted on the autonomous client, PlayRate 1, no jump (capture 745, lines 153, 448).
static bool retail_item_shape(const ElementState& e, const bs::PlayerSetup& setup, int64_t ability, const char* group, uint64_t montage, uint16_t yaw) {
    const Value* targets = item_value(e, 3);
    const Value* transforms = item_value(e, 4);
    const NameValue* g = item_value(e, 10) ? item_value(e, 10)->get_if<NameValue>() : nullptr;
    const Value* instigator = item_value(e, 17);
    const FIntrepidNetworkGUID* who = instigator ? instigator->get_if<FIntrepidNetworkGUID>() : nullptr;
    return item_get<int64_t>(e, 1) == ability &&
           (!targets || (targets->get_if<ArrayValue>() && targets->get_if<ArrayValue>()->elements.empty())) &&
           (!transforms || (transforms->get_if<ArrayValue>() && transforms->get_if<ArrayValue>()->elements.empty())) &&
           !item_get<bool>(e, 5) && item_get<int64_t>(e, 6) == 0 && item_get<float>(e, 7) == 0.0f && item_get<float>(e, 8) == 0.0f &&
           item_get<bool>(e, 9) && g && g->text == group && same_guid(item_value(e, 11), montage) && item_get<float>(e, 12) == 1.0f &&
           item_get<float>(e, 13) == 0.0f && !item_get<bool>(e, 14) && item_get<uint64_t>(e, 16) == yaw && who && *who == setup.guids.pawn;
}

/// The retail CurrentEffects item's constants: the character is both instigator and source, no PvP force, SourceNode 0.
static bool retail_effect_shape(const ElementState& e, const bs::PlayerSetup& setup, int64_t guid, float duration, float tick) {
    bool same_character = true;
    for (uint16_t i = 0; i < 4; ++i) {
        const int64_t word = static_cast<int32_t>(setup.character_guid[i]);
        same_character = same_character && item_get<int64_t>(e, static_cast<uint16_t>(7 + i)) == word && item_get<int64_t>(e, static_cast<uint16_t>(11 + i)) == word;
    }
    return item_get<int64_t>(e, 2) == guid && item_get<float>(e, 4) == duration && item_get<float>(e, 5) == tick && item_get<int64_t>(e, 6) == 1 && same_character &&
           !item_get<bool>(e, 15) && item_get<int64_t>(e, 16) == 0;
}

/// The retail server's Sprint and Dodge replies (capture 745: a Bard sprinting and dodging in Verra, decoded to their last bit,
/// lines 153 and 448), against the items the runtime sends: each item is built by apply_ability_change, written by
/// ability_update and read back from the chain.  What differs by session (ids, times, the character's GUID words, a talent's
/// +0.1 on MoveSpeedMult) is compared by what it must be, the rest is compared as retail has it.
static void test_retail_replies(const Rules& r) {
    std::printf("the retail server's Sprint and Dodge replies (capture 745), field by field\n");
    Pawn p;
    const bs::PlayerSetup& setup = p.pb.setup();
    const FIntrepidNetworkGUID ability = setup.guids.ability, stats = setup.guids.stats;
    Sim s(r.rules, stats_of(setup), 1000);
    s.move(true);
    s.trigger(content::kGuidSprint, 69);
    CHECK(s.log.size() == 1 && s.log[0].change.stats.size() == 3, "(a Sprint: one step, with three stats: Stamina, MoveSpeedMult and StaminaRegeneration)");
    if (s.log.size() != 1) return;
    const auto sprint = p.update(s.log[0].change);
    CHECK(sprint && sprint->role == bs::Role::Pawn && sprint->reliable(), "the Sprint's reply is ONE chain on the pawn's channel ...");
    const auto abilities = items_of(sprint, ability, "UAoCAbilityComponent", "ReplicatedActiveAbilities");
    const auto speed = items_of(sprint, stats, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
    const auto effects = items_of(sprint, stats, "UAoCStatsComponent", "CurrentEffects");
    const auto owner = items_of(sprint, stats, "UAoCStatsComponent", "StatRepInt32Owner");
    CHECK(abilities.size() == 1 && speed.size() == 1 && effects.size() == 1 && owner.size() == 2,
          "... with the ability item, the MoveSpeedMult item, the effect item, the first charge and StaminaRegeneration 0 (Owner items 172 and 176): the same packet (retail capture 745, t = 2.759)");
    {
        const StatInt32Inst* regen = nullptr;
        for (const auto& item : owner) {
            const StatInt32Inst* x = item_value(item.values, 1) ? item_value(item.values, 1)->get_if<StatInt32Inst>() : nullptr;
            if (x && x->record == content::kGuidStaminaRegeneration && item.id == 172) regen = x;
        }
        CHECK(regen && regen->value == std::bit_cast<uint32_t>(0.0f), "StaminaRegeneration is item 172 of the Owner array (retail's) and 0.0: nothing regenerates while the Sprint runs and the player moves");
    }
    if (abilities.size() != 1 || speed.size() != 1 || effects.size() != 1) return;
    CHECK(retail_item_shape(abilities[0].values, setup, content::kGuidSprint, "Sprint", 9309136633382094689ull, 0),
          "the ability item: General_Sprint, MontageGroup \"Sprint\", Montage 9309136633382094689, nothing targeted, PvP 0, no progress, "
          "predicted on the autonomous client, PlayRate 1, MontagePosition 0, no jump, MeshYaw 0, Instigator the pawn");
    CHECK(item_get<int64_t>(abilities[0].values, 2) == 0, "(its ServerInstanceId is the server's counter, 0: the retail one was 65 for ClientInstanceId 69)");
    const StatInt32Inst* mult = item_value(speed[0].values, 1) ? item_value(speed[0].values, 1)->get_if<StatInt32Inst>() : nullptr;
    CHECK(speed[0].id == 8 && mult && mult->record == kMoveSpeedMult && mult->value == std::bit_cast<uint32_t>(1.2f),
          "MoveSpeedMult: item 8 of StatRepInt32EveryoneProxy (retail's too), 1.2 = 1.0 + 0.2 (retail's characters had two skills: 1.3 = 0x3fa66666)");
    CHECK(retail_effect_shape(effects[0].values, setup, kSprintEffect, AbilityRuntime::kInfiniteDuration, 1.0f) &&
              item_get<double>(effects[0].values, 3) == bs::effect_clock(1000.0),
          "the effect item: General_Sprint, Duration FLT_MAX, TickTime 1.0, one stack, the character as instigator and source, no PvP force, SourceNode 0, StartTime on the synchronized clock");
// built by Cyb with C.O.A

    // the end: MoveSpeedMult back to 1.0, the effect removed, the DisableRegen effect added: one chain
    s.run_to(2.0);
    for (const AbilityEvent& e : s.log) {
        if (&e == &s.log[0]) continue;
        if (!e.change.empty()) p.pb.apply_ability_change(e.change);
    }
    (void)p.pb.ability_update();   // the charges so far
    s.cancel(69);
    const auto stop = p.update(s.log.back().change);
    const auto stop_speed = items_of(stop, stats, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
    const auto stop_effects = items_of(stop, stats, "UAoCStatsComponent", "CurrentEffects");
    const StatInt32Inst* back = stop_speed.size() == 1 && item_value(stop_speed[0].values, 1) ? item_value(stop_speed[0].values, 1)->get_if<StatInt32Inst>() : nullptr;
    CHECK(back && back->record == kMoveSpeedMult && back->value == std::bit_cast<uint32_t>(1.0f),
          "a release: one chain with MoveSpeedMult back to 1.0 (retail: 1.0 in the packet that removes the effect)");
    CHECK(stop_effects.size() == 1 && retail_effect_shape(stop_effects[0].values, setup, kDisableRegenEffect, 1.0f, 0.0f),
          "and the DisableRegen effect (Duration 1.0, no tick) added in the same chain (the removal of the Sprint effect is a delete)");

    // a Dodge (retail line 448: AnimationIndex 5, input (1, 0), the montage by index, Stamina -50, the Evasion effect, the cooldown)
    Sim d(r.rules, stats_of(setup), 1000);
    d.run_to(0.5);
    d.trigger(content::kGuidDodge, 70, TriggerInfo{5, 1, 0});
    CHECK(d.log.size() == 1, "(a Dodge: one step)");
    if (d.log.size() != 1) return;
    Pawn q;
    const auto dodge = q.update(d.log[0].change);
    const auto d_abilities = items_of(dodge, ability, "UAoCAbilityComponent", "ReplicatedActiveAbilities");
    const auto d_cooldowns = items_of(dodge, ability, "UAoCAbilityComponent", "Cooldowns");
    const auto d_effects = items_of(dodge, stats, "UAoCStatsComponent", "CurrentEffects");
    CHECK(dodge && d_abilities.size() == 1 && d_cooldowns.size() == 1 && d_effects.size() == 1, "the Dodge's reply is one chain with the ability item, the Cooldowns item and the Evasion effect");
    if (!dodge || d_abilities.size() != 1 || d_cooldowns.size() != 1 || d_effects.size() != 1) return;
    CHECK(retail_item_shape(d_abilities[0].values, q.pb.setup(), content::kGuidDodge, "DefaultGroup", 3289872098355792151ull, 0),
          "the ability item: General_Dodge, MontageGroup \"DefaultGroup\", the forward montage (AnimationIndex 5), MeshYaw 0, the rest as the Sprint's");
    const ElementState& cd = d_cooldowns[0].values;
    CHECK(item_get<int64_t>(cd, 1) == content::kGuidDodge && item_get<bool>(cd, 2) && item_get<float>(cd, 3) == 0.65f &&
              std::fabs((item_get<double>(cd, 5) - item_get<double>(cd, 4)) - 0.6499999761581421) < 1e-6 && item_get<int64_t>(cd, 6) == 1 &&
              item_get<double>(cd, 4) == bs::effect_clock(1000.5),
          "the Cooldowns item: General_Dodge, bCooldownTriggered, InitialCooldown 0.65, the end 0.65 after the start (retail: 0.6499999762), Charges 1");
    CHECK(retail_effect_shape(d_effects[0].values, q.pb.setup(), kEvasionEffect, 0.5f, 0.0f) && item_get<double>(d_effects[0].values, 3) == item_get<double>(cd, 4),
          "the Evasion effect: Duration 0.5, no tick, StartTime the cooldown's start (retail: the same double)");
    const auto d_owner = items_of(dodge, stats, "UAoCStatsComponent", "StatRepInt32Owner");
    const StatInt32Inst* paid = nullptr;
    const StatInt32Inst* d_regen = nullptr;
    for (const auto& item : d_owner) {
        const StatInt32Inst* x = item_value(item.values, 1) ? item_value(item.values, 1)->get_if<StatInt32Inst>() : nullptr;
        if (x && x->record == content::kGuidStamina && item.id == 176) paid = x;
        if (x && x->record == content::kGuidStaminaRegeneration && item.id == 172) d_regen = x;
    }
    CHECK(d_owner.size() == 2 && paid && std::bit_cast<float>(paid->value) == static_cast<float>(stats_of(setup)[content::kGuidStamina] - 50),
          "Stamina is item 176 of the Owner array (retail's) and 50 lower");
    CHECK(d_regen && d_regen->value == std::bit_cast<uint32_t>(0.0f), "...and StaminaRegeneration (item 172) is 0 with the Dodge: the Evasion stops the regeneration [R: capture 745]");
    CHECK(speed.size() == 1 && items_of(dodge, stats, "UAoCStatsComponent", "StatRepInt32EveryoneProxy").empty(), "a Dodge does not touch MoveSpeedMult");
}

/// Retail capture 745's last Sprint: triggered with 1.25 Stamina, charged to 0.001, and the reply carries MoveSpeedMult 1.0 and the
/// DisableRegen effect; ClientEndAbility comes 163 ms after.  Here the activation's charge leaves 0 and the Sprint ends in the same
/// instant, so the two steps are one chain: nothing of the Sprint is left but MoveSpeedMult's key and the pause.
static void test_out_of_stamina_reply(const Rules& r) {
    std::printf("a Sprint that starts with 1.25 Stamina: retail's reply\n");
    Pawn p;
    const bs::PlayerSetup& setup = p.pb.setup();
    auto st = stats_of(setup);
    st[content::kGuidStamina] = 1.25;
    Sim s(r.rules, st, 1000);
    s.move(true);
    s.trigger(content::kGuidSprint, 3);
    CHECK(s.log.size() == 2 && s.log[0].change.effects_added.size() == 1 && s.log[1].change.effects_removed.size() == 1 && s.log[1].change.effects_added.size() == 1,
          "the Sprint's start and its end are two steps of the runtime");
    if (s.log.size() != 2) return;
    bool applied = true;
    for (const AbilityEvent& e : s.log) applied = applied && p.pb.apply_ability_change(e.change);
    const auto reply = p.pb.ability_update();
    CHECK(applied && reply, "the pawn takes both, and sends one chain");
    const auto speed = items_of(reply, setup.guids.stats, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
    const auto effects = items_of(reply, setup.guids.stats, "UAoCStatsComponent", "CurrentEffects");
    const auto owner = items_of(reply, setup.guids.stats, "UAoCStatsComponent", "StatRepInt32Owner");
    const StatInt32Inst* mult = speed.size() == 1 && item_value(speed[0].values, 1) ? item_value(speed[0].values, 1)->get_if<StatInt32Inst>() : nullptr;
    CHECK(mult && mult->record == kMoveSpeedMult && mult->value == std::bit_cast<uint32_t>(1.0f), "the reply has MoveSpeedMult 1.0 (retail: 1.0 in the reply) ...");
    CHECK(effects.size() == 1 && retail_effect_shape(effects[0].values, setup, kDisableRegenEffect, 1.0f, 0.0f) && owner.size() == 2,
          "... the DisableRegen effect (and no Sprint effect), the Stamina left and StaminaRegeneration 0 (the pause), in that one chain");
    CHECK(s.ends().empty(), "ClientEndAbility has not gone with it");
    s.run_to(0.2);
    CHECK(s.ends() == std::vector<int32_t>{3} && s.log.back().end_ability == 3 && s.log.back().at > 0.16 && s.log.back().at < 0.17,
          "it follows 0.163 s later (retail: 163 ms), after the update");
}

/// Every Stamina value a client reads from a run of the runtime is a number a bar can show: the events go through the pawn's
/// arrays and the chains the session would send, and the values come back out of the chains.
static void test_client_never_sees_negative_stamina(const Rules& r) {
    std::printf("what the client reads back\n");
    Pawn p;
    const bs::PlayerSetup& setup = p.pb.setup();
    auto st = stats_of(setup);
    st[content::kGuidStamina] = 101;
    st[content::kGuidMaxStamina] = 101;
    Sim s(r.rules, st, 1000);
    s.move(true);
    s.trigger(content::kGuidSprint, 0);
    s.run_to(20.5);
    float lowest = 1000, last = -1;
    size_t chains = 0;
    bool all_applied = true;
    for (const AbilityEvent& e : s.log) {
        if (e.change.empty()) continue;
        const auto chain = p.update(e.change);
        all_applied = all_applied && chain.has_value();
        ++chains;
        for (const auto& item : items_of(chain, setup.guids.stats, "UAoCStatsComponent", "StatRepInt32Owner")) {
            const StatInt32Inst* v = item_value(item.values, 1) ? item_value(item.values, 1)->get_if<StatInt32Inst>() : nullptr;
            if (!v || v->record != content::kGuidStamina) continue;
            last = std::bit_cast<float>(v->value);
            lowest = std::min(lowest, last);
        }
    }
    CHECK(all_applied && chains > 80 && lowest == 0.0f && last == 0.0f,
          "a bar of 101 run down through the pawn: every chain applies, and the lowest Stamina the client reads is 0 (never -0.25)");
}

// ============================================================================================================================
//  the real content.db
// ============================================================================================================================

static void test_real_content_db(const Rules& fixture) {
    std::printf("the real content.db (AOC_CONTENT_DB)\n");
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (!env || !*env) {
        std::printf("  [skip] AOC_CONTENT_DB is not set (cd services && go run ./cmd/contentc; AOC_CONTENT_DB=dist\\content\\content.db)\n");
        return;
    }
    std::string error;
    auto db = content::ContentDb::open(env, error);
    CHECK(db.has_value(), "the real content.db opens");
    if (!db) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    const auto rules = db->ability_rules(error);
    CHECK(rules.has_value(), "and its ability rules read");
    if (!rules) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    const auto same_ability = [](const content::AbilityRule& a, const content::AbilityRule& b) {
        return a.guid == b.guid && a.name == b.name && a.held == b.held && a.requires_out_of_combat == b.requires_out_of_combat &&
               a.min_stamina == b.min_stamina && a.cost_stat_guid == b.cost_stat_guid && a.cost_amount == b.cost_amount &&
               a.cost_requires_moving == b.cost_requires_moving && a.charge_interval == b.charge_interval && a.cooldown == b.cooldown &&
               a.triggers_cooldown == b.triggers_cooldown && a.disable_during_cooldown == b.disable_during_cooldown && a.targets_self == b.targets_self &&
               a.effect_guid == b.effect_guid && a.montage_path == b.montage_path && a.cancel_section == b.cancel_section;
    };
    CHECK(same_ability(rules->sprint, fixture.rules.sprint) && same_ability(rules->dodge, fixture.rules.dodge) && same_ability(rules->rest, fixture.rules.rest),
          "Sprint, Dodge and Rest are the numbers the other tests use (the fixture is contentc's output of the same rows)");
    bool same_effects = rules->effects.size() == fixture.rules.effects.size();
    for (const auto& [guid, e] : fixture.rules.effects) {
        const content::EffectRule* o = rules->effect(guid);
        same_effects = same_effects && o && o->name == e.name && o->infinite == e.infinite && o->duration == e.duration && o->tick_timer == e.tick_timer &&
                       o->stops_regen_standing == e.stops_regen_standing && o->stops_regen_moving == e.stops_regen_moving &&
                       o->end_effect_guid == e.end_effect_guid && o->end_effect_duration == e.end_effect_duration && o->stat_mods.size() == e.stat_mods.size();
        for (size_t i = 0; o && i < e.stat_mods.size() && i < o->stat_mods.size(); ++i) {
            same_effects = same_effects && o->stat_mods[i].stat_guid == e.stat_mods[i].stat_guid && o->stat_mods[i].source_stat_guid == e.stat_mods[i].source_stat_guid &&
                           o->stat_mods[i].factor == e.stat_mods[i].factor && o->stat_mods[i].addend == e.stat_mods[i].addend;
        }
    }
    CHECK(same_effects && rules->stat_regen_time == 1.0 && rules->regen_rate == 5.0, "so are the four effects and their stat mods, and the regeneration (5 every 1 s)");
// [C_Y_B ~ C-O-A]

    // the whole Sprint on the real rules: 20 s to empty, 20 s back
    Sim s(*rules);
    s.move(true);
    s.trigger(content::kGuidSprint, 0);
    s.run_to(20.0);
    CHECK(!s.rt.sprinting() && s.rt.stamina() == 0.0 && s.ends() == std::vector<int32_t>{0}, "a Sprint on the real content.db empties the bar in 20 s and ends with ClientEndAbility");
    s.move(false);
    s.run_to(40.0);
    CHECK(s.rt.stamina() == 100.0, "and the bar is full again 20 s later");
}

/// Plan B3: the gear changes a stat the runtime holds (MaxStamina, the regeneration and MoveSpeedMult an effect raises, the maxima they scale from).
static void test_gear_stats(const Rules& r) {
    std::printf("gear: a change of a stat the runtime holds\n");
    constexpr int64_t kStrength = 109183598614138ll;
    {
        Sim s(r.rules);
        CHECK(s.rt.tracks(content::kGuidMaxStamina) && s.rt.tracks(kHealthRegen) && s.rt.tracks(kManaRegen) && s.rt.tracks(kMoveSpeedMult) && s.rt.tracks(kMaxHealth) &&
                  s.rt.tracks(kMaxMana),
              "the runtime holds MaxStamina, the two regenerations, MoveSpeedMult and the two maxima the Rest scales from");
        CHECK(!s.rt.tracks(content::kGuidStamina) && !s.rt.tracks(kStrength) && !s.rt.tracks(0), "it does not hold Stamina (it is the runtime's own, not the gear's) or an attribute");
        bs::AbilityChange none;
        s.rt.gear_stat(none, content::kGuidStamina, 10);
        CHECK(none.empty() && s.rt.stamina() == 100, "a gear value for Stamina is ignored: the runtime's current value is its own");
        // MaxStamina up: no clamp
        bs::AbilityChange up;
        s.rt.gear_stat(up, content::kGuidMaxStamina, 150);
        CHECK(up.stats.size() == 1 && up.stats[0].record == content::kGuidMaxStamina && up.stats[0].value == 150.0f && s.rt.max_stamina() == 150 && s.rt.stamina() == 100,
              "MaxStamina 100 -> 150: the change names MaxStamina only, Stamina stays 100");
        // down below the current Stamina: clamped, in the same change
        bs::AbilityChange down;
        s.rt.gear_stat(down, content::kGuidMaxStamina, 60);
        CHECK(down.stats.size() == 2 && down.stats[0].record == content::kGuidMaxStamina && down.stats[0].value == 60.0f && down.stats[1].record == content::kGuidStamina &&
                  down.stats[1].value == 60.0f && s.rt.stamina() == 60 && s.rt.max_stamina() == 60,
              "MaxStamina 150 -> 60 with Stamina 100: Stamina is clamped to 60 and says so in the change");
        s.trigger(content::kGuidDodge, 0);   // 50 of the 60: the runtime goes on with its own value
        CHECK(s.rt.stamina() == 10, "(the runtime spends and regenerates from the clamped value)");
        // a maximum the Rest scales from: held, so the Rest's raise reads the new one
        bs::AbilityChange maxhp;
        s.rt.gear_stat(maxhp, kMaxHealth, 190);
        CHECK(maxhp.stats.size() == 1 && maxhp.stats[0].value == 190.0f && s.rt.stat(kMaxHealth) == 190, "MaxHealth 190 is the runtime's too");
    }
    {   // the Rest is raising the regeneration when the gear changes its base: the raise stays on top, and the effect ends on the new base
        Sim s(r.rules);
        s.trigger(content::kGuidRest, 0);
        s.run_to(2.0);
        CHECK(s.rt.rest_effect_on() && std::fabs(s.rt.stat(kHealthRegen) - 2.5) < 1e-6, "(the Rest effect is on: the regeneration is 0.6 + 0.01 x 90 + 1 = 2.5)");
        bs::AbilityChange c;
        s.rt.gear_stat(c, kHealthRegen, 0.9);   // the gear moved the base from 0.6 to 0.9
        CHECK(c.stats.size() == 1 && c.stats[0].record == kHealthRegen && std::fabs(c.stats[0].value - 2.8f) < 1e-5 && std::fabs(s.rt.stat(kHealthRegen) - 2.8) < 1e-5,
              "the base 0.6 -> 0.9 under the Rest: the pawn shows 2.8, the raise of 1.9 stays on top");
        s.cancel(0);
        s.run_to(2.1);
        CHECK(!s.rt.rest_effect_on() && std::fabs(s.rt.stat(kHealthRegen) - 0.9) < 1e-9, "when the effect ends the regeneration is the new base 0.9, not the old 0.6");
        const bs::AbilityChange& end = s.log.back().change;
        bool back = false;
        for (const bs::StatChange& sc : end.stats) back = back || (sc.record == kHealthRegen && std::fabs(sc.value - 0.9f) < 1e-6);
        CHECK(back, "and the update that ends the effect says 0.9");
    }
    {   // a Sprint raises MoveSpeedMult; the gear moves its base
        Sim s(r.rules);
        s.move(true);
        s.trigger(content::kGuidSprint, 0);
        s.run_to(0.5);
        CHECK(s.rt.sprinting() && std::fabs(s.rt.stat(kMoveSpeedMult) - 1.2) < 1e-9, "(the Sprint is on: MoveSpeedMult 1.2)");
        bs::AbilityChange c;
        s.rt.gear_stat(c, kMoveSpeedMult, 1.1);
        CHECK(std::fabs(s.rt.stat(kMoveSpeedMult) - 1.3) < 1e-9, "a gear speed of 1.1 under the Sprint shows 1.3");
        s.cancel(0);
        CHECK(std::fabs(s.rt.stat(kMoveSpeedMult) - 1.1) < 1e-9, "and the Sprint ends on 1.1");
    }
}

int main() {
    const Rules r;
    test_rules(r);
    test_sprint(r);
    test_regeneration(r);
    test_dodge(r);
    test_dodge_cooldown_edge(r);
    test_clock_never_goes_back(r);
    test_dodge_items(r);
    test_rest(r);
    test_other_abilities(r);
    test_mounted(r);
    test_instances(r);
    test_rest_golden();
    test_gear_stats(r);
    test_stamina_delta();
    test_bucket_stat_changes();
    test_cooldown_item();
    test_end_ability();
    test_apply_refusals();
    test_retail_replies(r);
    test_out_of_stamina_reply(r);
    test_client_never_sees_negative_stamina(r);
    test_real_content_db(r);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

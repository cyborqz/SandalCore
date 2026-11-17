// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/mounts_regeneration_tests.inc: a mount's own regeneration on the controller alone (test_worldd_mounts.cpp includes it after its Rig).  The session
//  calls `regenerate` every tick with its rider's fight and then `advance`; these tests do the same on a controller: the rate is the Mount_Base rating at the
//  owner's level / 250 (five times out of combat), a tick a second from the open, a ceiling at the maximum the mount opened with, at most five ticks after a stall,
//  and nothing while the mount dissolves, dies or is not open yet.
// ============================================================================
#include "spawns/mount_state.h"

namespace {

constexpr int64_t kRegenerationRating = 109315114489464ll;

double mount_health_of(const Rig& rig) {
    const world::WorldActor* m = rig.mount();
    const std::optional<double> v = m ? spawns::stat_value(*m, content::kGuidHealth) : std::nullopt;
    return v.value_or(-1.0);
}

void set_mount_health(Rig& rig, double value) {
    rig.registry.modify(rig.mc->actor_id(), world::Change::State,
                        [value](world::WorldActor& a) { spawns::set_stat_value(a, content::kGuidHealth, static_cast<float>(value)); });
}

/// The session's tick of the mount from `from` to `to` every 50 ms: the regeneration, then what is due.
void tick_mount(Rig& rig, double from, double to, bool in_combat) {
    for (double t = from; t <= to + 1e-9; t += 0.05) {
        rig.mc->regenerate(t, in_combat);
        rig.mc->advance(t);
    }
}

/// A mount of `item` summoned for an owner of `level` with the Health its item kept, run until it is out (it opens 0.104 s after the request).
bool summon_out(Rig& rig, const char* item, int level, double health, double at) {
    const content::MountRow* row = row_named(rig.book, item);
    if (!row) return false;
    MountController::Request r = rig.request(*row);
    r.owner_level = level;
    r.health = health;
    if (rig.mc->summon(r, at) != MountController::Outcome::Started) return false;
    tick_mount(rig, at, at + 0.9, false);
    return rig.mc->phase() == MountController::Phase::Out && rig.mount() != nullptr;
}

}  // namespace

static void test_regeneration(const spawns::MountBook& book) {
    std::printf("a mount's regeneration: the rate, the ceiling, the stall, and when it stops\n");
    // the game's rate at the owner's levels the mounts are used at: the Mount_Base rating / 250, five times out of combat (level 25: 9.75 and 48.75)
    {
        Rig rig(book);
        const double t0 = 100.0;
        CHECK(summon_out(rig, "Mount_Grounded_TerrorBird_0", 25, 1000.0, t0), "(a terror bird is out for a level-25 owner with 1000 of its 4875 Health)");
        if (!rig.mount()) return;
        CHECK(mount_health_of(rig) == 1000.0, "it opens at the Health its item kept");
        rig.mc->regenerate(t0 + 0.5, false);
        CHECK(mount_health_of(rig) == 1000.0, "and nothing rises in the first second after it opened");
        tick_mount(rig, t0 + 0.95, t0 + 1.5, false);
        CHECK(mount_health_of(rig) == 1048.75, "the first second: one tick of 48.75 out of combat");
        tick_mount(rig, t0 + 1.55, t0 + 4.5, false);
        CHECK(mount_health_of(rig) == 1195.0, "three seconds more: three more ticks, 48.75 each (a tick a second, never more than once for a second)");
        tick_mount(rig, t0 + 4.55, t0 + 6.5, true);
        CHECK(mount_health_of(rig) == 1214.5, "in the rider's fight two seconds are two ticks of 9.75");
        // a stall owes at most five ticks (a 94 s gap): the player's own regeneration has the same bound
        set_mount_health(rig, 1000.0);
        rig.mc->regenerate(t0 + 100.0, false);
        CHECK(mount_health_of(rig) == 1243.75, "after a stall of 94 seconds five ticks come, not ninety-four");
        // a ceiling: the maximum the mount opened with
        set_mount_health(rig, 4870.0);
        rig.mc->regenerate(t0 + 102.5, false);
        CHECK(mount_health_of(rig) == 4875.0, "the Health stops at the maximum it opened with (4875)");
        rig.mc->regenerate(t0 + 110.0, false);
        CHECK(mount_health_of(rig) == 4875.0, "...and goes no further");
    }
    // the rate at other owner levels, from the data's table (level 1: 5, level 20: 35.72, level 50: 121.36 out of combat)
    {
        const struct { int level; double out_of_combat; } levels[] = {{1, 5.0}, {20, 35.72}, {50, 121.36}};
        for (const auto& l : levels) {
            Rig rig(book);
            const double t0 = 100.0;
            CHECK(summon_out(rig, "Mount_Grounded_Horse_0", l.level, 100.0, t0), ("(a horse is out for an owner of level " + std::to_string(l.level) + ")").c_str());
            if (!rig.mount()) continue;
            tick_mount(rig, t0 + 0.95, t0 + 1.5, false);
            CHECK(std::fabs(mount_health_of(rig) - (100.0 + l.out_of_combat)) < 0.01,
                  ("level " + std::to_string(l.level) + ": one tick is " + std::to_string(l.out_of_combat) + " out of combat, the table's rating / 250 x 5").c_str());
            const content::MountRow* row = row_named(book, "Mount_Grounded_Horse_0");
            CHECK(row && std::fabs(book.health_regeneration(*row, l.level, false) - l.out_of_combat) < 1e-9 &&
                      std::fabs(book.health_regeneration(*row, l.level, true) - l.out_of_combat / 5.0) < 1e-9,
                  "...and the book's rate agrees, out of combat and in it");
        }
    }
    // the item's Health factor scales the maximum only: the data's rating is the Mount_Base's for every mount
    {
        Rig rig(book);
        const double t0 = 100.0;
        CHECK(summon_out(rig, "Mount_Flying_PTR_ONLY", 25, 10000.0, t0), "(the PTR Eagle Gryphon, 500 % Health, is out for a level-25 owner with 10000 of its 24375)");
        if (!rig.mount()) return;
        tick_mount(rig, t0 + 0.95, t0 + 1.5, false);
        CHECK(mount_health_of(rig) == 10048.75, "a flyer's tick is the same 48.75 as a ground mount's: the factor is not in the rate");
        set_mount_health(rig, 24350.0);
        rig.mc->regenerate(t0 + 5.0, false);
        CHECK(mount_health_of(rig) == 24375.0, "and it stops at the flyer's own maximum (24375)");
    }
    // a mount that dissolves keeps the Health the desummon saved: nothing is regenerated, and the actor is gone at 3.2 s
    {
        Rig rig(book);
        const double t0 = 100.0;
        CHECK(summon_out(rig, "Mount_Grounded_TerrorBird_0", 25, 1000.0, t0), "(a terror bird is out, hurt)");
        if (!rig.mount()) return;
        CHECK(rig.mc->desummon(t0 + 2.0) && rig.mc->phase() == MountController::Phase::Desummoning, "(it is dismissed)");
        tick_mount(rig, t0 + 2.0, t0 + 3.5, false);
        CHECK(rig.mount() != nullptr && mount_health_of(rig) == 1000.0, "while it dissolves its Health does not rise");
        tick_mount(rig, t0 + 3.55, t0 + 6.0, false);
        CHECK(rig.mount() == nullptr, "(and it is gone)");
        rig.mc->regenerate(t0 + 10.0, false);
        CHECK(rig.mount() == nullptr, "a regeneration with no mount out does nothing");
    }
    // a mount that dies stays at 0
    {
        Rig rig(book);
        const double t0 = 100.0;
        CHECK(summon_out(rig, "Mount_Grounded_TerrorBird_0", 25, 1000.0, t0), "(a terror bird is out)");
        if (!rig.mount()) return;
        set_mount_health(rig, 0.0);
        CHECK(rig.mc->die(t0 + 2.0, 2'000'000'000) && rig.mc->phase() == MountController::Phase::Dying, "(its Health reached 0: it died)");
        tick_mount(rig, t0 + 2.0, t0 + 3.0, false);   // (it closes 1.5 s after it died)
        CHECK(rig.mount() != nullptr && mount_health_of(rig) == 0.0, "a dying mount's Health stays 0");
    }
    // before the mount is open there is nothing to regenerate, and a summon that is being cancelled leaves nothing behind
    {
        Rig rig(book);
        const double t0 = 100.0;
        const content::MountRow* row = row_named(book, "Mount_Grounded_TerrorBird_0");
        MountController::Request r = rig.request(*row);
        r.owner_level = 25;
        r.health = 1000.0;
        CHECK(row && rig.mc->summon(r, t0) == MountController::Outcome::Started, "(a summon starts)");
        rig.mc->regenerate(t0 + 0.05, false);
        CHECK(rig.mount() == nullptr && rig.registry.size() == 0, "a regeneration before the mount has opened makes nothing");
        CHECK(book.health_regeneration_rating(*row, 25) == 2437.5 && book.data().stat_at(25, kRegenerationRating, -1) == 2437.5, "(the rating at level 25 is 2437.5)");
    }
}

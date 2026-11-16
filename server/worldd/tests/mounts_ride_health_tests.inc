// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/mounts_ride_health_tests.inc: the mount's Health after it was hurt: the game's regeneration (the Mount_Base rating at the owner's level,
//  rating / 250 a second, five times out of combat) and a player's heal on his own mount.  Registered with AOC_REGISTER_TEST (tests/README.md).
// ============================================================================
#include <bit>

static constexpr int64_t kQuickWit = 6064630736949673984ll;
static constexpr int64_t kBardQuickWitApply = 6064632501410136576ll;   // Quick Wit's heal hit (its damage hit is the enemy half)
static constexpr int64_t kHealthRegenerationRating = 109315114489464ll;
#define HEALTH_FLUSH() std::fflush(stdout)

static svc::Character cyb25() {
    svc::Character c = cyb_record();
    c.level = 25;
    return c;
}

/// The mount's Health and MaxHealth as the server holds them (the EveryoneProxy stat items).
static double mount_health(const Harness& h) {
    const world::WorldActor* m = h.mount();
    const std::optional<double> v = m ? spawns::stat_value(*m, content::kGuidHealth) : std::nullopt;
    return v.value_or(-1.0);
}

static double mount_max_health(const Harness& h) {
    const world::WorldActor* m = h.mount();
    const std::optional<double> v = m ? spawns::stat_value(*m, content::kGuidMaxHealth) : std::nullopt;
    return v.value_or(-1.0);
}

/// The Health item of a stats component block a chain carries (the value the client's bar reads), if that chain has one.
static std::optional<double> shown_health(const fake::Delivered& d, const repl::Actor& actor) {
    const States st = states_of(d, actor);
    for (const auto& [id, o] : st) {
        if (!o.schema || std::string(o.schema->name) != "UAoCStatsComponent") continue;
        const schema::NetField* f = o.schema->field("StatRepInt32EveryoneProxy");
        const repl::FastArrayState* a = f ? o.arrays.find(f->index) : nullptr;
        if (!a) continue;
        for (const repl::FastArrayItem& it : a->items()) {
            const auto v = it.values.values.find(1);
            const auto* t = v != it.values.values.end() ? v->second.get_if<repl::StatInt32Inst>() : nullptr;
            if (t && t->record == content::kGuidHealth) return std::bit_cast<float>(t->value);
        }
    }
    return std::nullopt;
}

/// The last Health the client was shown for the mount from `from` on.
static std::optional<double> health_on_the_wire(const Harness& h, size_t from) {
    std::optional<double> seen;
    const world::WorldActor* m = h.mount();
    if (!m) return seen;
    for (const fake::Delivered* d : chains_on(h, h.mount_ch(), from)) {
        if (const auto v = shown_health(*d, m->actor)) seen = v;
    }
    return seen;
}

/// The game's HealthRegeneration for the mount of the test at the owner's level: the Mount_Base rating / 250, five times out of combat.
static double mount_regeneration_out_of_combat(const Harness& h, int owner_level) {
    const spawns::MountBook* book = h.fx.world->mounts();
    if (!book) return 0;
    return book->data().stat_at(owner_level, kHealthRegenerationRating) / 250.0 * 5.0;
}

/// A flat walking map for a world of the real content (the heal test): its adoption starts the fights, as the world's own load_ground does.
static bool adopt_flat_ground(PlayerWorld& w) {
    std::string err;
    auto nav = aoc::worldd::nav::AreaNav::from_file(aoc::worldd::navfix::build_file(), &err);
    if (!nav || !w.adopt_ground(std::move(nav), err)) {
        std::printf("  [FAIL] the flat ground: %s\n", err.c_str());
        return false;
    }
    return true;
}

/// A ServerTriggerAbility whose Targets names `target` (a mount's actor), on the pawn's ability component.
static BunchWriter trigger_at_target(const emit::FIntrepidNetworkGUID& component, int64_t ability, int32_t client_instance, const emit::FIntrepidNetworkGUID& target) {
    const schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(ability));
    if (ok && client_instance != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(client_instance));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = repl::Value(target);
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    if (!ok) {
        std::printf("  [FAIL] the targeted trigger's parameters do not fit\n");
        return BunchWriter();
    }
    BunchWriter out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, p, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] ServerTriggerAbility (a mount target) does not write\n");
        return BunchWriter();
    }
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = component;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// M3: a mount hurt, dismissed and summoned again keeps the Health its item saved, and once it is out it regenerates at the game's rate.
static void test_mount_regeneration() {
    std::printf("a hurt mount, dismissed and summoned again: its saved Health, then the game's regeneration\n");
    HEALTH_FLUSH();
    Harness h(cyb25());
    h.play();
    CHECK(ride_on(h), "(the player rides the mount)");
    if (!h.s->mounted()) return;
    const double max_health = mount_max_health(h);
    CHECK(max_health == 4875, "(at level 25 the mount's maximum is 4875)");
    const double before_damage = mount_health(h);
    h.s->take_damage(h.conn, 1000.0, h.t);
    h.run(milliseconds(100));
    CHECK(std::fabs(mount_health(h) - (before_damage - 1000.0)) < 0.01, "(1000 damage came off the mount's Health)");
    const double saved = mount_health(h);
    get_off(h);
    h.run(milliseconds(200));
    desummon_with(h, kMountSlot);
    h.run(milliseconds(3400));
    const svc::InventoryItem* item = h.s->inventory().at(Bag, kMountSlot);
    CHECK(item && item->persisted_health && std::fabs(*item->persisted_health - saved) < 0.01f, "dismissed, the item keeps the Health the mount had");
    // the summon starts at the item's Health: the first regeneration tick is a second after the open
    summon_with(h, kMountSlot, 21);
    h.run(milliseconds(600));
    CHECK(h.mount() != nullptr && std::fabs(mount_health(h) - saved) < 0.01, "summoned again, the mount starts at the saved Health");
    CHECK(h.mount() && h.s->mounts()->phase() == MountController::Phase::Out, "(and it is out)");
    const double before = mount_health(h);
    const double regen = mount_regeneration_out_of_combat(h, 25);
    CHECK(std::fabs(regen - 48.75) < 1e-9, "(the data's rate at level 25 is 48.75 a second out of combat)");
    const size_t from = h.got.size();
    h.run(milliseconds(3000));
    const double after = mount_health(h);
    const double ticks = (after - before) / regen;
    CHECK(ticks >= 2 - 1e-6 && ticks <= 4 + 1e-6 && std::fabs(ticks - std::round(ticks)) < 1e-3,
          "three seconds later its Health rose by a whole number of 48.75 ticks (2 to 4)");
    const auto shown = health_on_the_wire(h, from);
    CHECK(shown && std::fabs(*shown - after) < 0.01, "the client is shown the regenerated Health (the value its bar reads)");
    // the maximum is a ceiling: five points under it, four seconds of ticks heal it to the maximum and no further
    if (!h.mount()) return;
    h.fx.world->registry().modify(h.mount()->id, world::Change::State,
                                  [](world::WorldActor& a) { spawns::set_stat_value(a, content::kGuidHealth, 4870.0f); });
    h.run(milliseconds(2000));
    CHECK(mount_health(h) == 4875, "the regeneration stops at the mount's maximum (4875)");
    h.run(milliseconds(2000));
    CHECK(mount_health(h) == 4875, "...and goes no further");
}

/// M1/M3: a Health saved when the mount was smaller (at an earlier level's maximum) is kept as an absolute value, and regenerates on the new one.
static void test_mount_health_across_a_level_up() {
    std::printf("a mount's saved Health from a lower level, summoned at 25\n");
    HEALTH_FLUSH();
    svc::Character rec = cyb25();
    for (svc::InventoryItem& it : rec.items) {
        if (it.container == Bag && it.slot == kMountSlot) it.persisted_health = 250.0f;   // half of the level-1 maximum, 500
    }
    Harness h(rec);
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(600));
    CHECK(h.mount() != nullptr && mount_max_health(h) == 4875, "(summoned at level 25, the maximum is 4875)");
    CHECK(mount_health(h) == 250, "the item's absolute Health (250) is the mount's, a twentieth of the new maximum");
    const double before = mount_health(h);
    const double regen = mount_regeneration_out_of_combat(h, 25);
    h.run(milliseconds(3000));
    const double ticks = (mount_health(h) - before) / regen;
    CHECK(ticks >= 2 - 1e-6 && ticks <= 4 + 1e-6 && std::fabs(ticks - std::round(ticks)) < 1e-3, "and it regenerates at the owner's level's rate all the same");
}

/// M3: two mount items of one player keep their own Health; a regenerating one does not touch the other's.
static void test_two_mounts_keep_their_health() {
    std::printf("two mounts of one player: each item keeps its own Health\n");
    HEALTH_FLUSH();
    Harness h(cyb25());
    h.play();
    CHECK(ride_on(h, kMountSlot), "(the player rides the first mount)");
    const double first_before_damage = mount_health(h);
    h.s->take_damage(h.conn, 1000.0, h.t);
    h.run(milliseconds(100));
    const double first_saved = first_before_damage - 1000.0;
    get_off(h);
    h.run(milliseconds(200));
    desummon_with(h, kMountSlot);
    h.run(milliseconds(3400));
    CHECK(ride_on(h, kOtherMountSlot), "(the player rides the other mount)");
    const double other_max = mount_max_health(h);
    CHECK(other_max == 4875 && mount_health(h) == other_max, "(the other mount was never hurt: full at 4875)");
    h.s->take_damage(h.conn, 500.0, h.t);
    h.run(milliseconds(100));
    const double second_saved = other_max - 500.0;
    get_off(h);
    h.run(milliseconds(200));
    desummon_with(h, kOtherMountSlot);
    h.run(milliseconds(3400));
    const svc::InventoryItem* first = h.s->inventory().at(Bag, kMountSlot);
    const svc::InventoryItem* second = h.s->inventory().at(Bag, kOtherMountSlot);
    CHECK(first && first->persisted_health && std::fabs(*first->persisted_health - first_saved) < 0.01f, "the first item keeps the Health it had");
    CHECK(second && second->persisted_health && std::fabs(*second->persisted_health - second_saved) < 0.01f, "the second keeps its own");
    summon_with(h, kMountSlot, 31);
    h.run(milliseconds(600));
    CHECK(std::fabs(mount_health(h) - first_saved) < 0.01, "the first summoned again starts at its own Health, not the second's");
    const double before = mount_health(h);
    const double regen = mount_regeneration_out_of_combat(h, 25);
    h.run(milliseconds(3000));
    const double ticks = (mount_health(h) - before) / regen;
    CHECK(ticks >= 2 - 1e-6 && ticks <= 4 + 1e-6 && std::fabs(ticks - std::round(ticks)) < 1e-3, "and it regenerates while the second item's Health stays where it was");
    const svc::InventoryItem* after = h.s->inventory().at(Bag, kOtherMountSlot);
    CHECK(after && after->persisted_health && std::fabs(*after->persisted_health - second_saved) < 0.01f, "(the second item's saved Health did not move)");
}

/// M3: a reconnect (the session's end, then a new login with what the saver holds) keeps the mount's Health, and it regenerates.
static void test_reconnect_keeps_the_mount_health() {
    std::printf("a reconnect with a hurt mount: its saved Health, then the regeneration\n");
    HEALTH_FLUSH();
    Harness h(cyb25());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    const double before_damage = mount_health(h);
    h.s->take_damage(h.conn, 1000.0, h.t);
    h.run(milliseconds(100));
    const double saved_health = before_damage - 1000.0;
    get_off(h);
    h.run(milliseconds(200));
    h.s->finish("the test");
    const svc::InventoryItem* saved = h.s->inventory().at(Bag, kMountSlot);
    CHECK(saved && saved->persisted_health && std::fabs(*saved->persisted_health - saved_health) < 0.01f, "the session's end saves the mount's Health");
    svc::Character rec = cyb25();
    rec.items = h.s->inventory().items();
    Harness again(rec);
    again.play();
    summon_with(again, kMountSlot, 41);
    again.run(milliseconds(600));
    CHECK(again.mount() != nullptr && std::fabs(mount_health(again) - saved_health) < 0.01, "the new session's mount starts at the saved Health");
    const double before = mount_health(again);
    const double regen = mount_regeneration_out_of_combat(again, 25);
    again.run(milliseconds(3000));
    const double ticks = (mount_health(again) - before) / regen;
    CHECK(ticks >= 2 - 1e-6 && ticks <= 4 + 1e-6 && std::fabs(ticks - std::round(ticks)) < 1e-3, "and it regenerates in the new session");
}

/// M3: a dying mount does not regenerate (its Health stays 0; the item keeps 0 and its lock).
static void test_a_dying_mount_does_not_regenerate() {
    std::printf("a mount killed: no regeneration while it dies\n");
    HEALTH_FLUSH();
    Harness h(cyb25());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    HEALTH_FLUSH();
    h.s->take_damage(h.conn, 10000.0, h.t);
    HEALTH_FLUSH();
    h.run(milliseconds(300));
    HEALTH_FLUSH();
    CHECK(h.s->mounts()->phase() == MountController::Phase::Dying && mount_health(h) == 0, "(the mount died, its Health is 0)");
    HEALTH_FLUSH();
    h.run(milliseconds(700));
    HEALTH_FLUSH();
    CHECK(h.mount() != nullptr && mount_health(h) == 0, "a second later, still dying, its Health is still 0 (no regeneration)");
    HEALTH_FLUSH();
    const svc::InventoryItem* item = h.s->inventory().at(Bag, kMountSlot);
    CHECK(item && item->persisted_health && *item->persisted_health == 0.0f && item->lock_expiration > 0, "(the item keeps Health 0 and its lock)");
    HEALTH_FLUSH();
}

/// A mount that has appeared stays through a fight (the controller's own test covers before and after; here on a session).
static void test_a_mount_survives_a_fight() {
    std::printf("a mount summoned into a fight stays\n");
    HEALTH_FLUSH();
    Harness h(cyb25());
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(300));
    CHECK(h.mount() != nullptr, "(the mount is open)");
    HEALTH_FLUSH();
    h.s->take_damage(h.conn, 10.0, h.t);   // a hit on the player after the mount opened
    h.run(milliseconds(1600));
    CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Out, "the hit does not dismiss it");
    HEALTH_FLUSH();
}

/// M3: a player's heal names his own mount and the mount's Health rises (the data's ally heal, not a mount-specific one).
static void test_player_heals_his_mount() {
    std::printf("a player's heal reaches his own mount\n");
    HEALTH_FLUSH();
    svc::Character rec = cyb25();
    rec.has_position = true;
    rec.position = {0, 0, 1098, 0};   // the flat fixture's ground is at 1,000: a player's origin is 98 cm above its feet
    Harness h(rec, [](PlayerWorld& w) { (void)adopt_flat_ground(w); }, nullptr, t0, /*real_content=*/true);
    HEALTH_FLUSH();
    h.play();
    HEALTH_FLUSH();
    CHECK(h.fx.world->combat() != nullptr, "(the real tables and a ground: the world's fights are on)");
    CHECK(h.s->pipeline() != nullptr && h.s->pipeline()->handles(kQuickWit) && h.s->pipeline()->known(kQuickWit), "the level-25 Bard knows Quick Wit");
    if (!h.fx.world->combat()) return;
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    HEALTH_FLUSH();
    CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Out, "(the mount is summoned)");
    if (!h.mount()) {
        CHECK(false, "(no mount stands for the heal test to hurt)");
        return;
    }
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(the player rides it)");
    const double before_damage = mount_health(h);
    h.s->take_damage(h.conn, 1000.0, h.t);
    h.run(milliseconds(100));
    get_off(h);
    h.run(milliseconds(200));
    HEALTH_FLUSH();
    const double before = mount_health(h);
    const double max_health = mount_max_health(h);
    CHECK(std::fabs(before - (before_damage - 1000.0)) < 0.01 && before > 0 && before < max_health && max_health == 4875,
          "(the mount stands hurt, below its 4875 maximum)");
    const emit::FIntrepidNetworkGUID target = h.mount()->actor.guid;
    const size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_at_target(h.s->bootstrap().setup().guids.ability, kQuickWit, 61, target), true)});
    HEALTH_FLUSH();
    h.run(milliseconds(2200));
    HEALTH_FLUSH();
    const double after = mount_health(h);
    CHECK(after > before && after <= max_health, "the healed Health is up and stops at the mount's maximum");
    // the confirmation is the heal's own hit on the mount, with its Health number (not the damage half of Quick Wit, which cannot reach an ally)
    bool heal_confirmed = false;
    size_t confirmations = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientHitConfirmationsReliable") continue;
            ++confirmations;
            const auto parsed = repl::parse_hit_confirmations(c);
            if (!parsed) continue;
            for (const repl::HitInstance& hit : parsed->hits) {
                if (hit.record_guid != kBardQuickWitApply) continue;
                for (const repl::HitTarget& t : hit.targets) {
                    if (!(t.hit_comp == target)) continue;
                    for (const repl::HitStatMod& m : t.stat_mods) heal_confirmed = heal_confirmed || m.value > 0;
                }
            }
        }
    }
    CHECK(confirmations >= 1 && heal_confirmed, "the client is sent the heal's hit on the mount, with its Health number");
    const auto shown = health_on_the_wire(h, from);
    CHECK(shown && std::fabs(*shown - after) < 0.01, "and the mount's healed Health is what its bar is shown");
    HEALTH_FLUSH();
}

/// A level-25 Bard on the flat fixture ground (the real content, the world's fights on), standing at the world's origin.
static std::unique_ptr<Harness> flat_ground_bard() {
    svc::Character rec = cyb25();
    rec.has_position = true;
    rec.position = {0, 0, 1098, 0};   // the fixture's ground is at 1,000: a player's origin is 98 cm above its feet
    return std::make_unique<Harness>(rec, [](PlayerWorld& w) { (void)adopt_flat_ground(w); }, nullptr, t0, /*real_content=*/true);
}

/// The mount summoned, ridden, hurt by `damage` and left standing with the player off it. False when a step did not come about.
static bool hurt_mount_left_standing(Harness& h, double damage) {
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    if (!h.mount() || h.s->mounts()->phase() != MountController::Phase::Out) return false;
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    if (!h.s->mounted()) return false;
    h.s->take_damage(h.conn, damage, h.t);
    h.run(milliseconds(100));
    get_off(h);
    h.run(milliseconds(200));
    return h.mount() != nullptr && !h.s->mounted();
}

/// Each Health mod (value, effective value) of Quick Wit's heal hit on `target` that the client was sent from chain `from` on.
static std::vector<std::pair<double, double>> heal_numbers(const Harness& h, size_t from, const emit::FIntrepidNetworkGUID& target) {
    std::vector<std::pair<double, double>> out;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientHitConfirmationsReliable") continue;
            const auto parsed = repl::parse_hit_confirmations(c);
            if (!parsed) continue;
            for (const repl::HitInstance& hit : parsed->hits) {
                if (hit.record_guid != kBardQuickWitApply) continue;
                for (const repl::HitTarget& t : hit.targets) {
                    if (!(t.hit_comp == target)) continue;
                    for (const repl::HitStatMod& m : t.stat_mods) out.emplace_back(m.value, m.effective_value);
                }
            }
        }
    }
    return out;
}

/// A mount that is being dismissed is no target of a heal: its item has kept the Health it had when the dissolving began, so a heal that landed on it would be lost
/// (and its number shown for nothing). Nothing raises its Health while it dissolves, neither the heal nor the regeneration.
static void test_no_heal_on_a_dissolving_mount() {
    std::printf("a heal on a mount that is being dismissed does nothing\n");
    HEALTH_FLUSH();
    auto hp = flat_ground_bard();
    Harness& h = *hp;
    h.play();
    CHECK(h.fx.world->combat() != nullptr, "(the real tables and a ground: the world's fights are on)");
    if (!h.fx.world->combat()) return;
    CHECK(hurt_mount_left_standing(h, 1000.0), "(the mount stands hurt, the player off it)");
    if (!h.mount()) return;
    const double saved = mount_health(h);
    const emit::FIntrepidNetworkGUID target = h.mount()->actor.guid;
    desummon_with(h, kMountSlot);
    h.run(milliseconds(200));
    CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Desummoning, "(the mount is being dismissed)");
    const size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_at_target(h.s->bootstrap().setup().guids.ability, kQuickWit, 61, target), true)});
    double highest = mount_health(h);
    for (int i = 0; i < 26 && h.mount() != nullptr; ++i) {   // (2.6 s: the whole flight of the cast, and the mount is gone only at 3.2 s)
        h.run(milliseconds(100));
        if (h.mount()) highest = std::max(highest, mount_health(h));
    }
    CHECK(highest <= saved + 0.01, "its Health did not rise while it dissolved: no heal, no regeneration");
    CHECK(heal_numbers(h, from, target).empty(), "and the client was sent no heal hit on it");
    const svc::InventoryItem* item = h.s->inventory().at(Bag, kMountSlot);
    CHECK(item && item->persisted_health && std::fabs(*item->persisted_health - saved) < 0.01f, "(its item keeps the Health the dissolving began with)");
    HEALTH_FLUSH();
}

/// The client names the mount the way it names an NPC (by its collision cylinder), and the heal's number is what took effect: the whole heal below the maximum,
/// and at the maximum a heal whose Value is kept with an effective value of 0 (as the retail confirmations of a heal at full Health).
static void test_heal_numbers_on_a_mount() {
    std::printf("a heal on a mount: named by its collision cylinder, its number, and at full Health\n");
    HEALTH_FLUSH();
    auto hp = flat_ground_bard();
    Harness& h = *hp;
    h.play();
    CHECK(h.fx.world->combat() != nullptr, "(the real tables and a ground: the world's fights are on)");
    if (!h.fx.world->combat()) return;
    CHECK(hurt_mount_left_standing(h, 1000.0), "(the mount stands hurt, the player off it)");
    if (!h.mount()) return;
    const emit::FIntrepidNetworkGUID actor = h.mount()->actor.guid;
    emit::FIntrepidNetworkGUID cylinder = actor;
    cylinder.ObjectId = h.mount()->npc.collision_cylinder;
    CHECK(cylinder.ObjectId != 0 && cylinder.ObjectId != actor.ObjectId, "(the mount has a collision cylinder of its own)");
    const double before = mount_health(h);
    size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_at_target(h.s->bootstrap().setup().guids.ability, kQuickWit, 61, cylinder), true)});
    h.run(milliseconds(2200));
    const auto first = heal_numbers(h, from, actor);
    CHECK(first.size() == 1 && first[0].first > 0, "named by its collision cylinder, the heal lands on the mount and its hit is confirmed with a Health number");
    if (first.size() == 1) {
        std::printf("    (Quick Wit healed %.2f of %.2f)\n", first[0].second, first[0].first);
        CHECK(std::fabs(first[0].first - first[0].second) < 0.01, "below the maximum the whole heal took effect");
        CHECK(mount_health(h) >= before + first[0].second - 0.01, "and the mount's Health rose by at least that (its regeneration adds to it)");
    }
    // the cooldown of Quick Wit is 8 s: the mount is made full and healed again
    h.run(milliseconds(8500));
    h.fx.world->registry().modify(h.mount()->id, world::Change::State,
                                  [](world::WorldActor& a) { spawns::set_stat_value(a, content::kGuidHealth, static_cast<float>(*spawns::stat_value(a, content::kGuidMaxHealth))); });
    CHECK(mount_health(h) == mount_max_health(h), "(the mount is at its maximum)");
    from = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_at_target(h.s->bootstrap().setup().guids.ability, kQuickWit, 62, actor), true)});
    h.run(milliseconds(2200));
    const auto second = heal_numbers(h, from, actor);
    CHECK(second.size() == 1 && second[0].first > 0 && second[0].second == 0, "at full Health the heal's number is kept and its effective value is 0");
    CHECK(mount_health(h) == mount_max_health(h), "and the Health stays at the maximum");
    HEALTH_FLUSH();
}

AOC_REGISTER_TEST(test_mount_regeneration);
AOC_REGISTER_TEST(test_mount_health_across_a_level_up);
AOC_REGISTER_TEST(test_two_mounts_keep_their_health);
AOC_REGISTER_TEST(test_reconnect_keeps_the_mount_health);
AOC_REGISTER_TEST(test_a_dying_mount_does_not_regenerate);
AOC_REGISTER_TEST(test_a_mount_survives_a_fight);
AOC_REGISTER_TEST(test_player_heals_his_mount);
AOC_REGISTER_TEST(test_no_heal_on_a_dissolving_mount);
AOC_REGISTER_TEST(test_heal_numbers_on_a_mount);

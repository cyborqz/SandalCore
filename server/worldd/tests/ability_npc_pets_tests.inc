// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Summoner pets: contributed by MrJambix in #13.
// ============================================================================
//  tests/ability_npc_pets_tests.inc: a Summoner's pet in the combat world: its fights, its abilities, its owner's abilities on it, Dimensional Bolt and what reacts to a hit
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_npc.cpp runs it through aoc_tests::run; it is included from ability_npc_registered.inc)
// ============================================================================

/// What every world of these tests is made from: the Blueprints' table and the Wolf's path.
struct PetTestData {
    npc::MotionTable motion;
    std::string      wolf_path;
    bool             ok = false;
    PetTestData() {
        std::string error;
        ok = motion.load(NPC_MOTION_PATH, error);
        CHECKF(ok, "npc_motion.tsv: %s", error.c_str());
        for (const auto& [p, row] : motion.rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = p;
        }
    }
};

/// The effect the first of a hit's effects puts on its target; 0 when the hit or its effects are not there (a check on that effect's stacks then fails, and the run goes on).
static int64_t first_effect_of(const abilities::Catalog& catalog, int64_t hit) {
    const abilities::HitDef* found = catalog.hit(hit);
    return found && !found->effects.empty() ? found->effects.front().effect_guid : 0;
}

void test_pet_rules() {
    PetTestData shared;
    if (!shared.ok) return;
    const npc::MotionTable& motion = shared.motion;
    const std::string& wolf_path = shared.wolf_path;
    std::string error;
std::printf("the rules of a creature's effects, from the data\n");
{
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the rules)");
    if (r.ok() && data && w.problem.empty()) {
        combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
        combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
        combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
        cw.add_player(&player);
        double now = 1.0;
        const auto run = [&](double seconds) {
            for (double t = 0; t < seconds; t += 0.1) {
                now += 0.1;
                r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));   // (the owner outlives the wolves around it)
                r.take();
                w.npcs->note_player(77, 0, 0, now);
                w.npcs->tick(now);
                cw.tick(now);
            }
        };
        const auto put = [&](uint64_t id, const char* name, double seconds) {
            effects::Applied e;
            e.def = r.catalog->effect_named(name);
            e.duration = seconds;
            CHECK(e.def != nullptr, name);
            if (e.def) cw.apply_effects(id, 77, {e}, now);
        };
        const auto hit_named = [&](int64_t guid) { return r.catalog->hit(guid); };
        const auto loss = [](double amount) {
            combat::Resolved hurt;
            hurt.mods.push_back({0, content::kGuidHealth, -amount});
            return hurt;
        };
        const auto count_of = [&](uint64_t id, const char* name) {
            const auto* def = r.catalog->effect_named(name);
            return def ? cw.effect_stacks(id, def->row.guid) : -1;
        };
        const uint64_t a = w.spawn(wolf_path, 900, 0, 25, 1.0);
        cw.fighter_of(a);
        // a trip is a crowd control a creature takes (land_effect): it lands
        put(a, "Status_Tripped", 0.5);
        CHECK(count_of(a, "Status_Tripped") == 1, "a trip lands on a creature");
        // the effect's own requirement: a Boss takes no stun
        const abilities::HitDef* stunning = nullptr;
        for (const content::AbilityHitRow& row : r.catalog->ability(6064633748157319575ll)->hits) {
            const auto* h = r.catalog->hit(row.hit_guid);
            if (h && h->row.name == "Summoner_SoulFlare_Damage") stunning = h;
        }
        CHECK(stunning != nullptr, "(Soul Flare's damage hit)");
        const uint64_t stun_target = w.spawn(wolf_path, 900, 300, 25, 1.0);
        combat::Fighter* stun_fighter = cw.fighter_of(stun_target);
        if (stunning && stun_fighter) {
            combat::Resolved gate;
            combat::HitMeta meta;
            std::string why;
            CHECK(cw.resolver().resolve_effects(player.fighter(), *stun_fighter, *stunning, meta, gate, why), "(Soul Flare's effects run)");
            CHECK(gate.effects.size() == 1 && gate.effects[0].def == r.catalog->effect_named("Status_Stunned"), "the data's Soul Flare asks for a stun");
            cw.apply_effects(stun_target, 77, gate.effects, now);
            CHECK(count_of(stun_target, "Status_Stunned") == 1, "... which lands on the creature");
        }
        // a Hex's end detonates on the creature
        const uint64_t c = w.spawn(wolf_path, 900, 600, 25, 1.0);
        cw.fighter_of(c);
        put(c, "Summoner_Hex", 1.5);
        double before = cw.health_of(c);
        run(2.0);
        std::printf("  [info] Hex: %.1f -> %.1f, Exposed %d\n", before, cw.health_of(c), count_of(c, "Status_Exposed"));
        CHECK(count_of(c, "Summoner_Hex") == 0, "a Hex of a second and a half is over");
        CHECK(cw.health_of(c) < before - 1.5, "... and its end detonates on the creature");
        // a reaction on the holder: Shocked answers the next hit with a hit of its own
        const uint64_t d = w.spawn(wolf_path, 900, -600, 25, 1.0);
        cw.fighter_of(d);
        put(d, "Status_Shocked", 6.0);
        combat::HitOrigin strike;
        strike.hit = hit_named(stunning ? stunning->row.guid : 0);
        strike.source_type = content::kSourceTypeAbility;
        before = cw.health_of(d);
        player.dealt.clear();
        cw.apply_hit_to_npc(d, 77, loss(10.0), now, 0, &strike);
        std::printf("  [info] Shocked: a hit of 10 took %.1f, %zu further hits\n", before - cw.health_of(d), player.dealt.size());
        CHECK(player.dealt.size() == 1 && before - cw.health_of(d) > 10.5, "Status_Shocked answers a hit with its own");
        // the owner's hand on its pet: Reconstruct's ticks heal it, Extract's loss starts no fight and cannot make a corpse
        const pets::Definition& guardian = pets::kDefinitions[0];
        const uint64_t pet = w.spawn(guardian.archetype, 200, 0, 25, 1.0);
        CHECK(cw.pet_summoned(pet, 77), "the pet joins its owner's side");
        combat::TargetInfo seen = cw.inspect(pet, player.fighter());
        CHECK(seen.relation != combat::Relation::Hostile, "to its owner the pet is no enemy");
        const double pet_full = cw.max_health_of(pet);
        cw.apply_hit_to_npc(pet, 77, loss(pet_full * 0.5), now);
        CHECK(std::abs(cw.health_of(pet) - pet_full * 0.5) < 1.0, "the owner's Extract takes the pet's Health");
        CHECK(!cw.ai_of(pet) || cw.ai_state(pet) == npc::ai::State::Idle, "... and the pet does not turn on its owner");
        put(pet, "Summoner_Reconstruct", 10.0);
        const double hurt_at = cw.health_of(pet);
        run(4.5);
        std::printf("  [info] Reconstruct: pet %.0f -> %.0f of %.0f\n", hurt_at, cw.health_of(pet), pet_full);
        CHECK(cw.health_of(pet) > hurt_at, "Reconstruct's ticks heal the pet");
        // Soul Flare: on the pet, its Apply hit fires the damage around it
        const uint64_t prey = w.spawn(wolf_path, 350, 0, 25, 1.0);
        cw.fighter_of(prey);
        const double prey_before = cw.max_health_of(prey);
        put(pet, "Summoner_SoulFlare", 1.0);
        run(0.2);
        std::printf("  [info] Soul Flare: creature %.0f of %.0f\n", cw.health_of(prey), prey_before);
        CHECK(cw.health_of(prey) < prey_before, "Soul Flare's Apply hit hurts what stands around the pet");
        // Brace: the shield, and the hits it takes go back at whoever struck
        const double pet_shield = cw.shield_of(pet, now);
        CHECK(cw.pet_use(pet, 77, 6064634403600334848ll, 0, guardian.reach_cm), "the pet braces");
        run(1.5);
        CHECK(cw.effect_stacks(pet, first_effect_of(*r.catalog, 6064634403610624001ll)) == 1, "Brace lands on the Guardian");
        std::printf("  [info] Brace: shield %.0f -> %.0f\n", pet_shield, cw.shield_of(pet, now));
        const uint64_t striker = w.spawn(wolf_path, 300, 0, 25, 1.0);
        cw.fighter_of(striker);
        const double striker_full = cw.max_health_of(striker);
        cw.add_threat(striker, pet, 100000.0, now);
        for (int tries = 0; tries < 10 && cw.health_of(striker) >= striker_full; ++tries) run(6.0);   // (the hit it sends back rolls to hit like any other)
        std::printf("  [info] Brace: the creature that struck %.0f of %.0f\n", cw.health_of(striker), striker_full);
        CHECK(cw.health_of(striker) < striker_full, "Brace sends the hit back at the creature");
        // Taunt: it is shouted from afar, and a creature on the owner turns on the Guardian (the others of this world leave: they stand nearer the owner)
        for (const uint64_t other : {a, stun_target, c, d, prey, striker}) {
            cw.forget(other);
            w.registry.remove(other);
        }
        const uint64_t on_owner = w.spawn(wolf_path, 1600, 0, 25, 1.0);
        cw.fighter_of(on_owner);
        cw.add_threat(on_owner, 77, 5000.0, now);
        run(0.3);
        CHECK(cw.creature_target(on_owner) == 77, "(the creature is on the owner)");
        CHECK(cw.owner_attacker(77, pet, 3000.0) == on_owner, "a Defensive pet finds what is attacking its owner");
        CHECK(cw.pet_can_act(pet), "the Guardian can act");
        CHECK(cw.pet_use(pet, 77, pets::kGuardianTaunt, on_owner, pets::kGuardianTauntReach), "the Guardian takes Taunt");
        const double guardian_x = w.registry.find(pet)->x;
        run(2.5);
        CHECK(std::abs(w.registry.find(pet)->x - guardian_x) < 200.0, "it taunts from where it stands");
        CHECK(count_of(on_owner, "Status_Demoralized") == 1, "Taunt lands at range");
        CHECK(cw.creature_target(on_owner) == pet, "the creature turns on the Guardian");
        // following: one walk after a moving owner, not a walk that stops and starts
        {
            const pets::Definition& mystic = pets::kDefinitions[2];
            const uint64_t follower = w.spawn(mystic.archetype, 0, 2500, 25, 1.0);
            auto owner_at = std::make_shared<nav::Vec3>(nav::Vec3{0, 2000, 1010});
            CHECK(w.npcs->order_pursue(follower, [owner_at](nav::Vec3& out) { out = *owner_at; return true; }, 720.0, 250.0), "the pet sets off after its owner");
            int stops = 0;
            bool was_moving = true;
            double last_y = w.registry.find(follower)->y;
            for (int i = 0; i < 40; ++i) {
                owner_at->y -= 40.0;   // (a walking owner, 400 cm a second)
                now += 0.1;
                w.npcs->note_player(77, 0, 0, now);
                w.npcs->tick(now);
                const double y = w.registry.find(follower)->y;
                const bool moving = std::abs(y - last_y) > 0.5;
                if (i > 8 && was_moving && !moving) ++stops;
                was_moving = moving;
                last_y = y;
            }
            std::printf("  [info] following a walking owner: %d stops in 4 s, %.0f cm behind\n", stops, last_y - owner_at->y);
            CHECK(stops <= 2, "a pet following a walking owner keeps walking");
            w.npcs->halt(follower);
        }
        // Lunge: the melee Hunter closes in a leap, not a run
        const pets::Definition& hunter = pets::kDefinitions[1];
        const uint64_t lunger = w.spawn(hunter.archetype, -800, 0, 25, 1.0);
        const uint64_t far_prey = w.spawn(wolf_path, -200, 0, 1, 1.0);
        cw.pet_summoned(lunger, 77);
        CHECK(cw.pet_use(lunger, 77, pets::kHunterLunge, far_prey, 999.0), "the Hunter takes Lunge");
        const double from = w.registry.find(lunger)->x;
        run(0.4);
        CHECK(w.registry.find(lunger)->x > from + 300.0, "it covers the ground faster than it could run");
        run(2.0);
    }
}

}
AOC_REGISTER_TEST(test_pet_rules);

void test_pet_abilities() {
    PetTestData shared;
    if (!shared.ok) return;
    const npc::MotionTable& motion = shared.motion;
    const std::string& wolf_path = shared.wolf_path;
    std::string error;
std::printf("a pet's abilities\n");
for (const pets::PetAbility& use : pets::kPetAbilities) {
    if (use.ability == pets::kHunterStanceSwap) continue;   // (the session's)
    const pets::Definition* def = pets::for_effect(use.pet_effect);
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty() && def, "(the world of the pet's ability)");
    if (!r.ok() || !data || !w.problem.empty() || !def) break;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    const uint64_t pet = w.spawn(def->archetype, 200, 0, 25, 1.0);
    const uint64_t prey = w.spawn(wolf_path, 500, 0, 25, 1.0);
    const auto* a = r.catalog->ability(use.ability);
    CHECK(pet && prey && a, "(the pet, a creature and the ability)");
    if (!pet || !prey || !a) break;
    CHECK(cw.pet_use(pet, 77, use.ability, prey, def->reach_cm) && cw.pet_casting(pet), "the pet takes the ability");
    double now = 1.0;
    cw.fighter_of(prey);
    const double full = cw.max_health_of(prey);
    for (int i = 0; i < 80 && cw.pet_casting(pet); ++i) {
        now += 0.1;
        w.npcs->note_player(77, 0, 0, now);
        w.npcs->tick(now);
        cw.tick(now);
    }
    CHECK(!cw.pet_casting(pet), "the ability ends");
    std::printf("  [info] %s: creature %.0f of %.0f Health, %zu dealt, %zu on the owner\n", a->row.name.c_str(), cw.health_of(prey), full, player.dealt.size(), player.from_pet.size());
    const std::string& name = a->row.name;
    if (name.find("Stomp") != std::string::npos) {
        CHECK(cw.health_of(prey) < full && !player.dealt.empty(), "Stomp hurts what stands around the pet, and the owner is told");
    }
    if (name.find("Taunt") != std::string::npos) CHECK(cw.effect_stacks(prey, 6064630758760448234ll) == 1, "Taunt demoralizes the creature");
    if (name.find("Incapacitate") != std::string::npos) CHECK(cw.effect_stacks(prey, pets::kHunterIncapMelee) == 1, "Incapacitate plays and puts the creature to sleep");
    if (name.find("Eviscerate") != std::string::npos || name.find("Lunge") != std::string::npos) CHECK(cw.health_of(prey) < full, "its hit hurts the creature");
    if (name.find("Eruption") != std::string::npos) CHECK(cw.health_of(prey) < full, "Essence Eruption hurts around its target");
    if (name.find("Recreate") != std::string::npos || name.find("Cleanse") != std::string::npos) CHECK(!player.from_pet.empty(), "the Mystic's help lands on its owner");
    if (name.find("Brace") != std::string::npos) CHECK(cw.effect_stacks(pet, first_effect_of(*r.catalog, 6064634403610624001ll)) == 1, "Brace lands on the Guardian");
    CHECK(!cw.pet_use(pet, 77, 987654, prey, def->reach_cm), "a pet plays no ability that is not there");
}

}
AOC_REGISTER_TEST(test_pet_abilities);

void test_pet_owner_abilities() {
    PetTestData shared;
    if (!shared.ok) return;
    const npc::MotionTable& motion = shared.motion;
    const std::string& wolf_path = shared.wolf_path;
    std::string error;
std::printf("a Summoner's abilities on its pet, as the client asks for them\n");
{
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the pet)");
    if (r.ok() && data && w.problem.empty()) {
        combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
        combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
        combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
        cw.add_player(&player);
        combat::WorldHitPort port(cw, w.registry, player.fighter());
        r.pipe->set_combat(&port);
        r.pipe->set_reactions_enabled(true);
        const pets::Definition& guardian = pets::kDefinitions[0];
        const uint64_t pet = w.spawn(guardian.archetype, 200, 0, 25, 1.0);
        const uint64_t prey = w.spawn(wolf_path, 900, 0, 25, 1.0);
        cw.fighter_of(prey);
        CHECK(pet && prey && cw.pet_summoned(pet, 77), "(the Guardian is on its owner's side)");
        CHECK(r.pipe->grant_effect(guardian.effect, 1.0), "(its summon's effect is on the owner)");
        r.take();
        constexpr int64_t kExtract = 6064633645783911914ll, kExtractHit = 6064633645898797957ll, kSiphon = 6064633662084304872ll, kSiphonHit = 6064633662084304873ll;
        constexpr int64_t kBond = 6064633814845161472ll, kBondHit = 6064633815783636993ll, kBondPet = 6064633815887904770ll, kBondCaster = 6064633821403750774ll;
        constexpr int64_t kShred = 6064633271239453745ll, kShredHit = 6064633271360225904ll, kFortitude = 6064634608804770959ll;
        constexpr int64_t kHex = 6064633493499835793ll, kHexKillReset = 6064634568783891085ll;
        constexpr int64_t kReconstruct = 6064633663176909411ll, kReconstructHit = 6064633663176909412ll, kAmplify = 6064633844273079586ll, kAmplifyHit = 6064633861412726145ll;
        constexpr int64_t kPhaseOut = 6064633866966605140ll, kPhaseOutPet = 6064633867155810140ll;
        r.pipe->set_known({kExtract, kSiphon, kBond, kShred, kHex, kReconstruct, kAmplify, kPhaseOut});
        double t = 2.0;
        int32_t id = 100;
        // the client's order: the trigger, then at the montage's notify the request for the hit, naming who it reached
        const auto cast = [&](int64_t ability, int64_t hit, const std::vector<uint64_t>& aim, int64_t branch = 0) {
            const bool channelled = branch != 0;
            TriggerInfo info;
            info.target_count = static_cast<uint32_t>(aim.size());
            info.target_actors = aim;
            const Refusal why = r.pipe->trigger(ability, ++id, t, info);
            r.take();
            const size_t notes_from = r.log.size();
            t += channelled ? 1.6 : 0.3;
            if (channelled) r.pipe->branch_request(id, branch, t);
            t += channelled ? 0.2 : 0.0;
            r.pipe->refresh(t, "(the cost step)");
            r.take();
            const auto verdict = r.pipe->hit_request(ability, 0, hit, 1, aim.empty() ? 1 : 0, t, aim);
            std::printf("  [info] %s: trigger %d, request %s, targets %zu\n", r.catalog->ability(ability)->row.name.c_str(), static_cast<int>(why), AbilityPipeline::verdict_name(verdict), aim.size());
            r.take();
            t += 1.5;
            r.pipe->refresh(t, "(the hit lands)");
            r.take();
            for (size_t i = notes_from; i < r.log.size(); ++i) std::printf("    [note] %s\n", r.log[i].note.c_str());
            t += 40.0;   // (the next one owes nothing to this one's cooldown)
            return why;
        };
        const auto refill = [&] {
            r.pipe->refresh(t, "(refill)");
            r.take();
            r.set_current(pets::kEssence, pets::kEssenceMax);
            r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
            r.take();
        };
        refill();
        const double full = r.pawn.stat(content::kGuidMaxHealth);
        r.set_current(content::kGuidHealth, full * 0.5);
        r.take();
        const double pet_before = cw.health_of(pet);
        CHECK(cast(kExtract, kExtractHit, {pet}, 6064633645892571971ll) == Refusal::None, "Extract is taken with a pet out");
        std::printf("  [info] Extract: Health %.0f -> %.0f of %.0f, pet %.0f -> %.0f\n", full * 0.5, r.pawn.stat(content::kGuidHealth), full, pet_before, cw.health_of(pet));
        CHECK(r.pawn.stat(content::kGuidHealth) > full * 0.5 + 1.0, "Extract heals its owner (the reaction its hit carries)");
        CHECK(cw.health_of(pet) < pet_before, "and takes it from the pet");
        refill();
        r.set_current(content::kGuidMana, 1.0);
        r.take();
        CHECK(cast(kSiphon, kSiphonHit, {pet}, 6064633662084370415ll) == Refusal::None, "Siphon is taken with a pet out");
        std::printf("  [info] Siphon: Mana 1 -> %.0f\n", r.pawn.stat(content::kGuidMana));
        CHECK(r.pawn.stat(content::kGuidMana) > 2.0, "Siphon gives its owner Mana (the reaction its hit carries)");

        refill();
        CHECK(cast(kBond, kBondHit, {}) == Refusal::None, "Bond is taken");
        r.pipe->refresh(t, "(Bond ticks)");
        r.take();
        std::printf("  [info] Bond: on the pet %d, on the owner %d\n", cw.effect_stacks(pet, kBondPet), r.pipe->stacks_of(kBondCaster));
        CHECK(cw.effect_stacks(pet, kBondPet) == 0, "Mutual Bond, the pet's own effect, asks for its talent");
        CHECK(r.pipe->stacks_of(kBondCaster) > 0, "Bond's tick reaches the pet, whose hit gives its owner its side (the reaction it carries)");

        refill();
        {
            w.npcs->note_player(77, 0, 0, t);   // (the player is where it was, as a session says every step)
            w.npcs->tick(t);
            cw.tick(t);
            TriggerInfo shred;
            shred.target_count = 1;
            shred.target_actors = {prey};
            CHECK(r.pipe->trigger(kShred, ++id, t, shred) == Refusal::None, "Spirit Shred is taken");
            r.take();
            t += 0.3;
            r.pipe->refresh(t, "(its cost)");
            r.take();
            const std::vector<uint64_t> on_prey = {prey};
            r.pipe->hit_request(kShred, 0, kShredHit, 1, 0, t, on_prey);
            for (int step = 0; step < 16; ++step) {   // (its effect ticks on the creature)
                t += 0.5;
                r.pipe->refresh(t, "(it ticks)");
                r.take();
                w.npcs->note_player(77, 0, 0, t);
                w.npcs->tick(t);
                cw.tick(t);
            }
            CHECK(cw.effect_stacks(prey, first_effect_of(*r.catalog, kShredHit)) == 1 || cw.health_of(prey) < cw.max_health_of(prey), "(it landed on the creature)");
            t += 40.0;
        }
        std::printf("  [info] Spirit Shred: Fortitude on the Guardian %d\n", cw.effect_stacks(pet, kFortitude));
        CHECK(cw.effect_stacks(pet, kFortitude) >= 1, "its damage stacks Fortitude on the Guardian (the reaction its hit carries)");

        refill();
        CHECK(cast(kReconstruct, kReconstructHit, {pet}) == Refusal::None, "Reconstruct is taken");
        refill();
        CHECK(cast(kAmplify, kAmplifyHit, {pet}) == Refusal::None, "Amplify is taken");
        CHECK(cw.effect_stacks(pet, first_effect_of(*r.catalog, kAmplifyHit)) == 1, "Amplify lands on the pet");
        refill();
        CHECK(cast(kPhaseOut, kPhaseOutPet, {pet}) == Refusal::None, "Phase Out is taken");
        CHECK(first_effect_of(*r.catalog, kPhaseOutPet) != 0, "(Phase Out's hit puts an effect on the pet)");
        CHECK(cw.effect_stacks(pet, first_effect_of(*r.catalog, kPhaseOutPet)) == 0, "Phase Out's effect on the pet is a crowd control: it stays off");

        // a pet's ability: its Essence, and its cooldown where the owner's bar reads it
        refill();
        {
            double seconds = 0;
            const double essence = r.pawn.stat(pets::kEssence);
            CHECK(r.pipe->pay_pet_ability(pets::kGuardianTaunt, t, seconds), "Taunt is paid for");
            r.take();
            std::printf("  [info] Taunt: Essence %.1f -> %.1f, cooldown %.1f s\n", essence, r.pawn.stat(pets::kEssence), seconds);
            CHECK(r.pawn.stat(pets::kEssence) < essence, "it costs Essence");
            CHECK(std::abs(seconds - 10.0) < 0.01, "its cooldown is the record's 10 s");
            CHECK(r.pipe->charges_left(pets::kGuardianTaunt) == 0, "the cooldown runs on the owner, where the bar reads it");
            t += seconds + 1.0;
            r.pipe->refresh(t, "(the cooldown is over)");
            r.take();
            CHECK(r.pipe->charges_left(pets::kGuardianTaunt) == -1, "and ends");
            r.set_current(pets::kEssence, 0.0);
            r.take();
            CHECK(!r.pipe->pay_pet_ability(pets::kGuardianTaunt, t, seconds), "with no Essence it is refused");
            r.take();
            CHECK(r.pipe->charges_left(pets::kGuardianTaunt) == -1, "and starts no cooldown");
        }
        // a hit's ModifyCooldownsIds: a Hexed creature's death gives Hex back
        refill();
        TriggerInfo hex;
        hex.target_count = 1;
        hex.target_actors = {prey};
        CHECK(r.pipe->trigger(kHex, ++id, t, hex) == Refusal::None, "Hex is taken");
        r.take();
        t += 0.6;
        r.pipe->refresh(t, "(its cost)");
        r.take();
        CHECK(r.pipe->charges_left(kHex) != -1, "Hex's cooldown runs");
        r.pipe->land_pet_hit(kHexKillReset, kHex, t);
        r.take();
        CHECK(r.pipe->charges_left(kHex) == -1, "the kill gives Hex back (Summoner_Hex_KillReset)");
        // its Health comes back by itself, and 5 s on its hits land as before
        {
            const double hurt = cw.health_of(pet);
            for (int step = 0; step < 50; ++step) {
                t += 0.1;
                w.npcs->note_player(77, 0, 0, t);
                w.npcs->tick(t);
                cw.tick(t);
            }
            std::printf("  [info] a pet's regeneration: %.1f -> %.1f in 5 s of %.0f\n", hurt, cw.health_of(pet), cw.max_health_of(pet));
            CHECK(cw.health_of(pet) > hurt, "a hurt pet regenerates");
            r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));   // (what an effect's player applied ends when that player has died: the wolves around it may have killed the owner by now)
            const uint64_t beside = w.spawn(wolf_path, 350, 0, 25, 1.0);
            cw.fighter_of(beside);
            const double whole = cw.max_health_of(beside);
            effects::Applied flare;
            flare.def = r.catalog->effect_named("Summoner_SoulFlare");
            flare.duration = 1.0;
            for (int tries = 0; tries < 10 && cw.health_of(beside) >= whole; ++tries) {   // (it rolls to hit like any other)
                cw.apply_effects(pet, 77, {flare}, t);
                for (int step = 0; step < 15; ++step) {
                    t += 0.1;
                    w.npcs->note_player(77, 0, 0, t);
                    w.npcs->tick(t);
                    cw.tick(t);
                }
            }
            CHECK(cw.health_of(beside) < whole, "Soul Flare still hurts what stands around the pet");
        }

        r.pipe->set_combat(nullptr);
    }
}

}
AOC_REGISTER_TEST(test_pet_owner_abilities);

void test_pet_bolt() {
    PetTestData shared;
    if (!shared.ok) return;
    const npc::MotionTable& motion = shared.motion;
    const std::string& wolf_path = shared.wolf_path;
    std::string error;
std::printf("Dimensional Bolt: its presses, a held key's triggers and its cooldown\n");
{
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the bolt)");
    if (r.ok() && data && w.problem.empty()) {
        combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
        combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
        combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
        cw.add_player(&player);
        combat::WorldHitPort port(cw, w.registry, player.fighter());
        r.pipe->set_combat(&port);
        const uint64_t prey = w.spawn(wolf_path, 900, 0, 25, 1.0);
        cw.fighter_of(prey);
        r.pipe->set_known({pets::kBolt});
        TriggerInfo info;
        info.target_count = 1;
        info.target_actors = {prey};
        double t = 1.0;
        const auto press = [&](int32_t id, double at) {
            const Refusal why = r.pipe->trigger(pets::kBolt, id, at, info);
            r.take();
            return why;
        };
        CHECK(r.pipe->grant_effect(pets::kDefinitions[0].effect, t), "(a Guardian is out)");
        r.take();
        CHECK(press(1, t) == Refusal::None, "the first press throws the bolt");
        CHECK(press(2, t + 0.02) == Refusal::Busy, "a held key's next trigger, 20 ms on, throws no second bolt");
        CHECK(r.pipe->charges_left(pets::kBolt) == -1, "the bolt itself starts no cooldown");

        const abilities::EffectDef* mark = r.catalog->effect(pets::kBoltMark);
        CHECK(mark != nullptr, "the bolt's mark is in the data");
        if (mark) {
            effects::Applied landed;
            landed.def = mark;
            landed.duration = 5.0;
            t = 3.0;
            cw.apply_effects(prey, 77, {landed}, t);
            r.pipe->refresh(t, "the first bolt is paid for");
            r.take();
            const double mana = r.pawn.stat(content::kGuidMana);
            CHECK(press(3, t) == Refusal::None, "the next press on the marked creature, with a pet out, is taken");
            const auto recall = r.pipe->take_bolt_recall();
            CHECK(recall.target == prey && !recall.behind, "it sends the Guardian to the creature");
            CHECK(cw.effect_stacks(prey, pets::kBoltMark) == 0, "the press spends the mark");
            CHECK(r.pipe->stacks_of(pets::kBoltLockout) == 0 && r.pipe->stacks_of(pets::kBoltLockout2) == 0, "and the lockouts");
            CHECK(r.pawn.stat(content::kGuidMana) >= mana, "and costs no Mana");
            CHECK(r.pipe->charges_left(pets::kBolt) == 1, "it starts the cooldown, and spends no charge (retail's CD says 1)");
            CHECK(press(4, t + 0.02) == Refusal::Charges, "the next trigger of the held key is refused");
            CHECK(r.pipe->take_bolt_recall().target == 0, "and sends nothing");
            CHECK(press(5, t + 13.0) == Refusal::None, "12 s on the bolt can be thrown again");
            t += 32.0;   // (that bolt's own lockout and cooldown are over)
        }
        // no second press: the lockout refuses the bolt, and its second effect's end starts the cooldown
        CHECK(r.pipe->grant_effect(pets::kBoltLockout2, t), "(the bolt's second lockout)");
        r.take();
        CHECK(press(6, t + 0.1) == Refusal::Busy, "under the lockout, with no mark, nothing is thrown");
        r.pipe->refresh(t + 6.0, "the lockout is over");
        r.take();
        CHECK(r.pipe->charges_left(pets::kBolt) == 1, "the lockout's end starts the cooldown");
        r.pipe->set_combat(nullptr);
    }
}

}
AOC_REGISTER_TEST(test_pet_bolt);

void test_pets_fight() {
    PetTestData shared;
    if (!shared.ok) return;
    const npc::MotionTable& motion = shared.motion;
    const std::string& wolf_path = shared.wolf_path;
    std::string error;
std::printf("a Summoner's pets fight\n");
for (const pets::Definition& def : pets::kDefinitions) {
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the pet's fight)");
    if (!r.ok() || !data || !w.problem.empty()) break;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    const uint64_t pet = w.spawn(def.archetype, 200, 0, 25, 1.0);
    const uint64_t prey = w.spawn(wolf_path, 900, 0, 1, 1.0);
    CHECK(pet && prey, "(the pet and a creature stand in the world)");
    if (!pet || !prey) break;
    CHECK(r.catalog->ability(def.attack) != nullptr, "the pet's attack is in the data");
    CHECK(!cw.pet_attack(pet, 77, def.attack, pet) && !cw.pet_attack(pet, 77, def.attack, 987654), "a pet does not attack itself or nothing");
    CHECK(cw.pet_attack(pet, 77, def.attack, prey) && cw.pet_fighting(pet), "the pet takes the order to attack");
    double now = 1.0, full = cw.max_health_of(prey);
    size_t steps = 0;
    for (; steps < 900 && !cw.dead(prey); ++steps) {
        now += 0.1;
        w.npcs->note_player(77, 0, 0, now);
        w.npcs->tick(now);
        cw.tick(now);
    }
    std::printf("  [info] %s killed the creature (%.0f Health) in %.1f s\n", def.name, full, 0.1 * static_cast<double>(steps));
    CHECK(cw.dead(prey), "the pet's attacks kill the creature");
    for (int i = 0; i < 5; ++i) {
        now += 0.1;
        cw.tick(now);
    }
    CHECK(!cw.pet_fighting(pet), "the fight ends with the creature's death");
    // a creature of the pet's level, far from the owner: it answers by running at the owner, who is what it hits, so the seconds until it arrives are the pet's alone
    const uint64_t second = w.spawn(wolf_path, 3600, 0, 25, 1.0);
    CHECK(second && cw.pet_attack(pet, 77, def.attack, second), "it takes another order");
    double even = 0, lowest = second ? cw.max_health_of(second) : 0.0, lowest_at = 0;
    for (; even < 15.0 && second && !cw.dead(second) && cw.pet_fighting(pet); even += 0.1) {
        now += 0.1;
        w.npcs->note_player(77, 0, 0, now);
        w.npcs->tick(now);
        cw.tick(now);
        if (cw.health_of(second) < lowest) {
            lowest = cw.health_of(second);
            lowest_at = even;
        }
    }
    const double taken = second ? cw.max_health_of(second) - lowest : 0.0;
    std::printf("  [info] %s against a creature of its own level: %.0f of %.0f Health by %.1f s\n", def.name, taken, second ? cw.max_health_of(second) : 0.0, lowest_at);
    CHECK(taken > 0, "its hits count as its owner's and hurt a creature of its level");
    CHECK(taken < 0.9 * cw.max_health_of(second) || lowest_at > 5.0, "a pet does not cut a creature of its level down in a few seconds");
    // the creature turns on the pet, not on the owner, and when the pet is killed it goes for the owner rather than going home healed
    if (second) {   // (the last creature leaves: it was chasing the owner)
        cw.forget(second);
        w.registry.remove(second);
    }
    r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
    r.take();
    const double pet_full = cw.health_of(pet);
    const size_t hits_before = player.hits.size();
    const uint64_t foe = w.spawn(wolf_path, 3600, 0, 25, 1.0);
    CHECK(foe && cw.pet_attack(pet, 77, def.attack, foe), "the pet goes for a third creature");
    for (double t = 0; t < 180.0 && foe && !cw.dead(pet) && !cw.dead(foe); t += 0.1) {
        now += 0.1;
        w.npcs->note_player(77, 0, 0, now);
        w.npcs->tick(now);
        cw.tick(now);
    }
    std::printf("  [info] %s after the fight: pet %.0f of %.0f Health (%s), creature %.0f of %.0f\n", def.name, cw.health_of(pet), pet_full, cw.dead(pet) ? "killed" : "alive", foe ? cw.health_of(foe) : 0.0, foe ? cw.max_health_of(foe) : 0.0);
    CHECK(cw.health_of(pet) < pet_full, "the creature hurts the pet");
    CHECK(player.hits.size() == hits_before, "while the pet stands the creature leaves the owner alone");
    if (cw.dead(pet)) {
        CHECK(!cw.pet_fighting(pet), "a killed pet's fight is over");
        CHECK(foe && !cw.dead(foe) && cw.health_of(foe) < cw.max_health_of(foe), "(the creature lived, hurt)");
        for (int i = 0; i < 5; ++i) {
            now += 0.1;
            w.npcs->note_player(77, 0, 0, now);
            w.npcs->tick(now);
            cw.tick(now);
        }
        const auto state = cw.ai_state(foe);
        CHECK(state && *state != npc::ai::State::Reset && *state != npc::ai::State::Idle, "the creature does not go home healed: it goes for the owner");
    }
    cw.pet_stop(pet);
    CHECK(!cw.pet_fighting(pet), "a stop ends the fight");
    cw.pet_gone(pet);
    CHECK(!cw.is_combatant(pet), "a pet that leaves is forgotten");
}
}
AOC_REGISTER_TEST(test_pets_fight);
void test_pet_taunt_second_landing() {
    PetTestData shared;
    if (!shared.ok) return;
    std::printf("Taunt: the requirement of its effects reads what the creature holds\n");
    std::string error;
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the Taunt)");
    if (!r.ok() || !data || !w.problem.empty()) return;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    const pets::Definition& guardian = pets::kDefinitions[0];
    const uint64_t pet = w.spawn(guardian.archetype, 200, 0, 25, 1.0);
    const uint64_t prey = w.spawn(shared.wolf_path, 500, 0, 25, 1.0);
    cw.fighter_of(prey);
    const auto* demoralized = r.catalog->effect_named("Status_Demoralized");
    const auto* humiliated = r.catalog->effect_named("Status_Humiliated");
    CHECK(pet && prey && demoralized && humiliated, "(the Guardian, a creature and the two effects)");
    if (!pet || !prey || !demoralized || !humiliated) return;
    double now = 1.0;
    const auto taunt = [&] {
        CHECK(cw.pet_use(pet, 77, pets::kGuardianTaunt, prey, pets::kGuardianTauntReach), "the Guardian takes Taunt");
        for (int i = 0; i < 40 && cw.pet_casting(pet); ++i) {
            now += 0.1;
            r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
            r.take();
            w.npcs->note_player(77, 0, 0, now);
            w.npcs->tick(now);
            cw.tick(now);
        }
    };
    taunt();
    CHECK(cw.effect_stacks(prey, demoralized->row.guid) == 1 && cw.effect_stacks(prey, humiliated->row.guid) == 0, "its first landing demoralizes");
    taunt();
    CHECK(cw.effect_stacks(prey, humiliated->row.guid) == 1, "its second humiliates");
}
AOC_REGISTER_TEST(test_pet_taunt_second_landing);

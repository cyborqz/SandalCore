// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Summoner pets: contributed by MrJambix in #13.
// ============================================================================
//  tests/ability_pipeline_summon_tests.inc: a Summoner's cast: applied once by the server when it completes, cancelled by moving or by the client
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_pipeline.cpp runs it through aoc_tests::run; it is included from ability_pipeline_registered.inc)
// ============================================================================

void test_summoner_completion() {
    std::printf("Summoner: completion, cancellation, and one server-owned summon hit\n");
    const char* names[] = {"Summoner_Summon_SpiritGuardian", "Summoner_Summon_SpiritHunter", "Summoner_Summon_SpiritMystic"};
    for (const char* name : names) {
        Rig r(6, 25);
        CHECK(r.ok(), "the Summoner rig loads");
        if (!r.ok()) return;
        r.know({name});
        r.asks = false;
        const auto* d = pets::for_ability(r.ability(name));
        CHECK(d != nullptr, "the summon has a pet definition");
        if (!d) continue;
        CHECK(r.trigger(name, 1.0, false) == Refusal::None, "the summon cast is accepted");
        const auto* ability = r.catalog->ability(d->ability);
        CHECK(ability != nullptr && !ability->hits.empty(), "the summon has a record with a hit");
        if (!ability || ability->hits.empty()) continue;
        const int64_t hit = ability->hits.front().hit_guid;
        CHECK(r.pipe->hit_request(d->ability, content::kTargetingDefaultSelf, hit, 1, 1, 1.2) == AbilityPipeline::HitVerdict::Quiet,
              "a premature client hit cannot complete a summon");
        r.run_to(8.9);
        CHECK(!r.pawn.effect(d->effect), "the pet effect is absent before the cast completes");
        r.run_to(9.1);
        const auto* effect = r.pawn.effect(d->effect);
        CHECK(effect != nullptr, "completion applies the summon even without a client hit request");
        const int64_t effect_id = effect ? effect->effect_id : -1;
        r.pipe->hit_request(d->ability, content::kTargetingDefaultSelf, hit, 1, 1, 9.2);
        r.run_to(10.0);
        effect = r.pawn.effect(d->effect);
        CHECK(effect && effect->effect_id == effect_id && r.pawn.broken.empty(), "a late hit cannot summon a second pet");
    }
    for (const bool move : {false, true}) {
        Rig r(6, 25);
        r.know({names[0]});
        r.asks = false;
        r.trigger(names[0], 1.0, false);
        r.run_to(1.5);
        CHECK(!r.pawn.abilities.empty(), "the summon montage is present before cancellation");
        if (r.pawn.abilities.empty()) continue;
        const int64_t instance = r.pawn.abilities.begin()->first;
        if (move) r.pipe->set_moving(true);
        else r.pipe->end_request(static_cast<int32_t>(instance), 1.5);
        r.run_to(12.0);
        CHECK(!r.pawn.effect(pets::kDefinitions[0].effect), "a cancelled summon creates no pet effect");
        CHECK(r.pawn.broken.empty(), "cancelling keeps the replicated arrays valid");
    }
}
AOC_REGISTER_TEST(test_summoner_completion);

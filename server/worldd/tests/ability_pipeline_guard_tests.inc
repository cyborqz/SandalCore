// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_pipeline_guard_tests.inc: a Guard released at any moment gives Stamina recovery back
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_pipeline.cpp runs it through aoc_tests::run; it is included from ability_pipeline_registered.inc)
// ============================================================================

void test_guard_stamina_recovery() {
    std::printf("Guard releases restore stamina recovery, including quick taps and end requests\n");
    for (int release = 0; release < 4; ++release) {
        Rig r(5, 1);
        CHECKF(r.ok(), "the Rogue rig: %s", r.problem.c_str());
        if (!r.ok()) return;
        r.know({"General_Guard"});
        CHECK(r.trigger("General_Guard", 1.0) == Refusal::None, "Guard starts");
        const int32_t client_id = r.next_client_id - 1;
        if (release < 2) {
            // Release before the item, or between the item and its delayed effect.
            CHECK(r.pipe->cancel(client_id, release == 0 ? 1.01 : 1.31), "the quick release is accepted");
        }
        r.run_to(1.5);
        CHECK(r.a7->trigger(content::kGuidDodge, 100, 1.5) == Refusal::None, "the first Dodge starts");
        r.take();
        r.run_to(2.3);
        CHECK(r.a7->trigger(content::kGuidDodge, 101, 2.3) == Refusal::None, "the second Dodge starts after its cooldown");
        r.take();
        CHECK(r.a7->stamina() == 0 && r.pawn.stat(content::kGuidStamina) == 0, "two Dodges exhaust stamina on the server and client");
        if (release == 2) {
            CHECK(r.pipe->cancel(client_id, 2.3), "the active Guard release is accepted");
        } else if (release == 3) {
            int64_t instance = -1;
            for (const auto& [id, item] : r.pawn.abilities)
                if (item.ability_guid == r.ability("General_Guard")) instance = item.server_instance_id;
            CHECK(instance >= 0 && r.pipe->end_request(static_cast<int32_t>(instance), 2.3), "the active Guard end request is accepted");
        }
        r.run_to(5.0);
        r.run_to(6.0);   // A7's next tick follows the pipeline's restoration of its recovery rate.
        CHECKF(r.pawn.effect(r.effect("General_Guard")) == nullptr, "release %d leaves no Guard effect", release);
        CHECKF(near(r.pawn.stat(content::kGuidStaminaRegeneration), 5.0, 1e-6), "release %d restores the replicated recovery rate", release);
        CHECKF(r.a7->stamina() > 0 && r.pawn.stat(content::kGuidStamina) > 0, "release %d recovers from zero on the server and client", release);
        CHECK(r.pawn.broken.empty(), "every ability update applies");
    }
}
AOC_REGISTER_TEST(test_guard_stamina_recovery);

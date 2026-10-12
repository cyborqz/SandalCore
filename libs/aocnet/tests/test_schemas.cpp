// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_schemas.cpp
//
//  Session B exit-criterion test.  Loads all schemas and verifies:
//    - Each schema passes internal validation (no handle collisions)
//    - PC + Pawn + PS are registered
//    - Pawn schema has ≥ 30 properties across components (per plan)
//    - Key identity properties are findable by name
//    - Critical GM property bIsGM is present on PC
// ============================================================================
#include "protocol/schema/schema_registry.h"
#include <cstdio>

using namespace aoc::protocol::schema;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static size_t count_all_properties(const ActorSchema& s) {
    size_t n = s.root_properties.size();
    for (const auto& c : s.components) n += c.properties.size();
    return n;
}

int main() {
    std::printf("=== Session B schema validation ===\n\n");

    SchemaRegistry::instance().load_all();

    // ── Schema existence ──
    const ActorSchema* pc = SchemaRegistry::instance().get_schema(ActorType::PlayerController);
    const ActorSchema* pawn = SchemaRegistry::instance().get_schema(ActorType::Pawn);
    const ActorSchema* ps = SchemaRegistry::instance().get_schema(ActorType::PlayerState);

    CHECK(pc != nullptr, "PlayerController schema registered");
    CHECK(pawn != nullptr, "Pawn schema registered");
    CHECK(ps != nullptr, "PlayerState schema registered");
// [CoA ~ C-Y-B]

    if (!pc || !pawn || !ps) {
        std::printf("\nFATAL: schemas missing, cannot continue\n");
        return 1;
    }

    // ── Counts ──
    size_t pc_props = count_all_properties(*pc);
    size_t pawn_props = count_all_properties(*pawn);
    size_t ps_props = count_all_properties(*ps);

    std::printf("\n  PC   class=%s  properties=%zu  components=%zu\n",
                pc->class_name.c_str(), pc_props, pc->components.size());
    std::printf("  Pawn class=%s  properties=%zu  components=%zu\n",
                pawn->class_name.c_str(), pawn_props, pawn->components.size());
    std::printf("  PS   class=%s  properties=%zu  components=%zu\n",
                ps->class_name.c_str(), ps_props, ps->components.size());
    std::printf("\n");

    CHECK(pawn_props >= 30, "Pawn total properties >= 30 (plan exit criterion)");
    CHECK(pawn->components.size() == 6, "Pawn has 6 components (matches RE findings)");

    // ── Validation pass ──
    std::string errors = SchemaRegistry::instance().validate_all();
    if (!errors.empty()) {
        std::printf("Validation errors:\n%s\n", errors.c_str());
    }
    CHECK(errors.empty(), "validate_all() reports zero errors");

    // ── Critical properties present ──
    auto bIsGM = pc->find_by_name("bIsGM");
    CHECK(bIsGM.prop != nullptr, "PC schema contains bIsGM (RE'd @ 0xb6beb68)");
    if (bIsGM.prop) {
        CHECK(bIsGM.prop->type == PropType::Bool, "bIsGM type is Bool (1 bit)");
        CHECK(bIsGM.prop->is_rep_notify, "bIsGM is RepNotify (OnRep_bIsGM exists client-side)");
    }

    auto bIsDev = pc->find_by_name("bIsDev");
    CHECK(bIsDev.prop != nullptr, "PC schema contains bIsDev (RE'd @ 0xb31c5a8)");

    // Identity on Pawn
    auto name_prop = pawn->find_by_name("CharacterName");
    CHECK(name_prop.prop != nullptr, "Pawn has CharacterName (via CharacterInformationComponent)");
    if (name_prop.prop) {
        CHECK(name_prop.prop->type == PropType::FString, "CharacterName is FString");
        CHECK(name_prop.component_index >= 0, "CharacterName lives on a component, not root");
    }

    auto arch_prop = pawn->find_by_name("PrimaryArchetype");
    CHECK(arch_prop.prop != nullptr, "Pawn has PrimaryArchetype");

    auto race_prop = pawn->find_by_name("CharacterRace");
    CHECK(race_prop.prop != nullptr, "Pawn has CharacterRace");

    auto gender_prop = pawn->find_by_name("CharacterGender");
    CHECK(gender_prop.prop != nullptr, "Pawn has CharacterGender");

    // Stats component on Pawn
    auto hp_prop = pawn->find_by_name("CurrentHealth");
    CHECK(hp_prop.prop != nullptr, "Pawn has CurrentHealth (StatsComponent)");

    auto ability_prop = pawn->find_by_name("ActiveAbilities");
    CHECK(ability_prop.prop != nullptr, "Pawn has ActiveAbilities (AbilityComponent, CustomDelta)");

    // BP path lookup
    const ActorSchema* by_path = SchemaRegistry::instance()
        .get_schema_by_bp_path("/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP");
    CHECK(by_path == pc, "BP path lookup returns PC schema");

    // ── Dump everything for the log ──
    std::printf("\n=== Schema summary ===\n");
    SchemaRegistry::instance().dump_summary();

    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n", g_pass);
    std::printf("  Failed: %d\n", g_fail);
    std::printf("  Result: %s\n", g_fail == 0 ? "ALL PASSED" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/schema_registry.h
//
//  Global registry of all actor schemas.  Populated once at startup from
//  baked-in schema definitions (PC, Pawn, PlayerState, NPC, ...).
//
//  Later sessions may extend with runtime-loaded schemas (from
//  DataTables extracted out of the IoStore paks), but for MVP they're
//  compiled in.
//
//  LAYER:  Protocol / schema
//  SESSION: B
// ============================================================================
#pragma once

#include "protocol/schema/actor_schema.h"
#include <unordered_map>
#include <string>
#include <cstdint>

namespace aoc { namespace protocol { namespace schema {

class SchemaRegistry {
public:
    static SchemaRegistry& instance();

    /// Load all baked-in schemas.  Idempotent; safe to call at startup
    /// before any lookups.
    void load_all();

    /// Look up a schema by actor type.  Returns nullptr if not registered.
    const ActorSchema* get_schema(ActorType type) const;

    /// Look up by BP path (e.g. "/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP").
    const ActorSchema* get_schema_by_bp_path(const std::string& bp_path) const;

    /// Diagnostic: print all schema summaries to spdlog.
    void dump_summary() const;

    /// Diagnostic: validate all schemas, return concatenated error report.
    /// Empty string means everything OK.
    std::string validate_all() const;

private:
    SchemaRegistry() = default;
    bool loaded_ = false;
    std::unordered_map<ActorType, ActorSchema> schemas_;
    std::unordered_map<std::string, ActorType> by_bp_path_;

    // Populators (one per actor type; split across schema_*.cpp files)
    static ActorSchema build_pc_schema();
    static ActorSchema build_pawn_schema();
    static ActorSchema build_player_state_schema();
};

}}} // namespace aoc::protocol::schema

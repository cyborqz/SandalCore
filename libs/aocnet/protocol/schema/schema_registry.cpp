// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/schema_registry.cpp
// ============================================================================
#include "protocol/schema/schema_registry.h"
#include <spdlog/spdlog.h>
#include <sstream>

namespace aoc { namespace protocol { namespace schema {

SchemaRegistry& SchemaRegistry::instance() {
    static SchemaRegistry s;
    return s;
}

void SchemaRegistry::load_all() {
    if (loaded_) return;

    auto register_schema = [this](ActorSchema&& s) {
        ActorType t = s.type;
        const std::string bp = s.default_blueprint_path;
        schemas_[t] = std::move(s);
        if (!bp.empty()) by_bp_path_[bp] = t;
    };

    register_schema(build_pc_schema());
    register_schema(build_pawn_schema());
    register_schema(build_player_state_schema());

    loaded_ = true;

    // Run validation proactively; log warnings.
    std::string errors = validate_all();
    if (!errors.empty()) {
        spdlog::warn("[SchemaRegistry] validation warnings:\n{}", errors);
    } else {
        spdlog::info("[SchemaRegistry] {} schemas loaded clean", schemas_.size());
    }
}
// [C-O-A - C | Y | B]

const ActorSchema* SchemaRegistry::get_schema(ActorType type) const {
    auto it = schemas_.find(type);
    if (it == schemas_.end()) return nullptr;
    return &it->second;
}

const ActorSchema* SchemaRegistry::get_schema_by_bp_path(const std::string& bp_path) const {
    auto it = by_bp_path_.find(bp_path);
    if (it == by_bp_path_.end()) return nullptr;
    return get_schema(it->second);
}

void SchemaRegistry::dump_summary() const {
    spdlog::info("[SchemaRegistry] {} schemas registered", schemas_.size());
    for (const auto& [t, s] : schemas_) {
        size_t total_props = s.root_properties.size();
        for (const auto& c : s.components) total_props += c.properties.size();
        spdlog::info("  {:<25} class={}  components={}  total_properties={}",
                     s.class_name, s.default_blueprint_path,
                     s.components.size(), total_props);
    }
}

std::string SchemaRegistry::validate_all() const {
    std::ostringstream err;
    for (const auto& [t, s] : schemas_) {
        std::string e = s.validate();
        if (!e.empty()) {
            err << "[" << s.class_name << "]\n" << e << "\n";
        }
    }
    return err.str();
}

}}} // namespace aoc::protocol::schema

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/spawn_util.h  (mounts milestone, stage 2)
//
//  The small helpers an actor made from a retail open needs, shared by spawns/captured_spawns.cpp (the area's NPCs and nodes) and spawns/mount_spawn.cpp
//  (a summoned mount): SplitMix64 for the FGuid words, the angles as the wire holds them, and the setters of an ObjectState that leave a class without
//  the property alone.
// ============================================================================
#pragma once

#include "protocol/repl/replicator.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace aoc::worldd::spawns::util {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;

inline uint64_t splitmix(uint64_t& s) {
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

inline uint16_t angle16(double degrees) {
    double d = std::fmod(degrees, 360.0);
    if (d < 0) d += 360.0;
    return static_cast<uint16_t>(std::lround(d / 360.0 * 65536.0) & 0xFFFF);
}

inline uint16_t angle8(double degrees) {
    double d = std::fmod(degrees, 360.0);
    if (d < 0) d += 360.0;
    return static_cast<uint16_t>(std::lround(d / 360.0 * 256.0) & 0xFF);
}
// C-O-A.C-Y-B

/// Sets `path` on `o` (a class without that property has nothing to set: true). False, with `error`, when the value is refused.
inline bool set_path(repl::ObjectState& o, const char* path, repl::Value v, std::string& error) {
    if (!o.schema->command(path)) return true;
    if (!o.state.set(*o.schema, path, std::move(v))) {
        error = std::string("the value of ") + o.schema->name + "." + path + " is refused";
        return false;
    }
    return true;
}

/// Erases every value whose path starts with `prefix` (a struct's members).
inline void erase_prefix(repl::ObjectState& o, const char* prefix) {
    const std::string p = prefix;
    for (const schema::RepCommand& c : o.schema->commands) {
        if (std::string(c.path).rfind(p, 0) == 0) o.state.erase(c.handle);
    }
}

inline int64_t int_of(const repl::Value* v) {
    if (!v) return 0;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

}  // namespace aoc::worldd::spawns::util

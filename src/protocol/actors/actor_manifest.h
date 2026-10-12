// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/actors/actor_manifest.h
//
//  Aggregates all known actor specs into a single iterable list.
//
//  Any code that needs to enumerate "all actor types we know about" includes
//  THIS header rather than the individual per-actor files.  Adding a new
//  actor class is a 2-line diff here (new include + new entry in the array).
//
//  Phase 2 scope: informational only.  Future phases (3+) will extend the
//  entry type with a build() function pointer once builders exist.
// ============================================================================
#pragma once

#include "protocol/actors/actor_base.h"
#include "protocol/actors/player_controller.h"
#include "protocol/actors/characters.h"
#include "protocol/actors/npcs.h"
#include "protocol/actors/game_state.h"
#include "protocol/actors/interactables.h"

#include <cstddef>

namespace aoc { namespace protocol { namespace actors {

/// One entry in the manifest.  Extends ActorSpec with a symbolic name used
/// for logging + diagnostics.
struct ActorManifestEntry {
    const char*       name;       // e.g. "player_controller"
    const ActorSpec&  spec;
};

/// The complete set of actor classes we've identified in the bootstrap.
/// Each entry is a compile-time constexpr reference — no runtime cost.
inline constexpr ActorManifestEntry kActorManifest[] = {
    { "player_controller", player_controller::kSpec },
    { "characters",        characters::kSpec        },
    { "npcs",              npcs::kSpec              },
    { "game_state",        game_state::kSpec        },
    { "interactables",     interactables::kSpec     },
};

inline constexpr std::size_t kActorManifestCount =
    sizeof(kActorManifest) / sizeof(kActorManifest[0]);

}}} // namespace aoc::protocol::actors

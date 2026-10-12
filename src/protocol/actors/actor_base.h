// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/actors/actor_base.h
//
//  Common types for per-actor metadata + future builders.
//
//  Each actor header (player_controller.h, characters.h, etc.) declares a
//  constexpr ActorSpec describing the actor's wire-format identity.  In
//  Phase 3 the same namespaces will add builder functions that write bytes
//  into a BunchBuffer from a CharacterProfile.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace aoc { namespace protocol { namespace actors {

/// Description of one "actor class" as observed in the captured bootstrap.
///
/// Fields reflect what the ActorOpen bunch on the wire looked like:
/// - `channel`       : UE5 channel index carrying this actor.  Each actor
///                     instance uses a different channel (e.g. all Characters
///                     share a size/class but each is on its own channel).
/// - `bunch_data_bits` : Size of the ActorOpen bunch's payload region.
///                       Different actor classes have different sizes —
///                       this is effectively a class fingerprint.
/// - `ch_sequence`   : First reliable ChSequence on that channel.  For the
///                     world-bootstrap this is almost always 1978.
/// - `instance_count`: How many times this spec appears across the 400-
///                     packet bootstrap (1 for unique actors like the
///                     PlayerController, 7-19 for recurring classes).
/// - `hypothesis`    : Human-readable one-liner.  Confidence may be low
///                     until Phase 3 decodes ArchetypeGUID.
struct ActorSpec {
    uint16_t      channel;          // representative channel (for uniques);
                                    // for multi-instance classes see the
                                    // class-specific header for the full list
    uint16_t      bunch_data_bits;
    uint16_t      ch_sequence;
    uint16_t      instance_count;
    const char*   hypothesis;
};

// Forward declaration for Phase 3 (builders).  The interface below will be
// defined when we add the first synthesizing builder; declaring it here
// reserves the vocabulary.  Builders take a CharacterProfile (shared player
// data) and write bytes into a BunchBuffer.
//
//   struct CharacterProfile;     // in src/personalization/... (future)
//   struct BunchBuffer;          // in src/protocol/bunch_builder.h (future)
//
//   using BuildFn = bool(*)(BunchBuffer& out, const CharacterProfile& profile);

}}} // namespace aoc::protocol::actors

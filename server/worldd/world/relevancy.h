// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world/relevancy.h  (plan E1, stage 1)
//
//  Which other actors a connection should have, from the distance alone: retail's two rings, with a small hysteresis
//  (research/actors/evidence.md 3, 4):
//
//    awake      within about 150 m (a class may differ: some large creatures 119 m) the channel stays open and updates.
//    snapshot   from there to about 350 m the server opens the actor with its whole state and closes it with Dormancy about 134 ms later (two
//               server frames): the client keeps it, drawn as it was, and gets nothing more until something changes or the player comes near.
//    out        beyond that nothing is sent; an actor that was open is closed with Relevancy.
//
//  The hysteresis keeps an actor at the edge from opening and closing with every step: an open one leaves a ring only past its edge plus
//  `hysteresis_cm`, a closed one enters at the edge.  Retail's own is small (an open edge at or just inside the close edge); 10 m is plenty
//  at a walking pace of 6 m/s and the 15 Hz tick.
//
//  An actor that is never dormant (a player, a mount, a player's effect) has no snapshot ring: it is awake or out.
//
//  The view (session/actor_view.h) holds what the client has of each actor; this file holds the arithmetic and the clock's constants.
// ============================================================================
#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace aoc::worldd::world {

enum class Ring : uint8_t { Awake, Snapshot, Out };

/// What a connection has of one actor.
enum class ClientState : uint8_t {
    None,        // nothing: never opened, or forgotten
    Awake,       // its channel is open and updated
    Snapshot,    // opened as a snapshot; the Dormancy close is due
    Dormant,     // closed with Dormancy: the client still shows it, as it was
};

struct RelevancyConfig {
    double awake_cm = 15000.0;       // ~150 m
    double snapshot_cm = 35000.0;    // ~350 m
    double hysteresis_cm = 1000.0;   // 10 m
    /// The tick of a connection's pass: 15 Hz, the server tick the retail ReplicatedMovement keeps (67 ms).
    std::chrono::milliseconds tick{67};
    /// The Dormancy close follows a snapshot's open by two server frames: retail's median 134 ms (evidence 4).
    std::chrono::milliseconds dormancy_delay{134};
    /// Opens a connection may start in one pass: a teleport would open hundreds at once (retail's burst did; a 1,028-byte packet window does
    /// not like it). Closes are cheap and not capped.
    size_t max_opens_per_tick = 16;
};
// CoA x C_Y_B

/// Where an actor at horizontal distance `distance_cm` belongs for a connection that has it in state `prior`. `awake_cm` is the actor's own
/// awake radius (0: the config's). The edges are the config's; an actor that already has what it would be in has `hysteresis_cm` more room.
inline Ring classify(const RelevancyConfig& cfg, double distance_cm, double awake_cm, bool never_dormant, ClientState prior) {
    const double awake = awake_cm > 0 ? awake_cm : cfg.awake_cm;
    const double awake_edge = prior == ClientState::Awake ? awake + cfg.hysteresis_cm : awake;
    if (distance_cm <= awake_edge) return Ring::Awake;
    if (never_dormant) return Ring::Out;
    const bool in_snapshot = prior == ClientState::Snapshot || prior == ClientState::Dormant || prior == ClientState::Awake;
    // an awake one that left the awake ring is judged by the snapshot edge with its hysteresis too
    const double snapshot_edge = in_snapshot ? cfg.snapshot_cm + cfg.hysteresis_cm : cfg.snapshot_cm;
    if (distance_cm <= snapshot_edge) return Ring::Snapshot;
    return Ring::Out;
}

/// The radius a view must ask the registry for, to see every actor that could be in any ring.
inline double query_radius(const RelevancyConfig& cfg, double largest_awake_cm = 0) {
    const double r = cfg.snapshot_cm > largest_awake_cm ? cfg.snapshot_cm : largest_awake_cm;
    return r + cfg.hysteresis_cm;
}

inline double horizontal_distance(double ax, double ay, double bx, double by) { return std::hypot(ax - bx, ay - by); }

}  // namespace aoc::worldd::world

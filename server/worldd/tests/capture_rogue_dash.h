// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/capture_rogue_dash.h
//
//  The ServerMovePacked moves of a retail Rogue's Dodge dash: PCAPRepo capture character/movement/dodge/
//  aoc_rogue_dodge_with_advantage_20260204_075620 (a Rogue, Dodge AnimationIndex 2, input (0, 1); the capture's id 742 in the
//  research of 2026-10-02).  Each row is the NEW move of one client packet: the capture's time in milliseconds since its first
//  packet (the packet's time at the capturing client, used as the server's arrival time), the move's TimeStamp (float), its
//  Location (cm), movement mode and the control rotation's yaw.  The Dodge's ServerTriggerAbility is at 1658 ms; the dash covers
//  ~600 cm in 0.25 s at 2,236-2,237 cm/s (the other 30 retail Dodges peak at 1,408-1,688): above the A6 checker's 2,135.
//  Generated once from the capture (6 standing moves, the dash, the stop); not a generated file of the build.
// ============================================================================
#pragma once

#include <cstdint>

namespace capture_rogue_dash {

struct Move {
    int      ms;
    float    time_stamp;
    double   x, y, z;
    uint8_t  mode;
    uint16_t yaw;
};

/// The ServerTriggerAbility(General_Dodge, AnimationIndex 2) of the capture, in the same milliseconds.
inline constexpr int kDodgeTriggerMs = 1658;
// C O A : Cyb

inline constexpr Move kMoves[] = {
    {1416, 194.005859375f, -1039488.88, 473041.16, 15311.11, 1, 36728},
    {1450, 194.040039062f, -1039488.88, 473041.16, 15311.11, 1, 36728},
    {1485, 194.074707031f, -1039488.88, 473041.16, 15311.11, 1, 36728},
    {1519, 194.108886719f, -1039488.88, 473041.16, 15311.11, 1, 36728},
    {1555, 194.143554688f, -1039488.88, 473041.16, 15311.11, 1, 36728},
    {1588, 194.177734375f, -1039488.88, 473041.16, 15311.11, 1, 36728},
    {1624, 194.212402344f, -1039487.99, 473038.94, 15315.09, 1, 36728},
    {1658, 194.246582031f, -1039472.25, 472999.87, 15315.09, 1, 36766},
    {1675, 194.264160156f, -1039457.55, 472963.41, 15315.09, 1, 36766},
    {1693, 194.281738281f, -1039442.84, 472926.95, 15315.09, 1, 36766},
    {1711, 194.301269531f, -1039426.51, 472886.44, 15315.09, 1, 36766},
    {1728, 194.316894531f, -1039413.43, 472854.03, 15315.09, 1, 36766},
    {1746, 194.335937500f, -1039397.51, 472814.53, 15315.09, 1, 36766},
    {1764, 194.353515625f, -1039382.80, 472778.07, 15315.09, 1, 36766},
    {1784, 194.372070312f, -1039367.28, 472739.58, 15315.09, 1, 36766},
    {1801, 194.389648438f, -1039352.58, 472703.12, 15315.09, 1, 36766},
    {1819, 194.407714844f, -1039337.46, 472665.65, 15315.09, 1, 36766},
    {1836, 194.425292969f, -1039322.76, 472629.19, 15315.09, 1, 36766},
    {1854, 194.443847656f, -1039307.24, 472590.70, 15315.09, 1, 36766},
    {1871, 194.461425781f, -1039292.53, 472554.24, 15315.64, 1, 36766},
    {1891, 194.479492188f, -1039277.42, 472516.77, 15316.20, 1, 36766},
    {1908, 194.496582031f, -1039263.12, 472481.32, 15316.20, 1, 36766},
    {1926, 194.517089844f, -1039263.07, 472481.19, 15316.20, 1, 36766},
    {1941, 194.531738281f, -1039263.07, 472481.19, 15316.20, 1, 36766},
    {2011, 194.601562500f, -1039263.07, 472481.19, 15316.20, 1, 36766},
};

}  // namespace capture_rogue_dash

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Phase D1 — registry of concrete bunch generators.
//
// Small factory header so game_server.h can create generators without
// depending on any concrete generator class.  Each generator lives in its
// own .cpp file and exposes a single make_* factory function here.

#pragma once

#include "net/bunch_generator.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aoc::generators {

/// D1 no-op: logs invocations, emits no bunches.  Used to validate that
/// the pull-tick plumbing reaches the generator every tick.
std::unique_ptr<BunchGenerator> make_noop_generator(uint32_t channel);

/// D2 recorded: emits a single pre-recorded unreliable bunch once the
/// session clock reaches emit_at_ms.  payload/bit_count come from a row
/// of blueprint_bunches.csv.  label is a free-form tag that appears in
/// logs (e.g. the ch_name "#127" captured alongside the recording).
std::unique_ptr<BunchGenerator> make_recorded_generator(
    uint32_t channel,
    std::vector<uint8_t> payload,
    uint32_t bit_count,
    uint64_t emit_at_ms,
    std::string label);

} // namespace aoc::generators

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/catalog.h
//
//  ReplicatedPropertyDesc — static metadata for one replicated property,
//  derived from .rdata FPropertyParams entries (see src/protocol/tools/
//  ida_dump_*.idc scripts).
//
//  ClassCatalog — per-class collection of the above, plus a pointer to
//  the parent class's catalog.  Walking `parent` gives the full cmd_index
//  hierarchy (AAoCPlayerController -> APlayerController -> AController ->
//  AActor).
//
//  The `cmd_index` field is NOT the same as the property's position within
//  its own class's declaration — it's the expanded-struct position within
//  the flat RepLayout.  A simple property contributes 1 cmd_index; a struct
//  contributes (1 + sum of its sub-cmds).  This flat index is what the
//  rep-field-mask bits in the wire payload reference.
//
//  ── TODO (2026-04-24, Phase III M1) ──────────────────────────────────────
//  This catalog is currently a single flat list per class.  Per the
//  RE findings in docs/wire-format.md §16, UE5 RepLayout actually splits
//  properties into TWO lists: InitialRepProps (emitted at actor-open) and
//  LifetimeRepProps (emitted in deltas).  cmd_index is PER-LIST (starts at
//  0 in each phase), and a 0xDEADBEEF uint32 sentinel terminates each
//  phase on the wire.
//
//  Before M1 round-trip can pass, this struct needs:
//    - `std::vector<ReplicatedPropertyDesc> initial_props;`
//    - `std::vector<ReplicatedPropertyDesc> lifetime_props;`
//  replacing `own_props`.  The split requires RE'ing each class's
//  GetLifetimeReplicatedProps() implementation (look for DOREPLIFETIME_CONDITION
//  + COND_InitialOnly calls).
//
//  LAYER:  Protocol / emit / replayout
//  OWNER:  Phase II synthesizer
// ============================================================================
#pragma once

#include "protocol/emit/replayout/property_type.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace emit { namespace replayout {
// -- C-Y-B x C O A --

struct ReplicatedPropertyDesc {
    uint32_t      rep_index = 0;              // Declaration order WITHIN owning class
    uint32_t      cmd_index = 0;              // Position in flat expanded RepLayout
                                              //   (hierarchy-wide, inc. parents)
    std::string   name;                       // e.g. "Name", "ControllersOriginalPawn"
    FPropertyType type = FPropertyType::Unknown;
    uint32_t      offset_in_instance = 0;     // From FPropertyParams +0x34
                                              //   (not used for wire serialisation
                                              //    but handy for server-side state
                                              //    mapping)

    /// If type == Struct, this holds the struct's inner cmds, flattened
    /// by UE5 RepLayout construction.  Each sub-cmd gets its own
    /// (parent's cmd_index + N) position.
    std::vector<ReplicatedPropertyDesc> sub_cmds;

    /// If type == Array, this describes the element type (single entry).
    std::shared_ptr<ReplicatedPropertyDesc> element_desc;
};

struct ClassCatalog {
    std::string class_name;                    // "AAoCPlayerController"
    std::vector<ReplicatedPropertyDesc> own_props;  // Declared on this class only
    const ClassCatalog* parent = nullptr;      // Walks up to AActor → nullptr

    /// Total cmd_index count across the full hierarchy (including this
    /// class and every parent).  This is the width of the rep-field-mask
    /// that appears in the wire payload.
    uint32_t total_cmd_count() const;

    /// Look up a property by its cmd_index (hierarchy-wide).  Returns
    /// nullptr if out of range.
    const ReplicatedPropertyDesc* property_at_cmd(uint32_t cmd_index) const;

    /// Look up a property by name anywhere in the hierarchy.
    const ReplicatedPropertyDesc* property_by_name(const std::string& name) const;
};

/// ── Static catalog instances (populated in catalog.cpp) ────────────────
/// Each returns a const singleton.  Callers link them together via the
/// parent pointer to form the hierarchy.
const ClassCatalog& aactor_catalog();
const ClassCatalog& acontroller_catalog();
const ClassCatalog& aplayer_controller_catalog();
const ClassCatalog& aaoc_player_controller_catalog();

const ClassCatalog& aplayer_state_catalog();
const ClassCatalog& aaoc_player_state_catalog();

const ClassCatalog& apawn_catalog();
const ClassCatalog& acharacter_catalog();
const ClassCatalog& aaoc_pawn_catalog();

}}}} // namespace aoc::protocol::emit::replayout

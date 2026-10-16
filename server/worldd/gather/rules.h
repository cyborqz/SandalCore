// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  gather/rules.h  (Gathering stage 2, part "Harvesting")
//
//  What a player needs to start a harvest, as pure checks over the data (the session adds the rest it alone knows: alive, mounted, in range, the node open for its client).
//  The design data states the requirements in three places and the rule joins them [modelled: no capture shows a refusal; the client checks a certification itself and
//  sends nothing when it fails, which is why the user's stage-1 test saw its error]:
//
//    the interaction   GatheringInteractionData: a profession, a certification level (Novice 0 ... GrandMaster 4, the tier a node belongs to) and a tool slot
//    the character     its certification in that profession reaches the interaction's tier, and its level the tier's first level (certification_level.min_level)
//    the tool          the item in the interaction's slot is a gathering tool (gather_tool) of that profession whose tier reaches the node's, with durability left; and the
//                      character may use that tool: its own requirement (the item's RequiredProfession and RequiredCertification, else its tool data's) is met
//                      (worldd's equip rules do not check a tool's certification: the T3 tools need Journeyman and equip at Novice)
//    combat            a node whose interaction does not allow it is not harvested in combat
//
//  A refusal is one of the Refusal values with a sentence for the log; the session counts each kind.
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "session/professions.h"

#include <cstdint>
#include <string>

namespace aoc::worldd::gather {

enum class Refusal : uint8_t {
    None,
    NoProfessions,        // the world has no profession tables
    NotCertified,         // the character's certification in the profession is below the node's
    LevelTooLow,          // ... or its level is below the tier's first level
    NoTool,               // nothing worn in the tool slot
    NotATool,             // the worn item is no gathering tool
    WrongToolProfession,  // the tool is another profession's
    ToolTierTooLow,       // the tool reaches lower tiers than the node's
    ToolBroken,           // no durability left
    ToolNotUsable,        // the character lacks the certification or level the tool itself asks
    InCombat,             // the interaction does not allow combat
    kCount,
};
// C.O.A / Cyb

const char* refusal_name(Refusal r);

struct Verdict {
    Refusal     why = Refusal::None;
    std::string text;     // one sentence for the log
    bool ok() const { return why == Refusal::None; }
};

/// The range a player may start a harvest from, horizontally from the node's place: the interaction's radius and tolerance, and the move leeway the checker's own
/// positions need (a client moves between the server's last good position and the interact).  Retail's players stood 42 to 175 cm from the node of a 250 to 300 cm radius.
inline constexpr double kLeewayCm = 100.0;
inline double reach_cm(const content::GatherDefRow& def) { return def.interact_radius + def.interact_tolerance + kLeewayCm; }

/// What the checks read of the equipped tool: `tool` is its gather_tool row (nullptr: the worn item is no tool), `worn` whether anything is in the slot.
struct ToolInfo {
    bool                          worn = false;
    const content::GatherToolRow* tool = nullptr;
    float                         durability = 0;
};

Verdict check_requirements(const content::GatherDefRow& def, const professions::Professions& professions, const ToolInfo& tool, bool in_combat);

}  // namespace aoc::worldd::gather

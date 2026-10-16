// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/item_use.h
//
//  Using an item (plan D1, stage 1) as pure logic over content.db: the item's ItemActivationArchetype says what a use does
//  (ActivateActionResponses runs the item's ActionResponses; ConsumeOnUse takes one of the stack) and each ActionResponse's
//  ResponseType says what it is. Stage 1 runs what the GM items use:
//
//    GiveItem                 the action's RewardTables: every ItemRewards entry is granted into the bag as fresh instances
//                             (quantity split into stacks of the record's MaxStackSize), every ExperienceRewards entry of type
//                             Adventuring that contentc resolved to the level scrolls' formula grants that XP
//                             (leveling::scroll_xp); a table whose predicate GetPlayerLevel() < N the level fails refuses the
//                             use (nothing is consumed: the scroll stays for a character it is meant for)
//    SetPrimaryArchetype      the class of the action's CharacterClassId; the character's own class refuses the use
//
//    SetCertificationLevel    (Gathering stage 2) Promote: the profession is one certification tier up; Demote: one down. A GM artisan scroll lists them
//                             between its XP lines (professions.h says when a Promote takes effect)
//    a Profession XP reward   (Gathering stage 2) the reward's compiled expression, run with the live profession levels, is the XP the profession earns
//
//  The actions run IN ORDER, each seeing what the one before left (the XP of a scroll's second line depends on the tier its Promote reached). Everything else
//  refuses the use and says what was asked (the response type, an unmodelled table, an unmodelled predicate), and a reward stage 1 does not run (a Weapon XP
//  reward, an expression that is not the formula) is a note: the rest of the use goes on. Nothing here changes anything: the session applies a Plan (the
//  inventory, the XP, the professions, the class) and tells the client.
//
//  Evidence: the GM items' records in content.db (the research of 2026-10-02 and contentc's tables); no retail capture holds a
//  GM item use (scratchpad dcodecs/levelup_evidence.md), so the replies are modelled on what retail sent for the pieces (the
//  storage events of plan B2, the XP items and the level-up of leveling.h).
// ============================================================================
#pragma once

#include "content/content_db.h"
#include "session/leveling.h"
#include "session/professions.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::use {

/// A fresh instance to put into the bag (the record, how many, the record's default quality and durability).
struct Granted {
    int64_t record = 0;
    int32_t quantity = 1;
    int32_t quality = 1;
    float   durability = 0;
};

/// What a use does.
struct Plan {
    bool                     ok = false;
    std::string              refusal;         // why not (for the log); empty when ok
    bool                     consume = false; // one of the stack goes
    std::vector<Granted>     items;           // into the bag, in the records' order
    int64_t                  adventuring_xp = 0;
    std::optional<int>       set_class;       // ECharacterClass (1 Ranger ... 8 Mage)
    /// Gathering stage 2: what the character holds in its professions after the use (valid() only when the context had professions), and each profession that
    /// differs from before (before and after, in profession order). A use that moves no level and no tier leaves the professions as they were.
    professions::Professions         professions_after;
    std::vector<professions::Change> profession_changes;
    std::vector<std::string> notes;           // what was skipped
    std::string              summary;         // one line for the log ("+418,410 XP, 19 items")
};
// ~ CoA & C-Y-B ~

/// The character as the use sees it.
struct Context {
    int                         class_enum = 0;
    leveling::Progress          progress;
    double                      xp_add_mult = 0;       // the AdventuringXPAddMult stat
    const leveling::LevelCurve* curve = nullptr;       // the Adventuring curve (null: XP rewards are refused)
    /// Gathering stage 2: the professions' book, what the character holds now, and the runner of a Profession reward's expression. Without the book an item that
    /// changes a certification or gives profession XP is refused; without the runner a Profession XP reward is.
    const professions::Book*  professions = nullptr;
    professions::Professions  held;
    professions::XpRunner     profession_xp;
};

/// The most items one use grants (a gear scroll gives 19; the bag has 70 slots).
inline constexpr size_t kMaxGrantedItems = 70;

/// Plans the use of `item_record` by the character `ctx`. `ok` false with `refusal` set when the item is not used, or asks
/// for something stage 1 does not run.
Plan plan_use(const content::ContentDb& db, int64_t item_record, const Context& ctx);

/// The ECharacterClass of a class record guid (1 Ranger ... 8 Mage; content.db's `class` rows), 0 for none.
int class_enum_of(const content::ContentDb& db, int64_t class_guid);

}  // namespace aoc::worldd::use

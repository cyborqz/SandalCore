// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/leveling.h
//
//  ULevelingComponent in a typed form (plan D1): the PlayerController's XPTracker, the component that replicates what the
//  character has earned and bought.  Its class schema is generated from the exe (schema/generated/ULevelingComponent.h,
//  tools/ghidra/schemagen.py; stably named "XPTracker" in the full login capture) and needs no struct codec of its own:
//  every item of its five fast maps (FFastArraySerializer subclasses whose Items hold {Key, Value}) is a flat list of scalar
//  commands the generic fast-array reader and writer (fast_array.h) carry.  What this header adds is the typing: the key is
//  an FDesignDataObjectId, the values what each map holds, and a LevelingState that keeps the five maps by key.
//
//    ExperienceFastMap              leveling type -> int64 XP (Key {Guid, TypeId, Name}, Value int64); `Adventuring` and the
//                                   14 weapon lines (`Weapon_*`) [observed: 7,762,172 and per-weapon values in the testers]
//    SkillPointFastMap              leveling type -> uint16 (a point type's balance) [never in any capture: the client
//                                   derives its points from the level: 1 Archetype and 1 Stamina point per level, 24 a weapon
//                                   line, minus what it spent]
//    ExperienceDebtFastMap          leveling type -> int64
//    LevelingCertificationsFastMap  profession leveling type -> int64 certification id (22 entries at join, every one "Novice")
//    SkillTreePurchasesFastMap      skill tree -> PurchasedSkillRanks { IDs: TArray<FDesignDataObjectId> }: every purchased rank
//                                   of the tree, the whole list each time the tree changes [observed: a purchase re-sends its
//                                   tree's list with the new rank, a refund the list without it, a respec the list of what is
//                                   left]; the auto-granted ranks of a level are in it from the start (a level-1 Bard at join:
//                                   Bard_Rhetoric -> QuickWit, the five survey and fishing trees)
//
//  FDesignDataObjectId {Guid int64, TypeId int64, Name FName}: the design record's id.  The name is the record's name as a
//  plain string with a number (`Adventuring`, `Ranger_SkillTable`) for a map key and a skill tree, and NAME_None (the hardcoded
//  index 0) for a rank and a skill.
//
//  Evidence: all 443 S>C blocks of the PlayerController's XPTracker in 272 retail captures (708 map payloads: 55 first sends
//  against base 0, 183 updates whose base is the key the map had before, 49 resends against base 0 in the middle of a capture, 421
//  first sights of a map in a capture that starts mid-session) read to their last bit with this schema and write back to the same
//  bits (a scratch sweep; the original server's join state, 8,051 bits, was read the same way by the research).
//  tests/capture_leveling.h has a sample of them, each with the text an independent Python decoder
//  (tools/ghidra/fast_array_check.py) read, and test_repl_leveling reads them through the generic layer and through LevelingState,
//  and writes them back to the same bits.
//
//  The RPCs of the class: InterServerAwardXpType (server to server), SendDeltaDataToClient (S>C, three arrays of ids and
//  keys), ServerDemoteCertification (C>S, the interact actor's FGuid, an FDesignDataObjectId and a Key byte: 12 captured calls).  Not typed here.
// ============================================================================
#pragma once
// (C.O.A ~ C:Y:B)

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// FDesignDataObjectId: the id of a design record.
struct DesignDataId {
    int64_t   guid = 0;
    int64_t   type_id = 0;
    NameValue name = NameValue::hardcoded_name(0);   // NAME_None
    bool operator==(const DesignDataId&) const = default;

    /// A record named by a plain string (a leveling type, a skill tree).
    static DesignDataId named(int64_t guid, int64_t type_id, std::string name, int32_t number = 0) {
        DesignDataId d;
        d.guid = guid;
        d.type_id = type_id;
        d.name = NameValue::plain(std::move(name), number);
        return d;
    }
};

/// What a map of int64 holds (experience, experience debt, certifications).
struct LevelingInt64 {
    DesignDataId key;
    int64_t      value = 0;
    bool operator==(const LevelingInt64&) const = default;
};

/// SkillPointFastMap's item.
struct LevelingPoints {
    DesignDataId key;
    uint16_t     value = 0;
    bool operator==(const LevelingPoints&) const = default;
};

/// SkillTreePurchasesFastMap's item: a tree and every rank bought in it.
struct SkillTreePurchase {
    DesignDataId              tree;
    std::vector<DesignDataId> ranks;
    bool operator==(const SkillTreePurchase&) const = default;
};

/// The most ranks one tree's list may hold (the retail maximum is 49 in a weapon tree) and entries a map may hold.
constexpr size_t kMaxTreeRanks = 4096;

/// The five maps of a ULevelingComponent, kept by key.  The same class serves the server (set_* / erase_* mark the changes the
/// way MarkItemDirty / MarkArrayDirty do; `write` sends what a connection's shadows say it lacks) and a client or a test
/// (`apply` takes a decoded payload).
class LevelingState {
public:
    LevelingState();

    /// The class schema (ULevelingComponent) and the fast arrays, for write_fast_array_fields / read_fast_array_fields.
    const schema::ClassSchema& schema() const { return *schema_; }
    const FastArrays&          arrays() const { return arrays_; }

    // ---- the server's side: add or change an entry (false, nothing changed, when it does not fit) --------------------------
    bool set_experience(const DesignDataId& key, int64_t xp);
    bool set_experience_debt(const DesignDataId& key, int64_t debt);
    bool set_skill_points(const DesignDataId& key, uint16_t points);
    bool set_certification(const DesignDataId& key, int64_t certification);
    /// The whole rank list of a tree, as the retail server sends it (at most kMaxTreeRanks).
    bool set_purchased_ranks(const DesignDataId& tree, const std::vector<DesignDataId>& ranks);
    /// Marks a tree's item dirty without changing it, so the next update carries the whole list again (MarkItemDirty on an unchanged item: the
    /// server corrects a client whose view of the tree was wrong). False when the tree has no item.
    bool resend_purchased_ranks(const DesignDataId& tree);

    // ---- removing an entry: false when there is none -------------------------------------------------------------------------
    bool erase_experience(const DesignDataId& key);
    bool erase_experience_debt(const DesignDataId& key);
    bool erase_skill_points(const DesignDataId& key);
    bool erase_certification(const DesignDataId& key);
    bool erase_purchased_ranks(const DesignDataId& tree);

    // ---- reading, in the order of the items ----------------------------------------------------------------------------------
    std::vector<LevelingInt64>     experience() const;
    std::vector<LevelingInt64>     experience_debt() const;
    std::vector<LevelingPoints>    skill_points() const;
    std::vector<LevelingInt64>     certifications() const;
    std::vector<SkillTreePurchase> purchases() const;

    std::optional<int64_t>                   experience(const DesignDataId& key) const;
    std::optional<int64_t>                   experience_debt(const DesignDataId& key) const;
    std::optional<uint16_t>                  skill_points(const DesignDataId& key) const;
    std::optional<int64_t>                   certification(const DesignDataId& key) const;
    std::optional<std::vector<DesignDataId>> purchased_ranks(const DesignDataId& tree) const;

    // ---- the wire ------------------------------------------------------------------------------------------------------------
    /// The client's application of one decoded field (read_fast_array_fields): false, nothing applied, for a field that is not one
    /// of the five maps or an item that does not fit.
    bool apply(const FastArrayField& field);

    /// The fields of the maps that changed since `shadows`, written the way write_fast_array_fields does (nothing for an
    /// unchanged map); returns the bits appended, 0 with *failed true when a value cannot be carried.
    size_t write(emit::BunchWriter& out, FastArrayShadows& shadows, const CodecOptions& opts = {}, bool* failed = nullptr) const;

private:
    const schema::ClassSchema* schema_;
    FastArrays                 arrays_;
};

}}}  // namespace aoc::protocol::repl

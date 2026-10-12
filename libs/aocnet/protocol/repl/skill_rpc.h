// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/skill_rpc.h
//
//  The skill-tree and action-bar RPCs of plan D1 in a typed form.  What a retail purchase looks like on the wire (the
//  research of 2026-10-02: 128 ServerPurchaseSkillBuild calls in 27 captures, answered by 223 ClientUpdateSkillOnActionbar):
//
//    C>S  ServerPurchaseSkillBuild(BuildChanges)       on the pawn (APlayerCharacter field 172, flags 0x80220cc0)
//             FSkillPointAllocationChanges { AllocationChanges TArray<{SkillRank, SkillTree, LevelingType: FDesignDataObjectId,
//             bPurchase bool}> }
//    about 100 ms later, in one packet:
//    S>C  AbilityComponent   KnownAbilities and ReplicatedRecordsToLoad: the rank's ability appended (a RepLayout array update:
//                            the new count and the one new element)
//         StatsComponent     CurrentEffects: the rank's passive effect as an item (an infinite one for a class passive)
//         XPTracker          SkillTreePurchasesFastMap: the tree's whole rank list (leveling.h)
//         PlayerController   ClientUpdateSkillOnActionbar(SkillId, SkillTreeId, RankId, bIsRefunding, bUseHint)
//
//  A refund is the same call with bPurchase 0; a swap one call with a refund and a purchase (capture 44); a respec one call
//  listing every owned rank of a tree with bPurchase 0 (24, 24, 20, 16 ranks in four captures, 21 in a weapon tree, 11 in the
//  Universal tree), answered by the shrunk arrays and the removed effects and no ClientUpdateSkillOnActionbar.  There was
//  no refusal in any capture and no currency anywhere.  `SkillBuild::refund_only()` is what makes a call a respec.
//
//  The action bar (PlayerController):
//    S>C  ClientReceiveActionbarSlotPayload(ActionbarSlotData, bAutoGranted)   field 213: the whole bar once, on the client's
//         ServerRequestClientLoad (24 to 93 slots over the banks 0-3, 4, 16, 18, 19), and single slots later
//         (bAutoGranted: General_DropCrate on bank 2 slot 11 when a crate is picked up)
//    C>S  ServerSetActionbarSlotUseData(BankIndex, SlotIndex, UseData)          field 845: the client arranged its own bar; the
//         server sends nothing back [6 calls in 4 captures; UseType 0 clears a slot; a bank or slot of 0 is not on the wire]
//
//  Evidence: all 128 purchase calls, 223 skill updates, 150 action-bar payloads (1 to 494 slots), the 6 slot calls, the 443 XPTracker
//  blocks and, in the windows after the purchases, 32 AbilityComponent RepLayout array updates (KnownAbilities /
//  ReplicatedRecordsToLoad) and 128 StatsComponent blocks with CurrentEffects of the retail captures read to their last bit with
//  these codecs and write back to the same bits (a scratch sweep); tests/capture_leveling.h has the sample test_repl_leveling runs.
//
//  FActionbarSlotUseData { UseType, Instigator, AbilityId, SkillRankId, PetCommandId, Item {ItemRecordId, GUID, Rarity,
//  Quality}, ComboButton }: the enum constants below are the research's reading of the exe's enums (the generated schema
//  keeps only their widths: 4, 3, 4 and 4 bits).
//
//  The parse functions return nullopt for a call that is not that RPC or whose parameters were not decoded (the caller must not
//  act on a half-read call) and for a value of another kind; a parameter at its default is not on the wire and reads as that
//  default.  The `*_params` builders return nullopt when a value does not fit the command.
// ============================================================================
#pragma once

#include "protocol/repl/leveling.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/rpc.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

// ---- ServerPurchaseSkillBuild ---------------------------------------------------------------------------------------------

/// One entry of the call: a rank bought (`purchase`) or given back in its tree, charged to a leveling type's points.
struct SkillRankChange {
    DesignDataId rank;           // the SkillRank record
    DesignDataId tree;           // the SkillTree
    DesignDataId leveling_type;  // `Adventuring` for a class or Universal tree, the weapon line for a weapon tree
    bool         purchase = false;
    bool operator==(const SkillRankChange&) const = default;
};
// C O A with C | Y | B

/// The most changes one call may carry: a respec lists every owned rank (49 in the biggest retail tree), ours bounds what a
/// hostile call builds.
constexpr size_t kMaxSkillBuildChanges = 1024;

struct SkillBuild {
    std::vector<SkillRankChange> changes;
    /// True when the call buys nothing and refunds something: a respec (one call refunding every owned rank of a tree).
    bool refund_only() const;
    size_t purchases() const;
    size_t refunds() const;
    bool operator==(const SkillBuild&) const = default;
};

/// ServerPurchaseSkillBuild from a decoded call.
std::optional<SkillBuild> parse_purchase_skill_build(const RpcCall& call);

/// The parameters of a ServerPurchaseSkillBuild call on `rpc` (the pawn's schema entry).
std::optional<RpcParams> purchase_skill_build_params(const schema::RpcSchema& rpc, const SkillBuild& build);

// ---- ClientUpdateSkillOnActionbar -----------------------------------------------------------------------------------------

/// What the server tells the client to do with its action bar after a purchase.  `skill_id` is the Skill record (not the rank),
/// `rank_id` an int32 that is 0 in all 223 retail calls, `use_hint` true in all of them (the client places the skill itself).
struct SkillOnActionbar {
    int64_t skill_id = 0;
    int64_t skill_tree_id = 0;
    int32_t rank_id = 0;
    bool    is_refunding = false;
    bool    use_hint = false;
    bool operator==(const SkillOnActionbar&) const = default;
};

std::optional<SkillOnActionbar> parse_update_skill_on_actionbar(const RpcCall& call);
std::optional<RpcParams> update_skill_on_actionbar_params(const schema::RpcSchema& rpc, const SkillOnActionbar& update);

/// ClientUpdateSkillOnActionbar on the PlayerController `pc` (reliable); nullopt when its class has no such RPC.
std::optional<Emission> client_update_skill_on_actionbar(const Actor& pc, const SkillOnActionbar& update, const ExportAcks& acks);

// ---- the action bar ---------------------------------------------------------------------------------------------------------

/// EActionbarUseType.
namespace ActionbarUseType {
constexpr uint8_t None = 0, Ability = 1, Skill = 2, Item = 3, ComboInput = 4, MeleeAttack = 5, RangedAttack = 6,
                  SwapWeapons = 7, PetCommand = 8, Dismount = 9;
}
/// EActionbarInstigator.
namespace ActionbarInstigator {
constexpr uint8_t Player = 0, Pet = 1, Mount = 2, Vehicle = 3;
}
/// EActionBarType, the bank index: the class bars are 0 to 3, the weapon bank 4 (Guard, Sprint, SwapWeapons, Dodge), then
/// Dodge 5, Crafting 6 ... Pet 16, Mount 17, Vehicle 18, Fishing 19.
namespace ActionbarBank {
constexpr int32_t Primary = 0, Weapon = 4, Dodge = 5, Crafting = 6, Pet = 16, Mount = 17, Vehicle = 18, Fishing = 19;
}

/// FActionbarSlotUseData.
struct ActionbarUseData {
    uint8_t   use_type = 0;           // ActionbarUseType
    uint8_t   instigator = 0;         // ActionbarInstigator
    int64_t   ability_id = 0;
    int64_t   skill_rank_id = 0;
    int64_t   pet_command_id = 0;
    int64_t   item_record_id = 0;
    GuidValue item_guid;
    uint8_t   item_rarity = 0;
    int32_t   item_quality = 0;
    uint8_t   combo_button = 0;
    bool operator==(const ActionbarUseData&) const = default;
};

struct ActionbarSlot {
    int32_t          bank_index = 0;
    int32_t          slot_index = 0;
    ActionbarUseData use;
    bool operator==(const ActionbarSlot&) const = default;
};

/// ClientReceiveActionbarSlotPayload's parameters.
struct ActionbarPayload {
    std::vector<ActionbarSlot> slots;
    bool                       auto_granted = false;
    bool operator==(const ActionbarPayload&) const = default;
};

/// The most slots one payload may carry (the retail maximum is 93).
constexpr size_t kMaxActionbarSlots = 1024;

std::optional<ActionbarPayload> parse_actionbar_payload(const RpcCall& call);
std::optional<RpcParams> actionbar_payload_params(const schema::RpcSchema& rpc, const ActionbarPayload& payload);

/// ClientReceiveActionbarSlotPayload on the PlayerController `pc` (reliable); nullopt when its class has no such RPC or the
/// payload does not fit.
std::optional<Emission> client_receive_actionbar_slot_payload(const Actor& pc, const ActionbarPayload& payload, const ExportAcks& acks);

/// ServerSetActionbarSlotUseData: the client set one slot of its bar.
using SetActionbarSlot = ActionbarSlot;
std::optional<SetActionbarSlot> parse_set_actionbar_slot(const RpcCall& call);
std::optional<RpcParams> set_actionbar_slot_params(const schema::RpcSchema& rpc, const SetActionbarSlot& slot);

// ---- ServerSetUseRangedWeapon ------------------------------------------------------------------------------------------------

/// ServerSetUseRangedWeapon(bUseRanged) on the pawn (APlayerCharacter field 184): the player swapped between the melee and the ranged
/// weapon set [49 calls in 8 captures; 4-8 packets later the AbilityComponent's combo changed]. The bool is always on the wire.
std::optional<bool> parse_set_use_ranged_weapon(const RpcCall& call);
std::optional<RpcParams> set_use_ranged_weapon_params(const schema::RpcSchema& rpc, bool use_ranged);

}}}  // namespace aoc::protocol::repl

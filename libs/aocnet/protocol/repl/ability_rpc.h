// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/ability_rpc.h
//
//  The two ability RPCs the client sends the server and worldd's ability runtime
//  (plan A7) acts on, read from a decoded RpcCall (rpc.h), and the server's
//  ClientEndAbility.  Both client RPCs are reliable, on the UAoCAbilityComponent
//  subobject of the pawn (field 28 and 24 of the component's field table):
//
//    ServerTriggerAbility(AbilityGuid int64, ClientInstanceId int32, AnimationIndex int32,
//                         InputVector FVector2D, bPvpForceFlag bool, Targets TArray<UObject*>,
//                         TargetTransforms TArray<FTransform>, ItemData FItemAbilityData)
//    ServerNotifyHeldAbilityCancelled(ClientInstanceId int32, CancelledTime float,
//                                     CancelSectionIndex int32)
//
//  Evidence (research 2026-10-01, every call of 15 captures, 308 calls, read to their last
//  bit; the two captures in this repo are tests/capture_abilities.h, 20 + 34 calls):
//    * ClientInstanceId counts every trigger of any ability of the session from 0, so the
//      first trigger of a session omits it (a send bit of 0: the default).  A cancel names the
//      ClientInstanceId of the held ability it releases: all 58 captured cancels pair with an
//      earlier trigger (48 Sprint, 10 Guard).
//    * AnimationIndex is an index into the ability record's AnimMontages (Dodge: 5 forward,
//      7 and 8 once each; Sprint: omitted; combos 0..4).
//    * InputVector is the movement input, two doubles, not normalised: (1, 0) in 43 of 51
//      sprints, (1, 1), (0, 1), (0, 0) and (-1, 0) seen.
//    * bPvpForceFlag is 0 in all 250 triggers; Targets and TargetTransforms are empty for
//      Sprint and Dodge (the pawn's CollisionCylinder or object id 1 for combos).
//    * ItemData is Slot = -1, an empty FItemInstance and Location = 24 (EItemLocation::cCount),
//      "no item", in every one of these calls; the retail client's use of a consumable or a mount
//      names the item (55 calls of 40 retail captures: tests/capture_items.h has 15).
//    * CancelledTime is 0.0016-1.36 s and is NOT the time the ability was held (the same pairs
//      held 0.09-20 s): its meaning is unknown, so nothing here uses it.  CancelSectionIndex is
//      -1 in all 58.
//
//  The parse functions return nullopt for a call that is not that RPC or whose parameters were
//  not decoded (an ItemData instance the codec refuses, a read past the end): the caller
//  must not act on a half-read call.  Since plan B2 the whole FItemInstance is read (item_codec.h): a
//  trigger that names an item (a mount, a consumable) decodes, and TriggerAbility::item_slot is its slot.
// ============================================================================
#pragma once

#include "protocol/repl/replicator.h"
#include "protocol/repl/rpc.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

struct TriggerAbility {
    int64_t  ability_guid = 0;          // the ability record's Guid
    int32_t  client_instance_id = 0;    // the client's per-session counter (0 when omitted)
    int32_t  animation_index = 0;
    double   input_x = 0, input_y = 0;
    bool     pvp_force = false;
    uint32_t target_count = 0;
    /// plan E1, stage 3: the objects the Targets name by NetGUID, in order (an NPC's collision cylinder); the ones named in place (ObjectId 1 and the
    /// export: the outer's GUID and the component's name) are `inline_targets`, in order. The client names a component in place whenever it has no
    /// GUID for it: its own CollisionCylinder in 8 of the e2e capture's 16 Scepter combos, and every component of an NPC the server never exported
    /// (the E1 stage 3 fix: worldd exports no NPC component, so every cast at an NPC names its cylinder so). target_count is both together.
    std::vector<emit::FIntrepidNetworkGUID> targets;
    std::vector<InlineObject>               inline_targets;
    uint32_t target_transform_count = 0;
    int32_t  item_slot = -1;            // ItemData.Slot: -1 is "no item"
    uint32_t item_location = 0;         // ItemData.Location (EItemLocation)
};
// [C Y B - CoA]

struct HeldAbilityCancelled {
    int32_t client_instance_id = 0;
    float   cancelled_time = 0;
    int32_t cancel_section_index = -1;
};

/// ServerTriggerAbility from a decoded call; nullopt for another RPC or an undecoded one.
std::optional<TriggerAbility> parse_trigger_ability(const RpcCall& call);

/// The first two parameters of a ServerTriggerAbility, read by hand from its bits when the call as a whole did not decode (a
/// parameter the codecs refuse): the AbilityGuid and ClientInstanceId start the parameter list (a send bit each, 0 for the
/// default), so what to answer is known even then.  `pos` and `end` are the call's parameter bits in `data`
/// (RpcCall::param_pos, param_pos + param_bits); nothing past `end` or the buffer is read.  nullopt when they do not fit.
struct TriggerPrefix {
    int64_t ability_guid = 0;
    int32_t client_instance_id = 0;
};
std::optional<TriggerPrefix> read_trigger_prefix(const uint8_t* data, size_t data_bytes, size_t pos, size_t end);

/// ServerNotifyHeldAbilityCancelled from a decoded call; nullopt for another RPC or an undecoded one.
std::optional<HeldAbilityCancelled> parse_held_ability_cancelled(const RpcCall& call);

/// ServerEndAbility(ServerInstanceId): the client ends the instance the server gave it (the ReplicatedActiveAbilities item's ServerInstanceId);
/// nullopt for another RPC or an undecoded one.
std::optional<int32_t> parse_end_ability(const RpcCall& call);

/// ClientEndAbility(ClientInstanceId) on `ability` (the AbilityComponent subobject): the server ending an instance on the
/// client.  Reliable (function flags 0x01020cc0).  nullopt when the class has no such RPC.
std::optional<Emission> client_end_ability(const Actor& pawn, const Subobject& ability, int32_t client_instance_id,
                                           const ExportAcks& acks);

}}}  // namespace aoc::protocol::repl

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/dialogue_rpc.h
//
//  The talking window's client call in a typed form, over rpc.h's parameter sets:
//
//    client, on the PlayerController (AAoCPlayerController)
//      ServerSelectDialogueChoice(Response: FDialogueResponse)
//          every move the player makes in the talking window.  The response names what the player did:
//            ResponseType 1 (Choice)       a line of the node on screen: Choice.SourceNodeId is that node, Choice.ChoiceIndex the line's index in the
//                                           node's whole list of lines (not its place among the lines shown), Choice.ChoiceType what kind of line it was
//            ResponseType 2 (SubDialogue)  the quest panel's buttons (QuestResponse 1 Accept, 2 Decline, 3 Close).  Closing the window sends Close
//            ResponseType 3 (ActionWindow) a window a line opened was closed (WindowType says which)
//          The struct is flattened into eight parameters (Response.ResponseType ... Response.WindowType); a struct is always on the wire.
//
//  What the window shows is replicated state on the PlayerController (CurrentDialogueInstance, handles 141-170) which worldd sets and the
//  replicator sends as a property block (bootstrap's dialogue_update); the table below is the part of it the talking window uses.
//
//    PreviousNode.Guid                          the node the player came from
//    PreviousSelectedChoice.SourceNodeId.Guid   the node of the line chosen last, and ...ChoiceIndex the line's index
//    CurrentNode.Guid                           the node now (0 in the greeting)
//    CurrentChoices                             the lines shown: {SourceNodeId.Guid/TypeId/Name, ChoiceIndex, ChoiceType} each
//    SourceHandle.ObjectSource                  the NPC being talked to (the zero reference: nobody)
//    ResponseText.DialogueNodeId.Guid           the node whose text is shown
//    bIsInGreeting                              1 until the first line is chosen
// ============================================================================
#pragma once

#include "protocol/repl/rpc.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>

namespace aoc { namespace protocol { namespace repl {

/// The TypeId every dialogue node id carries (the node table's).
inline constexpr int64_t kDialogueNodeTypeId = 5024462864295026017LL;

enum class DialogueResponseType : uint8_t { None = 0, Choice = 1, SubDialogue = 2, ActionWindow = 3 };
enum class SubDialogueResponse : uint8_t { None = 0, Accept = 1, Decline = 2, Close = 3 };

/// FDialogueResponse as the client sends it.
struct DialogueResponse {
    uint8_t  response_type = 0;     // DialogueResponseType
    uint8_t  quest_response = 0;    // SubDialogueResponse
    int64_t  node = 0;              // Choice.SourceNodeId.Guid
    int64_t  node_type_id = 0;      // Choice.SourceNodeId.TypeId
    int32_t  choice_index = 0;      // Choice.ChoiceIndex
    uint8_t  choice_type = 0;       // Choice.ChoiceType (EDialogChoiceType: 0 Default)
    uint8_t  window_type = 0;       // WindowType (EDialogueActionWindowType)
    bool operator==(const DialogueResponse&) const = default;
};

/// ServerSelectDialogueChoice from a decoded call; nullopt for another RPC, an undecoded call or a parameter of another kind.
std::optional<DialogueResponse> parse_dialogue_response(const RpcCall& call);
/// The call's parameters, for a test that plays the client.  nullopt when `rpc` is not that RPC or a value does not fit.
std::optional<RpcParams> dialogue_response_params(const schema::RpcSchema& rpc, const DialogueResponse& response);

}}}  // namespace aoc::protocol::repl

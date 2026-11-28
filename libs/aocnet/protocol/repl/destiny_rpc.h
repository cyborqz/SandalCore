// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/destiny_rpc.h  (quests milestone, the Destiny window; header only)
//
//  The Destiny window's first RPC pair in a typed form, over rpc.h's parameter sets:
//
//    client, on the PlayerController
//      ServerGetDestinyClientData(DestinyTracks)
//          the window asks for its tracks when it opens (and again when a day counter moves): one element for each of the six tracks the client's design data lists, each
//          {DestinyTrackQuest, DestinyTrackSkillTree, MilestoneRewardObjectives}.  Only DestinyTrackQuest (the track's _02_Root quest) matters to the server.
//    server, on the PlayerController
//      ClientReceiveDestinyClientData(DestinyClientData)
//          the answer: DestinyTrackClientDataArray, one element for each track {DestinyTrackQuest, DestinyTrackInstanceGuid, CompletedTrackQuests, TotalTrackQuests, bIsActive}.
//          The client matches an element to its track by DestinyTrackQuest, not by place.  An element is 330 bits in the game's own answer: the quest id
//          (Guid, TypeId, the name None), the guid, the two numbers and the flag.
// ============================================================================
#pragma once

#include "protocol/repl/rep_block.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/value.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// The type id every quest record id carries (the quest table's, NarrativeQuestV2).
inline constexpr int64_t kQuestRecordTypeId = 6732126019658962568LL;

/// One element of ClientReceiveDestinyClientData.
struct DestinyTrackReply {
    int64_t  track_quest = 0;          // DestinyTrackQuest.Guid
    uint32_t guid[4] = {0, 0, 0, 0};   // DestinyTrackInstanceGuid A-D
    int32_t  completed = 0, total = 0;
    bool     active = false;
    bool operator==(const DestinyTrackReply&) const = default;
};

namespace destiny_detail {

/// An element with every member at its zero value (a name is None), the way the game's own server leaves what it does not set.
inline bool zeroed_element(std::span<const schema::RepCommand> elems, ElementState& e) {
    for (size_t i = 0; i < elems.size(); ++i) {
        auto z = zero_value(elems[i]);
        if (!z) return false;
        e.values[static_cast<uint16_t>(i + 1)] = std::move(*z);
    }
    return true;
}

}  // namespace destiny_detail

/// The DestinyTrackQuest guid of each element of a decoded ServerGetDestinyClientData, in the call's order; at most `limit` elements are read.  Empty for a call that is another RPC,
/// that did not decode, or that names no track.
inline std::vector<int64_t> parse_destiny_request(const RpcCall& call, size_t limit) {
    std::vector<int64_t> out;
    if (!call.decoded || !call.rpc) return out;
    const schema::RepCommand* array = rpc_command(*call.rpc, "DestinyTracks");
    if (!array) return out;
    const auto it = call.params.values.find(array->handle);
    const ArrayValue* list = it == call.params.values.end() ? nullptr : it->second.get_if<ArrayValue>();
    if (!list) return out;
    const auto elems = rpcf::elements_of(*call.rpc, *array);
    const uint16_t guid = rpcf::local(elems, "DestinyTracks.DestinyTrackQuest.Guid");
    if (!guid) return out;
    for (const ElementState& e : list->elements) {
        if (out.size() >= limit) break;
        out.push_back(rpcf::int_or(rpcf::element_value(e, guid), 0).value_or(0));
    }
    return out;
}

/// The request as the client writes it, for a test that plays the client: one element for each of `tracks` (the rest of an element at its default).  nullopt when `rpc` is not that RPC.
inline std::optional<RpcParams> destiny_request_params(const schema::RpcSchema& rpc, std::span<const int64_t> tracks) {
    const schema::RepCommand* array = rpc_command(rpc, "DestinyTracks");
    if (!array) return std::nullopt;
    const auto elems = rpcf::elements_of(rpc, *array);
    const uint16_t guid = rpcf::local(elems, "DestinyTracks.DestinyTrackQuest.Guid"), type = rpcf::local(elems, "DestinyTracks.DestinyTrackQuest.TypeId");
    if (!guid || !type) return std::nullopt;
    ArrayValue list;
    for (const int64_t t : tracks) {
        ElementState e;
        if (!destiny_detail::zeroed_element(elems, e)) return std::nullopt;
        e.values[guid] = Value::i64(t);
        e.values[type] = Value::i64(kQuestRecordTypeId);
        list.elements.push_back(std::move(e));
    }
    RpcParams params;
    if (!set_rpc_param(rpc, params, "DestinyTracks", Value(std::move(list)))) return std::nullopt;
    return params;
}

/// The answer's parameters: one element for each of `tracks`.  nullopt when `rpc` is not ClientReceiveDestinyClientData or a member does not fit.
inline std::optional<RpcParams> destiny_client_data_params(const schema::RpcSchema& rpc, std::span<const DestinyTrackReply> tracks) {
    const schema::RepCommand* array = rpc_command(rpc, "DestinyClientData.DestinyTrackClientDataArray");
    if (!array) return std::nullopt;
    const auto elems = rpcf::elements_of(rpc, *array);
    const uint16_t quest = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackQuest.Guid"), type = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackQuest.TypeId"),
                   a = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackInstanceGuid.A"), b = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackInstanceGuid.B"),
                   c = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackInstanceGuid.C"), d = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackInstanceGuid.D"),
                   done = rpcf::local(elems, "DestinyTrackClientDataArray.CompletedTrackQuests"), total = rpcf::local(elems, "DestinyTrackClientDataArray.TotalTrackQuests"),
                   active = rpcf::local(elems, "DestinyTrackClientDataArray.bIsActive");
    if (!quest || !type || !a || !b || !c || !d || !done || !total || !active) return std::nullopt;
    ArrayValue list;
    for (const DestinyTrackReply& t : tracks) {
        ElementState e;
        if (!destiny_detail::zeroed_element(elems, e)) return std::nullopt;
        e.values[quest] = Value::i64(t.track_quest);
        e.values[type] = Value::i64(kQuestRecordTypeId);
        e.values[a] = Value::i64(static_cast<int32_t>(t.guid[0]));
        e.values[b] = Value::i64(static_cast<int32_t>(t.guid[1]));
        e.values[c] = Value::i64(static_cast<int32_t>(t.guid[2]));
        e.values[d] = Value::i64(static_cast<int32_t>(t.guid[3]));
        e.values[done] = Value::i64(t.completed);
        e.values[total] = Value::i64(t.total);
        e.values[active] = Value::boolean(t.active);
        list.elements.push_back(std::move(e));
    }
    RpcParams params;
    if (!set_rpc_param(rpc, params, "DestinyClientData.DestinyTrackClientDataArray", Value(std::move(list)))) return std::nullopt;
    return params;
}

/// The members of an answer back from decoded parameters, for a test that reads what the server sent (nullopt: not that RPC, or a member of another kind).
inline std::optional<std::vector<DestinyTrackReply>> parse_destiny_client_data(const RpcCall& call) {
    if (!call.decoded || !call.rpc) return std::nullopt;
    const schema::RepCommand* array = rpc_command(*call.rpc, "DestinyClientData.DestinyTrackClientDataArray");
    if (!array) return std::nullopt;
    const auto elems = rpcf::elements_of(*call.rpc, *array);
    const uint16_t quest = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackQuest.Guid"), a = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackInstanceGuid.A"),
                   b = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackInstanceGuid.B"), c = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackInstanceGuid.C"),
                   d = rpcf::local(elems, "DestinyTrackClientDataArray.DestinyTrackInstanceGuid.D"), done = rpcf::local(elems, "DestinyTrackClientDataArray.CompletedTrackQuests"),
                   total = rpcf::local(elems, "DestinyTrackClientDataArray.TotalTrackQuests"), active = rpcf::local(elems, "DestinyTrackClientDataArray.bIsActive");
    if (!quest || !a || !b || !c || !d || !done || !total || !active) return std::nullopt;
    std::vector<DestinyTrackReply> out;
    const auto it = call.params.values.find(array->handle);
    if (it == call.params.values.end()) return out;   // an empty array is not on the wire
    const ArrayValue* list = it->second.get_if<ArrayValue>();
    if (!list) return std::nullopt;
    for (const ElementState& e : list->elements) {
        DestinyTrackReply r;
        const auto q = rpcf::int_or(rpcf::element_value(e, quest), 0);
        const auto w0 = rpcf::int_or(rpcf::element_value(e, a), 0), w1 = rpcf::int_or(rpcf::element_value(e, b), 0), w2 = rpcf::int_or(rpcf::element_value(e, c), 0),
                   w3 = rpcf::int_or(rpcf::element_value(e, d), 0), n = rpcf::int_or(rpcf::element_value(e, done), 0), t = rpcf::int_or(rpcf::element_value(e, total), 0);
        const auto on = rpcf::bool_or(rpcf::element_value(e, active), false);
        if (!q || !w0 || !w1 || !w2 || !w3 || !n || !t || !on) return std::nullopt;
        r.track_quest = *q;
        r.guid[0] = static_cast<uint32_t>(*w0);
        r.guid[1] = static_cast<uint32_t>(*w1);
        r.guid[2] = static_cast<uint32_t>(*w2);
        r.guid[3] = static_cast<uint32_t>(*w3);
        r.completed = static_cast<int32_t>(*n);
        r.total = static_cast<int32_t>(*t);
        r.active = *on;
        out.push_back(r);
    }
    return out;
}

}}}  // namespace aoc::protocol::repl

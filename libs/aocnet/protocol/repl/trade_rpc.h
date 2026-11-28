// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/trade_rpc.h
//
//  Typed parsers and builders for player-to-player trade RPCs on AAoCPlayerController:
//
//    client -> server
//      ServerRequestTrade(CharacterGuid)
//      ServerRequestTradeResponse(TargetCharacterGuid, Response)
//      ServerUpdateTradeItems(TradeGuid, TradeItems: FTradeItems)
//      ServerUpdateTradeTransaction(TradeCharacterGuid, TradeGuid, Request)
//
//    server -> client
//      ClientRequestTrade(CharacterGuid)
//      ClientProcessTradeRequestResponse(CharacterGuid, RequestResponse)
//      ClientBeginTrade(TradeRequestGuid, TargetGuid)
//      ClientUpdateTrade(CharacterGuid, Items: FTradeItems)
//      ClientCloseTrade(Status)
//
//  FTradeItems is flattened in schema into Items (TArray<FItemInstance>), Money
//  (FCurrencyContainer carrying the Money tier in copper) and Version (int32).
// ============================================================================
#pragma once

#include "protocol/repl/market_codec.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// The Money currency tier record GUID and CurrencyTierDefRecordBase type ID used in trade FCurrencyContainer entries.
inline constexpr int64_t kTradeMoneyTier = 6064629127873626112ll;
inline constexpr int64_t kTradeMoneyTierType = 456318420933392641ll;

/// ETradeRequestResponse (4 bits on the wire).
enum class TradeRequestResponse : uint8_t {
    Accept = 0,
    Decline = 1,
    PlayerNotFound = 2,
    Timeout = 3,
    Busy = 4,
    Reject = 5,
    RejectDistance = 6,
    RejectCorrupted = 7,
};

/// ETradeStatus (2 bits on the wire).
enum class TradeStatus : uint8_t {
    Failure = 0,
    Success = 1,
};

/// ETradeTransactionRequest (3 bits on the wire).
enum class TradeTransactionRequest : uint8_t {
    OpenTrade = 0,
    CancelTrade = 1,
    LockTrade = 2,
    UnlockTrade = 3,
    FinalizeTrade = 4,
};

/// Canonical FCurrencyContainer for trade copper: one entry naming the Money tier.
inline CurrencyContainer trade_money_container(int64_t copper) {
    CurrencyContainer c;
    c.entries.push_back(CurrencyEntry{kTradeMoneyTier, kTradeMoneyTierType, copper});
    return c;
}

/// Flattened FTradeItems payload (Items, Money, Version).
struct TradeOffer {
    std::vector<ItemInstance> items;
    CurrencyContainer         money;
    int32_t                   version = 0;
    bool operator==(const TradeOffer&) const = default;
};

/// ServerRequestTrade(CharacterGuid).
struct TradeRequest {
    GuidValue character_guid;
    bool operator==(const TradeRequest&) const = default;
};

/// ServerRequestTradeResponse(TargetCharacterGuid, Response).
struct TradeRequestResponseCall {
    GuidValue target_character_guid;
    uint8_t   response = 0;
    bool operator==(const TradeRequestResponseCall&) const = default;
};

/// ServerUpdateTradeItems(TradeGuid, TradeItems).
struct TradeItemsUpdate {
    GuidValue  trade_guid;
    TradeOffer offer;
    bool operator==(const TradeItemsUpdate&) const = default;
};

/// ServerUpdateTradeTransaction(TradeCharacterGuid, TradeGuid, Request).
struct TradeTransactionUpdate {
    GuidValue trade_character_guid;
    GuidValue trade_guid;
    uint8_t   request = 0;
    bool operator==(const TradeTransactionUpdate&) const = default;
};

/// ClientRequestTrade(CharacterGuid).
struct ClientTradeRequest {
    GuidValue character_guid;
    bool operator==(const ClientTradeRequest&) const = default;
};

/// ClientProcessTradeRequestResponse(CharacterGuid, RequestResponse).
struct ClientTradeRequestResponse {
    GuidValue character_guid;
    uint8_t   response = 0;
    bool operator==(const ClientTradeRequestResponse&) const = default;
};

/// ClientBeginTrade(TradeRequestGuid, TargetGuid).
struct ClientTradeBegin {
    GuidValue trade_request_guid;
    GuidValue target_guid;
    bool operator==(const ClientTradeBegin&) const = default;
};

/// ClientUpdateTrade(CharacterGuid, Items).
struct ClientTradeUpdate {
    GuidValue  character_guid;
    TradeOffer offer;
    bool operator==(const ClientTradeUpdate&) const = default;
};

/// ClientCloseTrade(Status).
struct ClientTradeClose {
    uint8_t status = 0;
    bool operator==(const ClientTradeClose&) const = default;
};

namespace trade_rpc_detail {

inline bool put_guid(const schema::RpcSchema& rpc, RpcParams& params, std::string_view prefix, const GuidValue& g) {
    const std::string p(prefix);
    return set_rpc_param(rpc, params, p + ".A", Value::i64(static_cast<int32_t>(g.a))) &&
           set_rpc_param(rpc, params, p + ".B", Value::i64(static_cast<int32_t>(g.b))) &&
           set_rpc_param(rpc, params, p + ".C", Value::i64(static_cast<int32_t>(g.c))) &&
           set_rpc_param(rpc, params, p + ".D", Value::i64(static_cast<int32_t>(g.d)));
}

inline std::optional<GuidValue> read_guid(const schema::RpcSchema& rpc, const RpcParams& params, std::string_view prefix) {
    const std::string p(prefix);
    const auto a = rpcf::int_or(rpcf::value_of(rpc, params, p + ".A"), 0);
    const auto b = rpcf::int_or(rpcf::value_of(rpc, params, p + ".B"), 0);
    const auto c = rpcf::int_or(rpcf::value_of(rpc, params, p + ".C"), 0);
    const auto d = rpcf::int_or(rpcf::value_of(rpc, params, p + ".D"), 0);
    if (!a || !b || !c || !d) return std::nullopt;
    GuidValue g;
    g.a = static_cast<uint32_t>(static_cast<int32_t>(*a));
    g.b = static_cast<uint32_t>(static_cast<int32_t>(*b));
    g.c = static_cast<uint32_t>(static_cast<int32_t>(*c));
    g.d = static_cast<uint32_t>(static_cast<int32_t>(*d));
    return g;
}

inline bool put_offer(const schema::RpcSchema& rpc, RpcParams& params, std::string_view prefix, const TradeOffer& offer) {
    const std::string p(prefix);
    const std::string items_path = p + ".Items";
    const schema::RepCommand* array = rpc_command(rpc, items_path);
    if (!array) return false;
    const auto elems = rpcf::elements_of(rpc, *array);
    const uint16_t item_handle = rpcf::local(elems, "Items");
    if (!item_handle) return false;
    ArrayValue list;
    list.elements.reserve(offer.items.size());
    for (const ItemInstance& inst : offer.items) {
        ElementState e;
        e.values[item_handle] = Value(inst);
        list.elements.push_back(std::move(e));
    }
    return set_rpc_param(rpc, params, items_path, Value(std::move(list))) &&
           set_rpc_param(rpc, params, p + ".Money", Value(offer.money)) &&
           set_rpc_param(rpc, params, p + ".Version", Value::i64(offer.version));
}

inline std::optional<TradeOffer> read_offer(const schema::RpcSchema& rpc, const RpcParams& params, std::string_view prefix) {
    const std::string p(prefix);
    const std::string items_path = p + ".Items";
    const schema::RepCommand* array = rpc_command(rpc, items_path);
    if (!array) return std::nullopt;
    const auto elems = rpcf::elements_of(rpc, *array);
    const uint16_t item_handle = rpcf::local(elems, "Items");
    if (!item_handle) return std::nullopt;
    TradeOffer offer;
    if (const Value* arr_v = rpcf::value_of(rpc, params, items_path)) {
        const auto* arr = arr_v->get_if<ArrayValue>();
        if (!arr) return std::nullopt;
        offer.items.reserve(arr->elements.size());
        for (const ElementState& e : arr->elements) {
            const auto it = e.values.find(item_handle);
            const auto* inst = it != e.values.end() ? it->second.get_if<ItemInstance>() : nullptr;
            if (!inst) return std::nullopt;
            offer.items.push_back(*inst);
        }
    }
    if (const Value* money_v = rpcf::value_of(rpc, params, p + ".Money")) {
        const auto* m = money_v->get_if<CurrencyContainer>();
        if (!m) return std::nullopt;
        offer.money = *m;
    }
    const auto ver = rpcf::int_or(rpcf::value_of(rpc, params, p + ".Version"), 0);
    if (!ver) return std::nullopt;
    offer.version = static_cast<int32_t>(*ver);
    return offer;
}

}  // namespace trade_rpc_detail

// ---- C->S parsers -----------------------------------------------------------

inline std::optional<TradeRequest> parse_trade_request(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string_view(call.rpc->name) != "ServerRequestTrade") return std::nullopt;
    const auto guid = trade_rpc_detail::read_guid(*call.rpc, call.params, "CharacterGuid");
    if (!guid) return std::nullopt;
    return TradeRequest{*guid};
}

inline std::optional<TradeRequestResponseCall> parse_trade_request_response(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string_view(call.rpc->name) != "ServerRequestTradeResponse") return std::nullopt;
    const auto target = trade_rpc_detail::read_guid(*call.rpc, call.params, "TargetCharacterGuid");
    const auto resp = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, "Response"), 0);
    if (!target || !resp || *resp > 0xFFu) return std::nullopt;
    return TradeRequestResponseCall{*target, static_cast<uint8_t>(*resp)};
}

inline std::optional<TradeItemsUpdate> parse_trade_items_update(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string_view(call.rpc->name) != "ServerUpdateTradeItems") return std::nullopt;
    const auto trade_guid = trade_rpc_detail::read_guid(*call.rpc, call.params, "TradeGuid");
    const auto offer = trade_rpc_detail::read_offer(*call.rpc, call.params, "TradeItems");
    if (!trade_guid || !offer) return std::nullopt;
    return TradeItemsUpdate{*trade_guid, *offer};
}

inline std::optional<TradeTransactionUpdate> parse_trade_transaction(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string_view(call.rpc->name) != "ServerUpdateTradeTransaction") return std::nullopt;
    const auto partner = trade_rpc_detail::read_guid(*call.rpc, call.params, "TradeCharacterGuid");
    const auto trade_guid = trade_rpc_detail::read_guid(*call.rpc, call.params, "TradeGuid");
    const auto req = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, "Request"), 0);
    if (!partner || !trade_guid || !req || *req > 0xFFu) return std::nullopt;
    return TradeTransactionUpdate{*partner, *trade_guid, static_cast<uint8_t>(*req)};
}

// ---- S->C parsers (for verification and tests) ------------------------------

inline std::optional<ClientTradeRequest> parse_client_request_trade(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string_view(call.rpc->name) != "ClientRequestTrade") return std::nullopt;
    const auto guid = trade_rpc_detail::read_guid(*call.rpc, call.params, "CharacterGuid");
    if (!guid) return std::nullopt;
    return ClientTradeRequest{*guid};
}

inline std::optional<ClientTradeRequestResponse> parse_client_trade_response(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string_view(call.rpc->name) != "ClientProcessTradeRequestResponse") return std::nullopt;
    const auto guid = trade_rpc_detail::read_guid(*call.rpc, call.params, "CharacterGuid");
    const auto resp = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, "RequestResponse"), 0);
    if (!guid || !resp || *resp > 0xFFu) return std::nullopt;
    return ClientTradeRequestResponse{*guid, static_cast<uint8_t>(*resp)};
}

inline std::optional<ClientTradeBegin> parse_client_begin_trade(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string_view(call.rpc->name) != "ClientBeginTrade") return std::nullopt;
    const auto trade_guid = trade_rpc_detail::read_guid(*call.rpc, call.params, "TradeRequestGuid");
    const auto target = trade_rpc_detail::read_guid(*call.rpc, call.params, "TargetGuid");
    if (!trade_guid || !target) return std::nullopt;
    return ClientTradeBegin{*trade_guid, *target};
}

inline std::optional<ClientTradeUpdate> parse_client_update_trade(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string_view(call.rpc->name) != "ClientUpdateTrade") return std::nullopt;
    const auto guid = trade_rpc_detail::read_guid(*call.rpc, call.params, "CharacterGuid");
    const auto offer = trade_rpc_detail::read_offer(*call.rpc, call.params, "Items");
    if (!guid || !offer) return std::nullopt;
    return ClientTradeUpdate{*guid, *offer};
}

inline std::optional<ClientTradeClose> parse_client_close_trade(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string_view(call.rpc->name) != "ClientCloseTrade") return std::nullopt;
    const auto status = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, "Status"), 0);
    if (!status || *status > 0xFFu) return std::nullopt;
    return ClientTradeClose{static_cast<uint8_t>(*status)};
}

// ---- S->C builders ----------------------------------------------------------

inline std::optional<RpcParams> client_request_trade_params(const schema::RpcSchema& rpc, const GuidValue& from) {
    RpcParams params;
    if (!trade_rpc_detail::put_guid(rpc, params, "CharacterGuid", from)) return std::nullopt;
    return params;
}

inline std::optional<RpcParams> client_trade_response_params(const schema::RpcSchema& rpc, const GuidValue& target, uint8_t response) {
    RpcParams params;
    if (!trade_rpc_detail::put_guid(rpc, params, "CharacterGuid", target) ||
        !set_rpc_param(rpc, params, "RequestResponse", Value::u64(response))) {
        return std::nullopt;
    }
    return params;
}

inline std::optional<RpcParams> client_begin_trade_params(const schema::RpcSchema& rpc, const GuidValue& trade_guid, const GuidValue& partner) {
    RpcParams params;
    if (!trade_rpc_detail::put_guid(rpc, params, "TradeRequestGuid", trade_guid) ||
        !trade_rpc_detail::put_guid(rpc, params, "TargetGuid", partner)) {
        return std::nullopt;
    }
    return params;
}

inline std::optional<RpcParams> client_update_trade_params(const schema::RpcSchema& rpc, const GuidValue& owner, const TradeOffer& offer) {
    RpcParams params;
    if (!trade_rpc_detail::put_guid(rpc, params, "CharacterGuid", owner) ||
        !trade_rpc_detail::put_offer(rpc, params, "Items", offer)) {
        return std::nullopt;
    }
    return params;
}

inline std::optional<RpcParams> client_close_trade_params(const schema::RpcSchema& rpc, uint8_t status) {
    RpcParams params;
    if (!set_rpc_param(rpc, params, "Status", Value::u64(status))) return std::nullopt;
    return params;
}

// ---- C->S builders (for tests) ----------------------------------------------

inline std::optional<RpcParams> server_request_trade_params(const schema::RpcSchema& rpc, const GuidValue& target) {
    RpcParams params;
    if (!trade_rpc_detail::put_guid(rpc, params, "CharacterGuid", target)) return std::nullopt;
    return params;
}

inline std::optional<RpcParams> server_trade_response_params(const schema::RpcSchema& rpc, const GuidValue& requester, uint8_t response) {
    RpcParams params;
    if (!trade_rpc_detail::put_guid(rpc, params, "TargetCharacterGuid", requester) ||
        !set_rpc_param(rpc, params, "Response", Value::u64(response))) {
        return std::nullopt;
    }
    return params;
}

inline std::optional<RpcParams> server_update_trade_items_params(const schema::RpcSchema& rpc, const GuidValue& trade_guid, const TradeOffer& offer) {
    RpcParams params;
    if (!trade_rpc_detail::put_guid(rpc, params, "TradeGuid", trade_guid) ||
        !trade_rpc_detail::put_offer(rpc, params, "TradeItems", offer)) {
        return std::nullopt;
    }
    return params;
}

inline std::optional<RpcParams> server_update_trade_transaction_params(const schema::RpcSchema& rpc, const GuidValue& partner,
                                                                       const GuidValue& trade_guid, uint8_t request) {
    RpcParams params;
    if (!trade_rpc_detail::put_guid(rpc, params, "TradeCharacterGuid", partner) ||
        !trade_rpc_detail::put_guid(rpc, params, "TradeGuid", trade_guid) ||
        !set_rpc_param(rpc, params, "Request", Value::u64(request))) {
        return std::nullopt;
    }
    return params;
}

}}}  // namespace aoc::protocol::repl

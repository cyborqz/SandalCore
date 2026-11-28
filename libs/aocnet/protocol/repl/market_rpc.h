// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/market_rpc.h
//
//  The marketplace calls of the window in a typed form, over rpc.h's parameter sets and market_codec.h's price:
//
//    client, on the PlayerController
//      ServerRequestMarketplaceItemRegistration(MarketplaceActorGuid, Item, Quantity, PricePerUnit)
//          the Sell tab's listing: the market, the stack named by its instance guid, the units listed and the unit price
//      ServerRequestMarketplaceHeaders(MarketplaceActorGuid, HeaderVersion)
//          the storefront page: the market and the version of the headers the client holds
//      ServerRequestMarketplaceData(MarketplaceActorGuid, PlayerGuid)
//          one seller's listings (the Sell tab sends its own guid); the storefront page is the Headers call's
//      ServerRequestMarketplaceItemPurchase(MarketplaceActorGuid, MarketplaceItemGuid, Quantity)
//          the Buy panel's confirmed purchase: the market, the listing and the units
//      ServerRequestMarketplaceItemRemoval(MarketplaceActorGuid, MarketplaceItemGuid)
//          the Sell panel's Cancel Listing: the market and the listing
//      ServerSearchMarketplaceHeaders(MarketplaceActorGuid, SearchItemString, ItemRarity)
//          the Buy tab's search: the market, the text typed and the rarity picked (7: any; the wire leaves 0 out)
//      ServerRequestMarketplaceHeaderUpdate(MarketplaceActorGuid, HeaderDescription)
//          the Sell tab's edit of the storefront description: the market and the new text (0 to 50 characters)
//    server, on the PlayerController
//      ClientMarketplaceItemRegistrationResponse(Item: FMarketplaceItemData, Response)
//          the listing as it was made (Response 0) or a refusal (no Item)
//      ClientReceiveMarketplaceHeaders(NodeId, Headers: FMarketplaceHeader[], HeaderVersion, SearchItemString, ItemRarity, bComplete)
//          one storefront per seller at the market, bComplete on the last page; the version and the rarity the client knows
//      ClientReceiveMarketplaceData(PlayerGuid, MarketplaceItemData: FMarketplaceItemData[])
//          that seller's listings at the market
//      ClientMarketplaceItemPurchaseResponse(MarketplaceItemGuid, Quantity, Response)
//          the answer to a purchase: the listing and the units asked for, as the request named them, and the response (0 success)
//      ClientMarketplaceItemRemovalResponse(MarketplaceItemGuid, Response)
//          the answer to a removal
//      ClientMarketplaceHeaderUpdateResponse(HeaderDescription, Response)
//          the answer to a description edit (0: the text the storefront shows now) and to a search that is refused (16: the text that was sent)
//
//  FMarketplaceItemData and FMarketplaceHeader are flattened schema structs, not NetSerialize: each field is a command and
//  an array element holds its fields by local handle (rpc_fields.h).  The price is FCurrencyContainer (market_codec.h).
// ============================================================================
#pragma once

#include "protocol/repl/market_codec.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// The Money tier's record type id (CurrencyTierDefRecordBase), beside items::kMoneyTier.
inline constexpr int64_t kMoneyTierType = 456318420933392641ll;
/// The version the client knows (the recorded header pages carry it).
inline constexpr uint64_t kMarketplaceHeaderVersion = 1843;
/// The rarity a page that filters nothing carries (the recorded unfiltered pages), and the rarity a search sends for "any".
inline constexpr uint64_t kMarketplaceAnyRarity = 7;
/// The text a search may hold (the game's own check: 1 to 50 characters) and the one a storefront description may hold.
inline constexpr size_t kMarketplaceSearchMax = 50;
inline constexpr size_t kMarketplaceDescriptionMax = 50;

/// One listing as FMarketplaceItemData carries it.
struct MarketListing {
    int64_t        node = 0;
    GuidValue      seller;
    std::string    seller_name;
    GuidValue      guid;
    ItemInstance   item;
    std::string    item_name;
    CurrencyContainer price;
    double         issue_date = 0;
    double         expiration_date = 0;
    uint8_t        status = 0;
    bool operator==(const MarketListing&) const = default;
};

/// One storefront as FMarketplaceHeader carries it.
struct MarketHeader {
    int64_t     node = 0;
    GuidValue   seller;
    std::string seller_name;
    std::string description;
    double      issue_date = 0;
    bool        city_node = false;
    bool operator==(const MarketHeader&) const = default;
};

/// ServerRequestMarketplaceItemRegistration's parameters.
struct MarketRegistration {
    GuidValue         market;
    ItemInstance      item;
    int32_t           quantity = 0;
    CurrencyContainer price;
};

/// ServerRequestMarketplaceItemPurchase's parameters: the market, the listing and the units asked for.
struct MarketPurchase {
    GuidValue market;
    GuidValue listing;
    int32_t   quantity = 0;
};

/// ServerRequestMarketplaceItemRemoval's parameters: the market and the listing.
struct MarketRemoval {
    GuidValue market;
    GuidValue listing;
};

/// ServerRequestMarketplaceHeaders' parameters: the market and the version of the headers the client already holds.
struct MarketHeadersRequest {
    GuidValue market;
    uint64_t  version = 0;
};

/// ServerSearchMarketplaceHeaders' parameters: the market, the text typed (empty when the wire has none) and the rarity picked (0 when the wire has none: Common; 7: any).
struct MarketSearchRequest {
    GuidValue   market;
    std::string text;
    uint64_t    rarity = 0;
};

/// ServerRequestMarketplaceHeaderUpdate's parameters: the market and the new description (empty when the wire has none).
struct MarketHeaderUpdateRequest {
    GuidValue   market;
    std::string description;
};

/// What a headers page says besides its headers: the version the client keeps them under, and the search and rarity the page answers.  The pages of the window's open and of a
/// refresh carry the market's version, no search and rarity 7; a search's pages carry no version (retail's did not: the client then keeps its own) and echo the text and the rarity.
struct MarketPageEcho {
    uint64_t    version = kMarketplaceHeaderVersion;
    std::string search;
    uint64_t    rarity = kMarketplaceAnyRarity;
};

/// ServerRequestMarketplaceItemRegistration from a decoded call; nullopt for another RPC, an undecoded call or no item.
std::optional<MarketRegistration> parse_market_registration(const RpcCall& call);

/// ServerRequestMarketplaceHeaders from a decoded call; nullopt for another RPC, an undecoded call or no market.
std::optional<MarketHeadersRequest> parse_market_headers(const RpcCall& call);

/// ServerSearchMarketplaceHeaders from a decoded call; nullopt for another RPC, an undecoded call or no market.
std::optional<MarketSearchRequest> parse_market_search(const RpcCall& call);

/// ServerRequestMarketplaceHeaderUpdate from a decoded call; nullopt for another RPC, an undecoded call or no market.
std::optional<MarketHeaderUpdateRequest> parse_market_header_update(const RpcCall& call);

/// ServerRequestMarketplaceItemPurchase from a decoded call; nullopt for another RPC, an undecoded call or a missing guid or quantity.
std::optional<MarketPurchase> parse_market_purchase(const RpcCall& call);

/// ServerRequestMarketplaceItemRemoval from a decoded call; nullopt for another RPC, an undecoded call or a missing guid.
std::optional<MarketRemoval> parse_market_removal(const RpcCall& call);

/// The purchase answer's parameters: the listing, the units asked for (the answer carries what the request named, a refusal included) and Response.
std::optional<RpcParams> market_purchase_params(const schema::RpcSchema& rpc, const GuidValue& listing, int32_t quantity, uint8_t response);

/// The removal answer's parameters: the listing and Response.
std::optional<RpcParams> market_removal_params(const schema::RpcSchema& rpc, const GuidValue& listing, uint8_t response);

/// The registration answer's parameters: the listing and Response (0: made; otherwise no Item is sent).
std::optional<RpcParams> market_registration_params(const schema::RpcSchema& rpc, const MarketListing& listing, uint8_t response);

/// The headers page's parameters: the node, one header per seller and whether the page is the last.  `echo` is what the page says besides: the market's version and the search
/// it answers (the default is the page of the window's open: the game's own version, no search, any rarity).
std::optional<RpcParams> market_headers_params(const schema::RpcSchema& rpc, int64_t node, std::span<const MarketHeader> headers, bool complete);
std::optional<RpcParams> market_headers_params(const schema::RpcSchema& rpc, int64_t node, std::span<const MarketHeader> headers, bool complete, const MarketPageEcho& echo);

/// ClientMarketplaceHeaderUpdateResponse's parameters: the text and Response (0: the description the storefront shows now; 14, 15: the refused text; 16: the search text that was
/// refused).
std::optional<RpcParams> market_header_update_params(const schema::RpcSchema& rpc, const std::string& text, uint8_t response);

/// One seller's listings at the market.
std::optional<RpcParams> market_data_params(const schema::RpcSchema& rpc, const GuidValue& seller, std::span<const MarketListing> listings);

// ---- the builders -----------------------------------------------------------

namespace market_rpc_detail {

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

/// Writes one listing into an array element by the element commands' local handles.
inline bool listing_element(const schema::RpcSchema& rpc, const schema::RepCommand& array, const MarketListing& l, ElementState& e) {
    const auto elems = rpcf::elements_of(rpc, array);
    const auto at = [&](const char* path) { return rpcf::local(elems, path); };
    const uint16_t node = at("MarketplaceItemData.NodeGuid"), seller = at("MarketplaceItemData.PlayerGuid.A"), seller_b = at("MarketplaceItemData.PlayerGuid.B"),
                   seller_c = at("MarketplaceItemData.PlayerGuid.C"), seller_d = at("MarketplaceItemData.PlayerGuid.D"), name = at("MarketplaceItemData.PlayerName"),
                   guid = at("MarketplaceItemData.MarketplaceItemGuid.A"), guid_b = at("MarketplaceItemData.MarketplaceItemGuid.B"),
                   guid_c = at("MarketplaceItemData.MarketplaceItemGuid.C"), guid_d = at("MarketplaceItemData.MarketplaceItemGuid.D"),
                   item = at("MarketplaceItemData.Item"), item_name = at("MarketplaceItemData.ItemName"), price = at("MarketplaceItemData.PricePerUnit"),
                   issue = at("MarketplaceItemData.IssueDate"), expiry = at("MarketplaceItemData.ExpirationDate"), status = at("MarketplaceItemData.Status");
    if (!node || !seller || !seller_b || !seller_c || !seller_d || !name || !guid || !guid_b || !guid_c || !guid_d || !item || !item_name || !price || !issue || !expiry || !status)
        return false;
    e.values[node] = Value::i64(l.node);
    e.values[seller] = Value::i64(static_cast<int32_t>(l.seller.a));
    e.values[seller_b] = Value::i64(static_cast<int32_t>(l.seller.b));
    e.values[seller_c] = Value::i64(static_cast<int32_t>(l.seller.c));
    e.values[seller_d] = Value::i64(static_cast<int32_t>(l.seller.d));
    e.values[name] = Value::str(l.seller_name);
    e.values[guid] = Value::i64(static_cast<int32_t>(l.guid.a));
    e.values[guid_b] = Value::i64(static_cast<int32_t>(l.guid.b));
    e.values[guid_c] = Value::i64(static_cast<int32_t>(l.guid.c));
    e.values[guid_d] = Value::i64(static_cast<int32_t>(l.guid.d));
    e.values[item] = Value(l.item);
    e.values[item_name] = Value::str(l.item_name);
    e.values[price] = Value(l.price);
    e.values[issue] = Value::f64(l.issue_date);
    e.values[expiry] = Value::f64(l.expiration_date);
    e.values[status] = Value::u64(l.status);
    return true;
}

inline bool header_element(const schema::RpcSchema& rpc, const schema::RepCommand& array, const MarketHeader& h, ElementState& e) {
    const auto elems = rpcf::elements_of(rpc, array);
    const auto at = [&](const char* path) { return rpcf::local(elems, path); };
    const uint16_t node = at("Headers.NodeGuid"), seller = at("Headers.PlayerGuid.A"), seller_b = at("Headers.PlayerGuid.B"), seller_c = at("Headers.PlayerGuid.C"),
                   seller_d = at("Headers.PlayerGuid.D"), name = at("Headers.PlayerName"), description = at("Headers.Description"),
                   issue = at("Headers.IssueDate"), city = at("Headers.bIsCityNodeHeader");
    if (!node || !seller || !seller_b || !seller_c || !seller_d || !name || !description || !issue || !city) return false;
    e.values[node] = Value::i64(h.node);
    e.values[seller] = Value::i64(static_cast<int32_t>(h.seller.a));
    e.values[seller_b] = Value::i64(static_cast<int32_t>(h.seller.b));
    e.values[seller_c] = Value::i64(static_cast<int32_t>(h.seller.c));
    e.values[seller_d] = Value::i64(static_cast<int32_t>(h.seller.d));
    e.values[name] = Value::str(h.seller_name);
    e.values[description] = Value::str(h.description);
    e.values[issue] = Value::f64(h.issue_date);
    e.values[city] = Value::boolean(h.city_node);
    return true;
}

}  // namespace market_rpc_detail

inline std::optional<MarketRegistration> parse_market_registration(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string(call.rpc->name) != "ServerRequestMarketplaceItemRegistration") return std::nullopt;
    const auto market = market_rpc_detail::read_guid(*call.rpc, call.params, "MarketplaceActorGuid");
    const auto* item = rpcf::as<ItemInstance>(rpcf::value_of(*call.rpc, call.params, "Item"));
    const auto quantity = rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "Quantity"), 0);
    if (!market || !item || !quantity) return std::nullopt;
    MarketRegistration r;
    r.market = *market;
    r.item = *item;
    r.quantity = static_cast<int32_t>(*quantity);
    if (const auto* price = rpcf::as<CurrencyContainer>(rpcf::value_of(*call.rpc, call.params, "PricePerUnit"))) r.price = *price;
    return r;
}

inline std::optional<MarketHeadersRequest> parse_market_headers(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string(call.rpc->name) != "ServerRequestMarketplaceHeaders") return std::nullopt;
    const auto market = market_rpc_detail::read_guid(*call.rpc, call.params, "MarketplaceActorGuid");
    if (!market) return std::nullopt;
    MarketHeadersRequest r;
    r.market = *market;
    r.version = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, "HeaderVersion"), 0).value_or(0);
    return r;
}

inline std::optional<MarketSearchRequest> parse_market_search(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string(call.rpc->name) != "ServerSearchMarketplaceHeaders") return std::nullopt;
    const auto market = market_rpc_detail::read_guid(*call.rpc, call.params, "MarketplaceActorGuid");
    if (!market) return std::nullopt;
    MarketSearchRequest r;
    r.market = *market;
    if (const auto* text = rpcf::as<std::string>(rpcf::value_of(*call.rpc, call.params, "SearchItemString"))) r.text = *text;   // (an empty text is left off the wire)
    r.rarity = rpcf::uint_or(rpcf::value_of(*call.rpc, call.params, "ItemRarity"), 0).value_or(0);                              // (so is the rarity 0)
    return r;
}

inline std::optional<MarketHeaderUpdateRequest> parse_market_header_update(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string(call.rpc->name) != "ServerRequestMarketplaceHeaderUpdate") return std::nullopt;
    const auto market = market_rpc_detail::read_guid(*call.rpc, call.params, "MarketplaceActorGuid");
    if (!market) return std::nullopt;
    MarketHeaderUpdateRequest r;
    r.market = *market;
    if (const auto* text = rpcf::as<std::string>(rpcf::value_of(*call.rpc, call.params, "HeaderDescription"))) r.description = *text;
    return r;
}

inline std::optional<MarketPurchase> parse_market_purchase(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string(call.rpc->name) != "ServerRequestMarketplaceItemPurchase") return std::nullopt;
    const auto market = market_rpc_detail::read_guid(*call.rpc, call.params, "MarketplaceActorGuid");
    const auto listing = market_rpc_detail::read_guid(*call.rpc, call.params, "MarketplaceItemGuid");
    const auto quantity = rpcf::int_or(rpcf::value_of(*call.rpc, call.params, "Quantity"), 0);
    if (!market || !listing || !quantity) return std::nullopt;
    MarketPurchase r;
    r.market = *market;
    r.listing = *listing;
    r.quantity = static_cast<int32_t>(*quantity);
    return r;
}

inline std::optional<MarketRemoval> parse_market_removal(const RpcCall& call) {
    if (!call.decoded || !call.rpc || std::string(call.rpc->name) != "ServerRequestMarketplaceItemRemoval") return std::nullopt;
    const auto market = market_rpc_detail::read_guid(*call.rpc, call.params, "MarketplaceActorGuid");
    const auto listing = market_rpc_detail::read_guid(*call.rpc, call.params, "MarketplaceItemGuid");
    if (!market || !listing) return std::nullopt;
    return MarketRemoval{*market, *listing};
}

inline std::optional<RpcParams> market_purchase_params(const schema::RpcSchema& rpc, const GuidValue& listing, int32_t quantity, uint8_t response) {
    RpcParams params;
    if (!market_rpc_detail::put_guid(rpc, params, "MarketplaceItemGuid", listing) || !set_rpc_param(rpc, params, "Quantity", Value::i64(quantity)) ||
        !set_rpc_param(rpc, params, "Response", Value::u64(response))) {
        return std::nullopt;
    }
    return params;
}

inline std::optional<RpcParams> market_removal_params(const schema::RpcSchema& rpc, const GuidValue& listing, uint8_t response) {
    RpcParams params;
    if (!market_rpc_detail::put_guid(rpc, params, "MarketplaceItemGuid", listing) || !set_rpc_param(rpc, params, "Response", Value::u64(response))) return std::nullopt;
    return params;
}

inline std::optional<RpcParams> market_registration_params(const schema::RpcSchema& rpc, const MarketListing& l, uint8_t response) {
    RpcParams params;
    if (response == 0) {
        const bool ok = set_rpc_param(rpc, params, "Item.NodeGuid", Value::i64(l.node)) &&
                        market_rpc_detail::put_guid(rpc, params, "Item.PlayerGuid", l.seller) &&
                        set_rpc_param(rpc, params, "Item.PlayerName", Value::str(l.seller_name)) &&
                        market_rpc_detail::put_guid(rpc, params, "Item.MarketplaceItemGuid", l.guid) &&
                        set_rpc_param(rpc, params, "Item.Item", Value(l.item)) &&
                        set_rpc_param(rpc, params, "Item.ItemName", Value::str(l.item_name)) &&
                        set_rpc_param(rpc, params, "Item.PricePerUnit", Value(l.price)) &&
                        set_rpc_param(rpc, params, "Item.IssueDate", Value::f64(l.issue_date)) &&
                        set_rpc_param(rpc, params, "Item.ExpirationDate", Value::f64(l.expiration_date)) &&
                        set_rpc_param(rpc, params, "Item.Status", Value::u64(l.status));
        if (!ok) return std::nullopt;
    }
    if (!set_rpc_param(rpc, params, "Response", Value::u64(response))) return std::nullopt;
    return params;
}

inline std::optional<RpcParams> market_headers_params(const schema::RpcSchema& rpc, int64_t node, std::span<const MarketHeader> headers, bool complete) {
    return market_headers_params(rpc, node, headers, complete, MarketPageEcho{});
}

inline std::optional<RpcParams> market_header_update_params(const schema::RpcSchema& rpc, const std::string& text, uint8_t response) {
    RpcParams params;
    if (!text.empty() && !set_rpc_param(rpc, params, "HeaderDescription", Value::str(text))) return std::nullopt;
    if (!set_rpc_param(rpc, params, "Response", Value::u64(response))) return std::nullopt;
    return params;
}

inline std::optional<RpcParams> market_headers_params(const schema::RpcSchema& rpc, int64_t node, std::span<const MarketHeader> headers, bool complete, const MarketPageEcho& echo) {
    const schema::RepCommand* array = rpc_command(rpc, "Headers");
    if (!array) return std::nullopt;
    ArrayValue list;
    for (const MarketHeader& h : headers) {
        ElementState e;
        if (!market_rpc_detail::header_element(rpc, *array, h, e)) return std::nullopt;
        list.elements.push_back(std::move(e));
    }
    RpcParams params;
    if (!set_rpc_param(rpc, params, "NodeId", Value::i64(node)) || !set_rpc_param(rpc, params, "Headers", Value(std::move(list))) ||
        !set_rpc_param(rpc, params, "HeaderVersion", Value::u64(echo.version)) || !set_rpc_param(rpc, params, "ItemRarity", Value::u64(echo.rarity)) ||
        !set_rpc_param(rpc, params, "bComplete", Value::boolean(complete)))
        return std::nullopt;
    if (!echo.search.empty() && !set_rpc_param(rpc, params, "SearchItemString", Value::str(echo.search))) return std::nullopt;
    return params;
}

inline std::optional<RpcParams> market_data_params(const schema::RpcSchema& rpc, const GuidValue& seller, std::span<const MarketListing> listings) {
    const schema::RepCommand* array = rpc_command(rpc, "MarketplaceItemData");
    if (!array) return std::nullopt;
    ArrayValue list;
    for (const MarketListing& l : listings) {
        ElementState e;
        if (!market_rpc_detail::listing_element(rpc, *array, l, e)) return std::nullopt;
        list.elements.push_back(std::move(e));
    }
    RpcParams params;
    if (!market_rpc_detail::put_guid(rpc, params, "PlayerGuid", seller) || !set_rpc_param(rpc, params, "MarketplaceItemData", Value(std::move(list)))) return std::nullopt;
    return params;
}

}}}  // namespace aoc::protocol::repl

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/hit_rpc.h
//
//  The hit-verification RPCs of plan D2 in a typed form.  Even a self buff goes through the client-verified hit path (35 of
//  the 43 self abilities and every aimed one have a NET_ServerVerify hit): the client sends the server
//
//    RequestHitVerification_Server(HitRequest)      on the pawn's UAoCAbilityComponent, reliable (field 22)
//        FAbilityHitRequest { HitComponents TArray<{HitComponent UObject*, ServerTimestamp float}>, AbilityGuid int64,
//                             TargetingGuid int64, HitGuid int64, ActorLocation FVector_NetQuantize,
//                             ActorYawAndPitch uint32 }       (TimeReceived is the server's own member, not replicated)
//
//  and the server answers with the hits it confirmed, in the PlayerController's
//
//    ClientHitConfirmationsReliable(Hits)           field 171, flags 0x01020cc0
//    ClientHitConfirmations(Hits)                   field 170, flags 0x01020c40 (unreliable)
//        FSerializedClientHitConfirmations           hit_codec.h
//
//  [observed, the retail captures]: 6,955 requests (290 captures), 6,845 + 270 confirmations.  A request names up to nine
//  components (one in 82% of them, two in 17%; a few none) by plain object GUID, never in the inline form; TargetingGuid is
//  `Default_Self` (6064628968549646338) in 317 requests (a self ability naming its own pawn) and a target record otherwise.
//  ActorLocation is the caster's position in whole centimetres; ActorYawAndPitch packs its view rotation.
//
//  The parse functions return nullopt for a call that is not that RPC or whose parameters were not decoded: the caller
//  must not act on a half-read call.  A request names a component either by GUID or, when the client cannot, in the inline
//  form of value.h (read only with CodecOptions::inline_objects); HitComponentRef keeps whichever it was.
// ============================================================================
#pragma once

#include "protocol/repl/hit_codec.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/rpc.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// `TargetingGuid` of a hit that names the caster's own pawn (the targeting record Default_Self).
constexpr int64_t kTargetingDefaultSelf = 6064628968549646338ll;

/// The most components a request may list (the retail maximum is 9: ours bounds what a hostile call builds).
constexpr size_t kMaxHitRequestComponents = 256;
// ~ C-Y-B - CoA ~

/// One component the client hit (a ClientHitComponent): the object, by GUID or in place, and the server timestamp the
/// client held for it (a float: the synchronized time, seconds).
struct HitComponentRef {
    emit::FIntrepidNetworkGUID object;
    std::optional<InlineObject> inline_object;      // set: the component is named in place and `object` is unused
    float                       server_timestamp = 0;
    bool operator==(const HitComponentRef& o) const {
        return object == o.object && inline_object == o.inline_object && same_bits(server_timestamp, o.server_timestamp);
    }
};

struct HitRequest {
    std::vector<HitComponentRef> components;
    int64_t                      ability_guid = 0;
    int64_t                      targeting_guid = 0;
    int64_t                      hit_guid = 0;
    QuantVec                     actor_location;    // FVector_NetQuantize: whole centimetres, as the wire integers
    uint32_t                     actor_yaw_and_pitch = 0;
    bool operator==(const HitRequest&) const = default;
};

/// RequestHitVerification_Server from a decoded call; nullopt for another RPC, an undecoded call or a value of another kind.
std::optional<HitRequest> parse_hit_request(const RpcCall& call);

/// The parameters of a RequestHitVerification_Server call on `rpc` (the AbilityComponent's schema entry); nullopt when `rpc`
/// is not that RPC or a value does not fit (a component object that is not writable, a quantized component over 63 bits).
std::optional<RpcParams> hit_request_params(const schema::RpcSchema& rpc, const HitRequest& request);

/// The hits of a decoded ClientHitConfirmationsReliable or ClientHitConfirmations call; nullopt for another RPC, an
/// undecoded call or no Hits value.
std::optional<HitConfirmations> parse_hit_confirmations(const RpcCall& call);

/// The parameters of a call of either confirmation RPC on `rpc` (the PlayerController's schema entry); nullopt for another
/// RPC or hits that do not fit hit_codec.h's bounds.
std::optional<RpcParams> hit_confirmations_params(const schema::RpcSchema& rpc, const HitConfirmations& hits);

/// ClientHitConfirmationsReliable (`reliable`) or ClientHitConfirmations on the PlayerController `pc`: the emission that
/// carries the hits.  nullopt when `pc`'s class has no such RPC or the hits do not fit.  `extra_exports`: the chains of the
/// objects the hits name that the client may have no GUID for yet (a target's CollisionCylinder: component_export of its
/// actor), exported with the call unless acknowledged (call_rpc's rule).
std::optional<Emission> client_hit_confirmations(const Actor& pc, const HitConfirmations& hits, bool reliable,
                                                 const ExportAcks& acks, std::span<const ExportChain> extra_exports = {});

}}}  // namespace aoc::protocol::repl

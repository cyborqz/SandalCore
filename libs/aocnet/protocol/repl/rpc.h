// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/rpc.h
//
//  RPC calls by their RpcSchema (schema/generated), both directions.
//
//  An RPC is a field of its target object: the field header (SerializeInt(
//  field, NumFields + 1), SerializeIntPacked(payload bits): rep_block.h's
//  write_field) and the parameters.  It travels in a content block of its own,
//  bHasRepLayout = 0 (subobject.h), the server's to the actor or to a
//  component, the client's C>S the same with the two-bit header only.
//
//  Parameters are FRepLayout::SendPropertiesForRPC / ReceivePropertiesForRPC
//  (UE 5.6 RepLayout.cpp): for each parameter in declaration order
//    a bool                one bit, bare
//    anything else         a "send" bit -- 1 when the value is not the default,
//                          i.e. not identical to zeroed memory -- and, when 1,
//                          the value's commands in order, no handles
//                          (SerializeProperties_r): scalars as in a RepLayout
//                          block, a dynamic array as uint16 count and each
//                          element's commands in turn (fast_array.h write_element)
//  A struct parameter is flattened into one command per member (RpcParam::cmd_begin
//  .. cmd_end); a NetSerialize struct is one.  UE compares a struct with nullptr,
//  which is never identical, so a struct parameter is always sent; an array is
//  default when empty; numbers, strings, names and objects when zero, empty,
//  None or null.  The values are RpcParams, keyed by the RpcSchema command handle
//  (1-based over all parameters).  A parameter that is absent is left at its
//  default: a 0 send bit, or a false bool.
//
//  Proven bit for bit against the capture (test_repl_rpc, test_repl_actor_open):
//    S>C on the PlayerController: ClientSetHUD (ch 3 pkt#22, 173 bits), ClientRestart
//    (pkt#97), and pkt#23's six: ClientVoiceHandshakeComplete, ClientEnableNetworkVoice
//    (a bare bool), ClientFlushLevelStreaming, ClientCapBandwidth, ClientSetTime (two
//    doubles) and ClientUpdateMultipleLevelsStreamingStatusCompressed (a 429-byte
//    array, a 3449-bit parameter), batched as the two bunches the server sent them in.
//    C>S read to the last bit and written back to the client's bits: the possession
//    bunch's 72 RPCs (ServerAcknowledgePossession among them), ServerClientIsReadyToPhaseIn,
//    ServerRequestStorageData, ServerSetActiveTarget and, on the AbilityComponent,
//    ServerNotifyHeldAbilityCancelled.
//  Also run once as a scratch sweep (not kept) over every RPC block of the capture's
//  player channels, the server's (replay_data.bin: 852 blocks of ch 3, 4,206 of ch 19)
//  and the client's (the e2e fixture's capture, counted twice, and two live runs of
//  2026-09-29: 4,461 blocks): the 1,280 calls whose structs have codecs read to their
//  last bit and write back the same bits, of the 9,519 calls.  The other 8,239 are
//  calls of 13 RPCs: twelve name a struct without a codec (ServerMovePacked and the two
//  ClientMoveResponsePacked: FCharacterServerMovePackedBits / FCharacterMoveResponsePackedBits;
//  ServerUpdateLevelVisibility: FUpdateLevelVisibilityLevelInfo; FItemInstance:
//  ServerTriggerAbility, ClientReceiveStoragePayload / ChatMessages; FItemStorageEvent,
//  FEssentialCitizenshipWindowData, FNodeTaxDetails, FClientBuyOrderData, FRewardInstance)
//  and RequestHitVerification_Server names a component by path (ObjectId 1).  They
//  are listed by read_rpc_fields with `decoded` false.  (Plan A7 gave FItemInstance its one
//  known form, the empty item: ServerTriggerAbility decodes since, protocol/repl/ability_rpc.h.  Plan B2
//  gave it all its fields and FItemStorageEvent a codec, item_codec.h: the storage RPCs decode since, and
//  storage_rpc.h gives them a typed form.)
// ============================================================================
#pragma once
// ~ C-O-A - C.Y.B ~

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/subobject.h"
#include "protocol/repl/value.h"
#include "protocol/schema/generated/schema_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// The parameter values of one call, by the RpcSchema's command handle.
using RpcParams = ElementState;

/// The command of `rpc` named `path` ("NewPawn", "ItemData.Slot"), or nullptr.
const schema::RepCommand* rpc_command(const schema::RpcSchema& rpc, std::string_view path);

/// Sets the parameter (a struct's member) named by `path`; false, leaving `params` unchanged, when the
/// RPC has no such command or the value does not fit it.
bool set_rpc_param(const schema::RpcSchema& rpc, RpcParams& params, std::string_view path, Value v);

/// True when every value of `params` names a command of `rpc` and fits it, and each parameter has all of
/// its commands or none: what write_rpc_params accepts.
bool rpc_params_match(const schema::RpcSchema& rpc, const RpcParams& params);

/// Appends the parameters (the field's payload) and returns the bits written.  0 with *failed true (`out`
/// as it was) when `params` do not match; also 0, without a failure, for an RPC without parameters.
size_t write_rpc_params(const schema::RpcSchema& rpc, const RpcParams& params, emit::BunchWriter& out,
                        const CodecOptions& opts = {}, bool* failed = nullptr);

/// Reads the parameters from bit `pos` of `data` to bit `end`; they must fill it exactly.  Advances `pos`
/// to `end`.  nullopt (`pos` unchanged) for a read past the end, a struct without a codec, an object named
/// by ObjectId 1 or bits left over.
std::optional<RpcParams> read_rpc_params(const schema::RpcSchema& rpc, const uint8_t* data, size_t end, size_t& pos,
                                         const CodecOptions& opts = {});

/// The call as a field of `cls` (the target's class schema): header and parameters.  Returns the bits
/// written; 0 with *failed true (`out` as it was) for an RPC that is not one of the class's or bad params.
size_t write_rpc_field(const schema::ClassSchema& cls, const schema::RpcSchema& rpc, const RpcParams& params,
                       emit::BunchWriter& out, const CodecOptions& opts = {}, bool* failed = nullptr);

/// One RPC read from a field loop.
struct RpcCall {
    const schema::RpcSchema* rpc = nullptr;
    RpcParams                params;          // empty unless `decoded`
    bool                     decoded = false; // the parameters read to their last bit
    size_t                   param_pos = 0;   // where the field's payload starts in the buffer
    size_t                   param_bits = 0;
};

/// The RPC fields of a client bunch that read_rpc_fields skipped: how many, and the first few field indices (what the caller says).
struct SkippedFields {
    size_t                count = 0;
    std::vector<uint32_t> first;   // at most kMaxSkippedKept
};
constexpr size_t kMaxSkippedKept = 8;

/// Reads the field loop from `pos` to `end`, every field an RPC of `cls`.  A field whose parameters do not
/// decode (a struct without a codec ...) is still listed, `decoded` false.  Advances `pos` to `end`.
/// nullopt (`pos` unchanged) for a header that does not read, a field that is not an RPC or more than
/// kMaxClientRpcsPerBunch fields.
/// With `skipped` (the client's bunches): a field whose header reads but is no RPC of `cls` (an index the class
/// does not have, a property) is skipped by its payload bits, as UE skips an RPC it cannot find, and counted in
/// *skipped; it counts against the kMaxClientRpcsPerBunch bound all the same.  Only a header that does not read
/// (its extent unknown) still fails the loop.
std::optional<std::vector<RpcCall>> read_rpc_fields(const schema::ClassSchema& cls, const uint8_t* data, size_t end,
                                                    size_t& pos, const CodecOptions& opts = {}, SkippedFields* skipped = nullptr);

// ---- the client's RPCs ---------------------------------------------------------

struct ClientRpc {
    bool                       on_actor = true;   // the channel's actor; else `object`
    emit::FIntrepidNetworkGUID object;
    const schema::ClassSchema* cls = nullptr;     // the target's class
    RpcCall                    call;
};

/// A block of a subobject the client named in place (subobject.h's InPlaceName): the name and the block's payload bits.
struct InPlaceBlock {
    InPlaceName name;
    size_t      payload_bits = 0;
};

constexpr size_t kMaxInPlaceKept = 8;

struct ClientBunch {
    std::vector<ClientRpc> rpcs;
    /// Blocks not read as RPCs: property blocks (bHasRepLayout, e.g. the PlayerController's client-side
    /// subobjects) and blocks of a subobject the resolver does not know (or the client named in place).
    size_t skipped_blocks = 0;
    /// The blocks of subobjects the client named in place: skipped by their payload bits, the rest of the bunch read.  The first
    /// kMaxInPlaceKept are kept (the caller says what they were); `in_place_blocks` counts them all (a client can send a dozen in one bunch).
    std::vector<InPlaceBlock> in_place;
    size_t                    in_place_blocks = 0;
    /// RPC fields skipped because their header names no RPC of the target's class (read_rpc_fields' `skipped`).
    SkippedFields skipped_fields;
};

/// The most RPC calls one client bunch may carry: a bunch holds thousands of one-field RPC blocks at a few bits each, and
/// every call is a parsed parameter set; a real client sends a handful per bunch (the capture's possession bunch: 72).
constexpr size_t kMaxClientRpcsPerBunch = 1024;

/// The class of a subobject the client names (its schema), or nullptr when unknown.
using ResolveSubobject = std::function<const schema::ClassSchema*(const emit::FIntrepidNetworkGUID&)>;

/// Reads the payload of a bunch from the client (`bits` bits from bit 0, after the bunch header): its
/// content blocks, each an RPC block of `actor_class` or of the class `resolve` gives the subobject.  A block
/// that is not the caller's to read -- a RepLayout block, a subobject nothing resolves, a subobject the client
/// names in place (ClientBunch::in_place), an RPC field the class does not have -- is skipped by its payload bits
/// and the rest of the bunch is read: UE writes each one with its bit count, and one call the server cannot read
/// must not take the others of the bunch with it (Gathering stage 3: a tree's start in the bunch of an unnamed
/// 550-bit call never arrived).
/// nullopt for a block or field header that does not read, or more than kMaxClientRpcsPerBunch RPC calls.
std::optional<ClientBunch> read_client_bunch(const schema::ClassSchema& actor_class, const ResolveSubobject& resolve,
                                             const uint8_t* data, size_t bits, const CodecOptions& opts = {});

}}}  // namespace aoc::protocol::repl

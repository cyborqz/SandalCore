// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/possession.h
//
//  Possession handshake RPCs on the PlayerController channel (ch=3).
//
//    S>C  APlayerController::ClientRestart(APawn*)                 field 45
//    S>C  APlayerController::ClientRetryClientRestart(APawn*)      field 46
//    S>C  APlayerController::ClientSetHUD(TSubclassOf<AHUD>)      field 53
//    C>S  APlayerController::ServerAcknowledgePossession(APawn*)   field 73
//    C>S  APlayerController::ServerCheckClientPossession()         field 77
//    C>S  APlayerController::ServerCheckClientPossessionReliable() field 78
//
//  Field indices: the client exe's UECodeGen FFunctionParams records for
//  APlayerController (tools/ghidra/ue_net_functions.py ClientRestart --base 19)
//  list 79 replicated functions sorted by name from 19; the same list puts
//  ClientAckUpdateLevelVisibility at 21, ServerSetSpectatorLocation at 85,
//  ServerShortTimeout at 87 and ServerUpdate(Multiple)LevelVisibility at
//  92/93, all matching live traffic.
//
//  Handshake (stock UE, APlayerController): the client asks with
//  ServerCheckClientPossession while it has no acknowledged pawn; the server
//  answers ClientRetryClientRestart(Pawn); ClientRestart_Implementation then
//  possesses the pawn and replies ServerAcknowledgePossession(Pawn).
//
//  APawn* parameter (RepLayout SendPropertiesForRPC + PackageMap):
//    send(1)=1, then UPackageMapClient::SerializeObject = the 128-bit
//    FIntrepidNetworkGUID (ObjectId lo/hi, ServerId, Randomizer).  Export
//    flags follow only for the default GUID (ObjectId 1), never for a pawn.
//    send(1)=0 means a null pawn.
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/wire/level_visibility.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstddef>
#include <cstdint>

namespace aoc { namespace protocol { namespace wire {

constexpr uint32_t kPcFieldIdx_ClientRestart                       = 45;
constexpr uint32_t kPcFieldIdx_ClientSetHUD                        = 53;
constexpr uint32_t kPcFieldIdx_ClientRetryClientRestart            = 46;
constexpr uint32_t kPcFieldIdx_ServerAcknowledgePossession         = 73;
constexpr uint32_t kPcFieldIdx_ServerCheckClientPossession         = 77;
constexpr uint32_t kPcFieldIdx_ServerCheckClientPossessionReliable = 78;

/// Writes a complete ch=3 actor content block calling PlayerController RPC
/// `field` with one object parameter: ClientRestart / ClientRetryClientRestart
/// (the pawn) or ClientSetHUD (the HUD class).  Returns bits written.
inline size_t write_pc_object_rpc(uint8_t* buf, size_t cap, size_t& off, uint32_t field,
                                  const emit::FIntrepidNetworkGUID& object) {
    constexpr size_t kParamBits = 1 + 128;
    const size_t field_bits = detail::serialize_int_bits(field, kPcMaxFieldIdx);
    const size_t cb_bits    = field_bits + detail::sip_bits(kParamBits) + kParamBits;
// ~ CoA & C.Y.B ~

    const size_t start = off;
    ::ue5::write_bits(buf, cap, off, 0, 1);  // bHasRepLayout
    ::ue5::write_bits(buf, cap, off, 1, 1);  // bIsActor (the PC itself)
    ::ue5::write_sip(buf, cap, off, cb_bits);
    ::ue5::write_serialize_int(buf, cap, off, field, kPcMaxFieldIdx);
    ::ue5::write_sip(buf, cap, off, kParamBits);
    ::ue5::write_bits(buf, cap, off, 1, 1);  // send bit: the object is set
    ::ue5::write_bits(buf, cap, off, static_cast<uint32_t>(object.ObjectId), 32);
    ::ue5::write_bits(buf, cap, off, static_cast<uint32_t>(object.ObjectId >> 32), 32);
    ::ue5::write_bits(buf, cap, off, object.ServerId, 32);
    ::ue5::write_bits(buf, cap, off, object.Randomizer, 32);
    return off - start;
}

/// Decodes a single APawn* parameter (ServerAcknowledgePossession,
/// ClientRestart).  `param_off` is FActorRpcBunch::param_bit_offset (the
/// params-size SIP); `end_bit` is the end of the bunch payload.  A cleared
/// send bit yields a null (all-zero) GUID.
inline bool parse_pawn_param(const uint8_t* data, size_t data_len, size_t param_off,
                             size_t end_bit, emit::FIntrepidNetworkGUID& out) {
    size_t off = param_off;
    const size_t param_bits = static_cast<size_t>(::ue5::read_sip(data, data_len, off));
    if (param_bits == 0 || off + param_bits > end_bit) return false;
    out = emit::FIntrepidNetworkGUID{};
    if (::ue5::read_bits(data, data_len, off, 1) == 0) return true;
    if (param_bits < 1 + 128) return false;
    out = emit::read_intrepid_guid(data, data_len, off);
    return true;
}

}}} // namespace aoc::protocol::wire

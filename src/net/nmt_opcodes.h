// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/nmt_opcodes.h
//
//  Canonical UE5 control-channel message code → DispatchOp mapping.
//
//  Sourced directly from UE5 source:
//     Engine/Source/Runtime/Engine/Public/Net/DataChannel.h (lines 173-194).
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Hello, 0, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Welcome, 1, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Upgrade, 2, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Challenge, 3, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Netspeed, 4, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Login, 5, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Failure, 6, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Join, 9, ...)              <- NOT 7
//     DEFINE_CONTROL_CHANNEL_MESSAGE(JoinSplit, 10, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Skip, 12, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(Abort, 13, ...)            <- NOT 10
//     DEFINE_CONTROL_CHANNEL_MESSAGE(PCSwap, 15, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(ActorChannelFailure, 16, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(DebugText, 17, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(NetGUIDAssign, 18, ...)    <- NOT 14
//     DEFINE_CONTROL_CHANNEL_MESSAGE(SecurityViolation, 19, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(GameSpecific, 20, ...)     <- UE5 says 20
//     DEFINE_CONTROL_CHANNEL_MESSAGE(EncryptionAck, 21)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(DestructionInfo, 22)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(CloseReason, 23, ...)
//     DEFINE_CONTROL_CHANNEL_MESSAGE(NetPing, 24, ...)
//
//  AoC special case (verified from captures + the existing
//  game_server.h comment "NMT_GameSpecific(18)"): the AoC client uses
//  opcode 18 as its "world is ready / game-specific signal" AS WELL AS
//  the stock NMT_GameSpecific (20) from UE5.  We treat BOTH as the
//  DispatchOp::NMT_GAMESPECIFIC entry so our state machine handles either.
//
//  LAYER:   Net
//  SESSION: H.1
// ============================================================================
#pragma once

#include "net/opcode_dispatcher.h"
#include <cstdint>

namespace aoc { namespace net {

// UE5 canonical wire codes (from DataChannel.h).  Use these constants
// everywhere instead of bare integer literals so a grep for `NMT_Hello`
// finds the definition instead of raw `0`.
namespace nmt {
    inline constexpr uint8_t Hello                = 0;
    inline constexpr uint8_t Welcome              = 1;
    inline constexpr uint8_t Upgrade              = 2;
    inline constexpr uint8_t Challenge            = 3;
    inline constexpr uint8_t Netspeed             = 4;
    inline constexpr uint8_t Login                = 5;
    inline constexpr uint8_t Failure              = 6;
    inline constexpr uint8_t Join                 = 9;   // not 7!
    inline constexpr uint8_t JoinSplit            = 10;
    inline constexpr uint8_t Skip                 = 12;
    inline constexpr uint8_t Abort                = 13;  // not 10!
    inline constexpr uint8_t PCSwap               = 15;
    inline constexpr uint8_t ActorChannelFailure  = 16;
    inline constexpr uint8_t DebugText            = 17;
    inline constexpr uint8_t NetGUIDAssign        = 18;  // not 14!
    inline constexpr uint8_t SecurityViolation    = 19;
    inline constexpr uint8_t GameSpecific         = 20;  // stock UE5 GameSpecific
    inline constexpr uint8_t EncryptionAck        = 21;
    inline constexpr uint8_t DestructionInfo      = 22;
    inline constexpr uint8_t CloseReason          = 23;
    inline constexpr uint8_t NetPing              = 24;

    // AoC-specific repurposing: 18 is documented as UE5 NetGUIDAssign, but
    // the AoC client sends it as its post-load "ready to spawn" signal.
    // The captured replay's handle_game_data in game_server.h already
    // treats code 18 as the world-ready trigger.  We keep both meanings
    // addressable and let the dispatcher disambiguate by phase.
    inline constexpr uint8_t AoC_GameReady_18     = 18;
}
// -- CoA . C_Y_B --

/// Map a UE5 wire-format NMT code to our internal DispatchOp.
/// Returns DispatchOp::UNKNOWN for anything outside the catalog.
inline DispatchOp dispatch_op_for_nmt(uint8_t nmt_code) {
    switch (nmt_code) {
        case nmt::Hello:               return DispatchOp::NMT_HELLO;
        case nmt::Welcome:             return DispatchOp::NMT_WELCOME;
        case nmt::Upgrade:             return DispatchOp::NMT_UPGRADE;
        case nmt::Challenge:           return DispatchOp::NMT_CHALLENGE;
        case nmt::Netspeed:            return DispatchOp::NMT_NETSPEED;
        case nmt::Login:               return DispatchOp::NMT_LOGIN;
        case nmt::Failure:             return DispatchOp::NMT_FAILURE;
        case nmt::Join:                return DispatchOp::NMT_JOIN;
        case nmt::JoinSplit:           return DispatchOp::NMT_JOINSPLIT;
        case nmt::Skip:                return DispatchOp::NMT_SKIP;
        case nmt::Abort:               return DispatchOp::NMT_ABORT;
        case nmt::PCSwap:              return DispatchOp::NMT_PCSWAP;
        case nmt::ActorChannelFailure: return DispatchOp::NMT_ACTORCHANNELFAIL;
        case nmt::DebugText:           return DispatchOp::NMT_DEBUGTEXT;
        // Code 18 has DUAL meaning: stock UE5 NetGUIDAssign + AoC
        // "world ready" signal.  Both should advance SPAWNING → IN_WORLD.
        // The dispatcher's handle_nmt_game_specific treats this correctly.
        case nmt::NetGUIDAssign:       return DispatchOp::NMT_GAMESPECIFIC;
        case nmt::SecurityViolation:   return DispatchOp::NMT_SECURITYVIOLATION;
        case nmt::GameSpecific:        return DispatchOp::NMT_GAMESPECIFIC;
        case nmt::EncryptionAck:       return DispatchOp::NMT_ENCRYPTIONACK;
        default:                        return DispatchOp::UNKNOWN;
    }
}

/// Human-readable name for a NMT code (for logs).
inline const char* nmt_name(uint8_t code) {
    switch (code) {
        case nmt::Hello:               return "Hello";
        case nmt::Welcome:             return "Welcome";
        case nmt::Upgrade:             return "Upgrade";
        case nmt::Challenge:           return "Challenge";
        case nmt::Netspeed:            return "Netspeed";
        case nmt::Login:               return "Login";
        case nmt::Failure:             return "Failure";
        case nmt::Join:                return "Join";
        case nmt::JoinSplit:           return "JoinSplit";
        case nmt::Skip:                return "Skip";
        case nmt::Abort:               return "Abort";
        case nmt::PCSwap:              return "PCSwap";
        case nmt::ActorChannelFailure: return "ActorChannelFailure";
        case nmt::DebugText:           return "DebugText";
        case nmt::NetGUIDAssign:       return "NetGUIDAssign/AoCGameReady";
        case nmt::SecurityViolation:   return "SecurityViolation";
        case nmt::GameSpecific:        return "GameSpecific";
        case nmt::EncryptionAck:       return "EncryptionAck";
        case nmt::DestructionInfo:     return "DestructionInfo";
        case nmt::CloseReason:         return "CloseReason";
        case nmt::NetPing:             return "NetPing";
        default:                        return "unknown";
    }
}

}} // namespace aoc::net

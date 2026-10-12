// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/move_packed.h
//
//  The parameter of ServerMovePacked (ACharacter field 44): one
//  FCharacterServerMovePackedBits, which carries the client's last movement
//  frames as UE 5.6's FCharacterNetworkMoveDataContainer.  Plan A6 reads it to
//  check each move (the spec's section 10); the session answers with
//  ClientMoveResponsePacked or ClientAdjustPosition.
//
//  Layout, from the client exe (AOCClient-Win64-Shipping.exe, UE 5.6.0) and
//  checked against every captured move (the e2e capture's 640 and the
//  2026-09-29 run's 1,843: tests/capture_moves.h, test_repl_move_packed).
//
//    The RPC parameter (FRepLayout::SendPropertiesForRPC for a struct, rpc.h):
//        1 bit   "send" (1: the struct is not zeroed; a client that sends a move sends 1)
//        then FCharacterServerMovePackedBits's NetSerialize, slot 10 of its TCppStructOps vtable
//        (0x143acf0b0, which jumps to 0x143de86d0):
//        SerializeIntPacked(NumBits)         refused above 4096 (DAT_14d26ee6c, the exe's limit)
//        NumBits bits                        the container, below
//        and nothing else: the object references of the moves go to the package map, not into
//        this parameter.  The parameter must end exactly where its field says.
//
//    The container (FCharacterNetworkMoveDataContainer::Serialize, 0x143dfc5b0; the slots
//    of its vtable are ClientFillNetworkMoveData, Serialize, GetNewMoveData, GetPendingMoveData,
//    GetOldMoveData):
//        move(NewMove)                       always, FIRST
//        1 bit   bHasPendingMove
//          if set: 1 bit bIsDualHybridRootMotionMove, move(PendingMove)
//        1 bit   bHasOldMove
//          if set: move(OldMove)
//        1 bit   bDisableCombinedScopedMove
//    (the captured moves: 12% carry a pending move, none an old one; UE sends an old move only
//    while a second move is waiting, which a client at 60 frames per second does not need.)
//
//    One move (AoC's own FCharacterNetworkMoveData::Serialize, 0x145ec10b0; the engine's, at
//    0x143dfc1c0, is the same without the last item; neither looks at the move type):
//        float32 TimeStamp                   the client's movement clock, seconds
//        Acceleration                        FVector_NetQuantize10 (UE::Net::WriteQuantizedVector)
//        Location                            FVector_NetQuantize100
//        ControlRotation                     FRotator: per axis a non-zero bit and 16 bits
//        CompressedMoveFlags                 optional uint8, default 0: a bit, and 8 bits when it is 1
//        MovementBase                        a bit and, when 1, an object reference
//        MovementBaseBoneName                a bit and, when 1, an FName
//        MovementMode                        optional uint8, default 1 (MOVE_Walking)
//        input direction                     AoC's own struct at +0x70 (0x1463e7a80): four bits,
//                                            x > 1e-4, x < -1e-4, y > 1e-4, y < -1e-4
//    Every move carries a location (the engine source says only the new move does; the exe and the
//    capture say all three).  Moves arrive newest first: the new move, then the pending one (the
//    previous frame, not yet acknowledged), then the old one.
//
//  What is not proven by a capture: a movement base (no captured move stands on one: its object
//  reference is read as any property's object, 128 bits) and an old move (the exe's code reads it
//  like the pending one).  A move that names a base the reader cannot read is refused.
//
//  Quantized values are kept as the integers on the wire, so write_move_packed puts back the same
//  bits (tests: every captured move round trips).  Accessors give the cm / degrees.
// ============================================================================
#pragma once
// ~ C O A - Cyb ~

#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/value.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace aoc { namespace protocol { namespace repl {

/// The exe's limit on NumBits (FCharacterNetworkSerializationPackedBits::NetSerialize).
constexpr uint32_t kMaxPackedMoveBits = 4096;

/// UE's EMovementMode values the checker cares about.
constexpr uint8_t kMoveWalking = 1;
constexpr uint8_t kMoveFalling = 3;
constexpr uint8_t kMoveSwimming = 4;   // (the mounts milestone, stage 4: an aquatic mount in water; a flyer's flight is 5)
constexpr uint8_t kMoveFlying = 5;

/// One FCharacterNetworkMoveData.
struct NetworkMove {
    float    time_stamp = 0;                 // the client's clock, seconds
    QuantVec acceleration;                   // value * 10 (FVector_NetQuantize10)
    QuantVec location;                       // value * 100 (FVector_NetQuantize100), cm
    Rot      control_rotation;               // 16-bit compressed pitch, yaw, roll
    uint8_t  compressed_flags = 0;           // FSavedMove_Character::CompressedFlags: 1 jump, 2 crouch, 0x10.. custom
    std::optional<emit::FIntrepidNetworkGUID> base;   // MovementBase
    std::optional<NameValue>                  base_bone;
    uint8_t  movement_mode = kMoveWalking;   // the client's mode at the end of the move
    uint8_t  input_bits = 0;                 // bit 0 x > 0, 1 x < 0, 2 y > 0, 3 y < 0

    bool operator==(const NetworkMove&) const = default;

    Vec3d  acceleration_cm() const { return dequantize_vector(acceleration, 10); }
    Vec3d  location_cm() const { return dequantize_vector(location, 100); }
    double yaw_degrees() const { return control_rotation.yaw * 360.0 / 65536.0; }
    /// The input direction as -1, 0 or 1 per axis (a set "positive" bit wins, as the exe's reader).
    int input_x() const { return (input_bits & 1) ? 1 : (input_bits & 2) ? -1 : 0; }
    int input_y() const { return (input_bits & 4) ? 1 : (input_bits & 8) ? -1 : 0; }
};

/// The container: the new move, optionally the pending move and the old move.
struct MovePacked {
    NetworkMove                new_move;
    std::optional<NetworkMove> pending;
    bool                       dual_hybrid_root_motion = false;   // only on the wire with a pending move
    std::optional<NetworkMove> old;
    bool                       disable_combined_scoped_move = false;

    bool operator==(const MovePacked&) const = default;
};

/// Reads the parameter that occupies bits [pos, end) of `data`, a buffer of `data_bytes` bytes: the "send" bit, NumBits
/// and the container, which must fill it exactly. nullopt (and `*why`, when given, saying where) for a zeroed struct,
/// NumBits above the limit, an `end` beyond the buffer (nothing past the buffer is ever read), a read past the end, a
/// base the reader cannot read or bits left over.
std::optional<MovePacked> read_move_packed(const uint8_t* data, size_t data_bytes, size_t pos, size_t end, std::string* why = nullptr);

/// Appends the parameter (the send bit, NumBits and the container). false, writing nothing, for a move a client could
/// not have sent (a base without a readable form, more than kMaxPackedMoveBits).
bool write_move_packed(emit::BunchWriter& out, const MovePacked& moves);

}}}  // namespace aoc::protocol::repl

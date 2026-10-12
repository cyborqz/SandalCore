// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/hit_codec.h
//
//  The NetSerialize codec of FSerializedClientHitConfirmations (plan D2): the parameter of the server's
//  AAoCPlayerController::ClientHitConfirmationsReliable (PC field 171) and ClientHitConfirmations (170), the answer to the
//  client's RequestHitVerification_Server (hit_rpc.h).  codecs.h's read_struct / write_struct reach it by the struct name
//  ("FSerializedClientHitConfirmations"), which is how an RPC parameter uses it; the functions here are the same codec for a
//  caller that holds a reader, plus the per-hit blob on its own.
//
//  Every layout is the client exe's own (AOCClient-Win64-Shipping.exe, read-only), in the order the code runs.  Bit stream
//  LSB first; SerializeBits(v, n) is n raw bits, Serialize(p, n) is n * 8 raw bits, a UBOOL a bool as 32 bits (a value above
//  1 is an archive error).
//
//  FSerializedClientHitConfirmations (the struct of one TArray<FSerializedClientHitConfirmation>), NetSerialize slot 10 of
//  its TCppStructOps vtable 0x14b2764b0: thunk 0x1459ac190 -> 0x1459af5c0
//
//    11       Hits.Num as SerializeBits (CeilLogTwo(1999 + 1) bits); saving clamps it to 1000 and reports failure
//    per hit  (FSerializedClientHitConfirmation, NetSerialize 0x1459abf90 -> 0x1459af460, which serializes its one
//             member, SerializedData, a TArray<uint8>)
//      15       SerializedData.Num as SerializeBits (CeilLogTwo(19999 + 1) bits); clamped to 10000
//      Num * 8  the bytes, one Serialize(p, 1) each
//
//  A parameter of this struct type goes out as a send bit (always 1: a struct is never "the default", rpc.h) and these bits.
//  The older protocol/wire/hit_confirmation.h wrote "uint16 array count per TArray" after the send bit and always an empty
//  Targets list; that framing is the first-guess form and does not match the retail bits (the count is 11 bits and each
//  blob's length 15).  Nothing in worldd uses it; use this codec.
//
//  SerializedData is a bit stream of its own, read by the client with a plain FBitReader over Num * 8 bits:
//  0x1463e2f90 constructs the reader (0x1414eb150), calls 0x1463e1750 and then resolves the guids to objects.
//
//  0x1463e1750   FHitInst (0x1463e1870), then two UBOOLs, bPredictedOnAutonomous and bPredictedOnSimulated
//  0x1463e1870   FHitInst:
//    64 x 4     InstGuid, RecordGuid, SourceRecordType, SourceRecordGuid
//    32         bPvPForceFlag as a UBOOL
//    128        Instigator, an FIntrepidNetGUID: four uint32 (0x14141e960), a weak object the client resolves
//    128        Source, the same
//    7          Targets.Num as SerializeBits (CeilLogTwo(79 + 1) bits; 0x1463d6180), clamped to 40
//    Num x      FHitTargetInst (0x1463e1da0)
//  FHitInst's DelayTime and SynchronizedTime are not serialized.  FHitTargetInst, 192 bytes in memory, of which the
//  exe serializes (in this order; the offsets are those of the reflection record's member order):
//    128        HitComp (+0x00), an FIntrepidNetGUID
//    192        ImpactLocation (+0x10), an FVector: three doubles (0x14141ed80; AoC's net version is past
//               SerializeDoubleVectorsAsDoubles)
//    192        ImpactNormal (+0x28), an FVector
//    5          StatMods.Num (+0x48) as SerializeBits (CeilLogTwo(19 + 1) bits; 0x1463d6070), clamped to 10; each
//               StatModInst: Guid 64, Value float 32, EffectiveValue float 32 (the other two members, ArrayStatGuid and
//               ShieldAbsorbValue, are not sent)
//    32 + 64 x  Interrupts (+0x68), a TArray<InterruptInst> through the archive's array operator (0x14171fc20): an int32
//               count and each InterruptInst's AbilityGuid, 64 bits
//    32         bAttemptedInterrupt (+0x78) as a UBOOL
//    8          Flags (+0xa0)
//    5          HitModInsts.Num (+0xa8) as SerializeBits, clamped to 10; each HitModInst (0x1463e1c40): HitModGuid 64 and
//               Owner, an FIntrepidNetGUID (128)
//  The members the exe leaves out of the wire are Dispels, LingeringZoneInsts, AppliedEffects, BoneName, PhysMaterial and
//  RedirectOwner.  The blob ends at the last of these bits; the bits up to the next byte are zero.
//
//  Evidence: every ClientHitConfirmationsReliable (6,845) and ClientHitConfirmations (270) of the 419 retail captures that
//  carry them reads to its last bit in this form (11,025 hits, 11,362 targets; at most 20 hits a call, 9 targets a hit, 3
//  stat mods, 2 interrupts and 9 hit mods a target, 1,322 bytes a blob), with every blob's padding bits zero;
//  tests/capture_hits.h has a sample of them with the text an independent Python decoder (tools/capture/hit_wire.py, written
//  from the exe's code) read from each, and test_repl_hits reads them with this codec, writes them back to the same bits and
//  compares the text.
//
//  Readers return nullopt for a read past the end, a UBOOL above 1, a count above the exe's own clamps (what its writer
//  never produces), a blob that does not end within its last byte or whose padding bits are not zero, and more Interrupts
//  than kMaxHitInterrupts (the exe has no bound of its own below 0x200000: a hostile count must not buy memory).  A count is
//  also checked against the bits left, so nothing is reserved for elements the input cannot hold.  A reader never reads
//  past `effective_bits`.
// ============================================================================
#pragma once
// (CoA) (C_Y_B)

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

/// The exe's clamps (the saving side reports failure past them).
constexpr size_t kMaxHitConfirmations = 1000;     // Hits.Num, 0x3e8
constexpr size_t kMaxHitBlobBytes = 10000;        // SerializedData.Num, 0x2710
constexpr size_t kMaxHitTargets = 40;             // Targets.Num, 0x28
constexpr size_t kMaxHitStatMods = 10;            // 0xa
constexpr size_t kMaxHitHitMods = 10;             // 0xa
/// Ours: the exe's array operator bounds an Interrupts count at 0x200000 only.  The retail maximum is 2.
constexpr size_t kMaxHitInterrupts = 64;

/// The bits SerializedData holds for `hit`, before the padding to a byte.
size_t hit_instance_bits(const HitInstance& hit);

/// True when serialize_hit_instance writes `hit` and deserialize_hit_instance reads it back equal: the counts within
/// the bounds above and the blob within kMaxHitBlobBytes.
bool hit_instance_fits(const HitInstance& hit);

/// One SerializedData blob, padded with zero bits to a whole byte.  nullopt when `hit` does not fit.
std::optional<std::vector<uint8_t>> serialize_hit_instance(const HitInstance& hit);

/// One SerializedData blob; nullopt for what the notes above refuse.
std::optional<HitInstance> deserialize_hit_instance(const uint8_t* data, size_t bytes);

/// True when write_hit_confirmations writes `c` and read_hit_confirmations reads it back equal.
bool hit_confirmations_fit(const HitConfirmations& c);

/// The struct as NetSerialize reads it (no send bit: the RPC's); nullopt for what the notes above refuse.
std::optional<HitConfirmations> read_hit_confirmations(wire::PacketReader& r);

/// False (writing nothing) when `c` does not fit.
bool write_hit_confirmations(emit::BunchWriter& out, const HitConfirmations& c);

}}}  // namespace aoc::protocol::repl

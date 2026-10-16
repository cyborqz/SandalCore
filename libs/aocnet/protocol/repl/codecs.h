// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/codecs.h
//
//  Readers and writers for the values a RepLayout command carries beyond the
//  fixed-width scalars, and for every NetSerialize struct the captured
//  PlayerController, pawn, PlayerState, GameState and component blocks use.
//  Each is the C++ port of tools/ghidra/schema_check.py's decoder (which reads
//  4,656 captured blocks and field loops to their last bit) checked against
//  the UE 5.6 source:
//
//    FString       int32 length incl. NUL: positive = ANSI bytes, negative =
//                  UCS-2 code units, 0 = empty (a string is sent as ANSI
//                  exactly when it is pure ASCII, FString::Serialize)
//    FName         bit hardcoded: SerializeIntPacked(EName index); else
//                  FString + int32 number (UPackageMap::SerializeName)
//    FText         u32 flags, int8 history; None: 32-bit bool + FString;
//                  Base: namespace, key, source FStrings
//    object        the 128-bit FIntrepidNetworkGUID (a property never names
//                  ObjectId 1, whose export flags would follow)
//    soft object   bit: path FString, else an object
//
//    FVector, FVector3d          3 doubles (AoC's net version is past
//                                SerializeDoubleVectorsAsDoubles)
//    FVector2D, FVector2d        2 doubles
//    FDateTime, FTimespan        64-bit ticks
//    FRotator                    per axis a non-zero bit + 16 bits
//    FVector_NetQuantize,
//    _NetQuantize10, _100        UE::Net::WriteQuantizedVector: see QuantVec
//    FVector_NetQuantizeNormal   3 x 16 bits
//    FRepMovement                see RepMovement; the rotation is 8 bits per
//                                axis (ByteComponents) or 16 (ShortComponents)
//                                as the object's RotationQuantizationLevel
//                                says: not on the wire, so CodecOptions
//    FCharacterLoadTracker       emit/net_serializers.h
//    FUniqueNetIdRepl            see UniqueNetId
//    FGameplayTag                its FName (no replication-method bits at
//                                AoC's net version)
//    FGameplayTagContainer       empty bit, else a 6-bit count + the tags
//    FStatInt32InstRep           int64 Record, 32-bit Value, then -- for a
//                                Record the client's stat table marks -- 32-bit
//                                Base and 32-bit EquipmentValue.  The client
//                                exe's NetSerialize (0x146164bf0, reached
//                                through TCppStructOps vtable slot 10 of
//                                StatInt32InstRep) reads Record, Value, looks
//                                the Record up and, when the entry's flag at
//                                +0x247 is set, reads Base and EquipmentValue.
//                                The wire does not say which Records those
//                                are: CodecOptions::stat_int32_extended
//
//  AoC's own structs.  Each NetSerialize is slot 10 of the struct's
//  TCppStructOps vtable (found from the struct's reflection record, as
//  tools/ghidra/ue_replayout.py's struct_caps does); for most it is a thunk
//  (`mov rcx, [rsp+0x28]; jmp F`) into the function F that does the work.
//  tools/ghidra/schema_check.py reads every captured block and field-loop
//  payload that holds one of them to the last bit, and the C++ ports here are
//  proven on the same bits (tests/test_repl_aoc_codecs.cpp).
//
//  FQuat
//      Three doubles X, Y, Z.  Ops vtable 0x149e8eda0, slot 10 0x1416c7780 ->
//      0x141438440.  With Ar.EngineNetVer() >= 22 and != 26 (AoC's is 35) X, Y
//      and Z are 8 bytes each, else floats.  Saving first makes the quaternion
//      unit and W non-negative (quat_to_wire), loading derives W (quat_from_wire).
//      UE 5.6 TQuat<T>::NetSerialize (UnrealMath.cpp) is the same code.
//  FGuidToBoolWrapper
//      `Ar << GuidToBool`, a TMap<FGuid, bool>.  Ops vtable 0x14b397b20, slot 10
//      0x145a7c230 -> 0x145a81ac0 -> 0x145a7d730: int32 Num, then per pair the
//      FGuid (four int32 A B C D, 0x14141e960) and the bool as an int32 that
//      must be 0 or 1 (FArchive& operator<<(bool&) sets the archive error else).
//  FAlignmentContainer
//      `Ar << AlignmentInstanceMap; Ar << InstanceTimers`.  Ops vtable
//      0x14b0d1c00, slot 10 0x1458bb810 -> 0x145e77630.  The first is a
//      TMap<int64, FGuidsSet> (0x145e637b0 -> 0x145e59340): int32 Num, per pair
//      the 8-byte key and the FGuidsSet's TSet<FGuid> (0x145a2e580 -> 0x145a2d570):
//      int32 Num and per element the FGuid.  The second is a
//      TMap<uint32, FAlignmentInstanceTimer> (0x145e63520 -> 0x145e58f00): int32
//      Num, per pair the 4-byte key, SetTimeStamp (8 bytes) and AlignmentId
//      (FCharacterAlignmentId: two 8-byte words; 0x145e64eb0, 0x1413df110).
//  FAppearanceId
//      Ops vtable 0x14b18a468, slot 10 0x145919170: TypeId (8 bytes), RecordGuid
//      (8), EnchantmentLevel (1), ItemEquipSlot (1).
//  FCharacterCustomizationSaveData
//      Ops vtable 0x14b753ad0, slot 10 0x145d8d3a0 -> 0x1463e75f0 -> 0x1463d6290.
//      A delta against a default the wire does not carry (`bChanged = Saving &&
//      Value != Default; Ar.SerializeBits(&bChanged, 1); if (bChanged) Ar << Value`):
//        bit + int64 PresetGuid (sent when non-zero); delta int32 RandomSeed;
//        Race and Gender, a byte each, when the client has no preset for
//        PresetGuid (its lookup, 0x14580d810, is not on the wire:
//        CodecOptions::customization_preset_known);
//        30 scalar deltas (CustomizationField); DecalData (0x1463d7980) and
//        DecalBlendGroups (0x1463d7d00), each a bit, an int32 Num and per element
//        a bit + the element (0x1463da010, 0x1463d8290); bIsHelmetVisible and
//        bIsCapeVisible (0x1463d8410: bit + int32 0/1); SectionsValues
//        (0x1463d42d0: bit, int32 Num, per pair a byte and an FVector of three
//        doubles, 0x14141ed80); FaceMorphWeightMaps (0x1463d39d0: bit, int32 Num,
//        per pair an FName as UPackageMap::SerializeName and a float).
//      See CharacterCustomization (value.h) and read_customization (codecs.cpp).
//
//  FItemInstance, FItemStorageEvent
//      The item structs of plan B2 live in item_codec.h (layouts, exe addresses, bounds); read_struct and
//      write_struct reach them by name.
//
//  FPlayerMovementInput (plan E1)
//      ABaseCharacter's CurrentMovementInput: four bits, the character's move input direction as ServerMovePacked
//      carries it (x > 0, x < 0, y > 0, y < 0; move_packed.h).  The struct is AoC's own NetSerialize; its four bits end
//      every captured NPC open (323 of 323 opens of capture 957 read to their last bit with it) and, alone, are a
//      21-bit update.  See PlayerMovementInput (value.h).
//
//  FLocalizedMessage (mounts milestone, stage 1)
//      A PlayerController callout's LocalizedMessage: the string table and the key as FStrings and an empty argument list
//      (callout_codec.h).  See LocalizedMessage (value.h).
//
//  FChannelingData (Gathering stage 2, the fix after test 1)
//      The pawn's InteractInfo.ChannelingData: what the pawn is channeling (the node, when it started, how long, the tool).  Read from ten retail harvests
//      (channeling_codec.h has the layout and what is inferred).  See ChannelingData (value.h).
//
//  A struct without a codec here (FCharacterServerMovePackedBits ...) is refused by name:
//  no reader guesses.
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/value.h"
#include "protocol/wire/packet_reader.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace aoc { namespace protocol { namespace repl {

/// Per-object settings a codec needs that the wire does not announce.
struct CodecOptions {
    /// FRepMovement::RotationQuantizationLevel == ShortComponents (else the
    /// default ByteComponents).  The captured pawn (pkt#78) uses bytes.
    bool rep_movement_short_rotation = false;

    /// FStatInt32InstRep reads: true when the client's stat table marks
    /// `record` as one whose element carries Base and EquipmentValue (the
    /// captured pawn's four such stats have Records 0x5429e8c0xxxxxxxx and
    /// 0x5429e8c4xxxxxxxx; nothing on the wire tells the two forms apart).
    /// Unset: no Record does.  Writes need no table: StatInt32Inst::extended
    /// says what goes out, and it must agree with the client's table.
    std::function<bool(int64_t record)> stat_int32_extended;

    /// FCharacterCustomizationSaveData reads: true when the client's preset
    /// table has a preset for `preset_guid` (0 when the struct names none), in
    /// which case the wire has no Race and Gender bytes.  The two captured
    /// blocks (the pawn's, an NPC's) have them: no preset is known to the
    /// client.  Unset: none is.  Writes need no table:
    /// CharacterCustomization::race_gender says what goes out, and it must
    /// agree with the client's table.
    std::function<bool(uint64_t preset_guid)> customization_preset_known;

    /// FRepMovement::LocationQuantizationLevel and VelocityQuantizationLevel as the scale the vector goes out at:
    /// RoundWholeNumber 1, RoundOneDecimal 10, RoundTwoDecimals 100 (EVectorQuantization, ReplicatedState.h).  The
    /// quantized components on the wire are value * scale; the reader has to know the scale, and a wrong one is a silent
    /// 10x or 100x error (quantize_location / quantize_velocity and the dequantize_ ones below apply them).  The defaults
    /// are the captured pawn's: APawn's constructor raises the location level to two decimals (AActor's FRepMovement
    /// default is a whole number: an actor that is not a Pawn sets location_scale 1) and the velocity level stays at its
    /// default, a whole number.  The linear velocity, the angular velocity and the acceleration all use the velocity scale.
    int32_t rep_movement_location_scale = 100;
    int32_t rep_movement_velocity_scale = 1;
// C O A and C:Y:B

    /// An object parameter of a client RPC may be named in place (value.h InlineObject: ObjectId 1, export flags, outer,
    /// path, checksum).  A server's property never is, so the default refuses ObjectId 1 as read_object does; the
    /// session sets it to read the client's ServerTriggerAbility Targets (plan A7).
    bool inline_objects = false;
};

// ---- primitives -------------------------------------------------------------

/// True when the reader consumed more than it had (a read past the end).
inline bool overran(const wire::PacketReader& r) { return r.overflowed() || r.pos() > r.effective_bits(); }

/// True when `s` is well-formed UTF-8 (what write_fstring can carry, length and NUL aside).
bool                           valid_utf8(const std::string& s);

/// The longest FString this file reads or writes, in characters (UTF-16 code units; ASCII bytes): the wire length
/// counts the terminating NUL and read_fstring refuses more than 65,536.
constexpr size_t kMaxFStringChars = 65535;

/// True when write_fstring writes `s` and read_fstring reads it back: well-formed UTF-8, no NUL (the wire's
/// terminator: an embedded one does not read back) and at most kMaxFStringChars characters.
bool                           fstring_fits(const std::string& s);
/// The same for the name's / text's / soft object's strings, for what the writer writes: a hardcoded name has no text.
bool                           name_fits(const NameValue& name);
bool                           text_fits(const TextValue& text);

std::optional<std::string>     read_fstring(wire::PacketReader& r);
bool                           write_fstring(emit::BunchWriter& out, const std::string& utf8);

std::optional<NameValue>       read_name(wire::PacketReader& r);
bool                           write_name(emit::BunchWriter& out, const NameValue& name);

std::optional<TextValue>       read_text(wire::PacketReader& r);
bool                           write_text(emit::BunchWriter& out, const TextValue& text);

/// An object reference; refuses ObjectId 1 (the export-flags form).
std::optional<emit::FIntrepidNetworkGUID> read_object(wire::PacketReader& r);
bool                           write_object(emit::BunchWriter& out, const emit::FIntrepidNetworkGUID& object);

/// An object parameter: the plain reference (read_object), or with `allow_inline` a reference with ObjectId 1 followed by
/// its export (an InlineObject; nullopt for flags the form does not define, an outer that is itself inline, or a path
/// that is not a well-formed string).
std::optional<Value> read_object_value(wire::PacketReader& r, bool allow_inline);
bool                 write_inline_object(emit::BunchWriter& out, const InlineObject& o);
/// True when write_inline_object writes `o`: flags within 1 | 2 | 4, a path exactly when the path flag is set (never
/// empty), an outer that is not ObjectId 1, a checksum only with its flag.
bool                 inline_object_fits(const InlineObject& o);

std::optional<SoftObjectValue> read_soft_object(wire::PacketReader& r);
bool                           write_soft_object(emit::BunchWriter& out, const SoftObjectValue& soft);

// ---- quantized vectors ------------------------------------------------------

std::optional<QuantVec> read_quantized_vector(wire::PacketReader& r);
/// True when write_quantized_vector writes `v`: a Scaled / Unscaled component needs at most 63 bits
/// (UE::Net::WriteQuantizedVector's limit), a RawFloat component fits 32 bits and is not negative.
bool                    quant_vec_fits(const QuantVec& v);
/// False when `v` does not fit (quant_vec_fits), writing nothing.
bool                    write_quantized_vector(emit::BunchWriter& out, const QuantVec& v);

/// UE::Net::QuantizeVector + the write's integer conversion: the components a
/// float vector goes out as at `scale` (1, 10 or 100 for the NetQuantize
/// structs; FRepMovement location 100 in the captured pawn).
QuantVec quantize_vector(int32_t scale, double x, double y, double z);
/// The vector a QuantVec reads back as.
Vec3d    dequantize_vector(const QuantVec& v, int32_t scale);

/// FRepMovement's vectors at the object's quantization levels (CodecOptions::rep_movement_location_scale /
/// rep_movement_velocity_scale): the location, and the linear velocity, angular velocity and acceleration.
QuantVec quantize_location(const CodecOptions& o, double x, double y, double z);
QuantVec quantize_velocity(const CodecOptions& o, double x, double y, double z);
Vec3d    dequantize_location(const CodecOptions& o, const QuantVec& v);
Vec3d    dequantize_velocity(const CodecOptions& o, const QuantVec& v);

// ---- FQuat ------------------------------------------------------------------

/// The wire value of the quaternion (x, y, z, w) as FQuat::NetSerialize saves
/// it (0x141438440, above): identity when |q|^2 <= (double)1e-8f, scaled to
/// unit length when |1 - |q|^2| > (double)0.01f (1/sqrt, then a multiply:
/// sqrtpd / divpd, exact), all four components negated when W is negative; W
/// itself is then not sent.
Quat quat_to_wire(double x, double y, double z, double w);

/// The quaternion {x, y, z, w} a wire value reads back as: W = sqrt(1 -
/// x^2 - y^2 - z^2); when that is negative W = 0 and x, y, z are scaled by
/// 1/sqrt(x^2 + y^2 + z^2).
std::array<double, 4> quat_from_wire(const Quat& q);

// ---- NetSerialize structs ---------------------------------------------------

/// True when `struct_name` has a codec here.
bool is_known_struct(std::string_view struct_name);

/// True when `v` is a value write_struct writes for `struct_name`: the codec's value type, each number within what it
/// carries (a quantized component at most 63 bits, also inside an FRepMovement; an FUniqueNetIdRepl of a non-V2 type at
/// most 255 id bytes), each string fitting an FString (fstring_fits) and the structure one the wire can hold (a map
/// without a duplicate key, at most 63 tags ...).  One thing depends on the object, not on the value: an FRepMovement
/// rotation component beyond a byte in the default byte form (CodecOptions::rep_movement_short_rotation), which fails at
/// the write.
bool struct_accepts(std::string_view struct_name, const Value& v);

/// What a zeroed `struct_name` holds, for the structs whose zero value is certain (vectors, rotator, ticks, tags);
/// nullopt for the others (their zero form on the wire is not verified).  An array element's local that is removed is
/// re-sent as this value (rep_block.h).
std::optional<Value> zero_struct(std::string_view struct_name);

/// Reads one `struct_name`; nullopt for an unknown struct, a malformed value
/// or a read past the end.
std::optional<Value> read_struct(std::string_view struct_name, wire::PacketReader& r,
                                 const CodecOptions& opts = {});

/// Writes one `struct_name`; false (and possibly partial bits: the caller
/// truncates) for an unknown struct or a value the codec cannot carry.
bool write_struct(std::string_view struct_name, const Value& v, emit::BunchWriter& out,
                  const CodecOptions& opts = {});

}}}  // namespace aoc::protocol::repl

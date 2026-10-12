// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/property_update_bunch_builder.cpp
//
//  Implementation of PropertyUpdateBunchBuilder.  Bunch header layout
//  matches ActorBuilder::write_bunch_header minus the control/open/close
//  fields (which don't apply to data bunches).
//
//  Wire-format reference: docs/wire-format.md §7 (bunch header format).
// ============================================================================
#include "protocol/emit/property_update_bunch_builder.h"
#include "protocol/emit/name_update_bunch.h"
#include <cstring>   // memcpy for float→uint32 punning

namespace aoc { namespace protocol { namespace emit {

// ── Public accumulators ────────────────────────────────────────────────

void PropertyUpdateBunchBuilder::add_name_update(const std::string& name) {
    // V1 — mid-bunch region bytes (for bit-identity testing only).
    BunchWriter tmp(64);
    size_t bits = build_name_update_bunch_payload(name, tmp);

    Blob b;
    b.bit_count = bits;
    const size_t n_bytes = (bits + 7) / 8;
    b.bytes.assign(tmp.data(), tmp.data() + n_bytes);
    queued_payloads_.push_back(std::move(b));
    payload_bits_ += bits;
}

void PropertyUpdateBunchBuilder::add_name_update_v2(const std::string& name,
                                                     uint32_t cmd_index) {
    // V2 — proper property-delta payload for LIVE sends.
    BunchWriter tmp(64);
    size_t bits = build_name_update_bunch_payload_v2(name, cmd_index, tmp);

    Blob b;
    b.bit_count = bits;
    const size_t n_bytes = (bits + 7) / 8;
    b.bytes.assign(tmp.data(), tmp.data() + n_bytes);
    queued_payloads_.push_back(std::move(b));
    payload_bits_ += bits;
}

// ── Generic property updates (Path B — 2026-04-26) ────────────────────
//
// Each method writes the same wire-format envelope as add_name_update_v2:
//    [bHasRepLayoutExport=0] (1 bit)
//    [NumGUIDsInBunch=0]     (32 bits)
//    [cmd_index]             (32 bits)
//    [body]                  (type-specific bits)
//
// The body is just the raw value bits — int32 = 32, float = 32, bool = 1.
// No length prefix needed because the receiver knows the type from the
// cmd_index → property mapping in the class's RepLayout.

void PropertyUpdateBunchBuilder::add_int32_update_v2(int32_t value,
                                                     uint32_t cmd_index) {
    BunchWriter tmp(64);
    tmp.write_bit(0);              // bHasRepLayoutExport = 0
    tmp.write_uint32(0);           // NumGUIDsInBunch = 0
    tmp.write_uint32(cmd_index);   // property identifier
    tmp.write_int32(value);         // 32-bit value (FIntProperty)
// -- CoA . C | Y | B --

    Blob b;
    b.bit_count = tmp.bit_pos();
    const size_t n_bytes = (b.bit_count + 7) / 8;
    b.bytes.assign(tmp.data(), tmp.data() + n_bytes);
    queued_payloads_.push_back(std::move(b));
    payload_bits_ += b.bit_count;
}

void PropertyUpdateBunchBuilder::add_float_update_v2(float value,
                                                     uint32_t cmd_index) {
    BunchWriter tmp(64);
    tmp.write_bit(0);              // bHasRepLayoutExport = 0
    tmp.write_uint32(0);           // NumGUIDsInBunch = 0
    tmp.write_uint32(cmd_index);   // property identifier
    // float = 32 bits IEEE 754; reinterpret as uint32 for bit write
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    tmp.write_uint32(bits);

    Blob b;
    b.bit_count = tmp.bit_pos();
    const size_t n_bytes = (b.bit_count + 7) / 8;
    b.bytes.assign(tmp.data(), tmp.data() + n_bytes);
    queued_payloads_.push_back(std::move(b));
    payload_bits_ += b.bit_count;
}

void PropertyUpdateBunchBuilder::add_bool_update_v2(bool value,
                                                    uint32_t cmd_index) {
    BunchWriter tmp(64);
    tmp.write_bit(0);              // bHasRepLayoutExport = 0
    tmp.write_uint32(0);           // NumGUIDsInBunch = 0
    tmp.write_uint32(cmd_index);   // property identifier
    tmp.write_bit(value ? 1 : 0);  // 1 bit (FBoolProperty)

    Blob b;
    b.bit_count = tmp.bit_pos();
    const size_t n_bytes = (b.bit_count + 7) / 8;
    b.bytes.assign(tmp.data(), tmp.data() + n_bytes);
    queued_payloads_.push_back(std::move(b));
    payload_bits_ += b.bit_count;
}

void PropertyUpdateBunchBuilder::add_uint8_update_v2(uint8_t value,
                                                     uint32_t cmd_index) {
    BunchWriter tmp(64);
    tmp.write_bit(0);              // bHasRepLayoutExport = 0
    tmp.write_uint32(0);           // NumGUIDsInBunch = 0
    tmp.write_uint32(cmd_index);   // property identifier
    tmp.write_uint8(value);        // 8 bits (FByteProperty / enum byte)

    Blob b;
    b.bit_count = tmp.bit_pos();
    const size_t n_bytes = (b.bit_count + 7) / 8;
    b.bytes.assign(tmp.data(), tmp.data() + n_bytes);
    queued_payloads_.push_back(std::move(b));
    payload_bits_ += b.bit_count;
}

// ─── V3 — content-block-framed format (2026-04-26) ────────────────────
//
// Implementation per RE of:
//   sub_143F2C340 (ReadContentBlockHeader): 1 bit bOutermostEnd, 1 bit
//     bIsChannelActor, optional NetGUID
//   sub_143F2DA40 (ReadContentBlockPayload): SerializeIntPacked NumPayloadBits
//   sub_143F2DC60 (ReadPropertyChangeHeader): SerializeInt cmd_handle,
//     SerializeIntPacked NumBits, then NumBits bits of value

void PropertyUpdateBunchBuilder::v3_begin_content_block_channel_actor(
        uint32_t num_properties_in_class) {
    v3_block_open_ = true;
    v3_block_is_channel_actor_ = true;
    v3_subobject_netguid_ = 0;
    v3_num_properties_ = num_properties_in_class > 0 ? num_properties_in_class : 256;
    v3_inner_payload_ = BunchWriter(256);   // reset inner accumulator
}
// ~ C.Y.B & CoA ~

void PropertyUpdateBunchBuilder::v3_begin_content_block_subobject(
        uint32_t subobject_netguid, uint32_t num_properties_in_class) {
    v3_block_open_ = true;
    v3_block_is_channel_actor_ = false;
    v3_subobject_netguid_ = subobject_netguid;
    v3_num_properties_ = num_properties_in_class > 0 ? num_properties_in_class : 256;
    v3_inner_payload_ = BunchWriter(256);
}

// Wire format selector behavior (set via set_use_modern_inner_format):
//   MODERN (default, per RE of sub_143F2DC60 modern path L169-220):
//     [SerializeInt handle, MAX][value bits — type-known size, NO NumBits]
//   LEGACY (backwards-compat path L50-148):
//     [SerializeInt handle, MAX][SIP NumBits][NumBits bits value]
//
// Empirical: LEGACY format silently dropped on AOC client.  MODERN expected.

void PropertyUpdateBunchBuilder::v3_add_property_int32(uint32_t cmd_handle,
                                                       int32_t value) {
    if (!v3_block_open_) return;
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    if (!v3_use_modern_inner_format_) v3_inner_payload_.write_sip(32);
    v3_inner_payload_.write_int32(value);
}

void PropertyUpdateBunchBuilder::v3_add_property_float(uint32_t cmd_handle,
                                                       float value) {
    if (!v3_block_open_) return;
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    if (!v3_use_modern_inner_format_) v3_inner_payload_.write_sip(32);
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    v3_inner_payload_.write_uint32(bits);
}

void PropertyUpdateBunchBuilder::v3_add_property_bool(uint32_t cmd_handle,
                                                      bool value) {
    if (!v3_block_open_) return;
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    if (!v3_use_modern_inner_format_) v3_inner_payload_.write_sip(1);
    v3_inner_payload_.write_bit(value ? 1 : 0);
}

void PropertyUpdateBunchBuilder::v3_add_property_uint8(uint8_t cmd_handle,
                                                       uint8_t value) {
    if (!v3_block_open_) return;
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    if (!v3_use_modern_inner_format_) v3_inner_payload_.write_sip(8);
    v3_inner_payload_.write_uint8(value);
}

void PropertyUpdateBunchBuilder::v3_add_property_fstring(uint32_t cmd_handle,
                                                          const std::string& s) {
    if (!v3_block_open_) return;
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    if (!v3_use_modern_inner_format_) {
        // Legacy NumBits prefix = 32 (length) + (s.size() + 1) * 8 bytes
        const uint32_t num_bits = 32 + static_cast<uint32_t>((s.size() + 1) * 8);
        v3_inner_payload_.write_sip(num_bits);
    }
    v3_inner_payload_.write_fstring_ansi(s);
}

void PropertyUpdateBunchBuilder::v3_add_property_netguid(uint32_t cmd_handle,
                                                          uint64_t object_id,
                                                          uint32_t server_id,
                                                          uint32_t randomizer) {
    if (!v3_block_open_) return;
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    if (!v3_use_modern_inner_format_) v3_inner_payload_.write_sip(128);
    // FIntrepidNetworkGUID layout: 4 × uint32 LSB-first (matches sub_14141E960).
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id));
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id >> 32));
    v3_inner_payload_.write_uint32(server_id);
    v3_inner_payload_.write_uint32(randomizer);
}

void PropertyUpdateBunchBuilder::v3_add_property_netguid_with_sip(uint32_t cmd_handle,
                                                                    uint64_t object_id,
                                                                    uint32_t server_id,
                                                                    uint32_t randomizer) {
    if (!v3_block_open_) return;
    // PM123 (2026-05-06) — empirically-correct LEGACY format from captured
    // replay analysis (replay_data.bin pkt#127 b0/b1 decoded by
    // tools/decode_pc_property_fixtures.py).
    //
    // The "MODERN expected" comment on v3_add_property_netguid was based on a
    // misinterpretation of sub_143F2DC60.  Captured AOC server bunches CLEARLY
    // use the LEGACY format for property updates:
    //
    //   pkt#127 b1: [SerializeInt(handle=1, MAX=10) 4b][SIP(NumValueBits=8) 8b]
    //               [value=66 8b]   → 20 bits, total NumPayloadBits=22
    //
    // Wire = SerializeInt(handle, MAX) + SIP(NumValueBits=128) + 128-bit
    //         FIntrepidNetGUID.  ALWAYS writes the SIP regardless of the
    //         v3_use_modern_inner_format_ flag (which only affects RPCs).
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    v3_inner_payload_.write_sip(128);
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id));
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id >> 32));
    v3_inner_payload_.write_uint32(server_id);
    v3_inner_payload_.write_uint32(randomizer);
}

void PropertyUpdateBunchBuilder::v3_add_rpc_handle_only(uint32_t cmd_handle) {
    if (!v3_block_open_) return;
    // PM68 probe: just the handle, no params.  Used to test whether the
    // RPC function takes 0 parameters.  If Reader.IsError fires, function
    // expects >0 params and we need to figure out the param size.
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
}
// [C O A :: C.Y.B]

void PropertyUpdateBunchBuilder::v3_add_rpc_no_params(uint32_t cmd_handle) {
    if (!v3_block_open_) return;
    // 2026-05-06 (PM99) — 0-param RPC, clean wire.
    //
    // Wire format (stock UE5 RepLayout for 0-param RPC inside V3 content block):
    //   [SerializeInt(cmd_handle, MAX)]  ceil(log2(MAX)) bits
    //   [SIP(NumPayloadBits=0)]          8 bits (single byte 0x00)
    //   (no payload)
    //
    // Total inner = ceil(log2(MAX)) + 8 bits.
    //
    // CRITICAL — DO NOT add a terminator or padding inside the content block.
    // Empirical (PM98 test, 2026-05-06):
    //   * pad=200 OR terminator(handle=0)+pad=145: bunch dispatches CIC, but
    //     AOC's RepLayout treats handle=0 as INVALID (not as terminator) and
    //     fails with "ReceivedBunch: Invalid replicated field 0" → connection
    //     close (ObjectReplicatorReceivedBunchFail).
    //   * pad=0 (clean 18-bit inner): bunch silent-drops at outer-parse layer
    //     because the 74-bit total bunch is below AOC's threshold; connection
    //     HOLDS but CIC never fires.
    //
    // SOLUTION (PM99): caller bundles the CIC into the SAME V3 content block
    // as a larger payload (e.g. ClientRestart's NetGUID param).  The combined
    // sub-reader exhausts after both fields are read → loop exits cleanly
    // without ever reading handle=0.  See pc_emitter.cpp emit_pawn_link.
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    v3_inner_payload_.write_sip(0);    // NumPayloadBits = 0 (no params)
}

void PropertyUpdateBunchBuilder::v3_add_property_netguid_sip(uint32_t cmd_handle,
                                                              uint64_t object_id,
                                                              uint32_t server_id,
                                                              uint32_t randomizer) {
    if (!v3_block_open_) return;
    // PM75 (2026-04-30) — SIP inner format observed in captured replay.
    //
    // Wire layout inside the V3 content block:
    //   SIP(handle+1)         — 8 bits (when value < 128, single byte)
    //   IntrepidNetGUID       — 128 bits (4 × uint32 LSB-first)
    //   SIP(0)                — 8 bits terminator
    //
    // Total inner payload: 8 + 128 + 8 = 144 bits.
    //
    // The +1 offset on handle means handle 0 cannot exist, and SIP(0) is
    // unambiguously the terminator.  Per PM6 RE: AOC's parser reads
    // (handle+1, payload, 0) tuples until handle+1 == 0.
    v3_inner_payload_.write_sip(cmd_handle + 1);
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id));
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id >> 32));
    v3_inner_payload_.write_uint32(server_id);
    v3_inner_payload_.write_uint32(randomizer);
    v3_inner_payload_.write_sip(0);  // terminator
}

void PropertyUpdateBunchBuilder::v3_add_rpc_object_param(uint32_t cmd_handle,
                                                          uint64_t object_id,
                                                          uint32_t server_id,
                                                          uint32_t randomizer,
                                                          uint32_t param_num_bits,
                                                          int32_t leading_null_bit,
                                                          uint32_t trailing_pad_bits) {
    if (!v3_block_open_) return;
    // PM81+: flexible RPC object-reference encoder for SIP-value brute-force.
    //   leading_null_bit:
    //     -1 → no leading bit
    //      0 → write a single 0 bit before the GUID
    //      1 → write a single 1 bit before the GUID
    //   trailing_pad_bits: extra zero bits after the GUID (0..32)
    //
    // The SIP value MUST equal the actual payload bits written:
    //   (leading_null_bit != -1 ? 1 : 0) + 128 + trailing_pad_bits == param_num_bits
    // (caller's responsibility — we just write what's requested)
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    v3_inner_payload_.write_sip(param_num_bits);
    if (leading_null_bit == 0) v3_inner_payload_.write_bit(0);
    else if (leading_null_bit == 1) v3_inner_payload_.write_bit(1);
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id));
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id >> 32));
    v3_inner_payload_.write_uint32(server_id);
    v3_inner_payload_.write_uint32(randomizer);
    for (uint32_t i = 0; i < trailing_pad_bits; ++i) v3_inner_payload_.write_bit(0);
}

void PropertyUpdateBunchBuilder::v3_add_rpc_short_netguid(uint32_t cmd_handle,
                                                            uint32_t netguid) {
    if (!v3_block_open_) return;
    // PM84-C: full AOC custom-mode RPC framing with debugger-confirmed
    // NumBits=8 for value sub-reader.
    //
    // Wire layout:
    //   [SerializeInt(cmd_handle, MAX)]   12 bits — function handle
    //   [1 bit prefix = 0]                 1 bit  — consumed by sub_7FF6BD814D20
    //   [SIP(1) = byte 0x02]               8 bits — field handle = 0 (first param)
    //   [SIP(8) = byte 0x10]               8 bits — value sub-reader size
    //   [SIP(netguid) = byte]              8 bits — NetGUID alias for our Pawn
    //   [SIP(0) = byte 0x00]               8 bits — field-list terminator
    //
    // Total: 45 bits inner payload.
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);
    v3_inner_payload_.write_bit(0);              // 1-bit prefix
    v3_inner_payload_.write_sip(1);              // SIP(field_handle+1) = SIP(1)
    v3_inner_payload_.write_sip(8);              // SIP(NumBits = 8)
    v3_inner_payload_.write_sip(netguid);        // 1 SIP byte = NetGUID alias
    v3_inner_payload_.write_sip(0);              // SIP(0) terminator
}

void PropertyUpdateBunchBuilder::v3_add_rpc_object_param_aoc_custom(uint32_t cmd_handle,
                                                                     uint64_t object_id,
                                                                     uint32_t server_id,
                                                                     uint32_t randomizer,
                                                                     uint32_t value_num_bits) {
    if (!v3_block_open_) return;
    // PM83: AOC custom mode RPC param format.
    // [SerializeInt(cmd_handle)] [1 bit prefix] [SIP(1)] [SIP(value_num_bits)]
    //   [128-bit GUID, truncated to value_num_bits if needed] [SIP(0)]
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);

    // 1-bit prefix (consumed by sub_7FF6BD814D20 line 109 advance).  Value
    // doesn't matter; AOC just increments PosBits.  Write 0.
    v3_inner_payload_.write_bit(0);

    // SIP(field_handle + 1) — first param at index 0 → SIP(1) = byte 0x02.
    v3_inner_payload_.write_sip(1);

    // SIP(field_size) — value bit count for sub-reader.
    v3_inner_payload_.write_sip(value_num_bits);

    // value bits — write 128-bit GUID, truncating to value_num_bits.
    // We always write up to 128 bits of the GUID and zero-pad if needed.
    const uint32_t total_value_bits = value_num_bits;
    uint32_t bits_written = 0;

    // Write low 32 bits of ObjectId
    if (bits_written + 32 <= total_value_bits) {
        v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id));
        bits_written += 32;
    } else if (bits_written < total_value_bits) {
        // Partial — write bit-by-bit
        uint32_t v = static_cast<uint32_t>(object_id);
        for (uint32_t i = 0; bits_written < total_value_bits && i < 32; ++i) {
            v3_inner_payload_.write_bit((v >> i) & 1);
            ++bits_written;
        }
    }
// (Cyb + C O A)

    // High 32 bits of ObjectId
    if (bits_written + 32 <= total_value_bits) {
        v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id >> 32));
        bits_written += 32;
    } else if (bits_written < total_value_bits) {
        uint32_t v = static_cast<uint32_t>(object_id >> 32);
        for (uint32_t i = 0; bits_written < total_value_bits && i < 32; ++i) {
            v3_inner_payload_.write_bit((v >> i) & 1);
            ++bits_written;
        }
    }

    // ServerId
    if (bits_written + 32 <= total_value_bits) {
        v3_inner_payload_.write_uint32(server_id);
        bits_written += 32;
    } else if (bits_written < total_value_bits) {
        for (uint32_t i = 0; bits_written < total_value_bits && i < 32; ++i) {
            v3_inner_payload_.write_bit((server_id >> i) & 1);
            ++bits_written;
        }
    }

    // Randomizer
    if (bits_written + 32 <= total_value_bits) {
        v3_inner_payload_.write_uint32(randomizer);
        bits_written += 32;
    } else if (bits_written < total_value_bits) {
        for (uint32_t i = 0; bits_written < total_value_bits && i < 32; ++i) {
            v3_inner_payload_.write_bit((randomizer >> i) & 1);
            ++bits_written;
        }
    }

    // Zero-pad if value_num_bits > 128
    while (bits_written < total_value_bits) {
        v3_inner_payload_.write_bit(0);
        ++bits_written;
    }

    // SIP(0) terminator for field loop.
    v3_inner_payload_.write_sip(0);
}

void PropertyUpdateBunchBuilder::v3_add_rpc_pawn_param_aoc(uint32_t cmd_handle,
                                                            uint64_t object_id,
                                                            uint32_t server_id,
                                                            uint32_t randomizer) {
    if (!v3_block_open_) return;
    // PM86 — RPC framing for APawn* param with leading null indicator.
    //
    // value_num_bits = 129 = 1 (bIsNullActor) + 128 (FIntrepidNetGUID).
    // The null-indicator bit comes FIRST (AOC reads it before the GUID).
    constexpr uint32_t kValueNumBits = 129;

    // Function handle.
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);

    // 1-bit prefix (consumed by AOC-custom field iterator).
    v3_inner_payload_.write_bit(0);

    // SIP(field_handle + 1) — first param at field index 0.
    v3_inner_payload_.write_sip(1);

    // SIP(value_num_bits) — sub-reader size.
    v3_inner_payload_.write_sip(kValueNumBits);

    // ── Value payload (129 bits) ──
    // bit 0: bIsNullActor = 0 (non-null).
    v3_inner_payload_.write_bit(0);

    // bits 1..128: FIntrepidNetGUID (ObjectId u64 LE + ServerId u32 LE + Randomizer u32 LE).
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id));
    v3_inner_payload_.write_uint32(static_cast<uint32_t>(object_id >> 32));
    v3_inner_payload_.write_uint32(server_id);
    v3_inner_payload_.write_uint32(randomizer);

    // SIP(0) terminator for field loop.
    v3_inner_payload_.write_sip(0);
}

void PropertyUpdateBunchBuilder::v3_add_rpc_pawn_param_brute(uint32_t cmd_handle,
                                                              uint64_t object_id,
                                                              uint32_t server_id,
                                                              uint32_t randomizer,
                                                              uint32_t value_num_bits,
                                                              uint32_t leading_zero_bits) {
    if (!v3_block_open_) return;

    // Function handle.
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);

    // 1-bit prefix (consumed by AOC-custom field iterator).
    v3_inner_payload_.write_bit(0);

    // SIP(field_handle + 1) — first param at field index 0.
    v3_inner_payload_.write_sip(1);

    // SIP(value_num_bits) — sub-reader size.
    v3_inner_payload_.write_sip(value_num_bits);

    // ── Value payload ──
    uint32_t bits_written = 0;

    // Leading zero bits (e.g., 1-bit bIsNullActor=0, or 16-bit name idx=0).
    const uint32_t lead = (leading_zero_bits > value_num_bits) ? value_num_bits
                                                                : leading_zero_bits;
    for (uint32_t i = 0; i < lead; ++i) {
        v3_inner_payload_.write_bit(0);
        ++bits_written;
    }

    // FIntrepidNetGUID — 128 bits as 4 little-endian uint32 lanes.
    // Write only as many bits as we have room for (value_num_bits - lead).
    auto write_u32_partial = [&](uint32_t v) {
        for (uint32_t i = 0; i < 32 && bits_written < value_num_bits; ++i) {
            v3_inner_payload_.write_bit((v >> i) & 1);
            ++bits_written;
        }
    };
    write_u32_partial(static_cast<uint32_t>(object_id));
    write_u32_partial(static_cast<uint32_t>(object_id >> 32));
    write_u32_partial(server_id);
    write_u32_partial(randomizer);

    // Zero-pad if value_num_bits > 128 + lead.
    while (bits_written < value_num_bits) {
        v3_inner_payload_.write_bit(0);
        ++bits_written;
    }

    // SIP(0) field-list terminator.
    v3_inner_payload_.write_sip(0);
}
// [C:Y:B x C | O | A]

void PropertyUpdateBunchBuilder::v3_add_rpc_pawn_param_bare(uint32_t cmd_handle,
                                                             uint64_t object_id,
                                                             uint32_t server_id,
                                                             uint32_t randomizer,
                                                             uint32_t value_num_bits,
                                                             uint32_t leading_zero_bits) {
    if (!v3_block_open_) return;
    // PM89 — bare wire per IDA RE.
    //
    //   [SerializeInt(handle)]   13 bits
    //   [1-bit prefix]            1 bit
    //   [value_num_bits raw]      value_num_bits bits
    //
    // No SIP(field_handle+1), no SIP(value_num_bits), no SIP(0) terminator.

    // Function handle.
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);

    // 1-bit prefix (consumed by param iterator advance per RE).
    v3_inner_payload_.write_bit(0);

    // ── Value payload ──
    uint32_t bits_written = 0;
    const uint32_t lead = (leading_zero_bits > value_num_bits) ? value_num_bits
                                                                : leading_zero_bits;
    for (uint32_t i = 0; i < lead; ++i) {
        v3_inner_payload_.write_bit(0);
        ++bits_written;
    }

    // FIntrepidNetGUID — 4 LE u32 lanes, truncated to remaining budget.
    auto write_u32_partial = [&](uint32_t v) {
        for (uint32_t i = 0; i < 32 && bits_written < value_num_bits; ++i) {
            v3_inner_payload_.write_bit((v >> i) & 1);
            ++bits_written;
        }
    };
    write_u32_partial(static_cast<uint32_t>(object_id));
    write_u32_partial(static_cast<uint32_t>(object_id >> 32));
    write_u32_partial(server_id);
    write_u32_partial(randomizer);

    // Pad with zeros if value_num_bits > 128.
    while (bits_written < value_num_bits) {
        v3_inner_payload_.write_bit(0);
        ++bits_written;
    }
}

void PropertyUpdateBunchBuilder::v3_add_rpc_pawn_param_v2(uint32_t cmd_handle,
                                                           uint64_t object_id,
                                                           uint32_t server_id,
                                                           uint32_t randomizer,
                                                           uint32_t value_num_bits,
                                                           uint32_t leading_zero_bits,
                                                           uint32_t prefix_bits) {
    if (!v3_block_open_) return;
    // PM90 — wire = handle + N_prefix bits + value_num_bits raw.
    //
    // PM91 fix — per stock UE5 RepLayout::ReceivePropertiesForRPC source
    // (Engine/Private/RepLayout.cpp:7135), each non-FBoolProperty param has a
    // per-property "is present" bit that MUST be 1 for NetSerializeItem to
    // fire.  We were writing 0, which made AOC skip the GUID read, leaving
    // PosBits far short of MaxBits → Mismatch.
    //
    // Convention: the LAST prefix bit is the per-property flag (write 1).
    // All earlier prefix bits are AOC custom-mode initial advances (write 0).

    // Function handle.
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);

    // Prefix bits.  Last one = per-property flag = 1; earlier ones = 0.
    for (uint32_t i = 0; i < prefix_bits; ++i) {
        const bool is_last = (i + 1 == prefix_bits);
        v3_inner_payload_.write_bit(is_last ? 1 : 0);
    }

    // Value payload.
    uint32_t bits_written = 0;
    const uint32_t lead = (leading_zero_bits > value_num_bits) ? value_num_bits
                                                                : leading_zero_bits;
    for (uint32_t i = 0; i < lead; ++i) {
        v3_inner_payload_.write_bit(0);
        ++bits_written;
    }

    auto write_u32_partial = [&](uint32_t v) {
        for (uint32_t i = 0; i < 32 && bits_written < value_num_bits; ++i) {
            v3_inner_payload_.write_bit((v >> i) & 1);
            ++bits_written;
        }
    };
    write_u32_partial(static_cast<uint32_t>(object_id));
    write_u32_partial(static_cast<uint32_t>(object_id >> 32));
    write_u32_partial(server_id);
    write_u32_partial(randomizer);

    while (bits_written < value_num_bits) {
        v3_inner_payload_.write_bit(0);
        ++bits_written;
    }
}

void PropertyUpdateBunchBuilder::v3_add_rpc_pawn_param_field(uint32_t cmd_handle,
                                                              uint64_t object_id,
                                                              uint32_t server_id,
                                                              uint32_t randomizer,
                                                              uint32_t guid_bits,
                                                              uint32_t leading_zero_bits) {
    if (!v3_block_open_) return;
    // PM92 — stock UE5 wire per ReadFieldHeaderAndPayload + ReceivePropertiesForRPC.
    //
    // PM93 augmentation: the per-prop bit is now controlled via the
    // v3_perprop_bit_ member (set by set_perprop_bit() before calling).
    //   true  → write per-prop=1 (stock UE5 RepLayout::ReceivePropertiesForRPC)
    //   false → omit the bit (AOC custom-mode hypothesis: no per-prop for RPC params)
    //
    // Wire layout:
    //   [SerializeInt(cmd_handle, MAX=4096)]   13 bits
    //   [SIP(NumPayloadBits)]                  variable
    //   [optional 1-bit per-prop flag = 1]     0 or 1 bit
    //   [leading_zero_bits zero bits]
    //   [128-bit FIntrepidNetGUID, truncated to guid_bits]

    const uint32_t perprop_bits = v3_perprop_bit_ ? 1u : 0u;

    // 1. Function handle.
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);

    // 2. Payload size SIP.
    const uint32_t num_payload_bits = perprop_bits + leading_zero_bits + guid_bits;
    v3_inner_payload_.write_sip(num_payload_bits);

    // 3a. Per-property "is present" flag (if enabled).
    if (perprop_bits) {
        v3_inner_payload_.write_bit(1);
    }

    // 3b. Leading zero bits inside value (e.g. 1-bit bIsNullActor=0).
    for (uint32_t i = 0; i < leading_zero_bits; ++i) {
        v3_inner_payload_.write_bit(0);
    }

    // 3c. The FIntrepidNetGUID, truncated to guid_bits (128 max), then
    // zero-pad up to guid_bits if guid_bits > 128 (so caller can include the
    // ExportFlags byte at the end).
    uint32_t bits_written = 0;
    auto write_u32_partial = [&](uint32_t v) {
        for (uint32_t i = 0; i < 32 && bits_written < guid_bits; ++i) {
            v3_inner_payload_.write_bit((v >> i) & 1);
            ++bits_written;
        }
    };
    write_u32_partial(static_cast<uint32_t>(object_id));
    write_u32_partial(static_cast<uint32_t>(object_id >> 32));
    write_u32_partial(server_id);
    write_u32_partial(randomizer);

    // PM94 fix — zero-pad to fill the full guid_bits.  Without this, when
    // guid_bits > 128 (e.g. 136 for AOC's GUID + ExportFlags), the SIP value
    // declares more bits than we actually write, leaving the sub-reader's
    // tail content undefined.
    while (bits_written < guid_bits) {
        v3_inner_payload_.write_bit(0);
        ++bits_written;
    }
// C | Y | B and C-O-A

    // No SIP(0) terminator — stock UE5 detects end via GetBitsLeft() == 0.
}

void PropertyUpdateBunchBuilder::v3_add_rpc_pawn_param_aoc_intrepid(
        uint32_t cmd_handle,
        uint64_t object_id,
        uint32_t server_id,
        uint32_t randomizer,
        uint32_t value_bits) {
    if (!v3_block_open_) return;
    // PM95 (2026-05-03) — AOC custom-mode field-loop wire format.
    //
    // Per F5 decomp of sub_7FF6BD814D20 (param deserializer entry) and
    // sub_7FF6BD8155B0 (the field-loop reader called when AOC custom-mode flag
    // at UNetConnection+0x240 bit 0 is set).
    //
    // Outer entry — sub_7FF6BD814D20:
    //   if (UNetConnection+240 & 1) {
    //     <consume 1-bit advance>
    //     sub_7FF6BD8155B0(...)        // delegate to field-loop reader
    //   } else {
    //     // stock UE5 path — per-prop bit + NetSerializeItem
    //   }
    //
    // Field-loop reader — sub_7FF6BD8155B0:
    //   while (1) {
    //     SIP(field_idx_plus_1) → v90
    //     if (v90 == 0) break;          // 0 = terminator
    //     uint32_t field_idx = v90 - 1; // wire is 1-based
    //     SIP(field_size_bits) → v98
    //     sub_7FF6BA823CC0(sub_sub_reader, reader, v98, 0);
    //     NetSerializeItem(sub_sub_reader);   // sub-reader sized exactly v98 bits
    //   }
    //
    // For ClientRestart(APawn* NewPawn) — single param at field index 0:
    //   field_idx_plus_1 = 1   → SIP(1) = 1 byte
    //   field_size_bits  = 136 → SIP(136) = 16 bits (>=128 needs 2 SIP bytes)
    //   value           = 128-bit FIntrepidNetGUID + 8 zero ExportFlags bits
    //   terminator      = SIP(0) = 1 byte
    //
    // Total inner (inside V3 content block):
    //   SerializeInt(handle, 4096) = 13 bits
    //   1-bit advance               =  1 bit
    //   SIP(1)                      =  8 bits
    //   SIP(value_bits)             =  8 or 16 bits depending on size
    //   value_bits raw              =  N bits
    //   SIP(0) terminator           =  8 bits
    //
    // value_bits is caller-controlled so we can probe alternate sizes:
    //   136 — 128 NetGUID + 8 ExportFlags (most likely per InternalLoadObject)
    //   128 — bare FIntrepidNetGUID
    //   144 — 128 NetGUID + 16 (e.g. 8 ExportFlags + 8 pad)
    //   137 — 128 NetGUID + 9 (FName-encoded ExportFlags)
    //   80  — alternative compact form
    //
    // Within the value payload we write the FIntrepidNetGUID first (up to 128
    // bits), then zero-pad to value_bits.  This matches InternalLoadObject's
    // read order:  Ar << NetGUID (128 bits), then optional flags/pad.
    if (value_bits == 0) return;

    // 1. Function handle (RPC dispatch index inside V3 content block).
    v3_inner_payload_.write_serialize_int(cmd_handle, v3_num_properties_);

    // 2. 1-bit advance (consumed by sub_7FF6BD814D20 line 109 before
    //    delegating to sub_7FF6BD8155B0).  Value is don't-care.
    v3_inner_payload_.write_bit(0);

    // 3. SIP(field_idx + 1) — first param at field index 0 → SIP(1).
    v3_inner_payload_.write_sip(1);

    // 4. SIP(field_size_bits) — sub-sub-reader will be exactly this size.
    v3_inner_payload_.write_sip(value_bits);

    // 5. Value payload — FIntrepidNetGUID (128 bits) + zero-pad to value_bits.
    uint32_t bits_written = 0;
    auto write_u32_partial = [&](uint32_t v) {
        for (uint32_t i = 0; i < 32 && bits_written < value_bits; ++i) {
            v3_inner_payload_.write_bit((v >> i) & 1);
            ++bits_written;
        }
    };
    write_u32_partial(static_cast<uint32_t>(object_id));
    write_u32_partial(static_cast<uint32_t>(object_id >> 32));
    write_u32_partial(server_id);
    write_u32_partial(randomizer);

    // Zero-pad up to the SIP-declared sub-reader size.  Per InternalLoadObject
    // (sub_7FF6BE3647B0): 128-bit GUID, then 8-bit ExportFlags read iff
    // (ExportFlags & 1) == 0 — we satisfy both branches by writing 8 zero bits
    // (ExportFlags = 0 → no extra subreads).  Any extra trailing bits are
    // benign because GetBitsLeft() check happens at the OUTER param boundary,
    // not inside the per-field sub-sub-reader (AOC seeks past unread bits).
    while (bits_written < value_bits) {
        v3_inner_payload_.write_bit(0);
        ++bits_written;
    }
// (CoA | C_Y_B)

    // 6. SIP(0) terminator — exits the field loop in sub_7FF6BD8155B0.
    v3_inner_payload_.write_sip(0);
}

void PropertyUpdateBunchBuilder::v3_add_terminator() {
    if (!v3_block_open_) return;
    // Field-list terminator: SerializeInt(0, MAX) — matches the field-handle
    // encoding used by v3_add_property_*.  All zero bits, count determined
    // by ceil_to_max_bits(v3_num_properties_).
    v3_inner_payload_.write_serialize_int(0, v3_num_properties_);
}

void PropertyUpdateBunchBuilder::v3_end_content_block() {
    if (!v3_block_open_) return;

    // Build the content block bytes (header + inner payload)
    BunchWriter blob_writer(256);

    // [1 bit] bOutermostEnd = 0  (this is NOT the end marker)
    blob_writer.write_bit(0);
    // [1 bit] bIsChannelActor
    blob_writer.write_bit(v3_block_is_channel_actor_ ? 1 : 0);
    // [SIP] NetGUID — only when targeting a subobject (bIsChannelActor=0).
    // Verified empirically by exact-fit decode of pkt#30 ch=3: the
    // subobject NetGUID is a plain SerializeIntPacked reference to a
    // previously-cached GUID (no inline export here — that only happens
    // in PME-tagged bunches, which we don't use for V3).
    if (!v3_block_is_channel_actor_) {
        blob_writer.write_sip(v3_subobject_netguid_);
    }

    // [SerializeIntPacked] NumPayloadBits
    const uint32_t num_payload_bits = static_cast<uint32_t>(v3_inner_payload_.bit_pos());
    blob_writer.write_sip(num_payload_bits);

    // [Inner bunch bits]
    blob_writer.write_bit_range(v3_inner_payload_.data(), 0, num_payload_bits);

    // Push as queued blob
    Blob b;
    b.bit_count = blob_writer.bit_pos();
    const size_t n_bytes = (b.bit_count + 7) / 8;
    b.bytes.assign(blob_writer.data(), blob_writer.data() + n_bytes);
    queued_payloads_.push_back(std::move(b));
    payload_bits_ += b.bit_count;

    // Reset state for next block
    v3_block_open_ = false;
    v3_inner_payload_ = BunchWriter(0);
}

void PropertyUpdateBunchBuilder::v3_finish_bunch() {
    // PM97 (2026-05-03) — DO NOT append an end marker bit.
    //
    // PRIOR BEHAVIOR (caused post-possession connection drop after PM96 success):
    //   We appended 1 bit (bOutermostEnd=1) hoping AOC would treat it as
    //   "end of content blocks".  Per RE of sub_143F2C340 (AOC's
    //   ReadContentBlockHeader), AOC ALWAYS reads 2 bits before deciding
    //   (bOutermostEnd + bIsChannelActor).  Our 1-bit marker triggered
    //   overflow on the second read → "Bunch.IsError() after reading actor bit"
    //   → ContentBlockHeaderIsActorFail → connection close.
    //
    // The payload is now exactly the V3 content block size with no trailing
    // marker.  After AOC processes the content block, GetBitsLeft = 0 and
    // ProcessBunch's loop exits cleanly without firing ReadContentBlockHeader.
    //
    // (Stock UE5 worked with the 1-bit marker because its ReadContentBlockHeader
    // returns immediately after reading bOutermostEnd=1.  AOC's variant doesn't.)
}

void PropertyUpdateBunchBuilder::add_raw_payload(const uint8_t* data,
                                                  size_t bit_count) {
    Blob b;
    b.bit_count = bit_count;
    const size_t n_bytes = (bit_count + 7) / 8;
    b.bytes.assign(data, data + n_bytes);
    queued_payloads_.push_back(std::move(b));
    payload_bits_ += bit_count;
}

// ── Bunch header writer ────────────────────────────────────────────────
// Matches ActorBuilder's parse path (sc_bunch_parser.h) for a data bunch:
//
//   bit 0   bControl          = 0
//   bit 1   bIsReplicationPaused = 0
//   bit 2   bReliable         = is_reliable_
//   bits 3+ ChIndex (SerializeIntPacked)
//   bit     bHasPackageMapExports   = 0
//   bit     bHasMustBeMappedGUIDs   = 0
//   bit     bPartial                = 0
//   bits    ChSequence (if bReliable) — ch=0 uses 10 bits, ch>0 uses 12 bits
//   bits    BunchDataBits (SerializeIntPacked)

void PropertyUpdateBunchBuilder::write_bunch_header(BunchWriter& out,
                                                     uint32_t bunch_data_bits) const {
    out.write_bit(0);                                 // bControl = 0 (data bunch)
    // bIsReplicationPaused — empirical: ALL captured ch=3 updates targeting
    // subobject NetGUID 7193 (PlayerState) set this bit to 1.  Without it,
    // V3 silent-drops despite header parsing OK.  See set_is_rep_paused().
    out.write_bit(is_rep_paused_ ? 1 : 0);            // bIsReplicationPaused
    out.write_bit(is_reliable_ ? 1 : 0);              // bReliable
    out.write_sip(channel_);                           // ChIndex via SIP
    out.write_bit(0);                                 // bHasPackageMapExports
    // bHasMustBeMappedGUIDs — empirical: every captured ch=3 property
    // update from replay_data.bin sets this to 1.  V3 originally sent 0
    // (silent drop hypothesis #2 from RE-AOC-BINARY-GROUND-TRUTH.md).
    // Configurable via PropertyUpdateBunchBuilder::set_has_mbg().
    out.write_bit(has_mbg_ ? 1 : 0);                  // bHasMustBeMappedGUIDs
    out.write_bit(0);                                 // bPartial = 0 (key: non-partial)

    if (is_reliable_) {
        // PM54 (2026-04-30) — same 12→10 ChSeq fix as PM50 applied to
        // actor_builder.cpp.  AOC client reads ChSeq as SerializeInt(MAX=1024)
        // = 10 bits regardless of channel.  Writing 12 bits put 2 extra bits
        // on the wire that drifted the client's ChName parser → CNSF.
        // This is the SAME bug that caused PC.Pawn link CNSF at PM53 test
        // (Size: 520251962 in client log around 13:13:47).
        out.write_serialize_int(ch_sequence_, 1024);
    }

    // bPartial=0 → no sub-flags (bPartialInitial / bPartialFinal etc. skipped).

    // ChName — present when (bReliable || bCtrlOpen) AND (!bPartial || bPartialInitial).
    // We never emit bControl/bPartial here, so the condition collapses to
    // "is_reliable_ == true".  Mirrors actor_builder.cpp L158-169 exactly.
    // Without this field, every reliable bunch produced by this builder was
    // structurally malformed and the parser misread BDB into ChName bits.
    if (is_reliable_) {
        out.write_bit(ch_name_is_hardcoded_ ? 1 : 0);
        if (ch_name_is_hardcoded_) {
            out.write_sip(ch_name_ename_idx_);
        } else {
            out.write_fstring_ansi(ch_name_string_);
            out.write_int32(ch_name_number_);
        }
    }

    // BunchDataBits — 13-bit fixed via SerializeInt(MAX_PKT_BITS=8192).
    // Per RE: parser reads exactly 13 bits (sc_bunch_parser.h L204-206), and
    // ActorBuilder writes via write_serialize_int(bdb, 1024*8) (actor_builder.cpp
    // L172).  Earlier we used write_sip() which produced 8/16/24-bit varint —
    // the client then misframed the bunch payload, silently dropping V3.
    out.write_serialize_int(bunch_data_bits, 1024 * 8);
}
// (C | Y | B + CoA)

// ── Build() — full bunch = header + queued payloads ───────────────────

size_t PropertyUpdateBunchBuilder::build(BunchWriter& out) const {
    const size_t start_bits = out.bit_pos();

    // 1. Render the payload so we know its total bit count for the header.
    BunchWriter payload(256);
    for (const Blob& b : queued_payloads_) {
        payload.write_bit_range(b.bytes.data(), 0, b.bit_count);
    }
    const uint32_t bunch_data_bits = static_cast<uint32_t>(payload.bit_pos());

    // 2. Header.
    write_bunch_header(out, bunch_data_bits);

    // 3. Payload.
    out.write_bit_range(payload.data(), 0, payload.bit_pos());

    return out.bit_pos() - start_bits;
}

}}} // namespace aoc::protocol::emit

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once
/// ============================================================================
/// UE5 Replication Wire Format — Bunch Builder & Actor Replicator
/// ============================================================================
///
/// Implements the UE5 actor replication wire format for constructing S>C
/// game data packets. Based on Phase 1-3 analysis of AoC traffic.
///
/// Architecture:
///   BitWriter       — Low-level bit stream builder (LSB-first per byte)
///   SIPWriter       — SerializeIntPacked (variable-length 7-bit encoding)
///   GUIDWriter      — FNetworkGUID serialization (SIP64)
///   BunchBuilder    — Constructs individual bunch headers + payload
///   ContentBlock    — Builds content block headers (bHasRepLayout + bIsActor)
///   PacketAssembler — Combines bunches into full UE5 data packets
///
/// Usage:
///   PacketAssembler pkt(client_state);
///   {
///       auto bunch = pkt.begin_bunch(ch, true, true, false, true); // ctrl,open,close,reliable
///       bunch.write_guid_exports(exports);
///       bunch.write_new_actor(actor_guid, archetype, level, location, rotation);
///       bunch.write_content_block(has_rep, is_actor, payload_data, payload_bits);
///   }
///   auto [buf, len] = pkt.finalize();
///   send_to_client(buf, len, addr);

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>
#include <array>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace ue5_repl {

// ─── BitWriter ──────────────────────────────────────────────────────────────
//
// Writes bits LSB-first per byte (matching UE5 FBitWriter layout).

class BitWriter {
public:
    explicit BitWriter(size_t initial_cap = 2048)
        : data_(initial_cap, 0), bit_pos_(0) {}

    /// Write `count` bits from `value` (LSB-first).
    void write(uint64_t value, int count) {
        ensure_capacity(bit_pos_ + count);
        for (int i = 0; i < count; ++i) {
            if ((value >> i) & 1) {
                size_t byte_idx = (bit_pos_ + i) / 8;
                int bit_idx = (bit_pos_ + i) % 8;
                data_[byte_idx] |= (1 << bit_idx);
            }
        }
        bit_pos_ += count;
    }

    /// Write a single bit.
    void write_bit(bool val) { write(val ? 1 : 0, 1); }

    /// Write N bits from a byte buffer (LSB-first per byte).
    void write_bits_from(const uint8_t* src, size_t num_bits) {
        ensure_capacity(bit_pos_ + num_bits);
        for (size_t i = 0; i < num_bits; ++i) {
            size_t src_byte = i / 8;
            int src_bit = i % 8;
            if ((src[src_byte] >> src_bit) & 1) {
                size_t dst_byte = (bit_pos_ + i) / 8;
                int dst_bit = (bit_pos_ + i) % 8;
                data_[dst_byte] |= (1 << dst_bit);
            }
        }
        bit_pos_ += num_bits;
    }
// -- C O A x C:Y:B --

    /// Write a uint8.
    void write_uint8(uint8_t val) { write(val, 8); }

    /// Write a uint16 (little-endian in bitstream).
    void write_uint16(uint16_t val) { write(val, 16); }

    /// Write a uint32 (little-endian in bitstream).
    void write_uint32(uint32_t val) { write(val, 32); }

    /// Write an int32 (little-endian in bitstream).
    void write_int32(int32_t val) {
        uint32_t u;
        std::memcpy(&u, &val, 4);
        write(u, 32);
    }

    /// Write a float32 (IEEE 754, little-endian in bitstream).
    void write_float(float val) {
        uint32_t u;
        std::memcpy(&u, &val, 4);
        write(u, 32);
    }

    /// Write a float64 (IEEE 754, little-endian in bitstream).
    void write_double(double val) {
        uint64_t u;
        std::memcpy(&u, &val, 8);
        write(u, 64);
    }

    /// Write SerializeIntPacked (uint32, variable-length 7-bit encoding).
    void write_sip(uint32_t val) {
        do {
            uint8_t byte = static_cast<uint8_t>((val & 0x7F) << 1);
            val >>= 7;
            if (val > 0) byte |= 1; // more flag
            write_uint8(byte);
        } while (val > 0);
    }

    /// Write SerializeIntPacked64 (uint64, variable-length 7-bit encoding).
    void write_sip64(uint64_t val) {
        do {
            uint8_t byte = static_cast<uint8_t>((val & 0x7F) << 1);
            val >>= 7;
            if (val > 0) byte |= 1; // more flag
            write_uint8(byte);
        } while (val > 0);
    }

    /// Write FNetworkGUID (SerializeIntPacked64 of ObjectId).
    /// ObjectId = (index << 1) | (is_static ? 1 : 0)
    void write_net_guid(uint64_t object_id) {
        write_sip64(object_id);
    }

    /// Write FString: int32(length_with_null) + chars + null.
    /// Empty string writes int32(0).
    void write_fstring(const std::string& s) {
        if (s.empty()) {
            write_int32(0);
        } else {
            int32_t save_num = static_cast<int32_t>(s.size() + 1); // include null
            write_int32(save_num);
            for (char c : s)
                write_uint8(static_cast<uint8_t>(c));
            write_uint8(0); // null terminator
        }
    }

    /// Write SerializeInt(Value, MaxValue) — minimum bits for [0, Max-1].
    void write_serialize_int(uint32_t value, uint32_t max_val) {
        if (max_val <= 1) return;
        int bits = 0;
        uint32_t tmp = max_val - 1;
        while (tmp > 0) { bits++; tmp >>= 1; }
        write(value, bits);
    }

    /// Write FRotator::SerializeCompressedShort (3 flags + 16-bit components).
    void write_rotation_short(float pitch, float yaw, float roll) {
        // Compress each axis to uint16
        auto compress = [](float angle) -> uint16_t {
            // Normalize to [0, 360)
            float n = std::fmod(angle, 360.0f);
            if (n < 0) n += 360.0f;
            return static_cast<uint16_t>(std::round(n * 65536.0f / 360.0f)) & 0xFFFF;
        };

        uint16_t cp = compress(pitch);
        uint16_t cy = compress(yaw);
        uint16_t cr = compress(roll);
// (C:Y:B) (C | O | A)

        // Write 3 flag bits
        write_bit(cp != 0); // pitch non-zero
        write_bit(cy != 0); // yaw non-zero
        write_bit(cr != 0); // roll non-zero

        if (cp != 0) write_uint16(cp);
        if (cy != 0) write_uint16(cy);
        if (cr != 0) write_uint16(cr);
    }

    /// Write QuantizedVector (FVector_NetQuantize with ScaleFactor).
    /// Format: SerializeInt(header, 128) + 3 × N-bit signed components.
    ///   header[4:0] = ComponentBitCount - 2 (if quantized)
    ///   header[5]   = bUseScaledValue (1 for scaled)
    ///   header[6]   = extra info (0 for quantized)
    /// If all components are 0, writes a minimal encoding.
    void write_quantized_vector(double x, double y, double z, int scale = 10) {
        // Scale the values
        int64_t sx = static_cast<int64_t>(std::round(x * scale));
        int64_t sy = static_cast<int64_t>(std::round(y * scale));
        int64_t sz = static_cast<int64_t>(std::round(z * scale));

        // Find the minimum bits needed to represent the biased values
        // Each component is stored as unsigned with bias = 1 << (bits - 1)
        auto abs_max = std::max({std::abs(sx), std::abs(sy), std::abs(sz)});

        int comp_bits = 2; // minimum
        while (comp_bits < 32) {
            int64_t bias = int64_t(1) << (comp_bits - 1);
            if (abs_max < bias) break;
            comp_bits++;
        }

        // Encode header: bits 0-4 = (comp_bits - 2), bit 5 = bUseScaled(1)
        uint32_t header = static_cast<uint32_t>(comp_bits - 2) | (1 << 5);
        write_serialize_int(header, 128); // 7 bits

        // Write 3 components as biased unsigned values
        int64_t bias = int64_t(1) << (comp_bits - 1);
        uint32_t ux = static_cast<uint32_t>(sx + bias);
        uint32_t uy = static_cast<uint32_t>(sy + bias);
        uint32_t uz = static_cast<uint32_t>(sz + bias);

        write(ux, comp_bits);
        write(uy, comp_bits);
        write(uz, comp_bits);
    }

    /// Write ConditionallySerializeQuantizedVector:
    ///   1 bit bWasSerialized, 1 bit bShouldQuantize, then vector data.
    void write_conditional_qvec(bool serialize, bool quantize,
                                 double x, double y, double z, int scale = 10) {
        write_bit(serialize);
        if (!serialize) return;
        write_bit(quantize);
        if (quantize) {
            write_quantized_vector(x, y, z, scale);
        } else {
            // Raw doubles
            write_double(x);
            write_double(y);
            write_double(z);
        }
    }

    /// Current bit position.
    size_t bit_pos() const { return bit_pos_; }

    /// Current byte count (rounded up).
    size_t byte_count() const { return (bit_pos_ + 7) / 8; }

    /// Access the data buffer.
    const uint8_t* data() const { return data_.data(); }
    uint8_t* data() { return data_.data(); }

    /// Get the data as a vector (trimmed to byte_count).
    std::vector<uint8_t> to_bytes() const {
        return std::vector<uint8_t>(data_.begin(), data_.begin() + byte_count());
    }

    /// Reset the writer.
    void reset() {
        std::fill(data_.begin(), data_.end(), 0);
        bit_pos_ = 0;
    }

    /// Save/restore position for length-prefixed sections.
    size_t save_pos() const { return bit_pos_; }
    void restore_pos(size_t pos) { bit_pos_ = pos; }

    /// Overwrite bits at a specific position (for patching length fields).
    void patch(size_t pos, uint64_t value, int count) {
        for (int i = 0; i < count; ++i) {
            size_t byte_idx = (pos + i) / 8;
            int bit_idx = (pos + i) % 8;
            if ((value >> i) & 1)
                data_[byte_idx] |= (1 << bit_idx);
            else
                data_[byte_idx] &= ~(1 << bit_idx);
        }
    }

private:
    std::vector<uint8_t> data_;
    size_t bit_pos_;

    void ensure_capacity(size_t bits_needed) {
        size_t bytes_needed = (bits_needed + 7) / 8;
        if (bytes_needed > data_.size()) {
            size_t new_cap = std::max(data_.size() * 2, bytes_needed + 256);
            data_.resize(new_cap, 0);
        }
    }
};

// ─── InternalWriteObject ────────────────────────────────────────────────────
//
// Recursive NetGUID + path export for the PackageMap.

struct GUIDExport {
    uint64_t guid_object_id = 0; // Full ObjectId (value << 1 | static_bit)
    uint8_t  export_flags = 0;   // bHasPath(0) | bNoLoad(1) | bHasNetworkChecksum(2)
    uint64_t outer_guid = 0;     // Outer's ObjectId (0 if no outer)
    std::string name;            // Object path name
    uint32_t checksum = 0;       // Network checksum (if flag set)

    // Nested outer exports (for recursive paths like Engine → Class → Package)
    std::vector<GUIDExport> outer_chain;
};

/// Write InternalWriteObject (recursive GUID export).
inline void write_internal_object(BitWriter& w, const GUIDExport& exp) {
    w.write_net_guid(exp.guid_object_id);

    if (exp.guid_object_id == 0) return; // null

    w.write_uint8(exp.export_flags);
// [Cyb - C | O | A]

    if (exp.export_flags & 0x01) { // bHasPath
        // Recurse for outer
        if (!exp.outer_chain.empty()) {
            write_internal_object(w, exp.outer_chain[0]);
        } else {
            // Write outer GUID only (no path)
            w.write_net_guid(exp.outer_guid);
            if (exp.outer_guid != 0) {
                w.write_uint8(0); // outer has no path
            }
        }

        // Object name
        w.write_fstring(exp.name);

        // Checksum
        if (exp.export_flags & 0x04) {
            w.write_uint32(exp.checksum);
        }
    }
}

// ─── SerializeNewActor ──────────────────────────────────────────────────────

struct NewActorData {
    uint64_t actor_guid = 0;     // ObjectId (value << 1 | is_static)
    bool     is_dynamic = false;

    // Dynamic actor fields
    uint64_t archetype_guid = 0; // Archetype ObjectId
    uint64_t level_guid = 0;     // Level ObjectId

    // Spawn info
    bool  has_location = true;
    bool  quantize_location = true;
    double loc_x = 0, loc_y = 0, loc_z = 0;

    bool  has_rotation = true;
    float rot_pitch = 0, rot_yaw = 0, rot_roll = 0;

    bool  has_scale = false;
    double scale_x = 1, scale_y = 1, scale_z = 1;

    bool  has_velocity = false;
    double vel_x = 0, vel_y = 0, vel_z = 0;
};

inline void write_new_actor(BitWriter& w, const NewActorData& actor) {
    // Actor NetGUID (bare, not in export mode)
    w.write_net_guid(actor.actor_guid);

    if (!actor.is_dynamic) return; // static actor: just the GUID

    // Archetype NetGUID
    w.write_net_guid(actor.archetype_guid);

    // Level NetGUID
    w.write_net_guid(actor.level_guid);

    // Location
    w.write_conditional_qvec(actor.has_location, actor.quantize_location,
                              actor.loc_x, actor.loc_y, actor.loc_z);

    // Rotation
    w.write_bit(actor.has_rotation);
    if (actor.has_rotation) {
        w.write_rotation_short(actor.rot_pitch, actor.rot_yaw, actor.rot_roll);
    }

    // Scale
    w.write_conditional_qvec(actor.has_scale, true,
                              actor.scale_x, actor.scale_y, actor.scale_z);

    // Velocity
    w.write_conditional_qvec(actor.has_velocity, true,
                              actor.vel_x, actor.vel_y, actor.vel_z);
}

// ─── Content Block ──────────────────────────────────────────────────────────

/// Write a content block header + payload.
///   bHasRepLayout=1 → SIP(NumPayloadBits) + payload
///   bHasRepLayout=0 → payload extends to end of bunch (no size prefix)
///   bIsActor=1      → actor-level properties (no sub-object GUID)
///   bIsActor=0      → sub-object (GUID + stably_named + class + payload)
inline void write_content_block_actor(BitWriter& w, bool has_rep,
                                       const uint8_t* payload, size_t payload_bits) {
    w.write_bit(has_rep);
    w.write_bit(true);  // bIsActor = 1

    if (has_rep) {
        // Explicit size via SIP
        w.write_sip(static_cast<uint32_t>(payload_bits));
    }
    // else: no size prefix, payload extends to end of bunch

    // Write payload bits
    if (payload && payload_bits > 0) {
        w.write_bits_from(payload, payload_bits);
    }
}

/// Write an empty content block (null terminator for the content loop).
/// The content block loop ends when the bunch reader hits AtEnd().
/// No explicit terminator is needed — the BunchDataBits field controls it.

// ─── Bunch Builder ──────────────────────────────────────────────────────────
//
// Constructs a single bunch header + data. The bunch data is accumulated
// in a BitWriter, then the header + data are combined.

struct BunchHeader {
    bool     control = false;
    bool     open = false;
    bool     close = false;
    uint8_t  close_reason = 0;   // EChannelCloseReason (0=Destroyed)
    bool     repl_paused = false;
    bool     reliable = false;
    uint32_t ch_index = 0;
    bool     has_exports = false;
    bool     has_must_map = false;
    bool     partial = false;
    uint16_t ch_sequence = 0;    // 10-bit (reliable)

    // Channel name — sent when (bReliable || bOpen)
    bool     ch_name_hardcoded = true;
    uint32_t ch_name_index = 255;   // UE5 hardcoded name index
    std::string ch_name_string;     // For non-hardcoded names
};

class BunchBuilder {
public:
    BunchHeader header;
    BitWriter   data; // Bunch payload data

    /// Write the complete bunch (header + data) into the target writer.
    /// Returns the number of bits written.
    size_t write_to(BitWriter& target) const {
        size_t start = target.bit_pos();

        // ── Bunch header bits ────────────────────────────────────────
        target.write_bit(header.control);
        if (header.control) {
            target.write_bit(header.open);
            target.write_bit(header.close);
            if (header.close) {
                // CloseReason: SerializeInt(value, MAX=5) → 3 bits
                target.write_serialize_int(header.close_reason, 5);
            }
        }
        target.write_bit(header.repl_paused);
        target.write_bit(header.reliable);
// C.O.A x Cyb

        // ChIndex (SerializeIntPacked)
        target.write_sip(header.ch_index);

        target.write_bit(header.has_exports);
        target.write_bit(header.has_must_map);
        target.write_bit(header.partial);

        // ChSequence (10 bits) if reliable
        if (header.reliable) {
            target.write(header.ch_sequence, 10);
        }

        // ChName — sent when (bReliable || bOpen)
        if (header.reliable || header.open) {
            target.write_bit(header.ch_name_hardcoded);
            if (header.ch_name_hardcoded) {
                // Write name index using SerializeIntPacked
                target.write_sip(header.ch_name_index);
            } else {
                // FString
                target.write_fstring(header.ch_name_string);
            }
        }

        // BunchDataBits (13 bits — CeilLog2(MaxPacket * 8) = CeilLog2(8192) = 13)
        uint16_t data_bits = static_cast<uint16_t>(data.bit_pos());
        target.write(data_bits, 13);

        // ── Bunch payload data ───────────────────────────────────────
        if (data_bits > 0) {
            target.write_bits_from(data.data(), data_bits);
        }

        return target.bit_pos() - start;
    }
};

// ─── GUID Export Bunch Writer ───────────────────────────────────────────────
//
// Writes bHasPackageMapExports=1 bunch data:
//   bHasRepLayoutExport(1) = 0
//   NumGUIDs(SIP32)
//   [InternalWriteObject × NumGUIDs]

inline void write_guid_export_bunch_data(BitWriter& w,
                                          const std::vector<GUIDExport>& exports) {
    w.write_bit(false); // bHasRepLayoutExport = 0
    w.write_uint32(static_cast<uint32_t>(exports.size())); // NumGUIDs (int32 via operator<<)

    for (const auto& exp : exports) {
        write_internal_object(w, exp);
    }
}

// ─── RepLayout Export Bunch Writer ──────────────────────────────────────────
//
// Writes bHasRepLayoutExport=1 bunch data:
//   bHasRepLayoutExport(1) = 1
//   NumLayoutCmdExports(int32)
//   for each:
//     PathNameIndex(uint32)
//     bExported(1)
//     if bExported: PathName(FString) + NumFields(int32) + fields...

struct RepLayoutField {
    uint32_t handle = 0;
    uint32_t compat_checksum = 0;
    bool     exported = true;
    std::string name;
    uint8_t  type = 0;
};

struct RepLayoutExport {
    uint32_t path_index = 0;
    bool     exported = true;
    std::string path_name;
    std::vector<RepLayoutField> fields;
};

inline void write_rep_layout_export_bunch_data(BitWriter& w,
                                                const std::vector<RepLayoutExport>& exports) {
    w.write_bit(true); // bHasRepLayoutExport = 1
    w.write_uint32(static_cast<uint32_t>(exports.size()));

    for (const auto& exp : exports) {
        w.write_uint32(exp.path_index);
        w.write_bit(exp.exported);

        if (exp.exported) {
            w.write_fstring(exp.path_name);
            w.write_uint32(static_cast<uint32_t>(exp.fields.size()));

            for (const auto& field : exp.fields) {
                w.write_uint32(field.handle);
                w.write_uint32(field.compat_checksum);
                w.write_bit(field.exported);
                if (field.exported) {
                    w.write_fstring(field.name);
                    w.write_uint8(field.type);
                }
            }
        }
    }
}
// == C.Y.B | CoA ==

// ─── MustBeMappedGUIDs ─────────────────────────────────────────────────────

inline void write_must_be_mapped(BitWriter& w, const std::vector<uint64_t>& guids) {
    w.write_uint16(static_cast<uint16_t>(guids.size()));
    for (auto guid : guids) {
        w.write_net_guid(guid);
    }
}

// ─── FRepMovement (Custom Delta, AoC format) ────────────────────────────────
//
// Phase 3 discovery: AoC uses bHasRepLayout=0 Custom Delta format:
//   [8b flags=0x2C][SIP(remaining_bits)][QVec loc][3×u8 rot][QVec vel][AoC state]
//
// Flags byte 0x2C:
//   bSleep=0, bPhys=0, LocQuant=3, VelQuant=2, RotQuant=0 (ByteCompressed)
//
// The SIP after flags = number of remaining bits in the field data.

inline void write_rep_movement_custom_delta(BitWriter& w,
                                             double loc_x, double loc_y, double loc_z,
                                             uint8_t rot_pitch, uint8_t rot_yaw, uint8_t rot_roll,
                                             double vel_x, double vel_y, double vel_z) {
    // Build the movement field data in a temporary writer
    BitWriter field_data;

    // Flags byte (0x2C = standard AoC movement)
    field_data.write_uint8(0x2C);

    // Location (WritePackedVector with scale=10)
    field_data.write_quantized_vector(loc_x, loc_y, loc_z, 10);

    // Rotation (3 × uint8, ByteCompressed)
    field_data.write_uint8(rot_pitch);
    field_data.write_uint8(rot_yaw);
    field_data.write_uint8(rot_roll);

    // Velocity (WritePackedVector with scale=10)
    field_data.write_quantized_vector(vel_x, vel_y, vel_z, 10);

    // Now we know the total size. Write as Custom Delta:
    // First the flags byte
    w.write_uint8(0x2C);

    // SIP = remaining bits after this SIP
    size_t remaining_bits = field_data.bit_pos() - 8; // everything after flags
    w.write_sip(static_cast<uint32_t>(remaining_bits));

    // Write the actual field data (skip the flags byte we already wrote)
    // Copy from field_data starting at bit 8
    for (size_t i = 8; i < field_data.bit_pos(); ++i) {
        size_t byte_idx = i / 8;
        int bit_idx = i % 8;
        bool val = (field_data.data()[byte_idx] >> bit_idx) & 1;
        w.write_bit(val);
    }
}

// ─── Packet Assembler ───────────────────────────────────────────────────────
//
// Builds a complete UE5 data packet:
//   [Outer header (38b)] [FNetPacketNotify (32b)] [History (32b×N)]
//   [AoC custom field (48b)] [PacketInfo] [Bunches...] [Sentinel] [Termination]

struct PacketConfig {
    uint32_t magic_header = 0;
    uint8_t  session_id = 0;
    uint8_t  client_id = 0;
    uint16_t out_seq = 0;
    uint16_t in_ack_seq = 0;
    uint16_t hist_count = 1;
    uint8_t  custom_field[6] = {};
    bool     custom_field_valid = false;
    bool     has_pkt_info = true;
    uint16_t jitter_ms = 1023;
    bool     has_srv_frame = false;
    uint8_t  frame_time = 0;
};
// (C_Y_B : C.O.A)

class PacketAssembler {
public:
    explicit PacketAssembler(const PacketConfig& config)
        : config_(config), writer_(16384) {}

    /// Add a bunch to the packet. Returns false if the bunch is too large.
    bool add_bunch(const BunchBuilder& bunch) {
        bunches_.push_back(&bunch);
        return true;
    }

    /// Finalize the packet: write outer + packed header + custom field +
    /// packet info + all bunches + sentinel + termination.
    /// Returns (buffer, byte_length).
    std::pair<const uint8_t*, size_t> finalize() {
        writer_.reset();

        // ── Outer layer (38 bits) ──────────────────────────────────
        writer_.write(config_.magic_header, 32);
        writer_.write(config_.session_id, 2);
        writer_.write(config_.client_id, 3);
        writer_.write(0, 1); // HandshakeBit = 0

        // ── FNetPacketNotify packed header (32 bits) ───────────────
        uint32_t packed = (static_cast<uint32_t>(config_.out_seq & 0x3FFF) << 18) |
                          (static_cast<uint32_t>(config_.in_ack_seq & 0x3FFF) << 4) |
                          (static_cast<uint32_t>((config_.hist_count - 1) & 0x0F));
        writer_.write(packed, 32);

        // ── History words ──────────────────────────────────────────
        for (uint16_t i = 0; i < config_.hist_count; ++i)
            writer_.write(0, 32);

        // ── AoC custom field (48 bits / 6 bytes) ───────────────────
        if (config_.custom_field_valid) {
            for (int i = 0; i < 6; ++i)
                writer_.write_uint8(config_.custom_field[i]);
        }

        // ── PacketInfo ─────────────────────────────────────────────
        writer_.write_bit(config_.has_pkt_info);
        if (config_.has_pkt_info) {
            writer_.write(config_.jitter_ms, 10);
        }
        writer_.write_bit(config_.has_srv_frame);
        if (config_.has_srv_frame) {
            writer_.write_uint8(config_.frame_time);
        }

        // ── Bunches ────────────────────────────────────────────────
        for (const auto* bunch : bunches_) {
            bunch->write_to(writer_);
        }

        // ── Sentinel bit (AoC-specific) ────────────────────────────
        writer_.write_bit(true);

        // ── Termination bit + padding ──────────────────────────────
        writer_.write_bit(true); // termination '1'
        size_t pad = (8 - (writer_.bit_pos() % 8)) % 8;
        for (size_t i = 0; i < pad; ++i)
            writer_.write_bit(false);

        return {writer_.data(), writer_.byte_count()};
    }

private:
    PacketConfig config_;
    BitWriter writer_;
    std::vector<const BunchBuilder*> bunches_;
};

// ─── Helper: build PacketConfig from ClientState ────────────────────────────
// (Forward-declared; actual ClientState is in game_server.h)

// ─── Channel Sequence Manager ───────────────────────────────────────────────
//
// Tracks per-channel reliable sequence numbers for actor channels.
// C-O-A and Cyb

class ChannelSequenceManager {
public:
    /// Get next reliable sequence for a channel.
    uint16_t next_seq(uint32_t ch) {
        auto it = seqs_.find(ch);
        if (it == seqs_.end()) {
            // New channel — first sequence
            // UE5: InReliable[ch] starts from a base, first outgoing = base + 1
            seqs_[ch] = 1;
            return 1;
        }
        it->second = (it->second + 1) & 0x3FF; // 10-bit wrap
        return it->second;
    }

    /// Initialize base sequence for all channels.
    void init_base(uint16_t base) {
        base_ = base;
        seqs_.clear();
    }

    /// Get next sequence initialized from base.
    uint16_t next_seq_from_base(uint32_t ch) {
        auto it = seqs_.find(ch);
        if (it == seqs_.end()) {
            uint16_t first = (base_ + 1) & 0x3FF;
            seqs_[ch] = first;
            return first;
        }
        it->second = (it->second + 1) & 0x3FF;
        return it->second;
    }

private:
    std::unordered_map<uint32_t, uint16_t> seqs_;
    uint16_t base_ = 0;
};

} // namespace ue5_repl

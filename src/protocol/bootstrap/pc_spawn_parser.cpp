// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/bootstrap/pc_spawn_parser.cpp
// ============================================================================
#include "protocol/bootstrap/pc_spawn_parser.h"
#include "protocol/wire/ue5_primitives.h"
#include <spdlog/spdlog.h>

namespace aoc { namespace protocol { namespace bootstrap {

namespace {

// ─── Bit reader wrapper ─────────────────────────────────────────────────

struct Reader {
    const uint8_t* data;
    size_t         data_size;    // bytes
    size_t         pos;          // bit position

    uint64_t read_bits(int n) {
        return ::ue5::read_bits(data, data_size, pos, n);
    }
    uint64_t read_sip() {
        return ::ue5::read_sip(data, data_size, pos);
    }
    uint32_t read_serialize_int(uint32_t max_val) {
        return ::ue5::read_serialize_int(data, data_size, pos, max_val);
    }
    bool read_bit() { return (read_bits(1) & 1) != 0; }
    uint8_t  read_u8()  { return static_cast<uint8_t>(read_bits(8)); }
    uint16_t read_u16() { return static_cast<uint16_t>(read_bits(16)); }
    uint32_t read_u32() { return static_cast<uint32_t>(read_bits(32)); }
    uint64_t read_u64() { return read_bits(64); }
    int32_t  read_i32() { return static_cast<int32_t>(read_u32()); }

    emit::FIntrepidNetworkGUID read_netguid() {
        emit::FIntrepidNetworkGUID g;
        const uint32_t lo = read_u32();
        const uint32_t hi = read_u32();
        g.ObjectId   = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
        g.ServerId   = read_u32();
        g.Randomizer = read_u32();
        return g;
    }

    std::string read_fstring_ansi() {
        const int32_t save_num = read_i32();
        if (save_num == 0) return "";
        if (save_num < 0 || save_num > 512) {
            spdlog::warn("[pc_spawn_parser] implausible FString save_num={} at bit {}",
                         save_num, pos - 32);
            return "";
        }
        std::string s;
        s.reserve(save_num);
        for (int32_t i = 0; i < save_num; ++i) {
            s.push_back(static_cast<char>(read_u8()));
        }
        // Strip trailing NUL for convenience
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
};
// [CoA x C_Y_B]

// ─── Parse one export entry (recursive) ──────────────────────────────
// Mirror of sub_1450360E0 / write_export_entry in PackageMapExporter.

std::unique_ptr<ParsedExport> parse_export_entry(Reader& r, int depth = 0) {
    if (depth > 16) {
        spdlog::warn("[pc_spawn_parser] export entry depth limit exceeded");
        return nullptr;
    }

    auto entry = std::make_unique<ParsedExport>();
    entry->guid = r.read_netguid();

    // If ObjectId == 0, this is a null terminator (no flags, no path)
    if (entry->guid.ObjectId == 0) {
        return entry;
    }

    const uint8_t flags = r.read_u8();
    entry->has_path     = (flags & 0x01) != 0;
    entry->no_load      = (flags & 0x02) != 0;
    entry->has_checksum = (flags & 0x04) != 0;

    if (!entry->has_path) {
        return entry;
    }

    // Recurse for outer
    entry->outer = parse_export_entry(r, depth + 1);

    // FString path
    entry->path = r.read_fstring_ansi();

    // Optional 32-bit checksum
    if (entry->has_checksum) {
        entry->checksum = r.read_u32();
    }

    return entry;
}

} // anonymous namespace


// ─── Parse one bunch header (control or data) and fill `out` ───────────
// Reader is advanced past the header (to bunch payload start).  Returns
// false if the header is malformed.

bool parse_one_bunch_header(Reader& r, ParsedBunchHeader& out) {
    // Canonical AoC S>C bunch header format.  Confirmed by:
    //   - docs/world-bootstrap-findings.md:175 — pkt#22 (orig_seq=14287)
    //     has ChSequence=1978, which requires 12 bits (>1023).
    //   - docs/serialize_new_actor_analysis.md:111 — "12-bit ChSequence
    //     (vs 10-bit stock)".
    //   - docs/aoc-wire-format-decoded.md — full 100%-decoded format.
    //   - src/net/sc_bunch_parser.h — the canonical parser validated
    //     against the full 29k-packet capture by replay_inspect.
    //
    // Format:
    //   bControl(1)
    //   if bControl:   bOpen(1) + bClose(1) + [CloseReason SerInt(7) 3 bits]
    //   bIsRepPaused(1) + bReliable(1)
    //   ChIndex (SerializeIntPacked)
    //   bExports(1) + bGuids(1) + bPartial(1)
    //   if bReliable:  ChSequence = 12 bits (ch>0) / 10 bits (ch=0 = NMT)
    //   if bPartial:   bPartialInitial(1) + bPartialCustomExportsFinal(1) +
    //                  bPartialFinal(1)
    //   if (bReliable || bOpen) && (!bPartial || bPartialInitial):
    //                  ChName = bHardcoded(1) + (SIP EName | FString + i32)
    //   BunchDataBits (13 bits via SerInt(8192))
    out.is_control = r.read_bit();

    if (out.is_control) {
        out.b_open  = r.read_bit();
        out.b_close = r.read_bit();
        if (out.b_close) {
            // CloseReason SerializeInt(max=7) = 3 bits
            r.read_serialize_int(7);
        }
    }
    const bool b_open = out.b_open;   // local alias used below for ChName check

    out.is_replication_paused = r.read_bit();
    out.is_reliable           = r.read_bit();
    out.channel               = static_cast<uint32_t>(r.read_sip());
    out.has_package_map_exports   = r.read_bit();
    out.has_must_be_mapped_guids  = r.read_bit();
    out.is_partial                = r.read_bit();

    if (out.is_reliable) {
        // ch=0 (NMT) uses 10 bits; all other channels use 12 bits.
        // pkt#22 ch=3 has ChSeq=1978 which proves 12-bit (>1023).
        const int chseq_bits = (out.channel == 0) ? 10 : 12;
        out.ch_sequence = static_cast<uint16_t>(r.read_bits(chseq_bits));
    }

    if (out.is_partial) {
        out.partial_initial              = r.read_bit();
        out.partial_custom_exports_final = r.read_bit();
        out.partial_final                = r.read_bit();
    }
// -- C-O-A x Cyb --

    // ChName is present only on the INITIAL fragment of a logical bunch,
    // or on non-partial reliable/open bunches.  Continuation fragments
    // inherit their ChName from the initial fragment.
    const bool chname_present =
        (out.is_reliable || b_open)
        && (!out.is_partial || out.partial_initial);
    if (chname_present) {
        const bool is_hardcoded = r.read_bit();
        out.ch_name_is_hardcoded = is_hardcoded;
        if (is_hardcoded) {
            out.ch_name_ename_idx = static_cast<uint32_t>(r.read_sip());
            out.channel_name = "EName[" + std::to_string(out.ch_name_ename_idx) + "]";
        } else {
            out.ch_name_string = r.read_fstring_ansi();
            out.ch_name_number = r.read_i32();
            out.channel_name   = out.ch_name_string;
        }
    }

    out.bunch_data_bits = r.read_serialize_int(8192);
    out.payload_start_bit = r.pos;
    return true;
}


// ─── Main entry ────────────────────────────────────────────────────────

std::optional<PcSpawnFields> parse_pc_spawn_bunch(const uint8_t* raw,
                                                     size_t raw_size_bytes,
                                                     size_t bunch_start_bit,
                                                     size_t bunch_bits) {
    if (!raw || raw_size_bytes == 0) return std::nullopt;
    if (bunch_start_bit + bunch_bits > raw_size_bytes * 8) {
        spdlog::warn("[pc_spawn_parser] bunch-data range out of bounds "
                     "(start={} bits={} raw_bits={})",
                     bunch_start_bit, bunch_bits, raw_size_bytes * 8);
        return std::nullopt;
    }

    PcSpawnFields fields;
    Reader r{raw, raw_size_bytes, bunch_start_bit};
    const size_t bunch_end_bit = bunch_start_bit + bunch_bits;
    int bunch_idx = 0;

    // ── Iterate through bunches until we find a DATA bunch with exports ──
    // (= the PC ActorOpen).  Skip past control bunches (channel-open etc.)
    while (r.pos < bunch_end_bit) {
        ParsedBunchHeader hdr;
        const size_t hdr_start = r.pos;
        if (!parse_one_bunch_header(r, hdr)) {
            spdlog::warn("[pc_spawn_parser] malformed bunch header at bit {}",
                         hdr_start);
            return std::nullopt;
        }
        spdlog::info("[pc_spawn_parser] bunch[{}] @bit {}: control={} reliable={} "
                     "channel={} ch_seq={} partial={} has_exports={} BDB={} "
                     "payload_start={}",
                     bunch_idx, hdr_start, hdr.is_control, hdr.is_reliable,
                     hdr.channel, hdr.ch_sequence, hdr.is_partial,
                     hdr.has_package_map_exports, hdr.bunch_data_bits,
                     hdr.payload_start_bit);

        // Heuristic: pick the bunch with has_package_map_exports=true.
        // This is the bunch that carries the inline NetGUID export
        // section, which precedes the SerializeNewActor payload in an
        // ActorOpen.  Works across both observed patterns:
        //
        //   pkt#22 (PC): bunch[0] has control=1 (bOpen=1), has_exports=1.
        //                Confirmed by docs/world-bootstrap-findings.md —
        //                ActorOpen IS a control bunch.  (Our earlier
        //                "!is_control" heuristic was wrong; it worked
        //                only for pkt#78 coincidentally.)
        //   pkt#78 (Pawn): bunch[0] is !control with has_exports=1.
        //
        // Bunches without exports (e.g. pkt#79's property-update bunches)
        // won't match, which is correct — they're not ActorOpens.
        if (hdr.has_package_map_exports) {
            fields.header = hdr;
            fields.bunch_start_bit_in_raw = hdr_start;
            fields.bunch_total_bits =
                (hdr.payload_start_bit - hdr_start) + hdr.bunch_data_bits;
            break;
        }
// -- CoA . C:Y:B --

        // Skip past this bunch's payload (BDB bits)
        r.pos = hdr.payload_start_bit + hdr.bunch_data_bits;
        ++bunch_idx;
    }

    if (fields.header.payload_start_bit == 0) {
        spdlog::warn("[pc_spawn_parser] no data bunch with exports found in "
                     "{} bunch-bits (scanned {} bunches)",
                     bunch_bits, bunch_idx);
        return std::nullopt;
    }

    // `r.pos` is now at the payload start of the PC ActorOpen bunch.
    // Parse the payload (export section + SerializeNewActor + tail).

    // ── Export section ────────────────────────────────────────────────
    // Two formats distinguished by the first bit of the payload:
    //   bHasRepLayoutExport=0 → UE5 stock NetGUID path export list
    //     (used by Pawn spawns, subobjects — see docs/aoc-wire-format-decoded.md)
    //   bHasRepLayoutExport=1 → AoC compact field-mask format
    //     (used by PlayerController spawn pkt#22 — see docs/world-
    //      bootstrap-findings.md §7 "Payload decode")
    if (fields.header.has_package_map_exports) {
        const bool b_rep_layout = r.read_bit();
        if (b_rep_layout) {
            // AoC compact field-mask format:
            //   [1 bit]   bHasRepLayoutExport = 1
            //   [u32]     NumExports (total replicated properties in class)
            //   [N bits]  FNetFieldExport bit-vector (1 = this field is
            //             carried in this bunch)
            //   [SerializeNewActor]
            //   [property values for set bits, per-property encoding]
            //
            // Fully decoding the bitmask + property values requires the
            // RepLayout cmd_index catalog (IDA RE, deferred).  For now
            // we treat the WHOLE payload (including bHasRepLayoutExport
            // bit + NumExports + mask + SNA + props) as opaque tail to
            // splice verbatim.  Sufficient for round-trip validation and
            // initial Phase II send-the-captured-bytes synthesis.
            fields.is_rep_layout_export = true;
            // NumExports field is still usable as a sanity check (we
            // read it here even though we splice the full payload below).
            // Note: advancing r here would desync with the splice range;
            // we intentionally DON'T advance past the flag — we want the
            // splice to cover the whole payload from payload_start_bit.
            // Instead, just peek NumExports without advancing (requires
            // a tiny peek helper; for now simpler: leave it and accept
            // that we don't expose NumExports to the caller).
            fields.rep_layout_num_exports = 0;  // unset; not needed for splice
            fields.tail_start_bit_in_bunch =
                fields.header.payload_start_bit - fields.bunch_start_bit_in_raw;
            fields.tail_bit_count = fields.header.bunch_data_bits;
            return fields;  // done — caller splices the full payload
        }
        const uint32_t num_guids = r.read_u32();
        if (num_guids > 100) {
            spdlog::warn("[pc_spawn_parser] implausible NumGUIDs={}", num_guids);
            return std::nullopt;
        }
        fields.exports.reserve(num_guids);
        for (uint32_t i = 0; i < num_guids; ++i) {
            auto entry = parse_export_entry(r, 0);
            if (!entry) return std::nullopt;
            fields.exports.push_back(std::move(*entry));
        }
    }

    // ── SerializeNewActor (3 × 128-bit GUIDs + transform flags) ──────
    fields.sna.actor_guid     = r.read_netguid();
    fields.sna.archetype_guid = r.read_netguid();
    fields.sna.level_guid     = r.read_netguid();

    // bSerializeLocation
    fields.sna.serialize_location = r.read_bit();
    if (fields.sna.serialize_location) {
        fields.sna.quantize_location = r.read_bit();
        if (fields.sna.quantize_location) {
            // UE::Net::ReadQuantizedVector: SerializeInt(scaled<<6 | bits, 128)
            // then three two's-complement components (bits 0 = raw floats,
            // which captured spawns never use).
            const uint32_t info = r.read_serialize_int(128);
            const uint32_t bits = info & 63;
            fields.sna.loc_max_bits = static_cast<int>(bits);
            if (bits > 0) {
                auto decode = [&]() -> int32_t {
                    const uint64_t raw_val = r.read_bits(static_cast<int>(bits));
                    const uint64_t sign = 1ULL << (bits - 1);
                    return static_cast<int32_t>(static_cast<int64_t>(raw_val ^ sign) -
                                                static_cast<int64_t>(sign));
                };
                fields.sna.loc_scaled_x = decode();
                fields.sna.loc_scaled_y = decode();
                fields.sna.loc_scaled_z = decode();
            }
        } else {
            // Non-quantized: 3 × double (192 bits)
            r.read_u64(); r.read_u64(); r.read_u64();
        }
    }

    // bSerializeRotation
    fields.sna.serialize_rotation = r.read_bit();
    if (fields.sna.serialize_rotation) {
        // Per-axis: 1-bit flag + optional int16
        for (int axis = 0; axis < 3; ++axis) {
            if (r.read_bit()) r.read_u16();
        }
    }
// C.O.A and Cyb

    // bSerializeScale
    fields.sna.serialize_scale = r.read_bit();
    if (fields.sna.serialize_scale) {
        // bQuantize, then the same quantized vector as location — skipped.
        if (r.read_bit()) {
            const uint32_t bits = r.read_serialize_int(128) & 63;
            for (int i = 0; i < 3; ++i) r.read_bits(static_cast<int>(bits));
        } else {
            r.read_u64(); r.read_u64(); r.read_u64();
        }
    }

    // bSerializeVelocity
    fields.sna.serialize_velocity = r.read_bit();
    if (fields.sna.serialize_velocity) {
        if (r.read_bit()) {
            const uint32_t bits = r.read_serialize_int(128) & 63;
            for (int i = 0; i < 3; ++i) r.read_bits(static_cast<int>(bits));
        } else {
            r.read_u64(); r.read_u64(); r.read_u64();
        }
    }

    // APlayerController::OnSerializeNewActor: NetPlayerIndex(8) +
    // ClientHandshakeId(32).  ActorBuilder writes these itself, so they are
    // not part of the spliced tail.
    r.read_bits(8);
    r.read_bits(32);

    // end_bit_in_bunch is relative to the target bunch's start (not
    // pkt.bunch_start_bit which is the start of ALL bunches).
    fields.sna.end_bit_in_bunch = r.pos - fields.bunch_start_bit_in_raw;

    // ── RepLayout tail ───────────────────────────────────────────────
    // The tail spans from the end of SerializeNewActor to the end of
    // THIS bunch (not the end of all bunches in the packet).
    fields.tail_start_bit_in_bunch = fields.sna.end_bit_in_bunch;
    const size_t this_bunch_end =
        fields.bunch_start_bit_in_raw + fields.bunch_total_bits;
    if (this_bunch_end >= r.pos) {
        fields.tail_bit_count = this_bunch_end - r.pos;
    } else {
        spdlog::warn("[pc_spawn_parser] SerializeNewActor overran bunch boundary "
                     "(pos={} bunch_end={})", r.pos, this_bunch_end);
        fields.tail_bit_count = 0;
    }

    (void)bunch_end_bit;  // silence unused warning
    return fields;
}

}}} // namespace aoc::protocol::bootstrap

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/bootstrap/bootstrap_sequence.cpp
// ============================================================================
#include "protocol/bootstrap/bootstrap_sequence.h"
#include "protocol/bootstrap/bootstrap_data.h"
#include "protocol/bootstrap/pc_spawn_parser.h"
#include "protocol/bunch_builder.h"
#include "protocol/character_profile.h"
#include "protocol/actors/player_controller.h"
#include "protocol/emit/actor_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/package_map_exporter.h"
#include "protocol/schema/schema_registry.h"
#include "net/game_server.h"  // for ReplayData / ReplayPacketInfo

#include <spdlog/spdlog.h>
#include <cstring>
#include <memory>

namespace aoc { namespace protocol {

bool BootstrapSequence::fill(ReplayData& out) {
    // ── Session metadata ──────────────────────────────────────────────────
    out.packet_count = static_cast<uint32_t>(bootstrap_data::kPacketCount);
    out.session_id   = bootstrap_data::kSessionId;
    out.client_id    = bootstrap_data::kClientId;
    out.initial_seq  = bootstrap_data::kInitialSeq;
    out.initial_ack  = bootstrap_data::kInitialAck;
    std::memcpy(out.server_custom_field, bootstrap_data::kServerCustomField, 6);
    std::memcpy(out.client_custom_field, bootstrap_data::kClientCustomField, 6);

    // ── Per-packet content ────────────────────────────────────────────────
    out.packets.clear();
    out.packets.resize(bootstrap_data::kPacketCount);
    for (std::size_t i = 0; i < bootstrap_data::kPacketCount; ++i) {
        const auto& src = bootstrap_data::kPackets[i];
        auto& dst = out.packets[i];
        dst.timestamp_ms    = src.timestamp_ms;
        dst.original_seq    = src.original_seq;
        dst.original_ack    = src.original_ack;
        dst.bunch_start_bit = src.bunch_start_bit;
        dst.bunch_bits      = src.bunch_bits;
        dst.has_pkt_info    = src.has_pkt_info;
        dst.has_srv_frame   = src.has_srv_frame;
        dst.frame_time      = src.frame_time;
        dst.jitter          = src.jitter;
        dst.hist_count      = src.hist_count;
        dst.raw.assign(src.raw, src.raw + src.raw_size);
    }

    spdlog::info("[BootstrapSequence] Embedded bootstrap: {} packets "
                 "(initSeq={} custom={:02x}{:02x}{:02x}{:02x}{:02x}{:02x})",
                 out.packets.size(), out.initial_seq,
                 out.server_custom_field[0], out.server_custom_field[1],
                 out.server_custom_field[2], out.server_custom_field[3],
                 out.server_custom_field[4], out.server_custom_field[5]);
    return !out.packets.empty();
}

// ─── Phase 3.8 — synthesize specific packets from CharacterProfile ─────

namespace {
// -- C.O.A . Cyb --

// Metadata about the PlayerController packet — constants from Phase 3.3
// decode and Phase 3.7 validation.
constexpr std::size_t kPlayerControllerPacketIndex = 22u;
constexpr std::size_t kPayloadStartBitInRaw        = 206u; // bunch_start_bit + header bits
constexpr std::size_t kPayloadBits                 = 3302u;

/// Overwrite bits [start_bit .. start_bit + n_bits) in `raw` with the
/// low `n_bits` bits of `src_bits` starting at its bit 0.  Caller ensures
/// raw is large enough (it is — we're replacing an existing region).
void splice_bits_into(std::vector<uint8_t>& raw,
                      std::size_t start_bit,
                      const std::vector<uint8_t>& src_bits,
                      std::size_t n_bits) {
    for (std::size_t i = 0; i < n_bits; ++i) {
        const std::size_t sb = i >> 3;
        const int src_bit = (src_bits[sb] >> (i & 7)) & 1;
        const std::size_t dbit = start_bit + i;
        const std::size_t db = dbit >> 3;
        const uint8_t mask = static_cast<uint8_t>(1u << (dbit & 7));
        if (src_bit) {
            raw[db] |= mask;
        } else {
            raw[db] &= static_cast<uint8_t>(~mask);
        }
    }
}

} // anonymous namespace

bool BootstrapSequence::apply_synthesis(ReplayData& data,
                                        const CharacterProfile& profile) {
    if (data.packets.size() <= kPlayerControllerPacketIndex) {
        spdlog::warn("[BootstrapSequence::apply_synthesis] packet index {} "
                     "out of range ({} packets loaded)",
                     kPlayerControllerPacketIndex, data.packets.size());
        return false;
    }

    auto& pkt = data.packets[kPlayerControllerPacketIndex];

    // Sanity check: raw packet must be large enough to contain the
    // PlayerController bunch payload we're replacing.
    const std::size_t end_bit = kPayloadStartBitInRaw + kPayloadBits;
    if (pkt.raw.size() * 8 < end_bit) {
        spdlog::warn("[BootstrapSequence::apply_synthesis] packet too small "
                     "to splice PlayerController payload ({}B raw)",
                     pkt.raw.size());
        return false;
    }

    // Build the synthesized payload.
    BunchBuffer buf;
    if (!actors::player_controller::build(buf, profile)) {
        spdlog::error("[BootstrapSequence::apply_synthesis] "
                      "player_controller::build() failed");
        return false;
    }
    if (buf.bit_count() != kPayloadBits) {
        spdlog::warn("[BootstrapSequence::apply_synthesis] builder produced "
                     "{} bits, expected {} — refusing to splice to avoid "
                     "bunch-size mismatch",
                     buf.bit_count(), kPayloadBits);
        return false;
    }

    // Splice the builder's bits into the packet's raw, replacing the
    // captured PlayerController payload in place.
    splice_bits_into(pkt.raw, kPayloadStartBitInRaw, buf.bytes(),
                     kPayloadBits);

    spdlog::info("[BootstrapSequence::apply_synthesis] "
                 "PlayerController packet #{} synthesized from profile "
                 "(name=\"{}\") — {} bits spliced into packet raw",
                 kPlayerControllerPacketIndex, profile.name, kPayloadBits);
    return true;
}

// NOTE: apply_live_pc_spawn / apply_live_pawn_spawn were REMOVED.
//
// They tried to re-emit a single fragment of a multi-packet logical
// bunch, which is conceptually impossible — a partial-bunch fragment
// has no standalone meaning.  See world-bootstrap-findings.md for the
// actual pkt#22 structure: it's a continuation fragment (pi=0, pce=1,
// pf=0) of a 4864-bit logical bunch split across multiple packets.
//
// Phase II will build the full 4864-bit logical bunch from scratch via
// ActorBuilder with runtime parameters (name, class, location, actor
// GUID) and let the wire layer fragment it.  That belongs in
// GameServer::spawn_player_controller_for_client, not here.
//
// The pc_spawn_parser.cpp module (sibling file) is kept because it's
// the canonical parser used for incoming bunches in Phase III.


// ───────────────────────────────────────────────────────────────────────
//  Phase II Stage 2.0 — DRY-RUN synthesis round-trip test
//
//  Feeds captured pkt#22 bunch[0] through our parse → ActorBuilder →
//  compare pipeline.  NO writes, NO state mutation.  Sole purpose:
//  confirm that our emitter produces byte-identical output to real AoC
//  wire format when given the captured field values.
//
//  If this test passes bit-identical, we know the framing side of live
//  synthesis is correct and can safely move to Phase II actual-send.
//  If it fails, the diff bit positions pinpoint format bugs before we
//  risk corrupting live traffic.
// ───────────────────────────────────────────────────────────────────────

namespace {

// Deep-copy a ParsedExport tree into an emit::ExportEntry tree.
aoc::protocol::emit::ExportEntry export_from_parsed(
    const aoc::protocol::bootstrap::ParsedExport& parsed)
{
    namespace emit = aoc::protocol::emit;
    emit::ExportEntry e;
    e.guid         = parsed.guid;
    e.has_path     = parsed.has_path;
    e.no_load      = parsed.no_load;
    e.has_checksum = parsed.has_checksum;
    e.path         = parsed.path;
    e.checksum     = parsed.checksum;
    if (parsed.outer) {
        e.outer = std::make_unique<emit::ExportEntry>(
            export_from_parsed(*parsed.outer));
    }
    return e;
}

} // anonymous namespace

// [CoA ~ C-Y-B]

bool BootstrapSequence::test_pc_spawn_round_trip(const ReplayData& data) {
    namespace emit   = aoc::protocol::emit;
    namespace schema = aoc::protocol::schema;
    using aoc::protocol::bootstrap::parse_pc_spawn_bunch;

    constexpr std::size_t kPcPacketIndex = 22u;
    constexpr const char* kTag = "RoundTripTest";

    // ── 1. Sanity check packet ─────────────────────────────────────────
    if (data.packets.size() <= kPcPacketIndex) {
        spdlog::warn("[{}] pkt#{} out of range ({} loaded)",
                     kTag, kPcPacketIndex, data.packets.size());
        return false;
    }
    const auto& pkt = data.packets[kPcPacketIndex];

    spdlog::info("[{}] pkt#{}: raw_size={}B bunch_start_bit={} bunch_bits={}",
                 kTag, kPcPacketIndex, pkt.raw.size(),
                 pkt.bunch_start_bit, pkt.bunch_bits);

    // ── 2. Parse to extract captured field values ─────────────────────
    auto parsed_opt = parse_pc_spawn_bunch(
        pkt.raw.data(), pkt.raw.size(),
        pkt.bunch_start_bit, pkt.bunch_bits);
    if (!parsed_opt) {
        spdlog::warn("[{}] failed to parse pkt#{} — check parser against "
                     "docs/world-bootstrap-findings.md expected values",
                     kTag, kPcPacketIndex);
        return false;
    }
    const auto& parsed = *parsed_opt;
    spdlog::info("[{}] parsed pkt#{} bunch[0]: channel={} ch_seq={} "
                 "ch_name=\"{}\" reliable={} partial={} has_exports={} "
                 "BDB={} #exports={} actor_guid.obj={} location=({},{},{}) "
                 "tail_bits={}",
                 kTag, kPcPacketIndex,
                 parsed.header.channel, parsed.header.ch_sequence,
                 parsed.header.channel_name,
                 parsed.header.is_reliable, parsed.header.is_partial,
                 parsed.header.has_package_map_exports,
                 parsed.header.bunch_data_bits,
                 parsed.exports.size(),
                 parsed.sna.actor_guid.ObjectId,
                 parsed.sna.loc_scaled_x, parsed.sna.loc_scaled_y,
                 parsed.sna.loc_scaled_z,
                 parsed.tail_bit_count);

    // Expected from docs/world-bootstrap-findings.md:
    //   channel=3, ch_seq=1978, BDB=3302, payload_start=206
    if (parsed.header.channel != 3
        || parsed.header.ch_sequence != 1978
        || parsed.header.bunch_data_bits != 3302u)
    {
        spdlog::warn("[{}] parsed values disagree with docs! "
                     "Expected channel=3 ch_seq=1978 BDB=3302; "
                     "got channel={} ch_seq={} BDB={}. "
                     "Parser may still have format bugs.",
                     kTag, parsed.header.channel, parsed.header.ch_sequence,
                     parsed.header.bunch_data_bits);
        // Don't return — continue to show the full diff anyway.
    } else {
        spdlog::info("[{}] parser output matches docs/world-bootstrap-"
                     "findings.md exactly. ✓", kTag);
    }

    // ── 3. Copy the RepLayout property stream tail verbatim ────────────
    std::vector<uint8_t> tail_buf((parsed.tail_bit_count + 7) / 8, 0);
    const std::size_t tail_src_base =
        parsed.bunch_start_bit_in_raw + parsed.tail_start_bit_in_bunch;
    for (std::size_t i = 0; i < parsed.tail_bit_count; ++i) {
        const std::size_t sb = tail_src_base + i;
        const int bit = (pkt.raw[sb >> 3] >> (sb & 7)) & 1;
        if (bit) tail_buf[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    }

    // ── 4. Build via ActorBuilder with parsed runtime values ──────────
    auto& schemas = schema::SchemaRegistry::instance();
    schemas.load_all();
    const auto* pc_schema =
        schemas.get_schema(schema::ActorType::PlayerController);
    if (!pc_schema) {
        spdlog::error("[{}] PlayerController schema not loaded", kTag);
        return false;
    }

    emit::ActorRuntime rt;
    rt.type                   = schema::ActorType::PlayerController;
    rt.actor_netguid          = parsed.sna.actor_guid.ObjectId;
    rt.actor_server_id        = parsed.sna.actor_guid.ServerId;
    rt.actor_randomizer       = parsed.sna.actor_guid.Randomizer;
    rt.archetype_netguid      = parsed.sna.archetype_guid.ObjectId;
    rt.archetype_server_id    = parsed.sna.archetype_guid.ServerId;
    rt.archetype_randomizer   = parsed.sna.archetype_guid.Randomizer;
    rt.level_netguid          = parsed.sna.level_guid.ObjectId;
    rt.level_server_id        = parsed.sna.level_guid.ServerId;
    rt.level_randomizer       = parsed.sna.level_guid.Randomizer;
    rt.serialize_location     = parsed.sna.serialize_location;
    rt.quantize_location      = parsed.sna.quantize_location;
    rt.location_scaled_x      = parsed.sna.loc_scaled_x;
    rt.location_scaled_y      = parsed.sna.loc_scaled_y;
    rt.location_scaled_z      = parsed.sna.loc_scaled_z;
    rt.location_max_bits      = parsed.sna.loc_max_bits;
    rt.serialize_rotation     = parsed.sna.serialize_rotation;
    rt.serialize_scale        = parsed.sna.serialize_scale;
    rt.serialize_velocity     = parsed.sna.serialize_velocity;
// [CoA :: C-Y-B]

    std::vector<emit::ExportEntry> exports;
    exports.reserve(parsed.exports.size());
    for (const auto& pe : parsed.exports) {
        exports.push_back(export_from_parsed(pe));
    }

    emit::BunchWriter bw;
    emit::EmitContext ctx;
    ctx.channel                = parsed.header.channel;
    ctx.ch_sequence            = parsed.header.ch_sequence;
    ctx.is_reliable            = parsed.header.is_reliable;
    ctx.is_partial             = parsed.header.is_partial;
    ctx.partial_initial        = parsed.header.partial_initial;
    ctx.partial_final          = parsed.header.partial_final;
    ctx.partial_custom_exports_final =
        parsed.header.partial_custom_exports_final;

    // Control-bunch flags (pkt#22 is a bOpen=1 channel-open control bunch
    // per docs/world-bootstrap-findings.md §7).
    ctx.is_control             = parsed.header.is_control;
    ctx.b_open                 = parsed.header.b_open;
    ctx.b_close                = parsed.header.b_close;

    // Explicit header bits preserved from capture.
    ctx.explicit_has_pme       = true;
    ctx.has_pme_value          = parsed.header.has_package_map_exports;
    ctx.has_mbg_value          = parsed.header.has_must_be_mapped_guids;

    ctx.ch_name_is_hardcoded   = parsed.header.ch_name_is_hardcoded;
    ctx.ch_name_ename_idx      = parsed.header.ch_name_ename_idx;
    ctx.ch_name_string         = parsed.header.ch_name_string;
    ctx.ch_name_number         = parsed.header.ch_name_number;

    // Full-payload splice vs. per-field emission path.
    if (parsed.is_rep_layout_export) {
        // AoC compact format — we don't yet know per-property cmd_index,
        // so splice the whole payload verbatim.  Header is still emitted
        // from parsed fields, so this exercises the header writer.
        ctx.splice_full_payload    = true;
        ctx.spliced_tail_bits      = tail_buf.data();
        ctx.spliced_tail_bit_count = parsed.tail_bit_count;
    } else {
        // Stock NetGUID export format — rebuild exports + SNA, splice
        // only the RepLayout tail.
        ctx.package_map_exports    = std::move(exports);
        ctx.spliced_tail_bits      = tail_buf.data();
        ctx.spliced_tail_bit_count = parsed.tail_bit_count;
    }

    emit::ActorBuilder builder;
    const std::size_t produced_bits =
        builder.build_spawn(*pc_schema, rt, ctx, bw);

    if (produced_bits != parsed.bunch_total_bits) {
        spdlog::warn("[{}] SIZE MISMATCH: builder produced {} bits, "
                     "parsed bunch = {} bits. Format bug in ActorBuilder "
                     "or parser disagreement on header size.",
                     kTag, produced_bits, parsed.bunch_total_bits);
        return false;
    }

    // ── 5. Compare bit-for-bit against captured ────────────────────────
    std::size_t diff_bits = 0;
    std::vector<std::size_t> first_diffs;
    const uint8_t* src = bw.data();
    const std::size_t bunch_start = parsed.bunch_start_bit_in_raw;
    for (std::size_t i = 0; i < produced_bits; ++i) {
        const int src_bit = (src[i >> 3] >> (i & 7)) & 1;
        const std::size_t dbit = bunch_start + i;
        const int dst_bit = (pkt.raw[dbit >> 3] >> (dbit & 7)) & 1;
        if (src_bit != dst_bit) {
            ++diff_bits;
            if (first_diffs.size() < 16) first_diffs.push_back(i);
        }
    }

    if (diff_bits > 0) {
        std::string diff_list;
        for (auto b : first_diffs) {
            if (!diff_list.empty()) diff_list += ",";
            diff_list += std::to_string(b);
        }
        spdlog::warn("[{}] DIVERGES: {} bits differ over {} compared "
                     "(first diffs at bunch-relative bits: [{}]). "
                     "Synthesis is NOT yet byte-identical to captured — "
                     "actual sending would likely corrupt the stream.",
                     kTag, diff_bits, produced_bits, diff_list);
        return false;
    }

    spdlog::info("[{}] ✓ VALIDATED: synthesized {} bits are BYTE-IDENTICAL "
                 "to captured pkt#{} bunch[0]. Framing pipeline "
                 "(parser + ActorBuilder + PackageMapExporter) is correct "
                 "end-to-end. Ready to proceed with Phase II actual-send.",
                 kTag, produced_bits, kPcPacketIndex);
    return true;
}


// ───────────────────────────────────────────────────────────────────────
//  Phase II Stage 2.1 — SPLICE synthesized PC spawn into replay stream
//
//  Wires our ActorBuilder pipeline's output into the actual replay
//  packet bytes the client will receive.  Safety-gated: only writes if
//  the synthesized bytes are bit-identical to captured (preventing
//  corruption while we're still pre-divergence).
//
//  Infrastructure-only effect while byte-identical (client sees the
//  same bytes).  BUT — this is the integration point for future
//  divergence: when RE gives us cmd_index for CharacterArchetype /
//  CharacterName, we plug those into `rt` (ActorRuntime) fields before
//  `build_spawn`, remove the safety gate, and the same splice delivers
//  diverged bytes.  No additional send-path work.
// ───────────────────────────────────────────────────────────────────────

bool BootstrapSequence::splice_pc_spawn_synthesis(ReplayData& data) {
    namespace emit   = aoc::protocol::emit;
    namespace schema = aoc::protocol::schema;
    using aoc::protocol::bootstrap::parse_pc_spawn_bunch;

    constexpr std::size_t kPcPacketIndex = 22u;
    constexpr const char* kTag = "PcSpawnSplice";

    if (data.packets.size() <= kPcPacketIndex) {
        spdlog::warn("[{}] pkt#{} out of range", kTag, kPcPacketIndex);
        return false;
    }
    auto& pkt = data.packets[kPcPacketIndex];
// [C O A - C:Y:B]

    auto parsed_opt = parse_pc_spawn_bunch(
        pkt.raw.data(), pkt.raw.size(),
        pkt.bunch_start_bit, pkt.bunch_bits);
    if (!parsed_opt) {
        spdlog::warn("[{}] parse failed, skipping splice", kTag);
        return false;
    }
    const auto& parsed = *parsed_opt;

    // Copy captured tail for splice (same as round-trip test).
    std::vector<uint8_t> tail_buf((parsed.tail_bit_count + 7) / 8, 0);
    const std::size_t tail_src_base =
        parsed.bunch_start_bit_in_raw + parsed.tail_start_bit_in_bunch;
    for (std::size_t i = 0; i < parsed.tail_bit_count; ++i) {
        const std::size_t sb = tail_src_base + i;
        const int bit = (pkt.raw[sb >> 3] >> (sb & 7)) & 1;
        if (bit) tail_buf[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    }

    auto& schemas = schema::SchemaRegistry::instance();
    schemas.load_all();
    const auto* pc_schema =
        schemas.get_schema(schema::ActorType::PlayerController);
    if (!pc_schema) {
        spdlog::error("[{}] PC schema not loaded", kTag);
        return false;
    }

    emit::ActorRuntime rt;
    rt.type                   = schema::ActorType::PlayerController;
    rt.actor_netguid          = parsed.sna.actor_guid.ObjectId;
    rt.actor_server_id        = parsed.sna.actor_guid.ServerId;
    rt.actor_randomizer       = parsed.sna.actor_guid.Randomizer;
    rt.archetype_netguid      = parsed.sna.archetype_guid.ObjectId;
    rt.archetype_server_id    = parsed.sna.archetype_guid.ServerId;
    rt.archetype_randomizer   = parsed.sna.archetype_guid.Randomizer;
    rt.level_netguid          = parsed.sna.level_guid.ObjectId;
    rt.level_server_id        = parsed.sna.level_guid.ServerId;
    rt.level_randomizer       = parsed.sna.level_guid.Randomizer;
    rt.serialize_location     = parsed.sna.serialize_location;
    rt.quantize_location      = parsed.sna.quantize_location;
    rt.location_scaled_x      = parsed.sna.loc_scaled_x;
    rt.location_scaled_y      = parsed.sna.loc_scaled_y;
    rt.location_scaled_z      = parsed.sna.loc_scaled_z;
    rt.location_max_bits      = parsed.sna.loc_max_bits;
    rt.serialize_rotation     = parsed.sna.serialize_rotation;
    rt.serialize_scale        = parsed.sna.serialize_scale;
    rt.serialize_velocity     = parsed.sna.serialize_velocity;

    std::vector<emit::ExportEntry> exports;
    exports.reserve(parsed.exports.size());
    for (const auto& pe : parsed.exports) {
        exports.push_back(export_from_parsed(pe));
    }

    emit::BunchWriter bw;
    emit::EmitContext ctx;
    ctx.channel                      = parsed.header.channel;
    ctx.ch_sequence                  = parsed.header.ch_sequence;
    ctx.is_reliable                  = parsed.header.is_reliable;
    ctx.is_partial                   = parsed.header.is_partial;
    ctx.partial_initial              = parsed.header.partial_initial;
    ctx.partial_final                = parsed.header.partial_final;
    ctx.partial_custom_exports_final = parsed.header.partial_custom_exports_final;
    ctx.is_control                   = parsed.header.is_control;
    ctx.b_open                       = parsed.header.b_open;
    ctx.b_close                      = parsed.header.b_close;
    ctx.explicit_has_pme             = true;
    ctx.has_pme_value                = parsed.header.has_package_map_exports;
    ctx.has_mbg_value                = parsed.header.has_must_be_mapped_guids;
    ctx.ch_name_is_hardcoded         = parsed.header.ch_name_is_hardcoded;
    ctx.ch_name_ename_idx            = parsed.header.ch_name_ename_idx;
    ctx.ch_name_string               = parsed.header.ch_name_string;
    ctx.ch_name_number               = parsed.header.ch_name_number;
// -- C Y B . CoA --

    if (parsed.is_rep_layout_export) {
        ctx.splice_full_payload    = true;
        ctx.spliced_tail_bits      = tail_buf.data();
        ctx.spliced_tail_bit_count = parsed.tail_bit_count;
    } else {
        ctx.package_map_exports    = std::move(exports);
        ctx.spliced_tail_bits      = tail_buf.data();
        ctx.spliced_tail_bit_count = parsed.tail_bit_count;
    }

    emit::ActorBuilder builder;
    const std::size_t produced_bits =
        builder.build_spawn(*pc_schema, rt, ctx, bw);

    if (produced_bits != parsed.bunch_total_bits) {
        spdlog::warn("[{}] size mismatch {} vs {} — aborting splice",
                     kTag, produced_bits, parsed.bunch_total_bits);
        return false;
    }

    // ── Safety gate: bytes MUST match captured before we write ─────────
    std::size_t diff_bits = 0;
    const uint8_t* src = bw.data();
    const std::size_t bunch_start = parsed.bunch_start_bit_in_raw;
    for (std::size_t i = 0; i < produced_bits; ++i) {
        const int src_bit = (src[i >> 3] >> (i & 7)) & 1;
        const std::size_t dbit = bunch_start + i;
        const int dst_bit = (pkt.raw[dbit >> 3] >> (dbit & 7)) & 1;
        if (src_bit != dst_bit) { ++diff_bits; }
    }
    if (diff_bits > 0) {
        spdlog::warn("[{}] SAFETY GATE tripped: {} bits diverge from "
                     "captured — NOT writing (would corrupt stream). "
                     "Run test_pc_spawn_round_trip for diff details.",
                     kTag, diff_bits);
        return false;
    }

    // ── Write synthesized bits into pkt[22].raw at bunch_start ────────
    // Byte-identical so this is a functional no-op, but exercises the
    // write path so when we diverge later it's already wired.
    for (std::size_t i = 0; i < produced_bits; ++i) {
        const int src_bit = (src[i >> 3] >> (i & 7)) & 1;
        const std::size_t dbit = bunch_start + i;
        const uint8_t mask = static_cast<uint8_t>(1u << (dbit & 7));
        if (src_bit) {
            pkt.raw[dbit >> 3] |= mask;
        } else {
            pkt.raw[dbit >> 3] &= static_cast<uint8_t>(~mask);
        }
    }

    spdlog::info("[{}] Spliced {} synthesized bits into pkt#{}.raw "
                 "starting at bit {}.  (bit-identical to captured — "
                 "no functional change; send-path infrastructure in "
                 "place for future divergence.)",
                 kTag, produced_bits, kPcPacketIndex, bunch_start);
    return true;
}

}} // namespace aoc::protocol

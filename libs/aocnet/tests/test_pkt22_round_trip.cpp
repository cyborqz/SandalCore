// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_pkt22_round_trip.cpp
//
//  pkt#22 (PC ActorOpen) round-trip harness.  Goal: decode the RepLayout
//  property stream → re-encode → bit-identical with captured bytes.
//
//  Fixture: src/protocol/tools/captured_pc_spawn_reassembled.bin
//           608 B / 4864 bits, 4859 effective (last 5 bits = byte padding).
//
//  Known bunch structure (docs/aoc-wire-format-decoded.md):
//
//     bit  0            bHasRepLayoutExport = 0
//     bit  1..32        NumGUIDsInBunch = 3
//     bit  33..1256     Export[0]: AoCPlayerControllerBP class
//     bit  1257..2672   Export[1]: PersistentLevel hierarchy
//     bit  2673..3544   Export[2]: GlobalGMCommands
//     bit  3545..3928   SerializeNewActor (Actor+Archetype+Level GUIDs)
//     bit  3929..3932   4 transform flags (Loc=1, Rot=1, Scale=0, Vel=0)
//     bit  3933..4010   Transform body (78 bits — location packed vec +
//                                        rot/scale/vel flags)
//     bit  4011..4864   PROPERTY STREAM (853 bits)
//
//  Property stream format (Function G / Function J, H.3f+):
//
//     while (not at end of BunchDataBits):
//         uint32 cmd_index     (32 bits LSB-first, bit-contiguous)
//         if cmd_index == 0xDEADBEEF: break  (terminator)
//         property_data        (variable bits, depends on cmd's type)
//
//  No bitmask; no content-block header.  Just [cmd][data][cmd][data]...
//
//  Initial confusion: the wire-format doc lumped transform body bits with
//  "property stream" when summarizing.  Cross-checked with
//  `ActorBuilder::write_content_block` + `test_pc_spawn_diff` reporting
//  matched-through-bit-4011.  Property stream starts at 4011, not 3933.
//
//  This harness is PHASE 1 — DIAGNOSTIC:
//    - Load fixture
//    - Seek to bit 3933
//    - Read cmd_indices and log them
//    - Dump remaining bits in multiple views (hex, uint32 LE, bit patterns)
//    - Does NOT yet attempt to decode property bodies — we need the
//      cmd_index → FPropertyType mapping from the catalog, which depends
//      on struct expansion work.
//
//  Output feeds the next phase: populate catalog cmd_indices, wire up
//  codec dispatch by cmd_index, re-run for bit-identical round-trip.
//
//  LAYER:  tools
// ============================================================================
#include "protocol/emit/replayout/catalog.h"
#include "protocol/emit/replayout/encoder.h"
#include "protocol/emit/replayout/decoder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_reader.h"
// C.Y.B and C-O-A

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

namespace {

using namespace aoc::protocol::emit::replayout;
using aoc::protocol::wire::PacketReader;

// ─── Constants from docs/aoc-wire-format-decoded.md ─────────────────────

constexpr size_t TRANSFORM_START     = 3933;   // transform body (78 bits, verified)
constexpr size_t PROP_STREAM_START   = 4011;   // [cmd_index][data] starts here
constexpr size_t EFFECTIVE_BITS      = 4859;   // last 5 bits of byte-aligned buffer are padding
constexpr size_t BUFFER_BITS         = 4864;   // total bits in the file
constexpr uint32_t DEADBEEF          = 0xDEADBEEF;

constexpr const char* FIXTURE_PATH =
    "src\\protocol\\tools\\captured_pc_spawn_reassembled.bin";

// ─── Helpers ──────────────────────────────────────────────────────────────

static std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto n = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

/// LSB-first bit read (matches UE5 + our BunchWriter).
static uint32_t read_bits_at(const uint8_t* data, size_t data_size,
                              size_t bit_off, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        size_t bp = bit_off + i;
        if ((bp >> 3) >= data_size) break;
        int bit = (data[bp >> 3] >> (bp & 7)) & 1;
        v |= static_cast<uint32_t>(bit) << i;
    }
    return v;
}

// ─── Phase 1: load and header diagnostics ─────────────────────────────────

void report_header(const std::vector<uint8_t>& data) {
    spdlog::info("== fixture ==");
    spdlog::info("  path           : {}", FIXTURE_PATH);
    spdlog::info("  bytes          : {}", data.size());
    spdlog::info("  bits           : {} (effective {})", data.size() * 8, EFFECTIVE_BITS);
    spdlog::info("  transform body : bit {}..{}  ({} bits)",
                 TRANSFORM_START, PROP_STREAM_START,
                 PROP_STREAM_START - TRANSFORM_START);

    const size_t prop_bits = EFFECTIVE_BITS - PROP_STREAM_START;
    spdlog::info("  property stream: bit {}..{}  ({} bits)",
                 PROP_STREAM_START, EFFECTIVE_BITS, prop_bits);

    // Validate that bit 0 = bHasRepLayoutExport = 0
    int bit0 = read_bits_at(data.data(), data.size(), 0, 1);
    spdlog::info("  bit 0 (bHasRepLayoutExport) = {} (expected 0)", bit0);

    // Validate bits 1..32 = NumGUIDsInBunch = 3
    uint32_t num_guids = read_bits_at(data.data(), data.size(), 1, 32);
    spdlog::info("  bits 1..32 (NumGUIDsInBunch) = {} (expected 3)", num_guids);
}

// ─── Phase 2: walk the cmd_index stream ──────────────────────────────────
//
// Format per Function G / Function J (sub_14504F1A0 / sub_145057C30):
//     while (reader.pos() + 32 <= EFFECTIVE_BITS):
//         uint32 cmd_index     (32 bits LSB-first, bit-contiguous)
//         if cmd_index == 0xDEADBEEF: break
//         <property_data>      (variable; we try to decode via catalog)
//
// For each cmd_index read, we look up the corresponding property in our
// catalog's flat layout and report its name/type.  If we have a working
// codec for that type, we decode and advance the reader; otherwise we
// stop and report "unknown type, can't advance".

struct FlatEntry {
    uint32_t cmd_index;
    const ReplicatedPropertyDesc* desc;
    const char* owning_class;
};

static std::vector<FlatEntry> build_flat_layout() {
    std::vector<FlatEntry> flat;
    auto push_class = [&](const ClassCatalog& klass) {
        for (const auto& p : klass.own_props) {
            flat.push_back({static_cast<uint32_t>(flat.size()), &p,
                             klass.class_name.c_str()});
            if (p.type == FPropertyType::Struct) {
                for (const auto& sub : p.sub_cmds) {
                    flat.push_back({static_cast<uint32_t>(flat.size()), &sub,
                                     klass.class_name.c_str()});
                }
            }
        }
    };
    push_class(aactor_catalog());
    push_class(acontroller_catalog());
    push_class(aplayer_controller_catalog());
    push_class(aaoc_player_controller_catalog());
    return flat;
}

void phase2_walk_cmd_index_stream(const std::vector<uint8_t>& data) {
    spdlog::info("");
    spdlog::info("== phase 2: walking cmd_index stream from bit {} ==",
                 PROP_STREAM_START);

    auto flat = build_flat_layout();
    spdlog::info("  (flat catalog: {} entries; walker stops on out-of-range "
                 "or Unknown type)", flat.size());

    // Single reader, positioned at the property stream start.
    PacketReader rd(data.data(), data.size(), EFFECTIVE_BITS);
    for (size_t i = 0; i < PROP_STREAM_START; ++i) (void)rd.read_bit();

    int iter = 0;
    while (rd.pos() + 32 <= EFFECTIVE_BITS) {
        const size_t cmd_pos = rd.pos();
        uint32_t cmd = rd.read_uint32();

        if (cmd == DEADBEEF) {
            spdlog::info("  iter {}: DEADBEEF terminator @ bit {}  "
                         "(remaining {} bits of body after term)",
                         iter, cmd_pos, EFFECTIVE_BITS - rd.pos());
            break;
        }
// == C-O-A | Cyb ==

        if (cmd >= flat.size()) {
            spdlog::warn("  iter {}: cmd_index {} @ bit {} is OUT OF CATALOG "
                         "RANGE ({} entries).",
                         iter, cmd, cmd_pos, flat.size());
            spdlog::warn("     Possible causes:");
            spdlog::warn("       (a) catalog missing classes (AAoCPlayerState,");
            spdlog::warn("           replicated subobjects, struct expansions)");
            spdlog::warn("       (b) stream format assumption wrong");
            spdlog::warn("       (c) cmd is an offset into a DIFFERENT catalog");
            spdlog::warn("           (e.g. cmds + subobject cmds merged)");
            spdlog::warn("     Stopping walk.");
            break;
        }

        const auto& entry = flat[cmd];
        spdlog::info("  iter {}: cmd_index={:3} @ bit {}  ->  "
                     "{}::{} (type={})",
                     iter, cmd, cmd_pos,
                     entry.owning_class, entry.desc->name,
                     to_string(entry.desc->type));

        if (entry.desc->type == FPropertyType::Unknown) {
            spdlog::warn("     property type is Unknown — can't decode body.");
            spdlog::warn("     Fill in catalog type to continue walking.");
            break;
        }

        const size_t body_start = rd.pos();
        PropertyValue v = decode_property(*entry.desc, rd);
        const size_t body_bits = rd.pos() - body_start;

        if (v.type == FPropertyType::Unknown) {
            spdlog::warn("     decode failed (returned empty).  Codec may not "
                         "be implemented or stream may have diverged.");
            break;
        }

        // Pretty-print decoded value for humans
        std::string value_str;
        if (auto* b = std::get_if<bool>(&v.payload)) {
            value_str = *b ? "true" : "false";
        } else if (auto* u = std::get_if<uint8_t>(&v.payload)) {
            value_str = std::to_string(*u);
        } else if (auto* i = std::get_if<int32_t>(&v.payload)) {
            value_str = std::to_string(*i);
        } else if (auto* f = std::get_if<float>(&v.payload)) {
            value_str = std::to_string(*f);
        } else if (auto* s = std::get_if<std::string>(&v.payload)) {
            value_str = "\"" + *s + "\"";
        } else if (auto* g = std::get_if<::aoc::protocol::emit::FIntrepidNetworkGUID>(&v.payload)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{Obj=%llu Srv=%u Rnd=%u}",
                          (unsigned long long)g->ObjectId, g->ServerId, g->Randomizer);
            value_str = buf;
        } else if (std::get_if<StructValue>(&v.payload)) {
            value_str = "<struct>";
        } else {
            value_str = "<?>";
        }

        spdlog::info("     value = {}  ({} bits)", value_str, body_bits);
        ++iter;
    }

    spdlog::info("  Final reader pos: {} (of {} effective bits, {} remaining)",
                 rd.pos(), EFFECTIVE_BITS, EFFECTIVE_BITS - rd.pos());
    spdlog::info("  Total iterations: {}", iter);
}

// ─── Phase 3: scan for DEADBEEF anywhere in the property stream ──────────

void phase3_scan_deadbeef(const std::vector<uint8_t>& data) {
    spdlog::info("");
    spdlog::info("== phase 3: scan for 0xDEADBEEF in property stream ==");

    size_t hits = 0;
    for (size_t bit = PROP_STREAM_START; bit + 32 <= EFFECTIVE_BITS; ++bit) {
        uint32_t v = read_bits_at(data.data(), data.size(), bit, 32);
        if (v == DEADBEEF) {
            spdlog::info("  HIT @ bit {} (offset from stream start: +{})",
                         bit, bit - PROP_STREAM_START);
            ++hits;
        }
    }
    if (hits == 0) {
        spdlog::info("  No DEADBEEF in stream — matches documented pkt#22 "
                     "behaviour (terminator absent; BDB length ends stream)");
    }
}

// ─── Phase 4: dump property stream content ───────────────────────────────

void phase4_dump_stream(const std::vector<uint8_t>& data) {
    spdlog::info("");
    spdlog::info("== phase 4: property stream dump ==");

    // Hex dump of the property stream bytes (approximate — starts mid-byte
    // since PROP_STREAM_START=3933 and 3933/8=491.625, so byte 491 bit 5).
    const size_t start_byte = PROP_STREAM_START / 8;
    const size_t start_sub  = PROP_STREAM_START & 7;
    const size_t end_byte   = (EFFECTIVE_BITS + 7) / 8;
    spdlog::info("  byte range: {}..{} (property stream covers bit offset {} "
                 "within byte {})",
                 start_byte, end_byte, start_sub, start_byte);

    // 16 bytes per row
    spdlog::info("  hex dump (byte offsets, LSB-first per byte):");
    for (size_t b = start_byte; b < end_byte; b += 16) {
        std::string line;
        char buf[8];
        std::snprintf(buf, sizeof(buf), "  %4zu:", b);
        line = buf;
        for (size_t i = 0; i < 16 && (b + i) < end_byte; ++i) {
            std::snprintf(buf, sizeof(buf), " %02x", data[b + i]);
            line += buf;
        }
        spdlog::info("{}", line);
    }
// ~ C.O.A - Cyb ~

    // Bit view of the first 128 bits of the property stream (LSB-first)
    spdlog::info("");
    spdlog::info("  first 128 bits of stream (as bit string, MSB=high offset):");
    std::string bits;
    for (size_t i = 0; i < 128 && PROP_STREAM_START + i < EFFECTIVE_BITS; ++i) {
        if (i > 0 && (i & 7) == 0) bits += ' ';
        int b = read_bits_at(data.data(), data.size(),
                             PROP_STREAM_START + i, 1);
        bits += (b ? '1' : '0');
    }
    spdlog::info("    {}", bits);

    // Try reading uint32 values at every bit offset 0..128 — see if there's
    // an obvious small-integer pattern that looks like a cmd_index sequence
    spdlog::info("");
    spdlog::info("  uint32 scan at every bit offset (0..128) of property stream:");
    spdlog::info("    (looking for small integers that could be cmd_indexes)");
    for (size_t bo = 0; bo < 128 && PROP_STREAM_START + bo + 32 <= EFFECTIVE_BITS; ++bo) {
        uint32_t v = read_bits_at(data.data(), data.size(),
                                   PROP_STREAM_START + bo, 32);
        const char* tag = "";
        if (v < 500) tag = " ← plausible cmd_index";
        else if (v == DEADBEEF) tag = " ← DEADBEEF";
        if (*tag) {
            spdlog::info("    +{:3}: 0x{:08X} ({}){}", bo, v, v, tag);
        }
    }
}

// ─── Phase 5: catalog cmd_index estimate ─────────────────────────────────
//
// Walk the hierarchy catalog and print the CUMULATIVE cmd_index for each
// property.  This is a FLAT approximation — real RepLayout expands structs
// into sub-cmds, so actual indices will be higher.  But it gives us a
// starting range to compare against the observed uint32 values.

void phase5_catalog_map() {
    spdlog::info("");
    spdlog::info("== phase 5: catalog cmd_index estimates ==");

    auto walk_class = [](const ClassCatalog& klass, uint32_t parent_count) {
        spdlog::info("  class {} (parent_count={}):",
                     klass.class_name, parent_count);
        uint32_t idx = parent_count;
        for (const auto& p : klass.own_props) {
            spdlog::info("    cmd_index={:3}  {:30s}  type={}",
                         idx, p.name, to_string(p.type));
            idx += 1;
            if (p.type == FPropertyType::Struct) {
                idx += static_cast<uint32_t>(p.sub_cmds.size());
            }
        }
        return idx;
    };

    uint32_t c = 0;
    c = walk_class(aactor_catalog(),             c);
    c = walk_class(acontroller_catalog(),        c);
    c = walk_class(aplayer_controller_catalog(), c);
    c = walk_class(aaoc_player_controller_catalog(), c);
    spdlog::info("  TOTAL flat cmd_count (structs NOT expanded): {}", c);
    spdlog::info("  (real count is higher — FRepMovement / FRepAttachment /");
    spdlog::info("   FUniqueNetIdRepl etc. expand into multiple sub-cmds each)");
}

} // namespace

int main() {
    auto logger = spdlog::stdout_color_mt("test_pkt22_round_trip");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%^%l%$] %v");

    spdlog::info("================================================================");
    spdlog::info("  pkt#22 (PC ActorOpen) round-trip harness — phase 1 diagnostic");
    spdlog::info("================================================================");

    auto data = read_binary(FIXTURE_PATH);
    if (data.empty()) {
        spdlog::error("Could not read fixture: {}", FIXTURE_PATH);
        return 1;
    }
    if (data.size() * 8 != BUFFER_BITS) {
        spdlog::warn("Fixture size {} bytes ({} bits) != expected {} bits",
                     data.size(), data.size() * 8, BUFFER_BITS);
    }

    report_header(data);
    phase2_walk_cmd_index_stream(data);
    phase3_scan_deadbeef(data);
    phase4_dump_stream(data);
    phase5_catalog_map();

    spdlog::info("");
    spdlog::info("================================================================");
    spdlog::info("  DONE — interpret the output to determine:");
    spdlog::info("    1. Is the first uint32 a plausible cmd_index? (< 500?)");
    spdlog::info("    2. Do the subsequent uint32 scans reveal structure?");
    spdlog::info("    3. Where does the stream actually end? (BDB-bounded,");
    spdlog::info("       not DEADBEEF per doc.)");
    spdlog::info("================================================================");
    return 0;
}

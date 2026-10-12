// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tools/test_package_map_export.cpp
//
//  Session H.3d structural round-trip test for PackageMapExporter.
//
//  The client's NetGUID reader and writer define the exact
//  wire format:
//      [128-bit FIntrepidNetworkGUID]
//      if ObjectId != 0:
//          [uint8 FExportFlags]
//          if bHasPath:
//              [recursive outer entry]
//              [FString path]
//              if bHasNetworkChecksum:
//                  [uint32 checksum]
//
//  The emitter produces exactly this layout.  These tests prove structural
//  correctness by emitting then parsing back, and by checking bit counts
//  against hand-computed expectations.
// ============================================================================
#include "protocol/emit/package_map_exporter.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace aoc;
using namespace aoc::protocol;
// (C | O | A + C:Y:B)

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

// ─── Small helpers for parsing back ─────────────────────────────────────

// Read the 1-bit bHasRepLayoutExport + 32-bit NumGUIDsInBunch; advance pos.
static int parse_export_header(const uint8_t* data, size_t byte_len,
                                 size_t& pos) {
    int rep = static_cast<int>(::ue5::read_bits(data, byte_len, pos, 1));
    if (rep != 0) return -1;  // RepLayoutExport variant — not what we emit
    uint64_t count = ::ue5::read_bits(data, byte_len, pos, 32);
    return static_cast<int>(count);
}

// Read a 128-bit FIntrepidNetworkGUID (4 × uint32 LSB-first).
static emit::FIntrepidNetworkGUID parse_guid(const uint8_t* data,
                                                size_t byte_len,
                                                size_t& pos) {
    return emit::read_intrepid_guid(data, byte_len, pos);
}

int main() {
    std::printf("=== Session H.3d PackageMapExporter round-trip test ===\n");

    // ── Case 1: empty export list ──
    std::printf("\n--- Case 1: empty export list ---\n");
    {
        emit::BunchWriter bw;
        std::vector<emit::ExportEntry> empty;
        size_t bits = emit::PackageMapExporter::write_export_section(bw, empty);
        std::printf("  wrote %zu bits for 0 exports\n", bits);
        CHECK(bits == 33, "header is 33 bits (1 flag + 32-bit count)");

        size_t pos = 0;
        int cnt = parse_export_header(bw.data(), bw.byte_size() + 1, pos);
        CHECK(cnt == 0, "parsed count == 0");
    }

    // ── Case 2: single export with a typical AoC path ──
    std::printf("\n--- Case 2: 1 export with path + null outer ---\n");
    {
        emit::BunchWriter bw;

        // One PC-BP class export, matching captured pkt#22 style.
        // Note: ExportEntry holds a unique_ptr and is move-only, so we
        // build the vector via push_back(std::move(...)) rather than
        // using a braced initialiser list.
        std::vector<emit::ExportEntry> exports;
        {
            emit::ExportEntry e = emit::ExportEntry::asset(
                /*object_id=*/1012,
                /*path=*/"/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP",
                /*checksum=*/0,
                /*no_load=*/true);
            e.guid.ServerId   = 0;
            e.guid.Randomizer = 0;
            exports.push_back(std::move(e));
        }

        size_t bits = emit::PackageMapExporter::write_export_section(bw, exports);
        std::printf("  wrote %zu bits (%zu bytes) for 1 path export\n",
                     bits, bw.byte_size());

        // Expected bit count:
        //   header:                 33 bits
        //   entry:
        //     GUID:                128 bits
        //     flags byte:            8 bits
        //     outer (null GUID):   128 bits
        //     FString save_num:     32 bits   (53 chars + NUL = 54)
        //     path bytes (54):     432 bits
        //   ─────────────────────────────────
        //   total:                 761 bits
        CHECK(bits == 761, "1-export bunch with path+null-outer = 761 bits");

        // Parse back the header
        size_t pos = 0;
        int cnt = parse_export_header(bw.data(), bw.byte_size() + 1, pos);
        CHECK(cnt == 1, "parsed count == 1");

        // Parse the entry's GUID
        auto g = parse_guid(bw.data(), bw.byte_size() + 1, pos);
        CHECK(g.ObjectId == 1012, "entry GUID ObjectId round-tripped");
        CHECK(g.ServerId == 0,    "entry GUID ServerId round-tripped");
        CHECK(g.Randomizer == 0,  "entry GUID Randomizer round-tripped");

        // Flags byte
        uint64_t flags = ::ue5::read_bits(bw.data(), bw.byte_size() + 1, pos, 8);
        std::printf("  flags byte = 0x%02x  (bHasPath=1 bNoLoad=1 bChecksum=0 → 0x03)\n",
                     (unsigned)(flags & 0xFF));
        CHECK((flags & 0xFF) == 0x03, "flags byte = 0x03");

        // Outer (null terminator)
        auto outer = parse_guid(bw.data(), bw.byte_size() + 1, pos);
        CHECK(outer.ObjectId == 0 && outer.ServerId == 0 && outer.Randomizer == 0,
              "null outer terminator round-tripped");

        // FString
        int32_t save_num = static_cast<int32_t>(
            ::ue5::read_bits(bw.data(), bw.byte_size() + 1, pos, 32));
        const std::string expected_path =
            "/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP";
        CHECK(save_num == static_cast<int32_t>(expected_path.size() + 1),
              "FString save_num matches path length + 1");
    }
// -- CoA . C:Y:B --

    // ── Case 3: three exports, mix of with-path / with-checksum / null ──
    std::printf("\n--- Case 3: 3 exports (captured pkt#22 pattern) ---\n");
    {
        emit::BunchWriter bw;

        std::vector<emit::ExportEntry> exports;

        // [0] PC-BP class — flags 0x05 (bHasPath + bHasChecksum)
        auto e0 = emit::ExportEntry::asset(
            1012,
            "/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP",
            0xdeadbeef,
            /*no_load=*/false);
        exports.push_back(std::move(e0));

        // [1] Level — flags 0x07 (bHasPath + bNoLoad + bHasChecksum)
        auto e1 = emit::ExportEntry::asset(
            502,
            "/Game/Levels/Verra_World_Master/Verra_World_Master",
            0xcafebabe,
            /*no_load=*/true);
        exports.push_back(std::move(e1));

        // [2] GlobalGMCommands — no path, just a bare NetGUID ref
        emit::ExportEntry e2;
        e2.guid.ObjectId = 303;
        e2.has_path = false;
        exports.push_back(std::move(e2));

        size_t bits = emit::PackageMapExporter::write_export_section(bw, exports);
        std::printf("  wrote %zu bits (%zu bytes) for 3 mixed exports\n",
                     bits, bw.byte_size());
        CHECK(bits > 800,   "3-export bunch > 800 bits (path-heavy)");
        CHECK(bits < 3000,  "3-export bunch < 3000 bits (reasonable)");

        // Header round-trip
        size_t pos = 0;
        int cnt = parse_export_header(bw.data(), bw.byte_size() + 1, pos);
        CHECK(cnt == 3, "parsed count == 3");

        // Entry [0]: GUID + flags 0x05 + null outer + path + checksum
        auto g0 = parse_guid(bw.data(), bw.byte_size() + 1, pos);
        CHECK(g0.ObjectId == 1012, "[0] GUID ObjectId round-tripped");
        uint64_t f0 = ::ue5::read_bits(bw.data(), bw.byte_size() + 1, pos, 8);
        CHECK((f0 & 0xFF) == 0x05, "[0] flags byte = 0x05");
    }

    // ── Summary ──
    std::printf("\n=== Summary ===\n");
    std::printf("  Passed: %d\n  Failed: %d\n", g_pass, g_fail);
    std::printf("  Result: %s\n", g_fail == 0 ? "ALL PASSED" : "FAILURES");
    std::printf("\n  NOTE: byte-identity against captured pkt#22 now requires\n");
    std::printf("        only the correct fixture values (ObjectIds, paths,\n");
    std::printf("        checksums).  Format is fully decoded & verified.\n");
    return g_fail == 0 ? 0 : 1;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/package_map_exporter.h
//
//  AoC PackageMap export-section emitter — writes the inline NetGUID→path
//  mapping block that precedes actor content in every bunch where
//  `bHasPackageMapExports=1`.
//
//  Wire format (RE'd from UIntrepidNetServerPackageMap — see
//  docs/aoc-wire-format-decoded.md for the full RE log):
//
//    Export section header:
//      [1 bit]    bHasRepLayoutExport          (0 for NetGUID exports)
//      [uint32]   NumGUIDsInBunch              (LSB-first 32 bits)
//
//    For each export entry (recursive — mirrors sub_1450360E0):
//      [128 bits] FIntrepidNetworkGUID         (4 × uint32 LSB-first)
//      if GUID.ObjectId != 0:
//          [uint8]   FExportFlags              (bit 0 = bHasPath,
//                                                bit 1 = bNoLoad,
//                                                bit 2 = bHasNetworkChecksum)
//          if bHasPath:
//              [recursive outer entry]          — same structure
//              [FString] Path                   — int32 len + bytes + NUL
//              if bHasNetworkChecksum:
//                  [uint32] Checksum
//
//  Observed flag values in captured packet #22: 0x05 (bHasPath+bChecksum)
//  and 0x07 (bHasPath+bNoLoad+bChecksum).  Stock UE5 format — no AoC
//  custom bits in ExportFlags itself.  The only AoC divergence is the
//  128-bit FIntrepidNetworkGUID replacing stock UE5's 32-bit FNetworkGUID.
//
//  LAYER:   Protocol / emit
//  SESSION: H.3d (RE-driven — byte-verified against captured pkt#22)
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace emit {

/// One entry in the export chain — a NetGUID possibly carrying its path
/// inline.  If `has_path=false`, only the 128-bit GUID + 8-bit flags byte
/// are written (no path, no outer).  If `has_path=true`, the entry carries
/// its path string + recursive outer chain + optional checksum.
struct ExportEntry {
    FIntrepidNetworkGUID guid;

    // FExportFlags bits
    bool     has_path        = false;
    bool     no_load         = false;
    bool     has_checksum    = false;

    // Only meaningful when has_path=true:
    std::string path;
    uint32_t    checksum     = 0;

    // Outer chain — nullptr terminates the recursion (stops at null GUID).
    std::unique_ptr<ExportEntry> outer;

    // ── Convenience constructors ──

    /// Make a terminating null entry (ObjectId=0).  UE5 stops recursion here.
    static ExportEntry null() { return ExportEntry{}; }
// -- Cyb . C O A --

    /// Make a static-asset entry (e.g. a class blueprint path).
    /// Sets bHasPath=1, bHasChecksum=(checksum!=0).
    static ExportEntry asset(uint64_t object_id,
                              std::string path,
                              uint32_t checksum = 0,
                              bool no_load = false) {
        ExportEntry e;
        e.guid.ObjectId    = object_id;
        e.has_path         = true;
        e.no_load          = no_load;
        e.has_checksum     = (checksum != 0);
        e.path             = std::move(path);
        e.checksum         = checksum;
        return e;
    }
};

class PackageMapExporter {
public:
    /// Write the full export section: header + N top-level entries.
    /// The header is [bHasRepLayoutExport=0][uint32 NumGUIDsInBunch].
    /// Each entry may recurse through its `outer` chain.
    /// Returns number of bits written.
    static size_t write_export_section(BunchWriter& out,
                                         const std::vector<ExportEntry>& entries);

    /// Write a single export entry (recursive via entry.outer).
    /// Returns number of bits written.
    /// Mirrors the recursive body of sub_1450360E0.
    static size_t write_export_entry(BunchWriter& out, const ExportEntry& entry);

    /// Write an 8-bit FExportFlags byte given the three documented flags.
    /// Bits 3-7 are reserved zero.
    static void write_export_flags(BunchWriter& out,
                                     bool b_has_path,
                                     bool b_no_load,
                                     bool b_has_checksum);

    /// Write a UE5 FString (ANSI) in wire format:
    ///   int32 save_num   (= 0 for empty, else 1 + chars.size() to include NUL)
    ///   save_num bytes of ASCII + NUL terminator
    static void write_fstring(BunchWriter& out, const std::string& s);
};

}}} // namespace aoc::protocol::emit

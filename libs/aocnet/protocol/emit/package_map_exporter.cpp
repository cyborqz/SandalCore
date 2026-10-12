// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/package_map_exporter.cpp
//
//  Implementation of the AoC package-map export emitter.
//
//  Wire format (the NetGUID writer):
//
//      write_export_entry(entry):
//          write 128-bit FIntrepidNetworkGUID            (4 × uint32 LSB-first)
//          if entry.guid.ObjectId != 0:
//              write 8-bit ExportFlags
//              if bHasPath:
//                  write_export_entry(entry.outer)       (recursive)
//                  write FString path
//                  if bHasNetworkChecksum:
//                      write uint32 checksum
//
//  When `entry.outer` is nullptr we emit a terminating null entry: 128 zero
//  bits (ObjectId=ServerId=Randomizer=0), matching what UE5 reads as the
//  chain terminator (`*(_QWORD *)a3 == 0` guard in the RE'd reader).
//
//  LAYER:   Protocol / emit
//  SESSION: H.3d
// ============================================================================
#include "protocol/emit/package_map_exporter.h"
#include "protocol/emit/intrepid_netguid.h"

namespace aoc { namespace protocol { namespace emit {

// ─── FExportFlags (8-bit) ──────────────────────────────────────────────────
// built by C-O-A with C-Y-B

void PackageMapExporter::write_export_flags(BunchWriter& out,
                                              bool b_has_path,
                                              bool b_no_load,
                                              bool b_has_checksum) {
    uint8_t flags = 0;
    if (b_has_path)     flags |= 0x01;   // bit 0
    if (b_no_load)      flags |= 0x02;   // bit 1
    if (b_has_checksum) flags |= 0x04;   // bit 2
    // bits 3-7 reserved zero
    out.write_uint8(flags);
}

// ─── FString (ANSI) ────────────────────────────────────────────────────────
//
// UE5 wire format:
//    int32 save_num   — 0 for empty, else (chars.size() + 1) to include NUL
//    save_num bytes   — ASCII content followed by one NUL terminator

void PackageMapExporter::write_fstring(BunchWriter& out, const std::string& s) {
    // Delegate to BunchWriter's canonical implementation.  This writes
    // exactly the format expected by sub_141340770 (the reader counterpart).
    out.write_fstring_ansi(s);
}

// ─── Single export entry (recursive) ───────────────────────────────────────
//
// Direct structural mirror of sub_1450360E0.  Each call:
//   1. writes the 128-bit GUID;
//   2. if GUID is non-null, writes the flags byte;
//   3. if bHasPath is set, recurses for the outer entry, then writes the
//      path FString and (optionally) a 32-bit checksum.

size_t PackageMapExporter::write_export_entry(BunchWriter& out,
                                                const ExportEntry& entry) {
    const size_t start_bit = out.bit_pos();

    // 1. Always write the 128-bit GUID.  If ObjectId == 0 the reader stops
    //    here (no flags, no path) — our struct's default-constructed
    //    ExportEntry produces exactly this terminator.
    write_intrepid_guid(out, entry.guid);

    if (entry.guid.ObjectId == 0) {
        return out.bit_pos() - start_bit;
    }

    // 2. 8-bit FExportFlags.
    write_export_flags(out, entry.has_path, entry.no_load, entry.has_checksum);

    if (!entry.has_path) {
        return out.bit_pos() - start_bit;
    }

    // 3a. Recurse for the outer chain.  A nullptr `outer` collapses to a
    //     terminating null entry (128 zero bits) — matches the UE5 reader
    //     which stops recursion at the first zero GUID.
    if (entry.outer) {
        write_export_entry(out, *entry.outer);
    } else {
        // Emit an explicit null terminator.
        ExportEntry null_terminator;
        write_intrepid_guid(out, null_terminator.guid);
    }

    // 3b. FString path.
    write_fstring(out, entry.path);

    // 3c. Optional 32-bit network checksum.
    if (entry.has_checksum) {
        out.write_uint32(entry.checksum);
    }

    return out.bit_pos() - start_bit;
}

// ─── Export section: header + N top-level entries ─────────────────────────
//
// Header:
//    [1 bit]   bHasRepLayoutExport = 0   (we emit NetGUID exports)
//    [uint32]  NumGUIDsInBunch            (LSB-first 32 bits)
//
// Followed by `entries.size()` export entries written via
// write_export_entry (each of which may recurse through its outer chain).

size_t PackageMapExporter::write_export_section(
    BunchWriter& out,
    const std::vector<ExportEntry>& entries) {
    const size_t start_bit = out.bit_pos();

    // bHasRepLayoutExport = 0 — this is a NetGUID-export block, not a
    // RepLayout field-export block.  AoC only uses the NetGUID form in
    // the observed spawn bunches.
    out.write_bit(0);

    // NumGUIDsInBunch: LSB-first 32-bit int.
    out.write_uint32(static_cast<uint32_t>(entries.size()));

    // Top-level entries.  UE5's NET_ENABLE_CHECKSUMS=0 in stock builds,
    // so no per-bunch checksum marker follows.  If AoC enabled checksums
    // in their build a 32-bit marker would appear here — adjust if a
    // captured example forces it.
    for (const ExportEntry& entry : entries) {
        write_export_entry(out, entry);
    }

    return out.bit_pos() - start_bit;
}

}}} // namespace aoc::protocol::emit

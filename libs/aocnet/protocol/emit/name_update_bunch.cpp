// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/name_update_bunch.cpp
//
//  Native synthesizer for AAoCPlayerController.Name property-update bunch.
//  See name_update_bunch.h for rationale and wire-format details.
//
//  The 16-byte prefix is COPIED VERBATIM from captured pkt#104 — we haven't
//  yet decoded what it encodes semantically (hypothesis: three uint32 refs
//  plus a 1-byte cmd_index tag).  Since it's constant across captures and
//  our test rig confirms bit-exact identity when paired with "RandomChar",
//  copying it is the safe path for Phase III M1.  If the live client
//  rejects this bunch, RE the prefix; see docs/phase-iii-roadmap.md §M1
//  risks for the investigation plan.
//
//  LAYER:   Protocol / emit
//  OWNER:   Phase III M1
//  SESSION: 2026-04-24
// ============================================================================
#include "protocol/emit/name_update_bunch.h"

namespace aoc { namespace protocol { namespace emit {

namespace {

/// The 16-byte header that precedes the FString in every captured Name-
/// property-update bunch we've observed.  Bit-identical match confirmed
/// against captured pkt#104 bits 1496..1624.
///
/// Byte-level layout (little-endian interpretation of captured stream):
///   bytes  0..3 :  00 00 00 01
///   bytes  4..7 :  00 00 00 01
///   bytes  8..11:  00 00 00 01
///   bytes 12..15:  00 00 00 6A
///
/// Hypotheses (unverified):
///   - three uint32_LE refcount/flag words (value=0x01000000 each)
///     followed by a uint32 with low-byte cmd_index 0x6A in its top byte
///   - three SIP-packed zeros + one SIP-packed "106" (unlikely — SIP of
///     106 would be a single 0x6A byte, leaving 3 pad bytes unexplained)
///   - some kind of 128-bit GUID with specific format
///
/// For emission we copy verbatim.
constexpr uint8_t kPrefix[16] = {
    0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x6A,
};
// [Cyb - C | O | A]

} // namespace

size_t build_name_update_bunch_payload(const std::string& name,
                                        BunchWriter& out) {
    // DEPRECATED for live sends — use build_name_update_bunch_payload_v2.
    // Kept for test_name_update_bunch's bit-identity test against the
    // captured pkt#104 region (which this verbatim 16-byte prefix was
    // extracted from).  When used in a standalone bunch the client rejects
    // it because the first 32 bits of payload parse as NumGUIDsInBunch =
    // 0x01000000 (16M), which is an impossible value.
    const size_t start_bits = out.bit_pos();
    out.write_bytes(kPrefix, sizeof(kPrefix));
    out.write_fstring_ansi(name);
    return out.bit_pos() - start_bits;
}

// ─── V2 — proper property-delta bunch payload ──────────────────────────────
//
// A standalone property-delta bunch on an already-open actor channel has
// the following payload structure (matches what UActorChannel::ProcessBunch
// expects when applying a delta to an existing actor):
//
//   [1 bit]   bHasRepLayoutExport = 0
//   [32 bits] NumGUIDsInBunch     = 0    (no export section)
//   [32 bits] cmd_index           = <target property>
//   [32 bits] FString save_num    = name.size() + 1 (ASCII positive)
//   [(N+1)*8] FString bytes       = name + NUL
//
// The caller supplies cmd_index; two plausible values exist for the Name:
//   - 28  (0x1C) — our flat hierarchy catalog position
//   - 106 (0x6A) — the observed value in captured pkt#104 at the Name slot
//
// The captured chain's 0x6A may be an FName table index or an AoC-specific
// scheme; we've empirically observed it sits at the Name property position
// in pkt#104's reassembled content block.  Live test determines which the
// client actually accepts.
size_t build_name_update_bunch_payload_v2(const std::string& name,
                                            uint32_t cmd_index,
                                            BunchWriter& out) {
    const size_t start_bits = out.bit_pos();

    out.write_bit(0);                // bHasRepLayoutExport = 0
    out.write_uint32(0);             // NumGUIDsInBunch     = 0
    out.write_uint32(cmd_index);     // property cmd_index  = caller's choice
    out.write_fstring_ansi(name);    // save_num + bytes + NUL

    return out.bit_pos() - start_bits;
}

}}} // namespace aoc::protocol::emit

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replay_mutator.h
//
//  Reusable library for in-place mutation of captured replay bunches.
//  Specifically: rewrites FString properties (like the character name)
//  inside known packet sites, handling the bit-shift + BDB update that
//  variable-length changes require.
//
//  Used by GameServer's replay_loop to substitute the live character name
//  into pkt#104 (HUD name) and pkt#79 (floating nametag) before each
//  UDP send.  Symmetric to `ActorBuilder` (which BUILDS bunches from
//  scratch using live state) — this class MUTATES captured bunches.
//
//  Both paths will eventually converge: once the Phase II RepLayout
//  synthesizer can produce pkt#104 / pkt#79 from scratch, ReplayMutator
//  becomes redundant and can be retired.  Until then it's the pragmatic
//  bridge that gets custom names visible in-game.
//
//  IMPORTANT: this replaces the (removed) kPawnNametagSite / kHudNameSite
//  + patch_fstring_variable machinery from game_server.h.  The old code
//  was abandoned because its bit-shift logic had bugs; this one was
//  proven via a byte-identity self-test and round-trip verification.
//
//  LAYER:  Protocol / emit
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace emit {

/// Metadata for one FString site inside a captured replay packet.
/// Bit offsets are relative to the RAW packet bytes (including all UE5
/// packet/bunch framing, not just payload).
struct NameSite {
    const char* label;             // human-readable, for logs
    uint16_t    pkt_index;         // index into replay_data.packets
    size_t      bdb_bit;           // 13-bit BDB field within the bunch header
    size_t      name_len_bit;      // int32 save_num (FString length prefix)
    size_t      name_bytes_bit;    // first byte of the FString payload
    size_t      name_region_bits;  // total bits of captured FString region
                                   //   (save_num + bytes)
                                   // — for "RandomChar\0" this is 120
};

class ReplayMutator {
public:
    // ── Known sites (calibrated against the captured RandomChar fixture) ──

    /// pkt#104 — the HUD name bunch (top-right corner name).  FString
    /// at bits 1624..1744.  Bunch BDB at bit 183.
    static const NameSite kPkt104HudName;

    /// pkt#79 — the floating nametag bunch (name above character head).
    /// FString at bits 1353..1473.  Bunch BDB at bit 183.
    static const NameSite kPkt79PawnNametag;

    // ── Core mutation ────────────────────────────────────────────────────
// C.Y.B captures, CoA code

    /// Rewrite the FString at `site` in `raw_bytes` with `new_name`.
    ///
    /// Behaviour:
    ///   1. Verify the input's save_num at `site.name_len_bit` is 11
    ///      (i.e. "RandomChar\0" — the captured fixture value).  If not,
    ///      the input isn't the expected packet and we return empty.
    ///   2. Encode `new_name` as an ASCII FString using replayout::encode_fstring.
    ///   3. Compute delta = new_fstring_bits - site.name_region_bits.
    ///   4. Compose output:
    ///        - bits [0, name_len_bit)       : unchanged
    ///        - bits at name_len_bit         : new FString (variable length)
    ///        - tail bits after              : shifted verbatim
    ///   5. Update the BDB field at `site.bdb_bit` to reflect the new
    ///      bunch payload size (+delta).
    ///
    /// Returns the mutated packet bytes on success, empty vector on
    /// failure (wrong fixture, encoding error, etc.).
    ///
    /// `raw_bytes` is borrowed only for the duration of the call; the
    /// output is a fresh buffer (may be shorter OR longer than input).
    static std::vector<uint8_t> rewrite_name_site(
        const std::vector<uint8_t>& raw_bytes,
        const NameSite&             site,
        const std::string&          new_name);

    // ── Convenience wrappers ────────────────────────────────────────────

    static std::vector<uint8_t> rewrite_hud_name(
        const std::vector<uint8_t>& raw_bytes,
        const std::string&          new_name) {
        return rewrite_name_site(raw_bytes, kPkt104HudName, new_name);
    }

    static std::vector<uint8_t> rewrite_pawn_nametag(
        const std::vector<uint8_t>& raw_bytes,
        const std::string&          new_name) {
        return rewrite_name_site(raw_bytes, kPkt79PawnNametag, new_name);
    }
};

}}} // namespace aoc::protocol::emit

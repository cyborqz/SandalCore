// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replay_mutator.cpp
//
//  See replay_mutator.h for the API.  This is the in-place FString
//  rewriter that drops live character names into captured pkt#104 and
//  pkt#79 bunches.
//
//  Reviewed against synthesize_pkt104_with_name.cpp (now deleted) —
//  the mutation pipeline here is bit-for-bit identical to the one
//  verified with the identity self-test and round-trip decode.
// ============================================================================
#include "protocol/emit/replay_mutator.h"

#include "protocol/emit/replayout/encoders/fstring_codec.h"
#include "protocol/emit/replayout/property_value.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_reader.h"

#include "spdlog/spdlog.h"

namespace aoc { namespace protocol { namespace emit {

// ─── Site definitions ──────────────────────────────────────────────────

// BDB_BIT calibrated via inspect_bunch_header.py (2026-04-23).  The old
// patcher's "bdb_bit=183" was WRONG by 7 bits — it pointed into the
// bunch payload, not the BDB field.  Real BDB is at bit 176 (width 13),
// payload starts at bit 189.  Real BDB value in captured fixture is 1636.
const NameSite ReplayMutator::kPkt104HudName = {
    /*label*/           "HUD name",
    /*pkt_index*/       104,
    /*bdb_bit*/         176,
    /*name_len_bit*/    1624,
    /*name_bytes_bit*/  1656,
    /*name_region_bits*/ 120,  // 32 (save_num) + 11 * 8 ("RandomChar\0")
};

const NameSite ReplayMutator::kPkt79PawnNametag = {
    /*label*/           "floating nametag",
    /*pkt_index*/       79,
    /*bdb_bit*/         176,
    /*name_len_bit*/    1353,
    /*name_bytes_bit*/  1385,
    /*name_region_bits*/ 120,  // 32 + 88 (same "RandomChar\0" fixture)
};

// ─── Bit IO helpers (LSB-first, matches BunchWriter + UE5 FBitReader) ───

namespace {

constexpr size_t kBdbWidth = 13;

uint32_t read_bits_at(const uint8_t* data, size_t len,
                       size_t bit_off, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        const size_t bp = bit_off + i;
        if ((bp >> 3) >= len) break;
        const int bit = (data[bp >> 3] >> (bp & 7)) & 1;
        v |= static_cast<uint32_t>(bit) << i;
    }
    return v;
}
// Cyb with C O A

void write_bits_at(std::vector<uint8_t>& out, size_t bit_off,
                    uint64_t value, int n) {
    for (int i = 0; i < n; ++i) {
        const size_t bp = bit_off + i;
        const size_t byte = bp >> 3;
        const uint8_t mask = static_cast<uint8_t>(1u << (bp & 7));
        if ((value >> i) & 1) out[byte] |= mask;
        else                  out[byte] &= static_cast<uint8_t>(~mask);
    }
}

void copy_bit_range(std::vector<uint8_t>& dst, size_t dst_bit_off,
                    const uint8_t* src, size_t src_bit_off,
                    size_t n_bits) {
    for (size_t i = 0; i < n_bits; ++i) {
        const size_t sp = src_bit_off + i;
        const size_t dp = dst_bit_off + i;
        const int bit = (src[sp >> 3] >> (sp & 7)) & 1;
        const uint8_t mask = static_cast<uint8_t>(1u << (dp & 7));
        if (bit) dst[dp >> 3] |= mask;
        else     dst[dp >> 3] &= static_cast<uint8_t>(~mask);
    }
}

} // namespace

// ─── Core mutation ─────────────────────────────────────────────────────

std::vector<uint8_t> ReplayMutator::rewrite_name_site(
    const std::vector<uint8_t>& raw_bytes,
    const NameSite&             site,
    const std::string&          new_name)
{
    const size_t raw_bits = raw_bytes.size() * 8;

    // ── 1. Sanity-check the input matches our known fixture shape.
    if (site.name_len_bit + 32 > raw_bits) {
        spdlog::error("[ReplayMutator {}] packet too small ({} bits, "
                       "need at least {})",
                       site.label, raw_bits, site.name_len_bit + 32);
        return {};
    }
    const uint32_t save_num_read = read_bits_at(
        raw_bytes.data(), raw_bytes.size(), site.name_len_bit, 32);
    if (save_num_read != 11) {
        spdlog::warn("[ReplayMutator {}] save_num at bit {} = {} "
                      "(expected 11 for 'RandomChar\\0' fixture). "
                      "Refusing to mutate — packet shape differs from "
                      "what this site was calibrated for.",
                      site.label, site.name_len_bit, save_num_read);
        return {};
    }

    // ── 2. Encode the new FString with our codec.
    BunchWriter new_fstring_writer;
    if (!replayout::encode_fstring(
            replayout::PropertyValue::make_string(new_name),
            new_fstring_writer))
    {
        spdlog::error("[ReplayMutator {}] encode_fstring failed", site.label);
        return {};
    }
    const size_t new_fstring_bits = new_fstring_writer.bit_pos();
    const int    delta = static_cast<int>(new_fstring_bits)
                         - static_cast<int>(site.name_region_bits);

    // ── 3. Read the ORIGINAL BDB so we can apply delta.
    //    Sanity-check: expected value is 1636 for our calibrated fixture.
    //    If it's WILDLY different the constants are wrong (e.g. bdb_bit
    //    pointing into the wrong part of the packet).
    const uint32_t bdb_orig = read_bits_at(
        raw_bytes.data(), raw_bytes.size(), site.bdb_bit, kBdbWidth);
    constexpr uint32_t kExpectedBdb = 1636;
    if (bdb_orig != kExpectedBdb) {
        spdlog::warn("[ReplayMutator {}] BDB at bit {} = {} (expected {}). "
                      "Either bdb_bit constant is wrong, or the fixture "
                      "has drifted from the calibration capture.",
                      site.label, site.bdb_bit, bdb_orig, kExpectedBdb);
        // Not fatal — still proceed with read-modify-write.
    }
    const int32_t new_bdb = static_cast<int32_t>(bdb_orig) + delta;
    if (new_bdb <= 0 || new_bdb >= (1 << kBdbWidth)) {
        spdlog::error("[ReplayMutator {}] BDB out of range after delta: "
                       "orig={} delta={} new={}",
                       site.label, bdb_orig, delta, new_bdb);
        return {};
    }

    // ── 4. Compose the output buffer.
    const size_t name_end_bit_orig = site.name_len_bit + site.name_region_bits;
    const size_t tail_bits         = raw_bits - name_end_bit_orig;
    const size_t new_raw_bits      = site.name_len_bit
                                     + new_fstring_bits
                                     + tail_bits;
    const size_t new_byte_count    = (new_raw_bits + 7) / 8;

    std::vector<uint8_t> out(new_byte_count, 0);

    // 4a. Prefix bits [0, name_len_bit) — unchanged verbatim copy.
    copy_bit_range(out, 0, raw_bytes.data(), 0, site.name_len_bit);

    // 4b. New FString bits at [name_len_bit, +new_fstring_bits).
    copy_bit_range(out, site.name_len_bit,
                    new_fstring_writer.data(), 0, new_fstring_bits);

    // 4c. Tail: everything after the old FString, shifted by `delta`.
    copy_bit_range(out, site.name_len_bit + new_fstring_bits,
                    raw_bytes.data(), name_end_bit_orig, tail_bits);

    // 4d. Update the BDB field.
    write_bits_at(out, site.bdb_bit, static_cast<uint64_t>(new_bdb),
                   kBdbWidth);

    spdlog::debug("[ReplayMutator {}] \"{}\" -> \"{}\" "
                   "(fstring {}->{} bits, BDB {}->{}, raw {}->{} bytes)",
                   site.label, "RandomChar", new_name,
                   site.name_region_bits, new_fstring_bits,
                   bdb_orig, new_bdb,
                   raw_bytes.size(), out.size());
    return out;
}

}}} // namespace aoc::protocol::emit

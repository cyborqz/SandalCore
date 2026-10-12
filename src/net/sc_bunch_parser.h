// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  sc_bunch_parser.h — shared S>C bunch header parser
//
//  Extracted from replay_inspect.cpp (April 2026) so both the offline
//  replay inspector AND the live game_server can agree on what a bunch
//  looks like on the wire.  Previously each had its own parser with
//  subtle drift.
//
//  Wire format (authoritative canonical spec — see game_server.h ~L1944
//  "AoC S>C bunch header — canonical wire-format spec" comment block):
//
//      bControl(1)
//      if bControl:  bCtrlOpen(1) + bClose(1) + [CloseReason(SerializeInt MAX=7)]
//      bIsReplicationPaused(1)
//      bReliable(1)
//      ChIndex(SerializeIntPacked)
//      bHasPackageMapExports(1) / bHasMustBeMappedGUIDs(1) / bPartial(1)
//      if bReliable: ChSequence(12)
//      if bPartial:  bPartialInitial(1) + bPartialCustomExportsFinal(1) + bPartialFinal(1)
//      if (bReliable || bCtrlOpen) && (!bPartial || bPartialInitial):
//                    ChName = bHardcoded(1) + (SerializeIntPacked | FString)
//      BunchDataBits(13)
//      <payload>
//
//  Dependencies (caller must provide in the ue5:: namespace):
//      inline uint64_t ue5::read_bits(const uint8_t*, size_t, size_t&, int);
//      inline uint32_t ue5::read_serialize_int(const uint8_t*, size_t, size_t&, uint32_t);
//
//  Both replay_inspect.cpp and game_server.h already define these, so
//  including this header is zero-dep aside from <cstdint>/<string>/<vector>.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace aoc {

enum class BunchKind : uint8_t {
    Control,
    ActorOpen,
    ActorClose,
    GUIDExport,
    PartialCont,
    ActorReliable,
    ActorUpdate,
};

inline const char* bunch_kind_name(BunchKind k) {
    switch (k) {
        case BunchKind::Control:       return "Control";
        case BunchKind::ActorOpen:     return "ActorOpen";
        case BunchKind::ActorClose:    return "ActorClose";
        case BunchKind::GUIDExport:    return "GUIDExport";
        case BunchKind::PartialCont:   return "PartialCont";
        case BunchKind::ActorReliable: return "ActorReliable";
        case BunchKind::ActorUpdate:   return "ActorUpdate";
    }
    return "?";
}
// C O A captures, Cyb code

struct BunchSummary {
    uint32_t pkt_index      = 0;
    uint32_t ch_idx         = 0;
    bool     b_control      = false;
    bool     b_open         = false;
    bool     b_close        = false;
    bool     b_reliable     = false;
    bool     b_exports      = false;
    bool     b_guids        = false;
    bool     b_partial      = false;
    bool     b_partial_initial = false;   // only meaningful when b_partial
    bool     b_partial_cef     = false;   // AoC ext: bPartialCustomExportsFinal
    bool     b_partial_final   = false;   // only meaningful when b_partial
    uint16_t ch_seq         = 0;
    uint16_t bunch_data_bits = 0;
    std::string ch_name;                // if carried
    std::vector<uint8_t> payload_bytes; // byte-aligned reproduction of payload
    uint64_t payload_hash   = 0;
    bool     parse_ok       = false;
    BunchKind kind          = BunchKind::ActorUpdate;
};

// Classify a parsed bunch using *only* header flags.  Called at the tail
// of parse_sc_bunch once all fields are populated.
inline BunchKind classify_bunch(const BunchSummary& b) {
    if (b.b_control && b.b_open)  return BunchKind::ActorOpen;
    if (b.b_control && b.b_close) return BunchKind::ActorClose;
    if (b.b_control)              return BunchKind::Control;
    if (b.b_partial && !b.b_partial_initial) return BunchKind::PartialCont;
    if (b.b_exports || b.b_guids) return BunchKind::GUIDExport;
    if (b.b_reliable)             return BunchKind::ActorReliable;
    return BunchKind::ActorUpdate;
}

// FNV-1a 64-bit over the bit-exact payload (bytes + trailing partial byte).
inline uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

// Parse one S>C bunch starting at bit offset `b`.  On success, advances
// `b` past the entire bunch (header + data) and fills `out`.  Returns
// true on success, false if the bunch looks malformed.
//
// Call with qualified namespace: aoc::parse_sc_bunch(...).  Requires
// ue5::read_bits and ue5::read_serialize_int to be declared in the
// including translation unit.
inline bool parse_sc_bunch(const uint8_t* data, size_t len, size_t eff_bits,
                           size_t& b, BunchSummary& out) {
    if (b + 20 > eff_bits) return false;

    out.b_control = ue5::read_bits(data, len, b, 1) != 0;
    bool b_ctrl_open = false;
    if (out.b_control) {
        b_ctrl_open = ue5::read_bits(data, len, b, 1) != 0;
        out.b_open  = b_ctrl_open;
        out.b_close = ue5::read_bits(data, len, b, 1) != 0;
        if (out.b_close) {
            // CloseReason SerializeInt(max=7) — 1-3 adaptive bits
            (void)ue5::read_serialize_int(data, len, b, 7);
        }
    }
    (void)ue5::read_bits(data, len, b, 1); // bIsReplicationPaused
    out.b_reliable = ue5::read_bits(data, len, b, 1) != 0;

    // ChIndex (SerializeIntPacked)
    {
        uint32_t idx = 0; int shift = 0;
        for (int tries = 0; tries < 5 && b + 8 <= eff_bits; ++tries) {
            uint8_t bv = static_cast<uint8_t>(ue5::read_bits(data, len, b, 8));
            idx |= (static_cast<uint32_t>(bv >> 1) << shift);
            shift += 7;
            if ((bv & 1) == 0) break;
        }
        out.ch_idx = idx;
    }
    if (out.ch_idx > 32766u) return false;

    out.b_exports = ue5::read_bits(data, len, b, 1) != 0;
    out.b_guids   = ue5::read_bits(data, len, b, 1) != 0;
    out.b_partial = ue5::read_bits(data, len, b, 1) != 0;

    // ChSequence — AoC has TWO formats:
    //   - S>C replication bunches (normal actor traffic): 12 bits
    //   - NMT bunches (control channel 0, C>S-compatible framing): 10 bits
    // We auto-detect: channel 0 is reserved for NMT in AoC's design.
    // This matches send_nmt() in game_server.h which writes 10 bits for ch=0
    // and 12-bit ChSeq for every other S>C bunch.
    if (out.b_reliable) {
        int chseq_bits = (out.ch_idx == 0) ? 10 : 12;
        if (b + static_cast<size_t>(chseq_bits) > eff_bits) return false;
        out.ch_seq = static_cast<uint16_t>(ue5::read_bits(data, len, b, chseq_bits));
    }

    // Partial sub-flags — 3 bits (AoC: Initial + CEF + Final).
    if (out.b_partial) {
        if (b + 3 > eff_bits) return false;
        out.b_partial_initial = ue5::read_bits(data, len, b, 1) != 0;
        out.b_partial_cef     = ue5::read_bits(data, len, b, 1) != 0;
        out.b_partial_final   = ue5::read_bits(data, len, b, 1) != 0;
    }
// C O A x C.Y.B

    // ChName — present when (bReliable || bCtrlOpen) AND NOT a partial
    // continuation.
    bool chname_present = (out.b_reliable || b_ctrl_open)
                       && (!out.b_partial || out.b_partial_initial);
    if (chname_present && b + 1 <= eff_bits) {
        bool hardcoded = ue5::read_bits(data, len, b, 1) != 0;
        if (hardcoded) {
            uint32_t name_idx = 0; int shift = 0;
            for (int tries = 0; tries < 5 && b + 8 <= eff_bits; ++tries) {
                uint8_t bv = static_cast<uint8_t>(ue5::read_bits(data, len, b, 8));
                name_idx |= (static_cast<uint32_t>(bv >> 1) << shift);
                shift += 7;
                if ((bv & 1) == 0) break;
            }
            out.ch_name = "#" + std::to_string(name_idx);
        } else {
            if (b + 32 > eff_bits) return false;
            int32_t save_num = static_cast<int32_t>(ue5::read_bits(data, len, b, 32));
            bool unicode = save_num < 0;
            int32_t char_count = unicode ? -save_num : save_num;
            if (char_count < 0 || char_count > 256) return false;
            size_t skip_bits = static_cast<size_t>(unicode ? char_count * 16 : char_count * 8);
            if (b + skip_bits > eff_bits) return false;
            std::string s;
            for (int32_t ci = 0; ci < char_count; ++ci) {
                if (unicode) {
                    uint16_t wc = static_cast<uint16_t>(ue5::read_bits(data, len, b, 16));
                    if (wc && wc < 128) s += static_cast<char>(wc);
                } else {
                    char c = static_cast<char>(ue5::read_bits(data, len, b, 8));
                    if (c) s += c;
                }
            }
            out.ch_name = s;
        }
    }

    // BunchDataBits — 13 bits (CeilLog2(MaxPacket*8)=CeilLog2(8192)=13).
    if (b + 13 > eff_bits) return false;
    out.bunch_data_bits = static_cast<uint16_t>(ue5::read_bits(data, len, b, 13));

    size_t data_start = b;
    if (out.bunch_data_bits > 16383) return false;
    if (data_start + out.bunch_data_bits > eff_bits) return false;

    // Reproduce payload as bytes (bit-accurate) for hashing + diff.
    size_t payload_end = data_start + out.bunch_data_bits;
    size_t tb = data_start;
    while (tb + 8 <= payload_end) {
        out.payload_bytes.push_back(
            static_cast<uint8_t>(ue5::read_bits(data, len, tb, 8)));
    }
    size_t leftover = (payload_end > tb) ? (payload_end - tb) : 0;
    if (leftover > 0) {
        out.payload_bytes.push_back(
            static_cast<uint8_t>(ue5::read_bits(data, len, tb, (int)leftover)));
    }
    out.payload_hash = fnv1a64(out.payload_bytes.data(), out.payload_bytes.size());

    b = data_start + out.bunch_data_bits;
    out.parse_ok = true;
    out.kind = classify_bunch(out);
    return true;
}

} // namespace aoc

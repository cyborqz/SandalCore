// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/bunch_reassembler.cpp
//
//  C++ port of phase1_parser.py's reassemble_partial_bunches().
// ============================================================================
#include "protocol/wire/bunch_reassembler.h"
#include "protocol/wire/ue5_primitives.h"
#include <algorithm>

namespace aoc { namespace protocol { namespace wire {

namespace {

/// A single fragment record — same semantics as Python's entry dict.
struct Fragment {
    size_t pkt_index;
    size_t bunch_index;
    const ParsedBunch* bunch;
    const uint8_t* raw_buf;
    size_t raw_len;
    Direction direction;
};

/// Concatenate the payload bits of a chain of fragments into a single
/// reassembled buffer.  Matches Python's reassembly loop 1000-1015.
ReassembledBunch finalize_chain(const std::vector<Fragment>& chain) {
    ReassembledBunch out;
    out.direction = chain.front().direction;
    out.source_pkt_first = chain.front().pkt_index;
    out.source_pkt_last = chain.back().pkt_index;

    // Copy header from first fragment, then override bunch_data_bits to the
    // TOTAL across all fragments (so downstream consumers see a "complete" bunch).
    out.header = *chain.front().bunch;
    out.header.partial_initial = true;
    out.header.partial_final = true;  // synthetic: treat as single complete bunch
// [C-O-A :: Cyb]

    size_t total_bits = 0;
    for (const auto& f : chain) total_bits += f.bunch->bunch_data_bits;
    out.total_bits = total_bits;
    out.header.bunch_data_bits = static_cast<uint32_t>(total_bits);

    // Allocate buffer, write each fragment's payload bits in order.
    out.data.assign((total_bits + 7) / 8, 0);

    size_t write_pos = 0;
    for (const auto& f : chain) {
        size_t src_start = f.bunch->data_start_bit;
        size_t num_bits = f.bunch->bunch_data_bits;
        for (size_t i = 0; i < num_bits; ++i) {
            size_t src_bit = src_start + i;
            size_t src_byte = src_bit >> 3;
            if (src_byte < f.raw_len) {
                uint8_t bit = (f.raw_buf[src_byte] >> (src_bit & 7)) & 1;
                if (bit) {
                    out.data[write_pos >> 3] |= (1 << (write_pos & 7));
                }
            }
            ++write_pos;
        }
    }

    return out;
}

} // namespace

ReassemblyResult reassemble_partial_bunches(const std::vector<ReassemblyInput>& packets) {
    ReassemblyResult result;

    // ── Pass 1: collect partial bunches per channel, in packet order ──
    std::unordered_map<uint32_t, std::vector<Fragment>> partial_by_channel;

    for (size_t pkt_idx = 0; pkt_idx < packets.size(); ++pkt_idx) {
        const auto& in = packets[pkt_idx];
        if (!in.parsed) continue;
        for (size_t b_idx = 0; b_idx < in.parsed->bunches.size(); ++b_idx) {
            const ParsedBunch& b = in.parsed->bunches[b_idx];
            if (b.is_partial) {
                partial_by_channel[b.channel].push_back({
                    pkt_idx, b_idx, &b, in.raw_buf, in.raw_len,
                    Direction::ServerToClient  // TODO: track direction per-packet
                });
            }
        }
    }

    // ── Pass 2: find complete chains per channel, reassemble ──
    // Algorithm mirrors Python 961-1028:
    //   walk fragments in order; start a chain at partial_initial;
    //   extend with middle fragments; complete at partial_final.
    for (auto& [ch, entries] : partial_by_channel) {
        std::vector<Fragment> current_chain;
        for (auto& frag : entries) {
            const ParsedBunch& b = *frag.bunch;

            if (b.partial_initial) {
                // A new chain starts here.  If we had a previous incomplete chain,
                // drop it (don't add its fragments to skip_set — they stay as-is).
                current_chain.clear();

                if (b.partial_final) {
                    // Self-contained single-fragment bunch: NOT a chain to reassemble,
                    // skip_set should NOT contain it.  (Python 976 does same.)
                    current_chain.clear();
                    continue;
                }
                current_chain.push_back(frag);
            } else if (!current_chain.empty()) {
                current_chain.push_back(frag);
                if (b.partial_final) {
                    // Complete chain!  Reassemble + mark fragments for skip.
                    for (const auto& f : current_chain) {
                        result.skip_set.insert({f.pkt_index, f.bunch_index});
                    }
                    result.bunches.push_back(finalize_chain(current_chain));
                    current_chain.clear();
                }
            }
            // else: middle fragment with no initial → dangling, ignore
        }
        // Any leftover current_chain is an incomplete chain — leave fragments as-is
    }

    return result;
}

}}} // namespace aoc::protocol::wire

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/block_reader.cpp
//
//  See block_reader.h.
// ============================================================================
#include "protocol/repl/block_reader.h"

#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"

#include <vector>

namespace aoc { namespace protocol { namespace repl {

namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

CodecOptions default_block_options() {
    CodecOptions o;
    // the client's stat table is not on the wire: the captured pawn's four extended stats have Records 0x5429e8c0xxxxxxxx and 0x5429e8c4xxxxxxxx
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}
// [Cyb ~ C-O-A]

std::optional<ObjectState> load_object_state(const schema::ClassSchema& schema, const uint8_t* data, size_t bits, bool has_rep_layout,
                                             std::string* why) {
    std::string last = "the payload does not read to its last bit";
    for (const bool short_rotation : {false, true}) {
        for (const bool preset_known : {false, true}) {
            ObjectState out;
            out.schema = &schema;
            out.opts = default_block_options();
            out.opts.rep_movement_short_rotation = short_rotation;
            if (preset_known) out.opts.customization_preset_known = [](uint64_t) { return true; };
            size_t pos = 0;
            if (has_rep_layout) {
                auto state = read_rep_block(schema, data, bits, pos, out.opts);
                if (!state) continue;
                out.state = std::move(*state);
            }
            bool ok = true;
            while (pos < bits && ok) {
                const auto header = read_field_header(schema, data, bits, pos);
                if (!header) { ok = false; break; }
                const schema::NetField* field = schema.field(header->field);
                if (!field) { ok = false; break; }
                if (field->kind == schema::FieldKind::Function) {
                    last = std::string("the field loop holds the RPC ") + field->name;
                    ok = false;
                    break;
                }
                const auto layout = fast_array_layout(schema, header->field);
                if (!layout) {
                    last = std::string("the property field ") + field->name + " is no fast array";
                    ok = false;
                    break;
                }
                const size_t end = pos + header->payload_bits;
                size_t p = pos;
                const auto delta = read_fast_array_delta(*layout, data, end, p, out.opts);
                if (!delta || p != end) { ok = false; break; }
                FastArrayState* st = out.arrays.get(schema, header->field);
                if (!st || !apply_fast_array_delta(*st, *delta)) { ok = false; break; }
                pos = end;
            }
            if (ok && pos == bits) return out;
        }
    }
    if (why) *why = last;
    return std::nullopt;
}

std::optional<ObjectState> load_object_state_hex(const schema::ClassSchema& schema, const char* hex, size_t bits, bool has_rep_layout,
                                                 std::string* why) {
    std::vector<uint8_t> bytes((bits + 7) / 8, 0);
    size_t i = 0;
    for (; hex[i] && hex[i + 1]; i += 2) {
        const int hi = hex_digit(hex[i]), lo = hex_digit(hex[i + 1]);
        if (hi < 0 || lo < 0 || i / 2 >= bytes.size()) {
            if (why) *why = "the payload is not hex of the right length";
            return std::nullopt;
        }
        bytes[i / 2] = static_cast<uint8_t>(hi << 4 | lo);
    }
    if (i / 2 != bytes.size() || hex[i]) {
        if (why) *why = "the payload is not hex of the right length";
        return std::nullopt;
    }
    return load_object_state(schema, bytes.data(), bits, has_rep_layout, why);
}

}}}  // namespace aoc::protocol::repl

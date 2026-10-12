// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/rep_layout_writer.h
//
//  RepLayout property blocks (FRepLayout::SendProperties) in the form the
//  AoC client's FRepLayout::ReceiveProperties (0x1444e4a40) reads:
//
//    bDoChecksum (1 bit)       AoC ships with ENABLE_PROPERTY_CHECKSUMS on;
//                              when set, a 32-bit checksum follows every
//                              handle.  The original server always sent 0.
//    { SerializeIntPacked(handle), value }   changed properties, ascending
//    SerializeIntPacked(0)     end of the block
//
//  Value encodings (FProperty::NetSerializeItem): bool 1 bit, integers their
//  full width, an enum CeilLogTwo(MaxEnumValue + 1) bits, an object
//  reference the 128-bit FIntrepidNetworkGUID (export flags only follow for
//  ObjectId 1, which a property never names).
//
//  write_actor_property_block wraps a finished block in the content block
//  for the channel's actor: bHasRepLayout=1, bIsActor=1, SIP payload bits.
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include <cstddef>
#include <cstdint>

namespace aoc { namespace protocol { namespace emit {

class RepLayoutWriter {
public:
    /// Starts a block in `out` (writes bDoChecksum = 0).
    explicit RepLayoutWriter(BunchWriter& out) : out_(out) { out_.write_bit(0); }

    void write_bool(uint32_t handle, bool value) {
        begin(handle);
        out_.write_bit(value ? 1 : 0);
    }
    void write_uint16(uint32_t handle, uint16_t value) {
        begin(handle);
        out_.write_uint16(value);
    }
    void write_uint32(uint32_t handle, uint32_t value) {
        begin(handle);
        out_.write_uint32(value);
    }
    void write_enum(uint32_t handle, uint32_t value, int bits) {
        begin(handle);
        out_.write_bits(value, bits);
    }
    void write_object(uint32_t handle, const FIntrepidNetworkGUID& object) {
        begin(handle);
        write_intrepid_guid(out_, object);
    }
// -- CoA & C-Y-B --

    /// Writes `handle` and returns the stream for a value with its own
    /// NetSerialize (see protocol/emit/net_serializers.h).
    BunchWriter& write_handle(uint32_t handle) {
        begin(handle);
        return out_;
    }

    /// Writes the terminating handle 0.
    void finish() { out_.write_sip(0); }

    /// False once a handle was not greater than the one before it: the
    /// receiver walks its commands forward and cannot go back.
    bool ok() const { return ok_; }

private:
    void begin(uint32_t handle) {
        if (handle <= last_handle_) ok_ = false;
        last_handle_ = handle;
        out_.write_sip(handle);
    }

    BunchWriter& out_;
    uint32_t last_handle_ = 0;
    bool ok_ = true;
};

/// Appends the content block for the channel's actor carrying `props` (a
/// finished RepLayoutWriter block).  Returns bits written.
inline size_t write_actor_property_block(BunchWriter& out, const BunchWriter& props) {
    const size_t start = out.bit_pos();
    out.write_bit(1);                  // bHasRepLayout
    out.write_bit(1);                  // bIsActor
    out.write_sip(props.bit_pos());    // payload bits
    out.write_bit_range(props.data(), 0, props.bit_pos());
    return out.bit_pos() - start;
}

}}}  // namespace aoc::protocol::emit

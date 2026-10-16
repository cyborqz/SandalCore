// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/subobject.cpp
//
//  See subobject.h.
// ============================================================================
#include "protocol/repl/subobject.h"

#include "protocol/emit/package_map_exporter.h"
#include "protocol/repl/codecs.h"
#include "protocol/wire/packet_reader.h"

#include <memory>

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using emit::ExportEntry;
using emit::FIntrepidNetworkGUID;
using emit::PackageMapExporter;

namespace {

using GuidKey = std::array<uint64_t, 2>;

GuidKey key_of(const FIntrepidNetworkGUID& g) {
    return {g.ObjectId, (static_cast<uint64_t>(g.ServerId) << 32) | g.Randomizer};
}

/// The entry of `links[i]` and, through its outers, the rest of the chain.  UE gives a link its path when
/// UPackageMapClient::ShouldSendFullPath says so: not when its GUID already went into this export bunch (`seen`) or its
/// export is acknowledged (`acks`, outers only: an acknowledged first link is not exported at all), and never for an
/// object without a name.  The others are written as their GUID and flags alone, and the chain ends there.  Records
/// every GUID it writes in `seen` and, with a path, in `written`.
ExportEntry build_entry(const ExportChain& links, size_t i, std::set<GuidKey>& seen, const ExportAcks& acks,
                        std::vector<FIntrepidNetworkGUID>& written) {
    const ExportLink& l = links[i];
    ExportEntry e;
    e.guid = l.guid;
    e.no_load = l.no_load;
    e.has_checksum = l.has_checksum;
    const bool repeated = seen.count(key_of(l.guid)) != 0 || (i > 0 && acks.acked(l.guid));
    seen.insert(key_of(l.guid));
    if (l.has_path() && !repeated) {
        e.has_path = true;
        e.path = l.name;
        e.checksum = l.checksum;
        written.push_back(l.guid);
        // No outer left: the writer ends the chain with the null GUID.
        if (i + 1 < links.size()) e.outer = std::make_unique<ExportEntry>(build_entry(links, i + 1, seen, acks, written));
    }
    return e;
}

}  // namespace
// built by C.O.A with Cyb

ExportChain component_export(const FIntrepidNetworkGUID& actor, const FIntrepidNetworkGUID& component,
                             std::string name, uint32_t checksum) {
    ExportLink c;
    c.guid = component;
    c.name = std::move(name);
    c.no_load = true;
    c.has_checksum = true;
    c.checksum = checksum;
    ExportLink a;   // the dynamic actor: flags 6 and nothing more
    a.guid = actor;
    a.no_load = true;
    a.has_checksum = true;
    return {std::move(c), std::move(a)};
}

PackedExports pack_exports(std::span<const ExportChain> chains, const ExportAcks& acks, size_t max_bits) {
    constexpr size_t kHeaderBits = 1 + 32;   // bHasRepLayoutExport, the count
    PackedExports out;
    BunchWriter entries;      // the entries of the bunch being filled
    uint32_t count = 0;
    std::set<GuidKey> seen;   // GUIDs written into it
    std::vector<FIntrepidNetworkGUID> pending;   // the GUIDs it writes with a path

    auto flush = [&]() {
        if (count == 0) return;
        BunchWriter b;
        b.write_bit(0);
        b.write_uint32(count);
        b.write_bit_range(entries.data(), 0, entries.bit_pos());
        out.bunches.push_back(std::move(b));
        out.exported.insert(out.exported.end(), pending.begin(), pending.end());
        out.exported_by_bunch.push_back(std::move(pending));
        entries.reset();
        seen.clear();
        pending.clear();
        count = 0;
    };

    for (const ExportChain& chain : chains) {
        // A first GUID the bunch being filled already holds (as an entry or as an outer) is not exported again: UE's
        // ShouldSendFullPath says no, and writes nothing for it.
        if (chain.empty() || acks.acked(chain.front().guid) || seen.count(key_of(chain.front().guid))) continue;
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::set<GuidKey> trial = seen;
            std::vector<FIntrepidNetworkGUID> written;
            BunchWriter one;
            PackageMapExporter::write_export_entry(one, build_entry(chain, 0, trial, acks, written));
            if (kHeaderBits + entries.bit_pos() + one.bit_pos() <= max_bits) {
                entries.write_bit_range(one.data(), 0, one.bit_pos());
                seen = std::move(trial);
                pending.insert(pending.end(), written.begin(), written.end());
                ++count;
                break;
            }
            if (count == 0) {   // alone in a fresh bunch and still too big
                out.ok = false;
                return out;
            }
            flush();
        }
    }
    flush();
    return out;
}

// ---- content blocks -----------------------------------------------------------

size_t write_block_header(BunchWriter& out, const BlockHeader& h, bool server_form) {
    // ObjectId 1 is the export-flags form of a reference (codecs.h refuses it): no block names it by GUID.  The client's form can
    // name the subobject in place (`in_place`: what UPackageMapClient writes for an object the server never gave it a GUID for).
    if (!h.is_actor && (h.object.ObjectId == 1 || (server_form && !h.stably_named &&
                                                   (h.dynamic_class.ObjectId == 1 || (h.outer && h.outer->ObjectId == 1)))))
        return 0;
    if (!h.is_actor && h.in_place && server_form) return 0;   // the server names a subobject by its GUID
    const size_t start = out.bit_pos();
    if (!h.is_actor && h.in_place) {
        InlineObject name;
        name.flags = h.in_place->flags;
        name.outer = h.in_place->outer;
        name.path = h.in_place->path;
        name.checksum = h.in_place->checksum;
        if (!inline_object_fits(name)) return 0;
        out.write_bit(h.has_rep_layout);
        out.write_bit(false);
        if (!write_inline_object(out, name)) {
            out.truncate(start);
            return 0;
        }
        return out.bit_pos() - start;
    }
    out.write_bit(h.has_rep_layout);
    out.write_bit(h.is_actor);
    if (h.is_actor) return out.bit_pos() - start;
    write_object(out, h.object);
    if (!server_form) return out.bit_pos() - start;
    out.write_bit(h.stably_named);
    if (!h.stably_named) {
        out.write_bit(0);                  // not a destroy message
        write_object(out, h.dynamic_class);
        out.write_bit(!h.outer);           // bActorIsOuter
        if (h.outer) write_object(out, *h.outer);
    }
    return out.bit_pos() - start;
}
// C_Y_B with CoA

size_t write_content_block(BunchWriter& out, const BlockHeader& h, const BunchWriter& payload, bool server_form) {
    const size_t start = out.bit_pos();
    if (write_block_header(out, h, server_form) == 0) return 0;
    out.write_sip(payload.bit_pos());
    out.write_bit_range(payload.data(), 0, payload.bit_pos());
    return out.bit_pos() - start;
}

std::optional<BlockView> read_content_block(const uint8_t* data, size_t end, size_t& pos, bool server_form) {
    wire::PacketReader r(data, (end + 7) / 8, end);
    r.set_pos(pos);
    BlockView v;
    v.start = pos;
    v.header.has_rep_layout = r.read_bit() != 0;
    v.header.is_actor = r.read_bit() != 0;
    if (!v.header.is_actor && !server_form) {
        // the client's form: the subobject's GUID, or, when the server never gave the client one, its name in place (ObjectId 1 and the
        // export).  The name is read to its last bit so that the block can be skipped by its payload bits.
        const auto object = read_object_value(r, /*allow_inline=*/true);
        if (!object) return std::nullopt;
        if (const auto* guid = object->get_if<FIntrepidNetworkGUID>()) {
            v.header.object = *guid;
        } else if (const auto* name = object->get_if<InlineObject>()) {
            InPlaceName in_place;
            in_place.flags = name->flags;
            in_place.outer = name->outer;
            in_place.path = name->path;
            in_place.checksum = name->checksum;
            v.header.in_place = std::move(in_place);
        } else {
            return std::nullopt;
        }
    } else if (!v.header.is_actor) {
        const auto object = read_object(r);
        if (!object) return std::nullopt;
        v.header.object = *object;
        if (server_form) {
            v.header.stably_named = r.read_bit() != 0;
            if (!v.header.stably_named) {
                if (r.read_bit()) return std::nullopt;   // a destroy message: not supported
                const auto cls = read_object(r);
                if (!cls) return std::nullopt;
                v.header.dynamic_class = *cls;
                if (!r.read_bit()) {                     // bActorIsOuter = 0: the outer follows
                    const auto outer = read_object(r);
                    if (!outer) return std::nullopt;
                    v.header.outer = *outer;
                }
            }
        }
    }
    const auto bits = r.read_sip();
    if (!bits || overran(r) || *bits > end - r.pos()) return std::nullopt;
    v.payload_pos = r.pos();
    v.payload_bits = static_cast<size_t>(*bits);
    pos = v.payload_pos + v.payload_bits;
    return v;
}

std::optional<std::vector<BlockView>> read_content_blocks(const uint8_t* data, size_t end, size_t pos,
                                                          bool server_form) {
    std::vector<BlockView> blocks;
    while (pos < end) {
        auto b = read_content_block(data, end, pos, server_form);
        if (!b) return std::nullopt;
        blocks.push_back(std::move(*b));
    }
    return blocks;
}

}}}  // namespace aoc::protocol::repl

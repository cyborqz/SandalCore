// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/rpc.cpp
//
//  See rpc.h.
// ============================================================================
#include "protocol/repl/rpc.h"

#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"
#include "protocol/wire/packet_reader.h"

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using schema::PropType;
using schema::RepCommand;
using schema::RpcParam;
using schema::RpcSchema;
using wire::PacketReader;

namespace {

std::span<const RepCommand> commands_of(const RpcSchema& rpc, const RpcParam& p) {
    if (p.cmd_end < p.cmd_begin || p.cmd_end > rpc.commands.size()) return {};
    return rpc.commands.subspan(p.cmd_begin, p.cmd_end - p.cmd_begin);
}

/// The element commands of an array command, or nullopt when its range is bad.
std::optional<std::span<const RepCommand>> elements_of(const RepCommand& cmd, std::span<const RepCommand> table) {
    if (cmd.elem_end < cmd.elem_begin || cmd.elem_end > table.size()) return std::nullopt;
    return table.subspan(cmd.elem_begin, cmd.elem_end - cmd.elem_begin);
}

/// A struct parameter, flattened (Struct) or one NetSerialize command: UE never finds it identical to the default.
bool is_struct_param(const RpcParam& p) {
    return p.type == PropType::Struct || p.type == PropType::NetSerializeStruct;
}

/// True when `v` is what zeroed memory holds for `cmd`: the property is Identical to nullptr.
bool is_default(const RepCommand& cmd, const Value& v) {
    switch (cmd.type) {
        case PropType::Int8:
        case PropType::Int16:
        case PropType::Int32:
        case PropType::Int64: {
            const int64_t* i = v.get_if<int64_t>();
            return i && *i == 0;
        }
        case PropType::UInt8:
        case PropType::UInt16:
        case PropType::UInt32:
        case PropType::UInt64:
        case PropType::Enum: {
            const uint64_t* u = v.get_if<uint64_t>();
            return u && *u == 0;
        }
        case PropType::Float: {
            const float* f = v.get_if<float>();
            return f && *f == 0.0f;
        }
        case PropType::Double: {
            const double* d = v.get_if<double>();
            return d && *d == 0.0;
        }
        case PropType::Str: {
            const std::string* s = v.get_if<std::string>();
            return s && s->empty();
        }
        case PropType::Name: {
            const NameValue* n = v.get_if<NameValue>();
            return n && n->number == 0 && ((n->hardcoded && n->index == 0) || (!n->hardcoded && n->text == "None"));
        }
        case PropType::Object: {
            const auto* o = v.get_if<emit::FIntrepidNetworkGUID>();
            return o && o->ObjectId == 0;
        }
        case PropType::SoftObject: {
            const SoftObjectValue* s = v.get_if<SoftObjectValue>();
            return s && (s->is_path ? s->path.empty() : s->object.ObjectId == 0);
        }
        case PropType::DynamicArray: {
            const ArrayValue* a = v.get_if<ArrayValue>();
            return a && a->elements.empty();
        }
        default:   // structs, text and bool: sent
            return false;
    }
}
// C.O.A / C.Y.B

/// A command's value fits: an array as its elements (every member of each set), else as in a RepLayout block.
bool fits(const RepCommand& cmd, std::span<const RepCommand> table, const Value& v) {
    if (cmd.type != PropType::DynamicArray) return value_matches(cmd, table, v);
    const ArrayValue* a = v.get_if<ArrayValue>();
    const auto elems = elements_of(cmd, table);
    if (!a || !elems || a->elements.size() > 0xFFFF) return false;
    for (const ElementState& e : a->elements) {
        if (!element_fits(*elems, table, e)) return false;
    }
    return true;
}

bool write_command(const RepCommand& cmd, std::span<const RepCommand> table, const Value& v, BunchWriter& out,
                   const CodecOptions& opts) {
    if (cmd.type != PropType::DynamicArray) return write_value(cmd, table, v, out, opts);
    const ArrayValue* a = v.get_if<ArrayValue>();
    const auto elems = elements_of(cmd, table);
    if (!a || !elems || a->elements.size() > 0xFFFF) return false;
    out.write_bits(a->elements.size(), 16);
    for (const ElementState& e : a->elements) {
        if (!write_element(*elems, table, e, out, opts)) return false;
    }
    return true;
}

std::optional<Value> read_command(const RepCommand& cmd, std::span<const RepCommand> table, PacketReader& r,
                                  const CodecOptions& opts) {
    if (cmd.type != PropType::DynamicArray) return read_value(cmd, table, r, opts);
    const auto elems = elements_of(cmd, table);
    const size_t count = static_cast<size_t>(r.read_bits(16));
    if (!elems || overran(r)) return std::nullopt;
    ArrayValue a;
    for (size_t k = 0; k < count; ++k) {
        auto e = read_element(*elems, table, r, opts);
        if (!e) return std::nullopt;
        a.elements.push_back(std::move(*e));
    }
    return Value(std::move(a));
}

/// How many of a parameter's commands `params` has values for.
size_t present_count(std::span<const RepCommand> cmds, const RpcParams& params) {
    size_t n = 0;
    for (const RepCommand& c : cmds) n += params.values.count(c.handle);
    return n;
}

}  // namespace

const RepCommand* rpc_command(const RpcSchema& rpc, std::string_view path) {
    for (const RepCommand& c : rpc.commands) {
        if (path == c.path) return &c;
    }
    return nullptr;
}

bool set_rpc_param(const RpcSchema& rpc, RpcParams& params, std::string_view path, Value v) {
    const RepCommand* cmd = rpc_command(rpc, path);
    if (!cmd || !fits(*cmd, rpc.element_commands, v)) return false;
    params.values[cmd->handle] = std::move(v);
    return true;
}

bool rpc_params_match(const RpcSchema& rpc, const RpcParams& params) {
    for (const auto& [handle, v] : params.values) {
        if (handle < 1 || handle > rpc.commands.size() || !fits(rpc.commands[handle - 1], rpc.element_commands, v))
            return false;
    }
    for (const RpcParam& p : rpc.params) {
        const auto cmds = commands_of(rpc, p);
        const size_t n = present_count(cmds, params);
        if (n != 0 && n != cmds.size()) return false;
        if (!p.send_bit && n != 0) {   // a bool
            const bool* b = params.values.at(cmds[0].handle).get_if<bool>();
            if (!b) return false;
        }
    }
    return true;
}
// C_Y_B + C.O.A

size_t write_rpc_params(const RpcSchema& rpc, const RpcParams& params, BunchWriter& out, const CodecOptions& opts,
                        bool* failed) {
    if (failed) *failed = false;
    if (!rpc_params_match(rpc, params)) {
        if (failed) *failed = true;
        return 0;
    }
    const size_t start = out.bit_pos();
    for (const RpcParam& p : rpc.params) {
        const auto cmds = commands_of(rpc, p);
        const bool present = !cmds.empty() && present_count(cmds, params) == cmds.size();
        if (!p.send_bit) {   // a bool goes bare
            out.write_bit(present && *params.values.at(cmds[0].handle).get_if<bool>());
            continue;
        }
        const bool send = present && (is_struct_param(p) || cmds.size() != 1 ||
                                      !is_default(cmds[0], params.values.at(cmds[0].handle)));
        out.write_bit(send);
        if (!send) continue;
        for (const RepCommand& c : cmds) {
            if (!write_command(c, rpc.element_commands, params.values.at(c.handle), out, opts)) {
                out.truncate(start);
                if (failed) *failed = true;
                return 0;
            }
        }
    }
    return out.bit_pos() - start;
}

std::optional<RpcParams> read_rpc_params(const RpcSchema& rpc, const uint8_t* data, size_t end, size_t& pos,
                                         const CodecOptions& opts) {
    PacketReader r(data, (end + 7) / 8, end);
    r.set_pos(pos);
    RpcParams params;
    for (const RpcParam& p : rpc.params) {
        const auto cmds = commands_of(rpc, p);
        if (!p.send_bit) {
            if (cmds.size() != 1 || cmds[0].type != PropType::Bool) return std::nullopt;
            params.values.emplace(cmds[0].handle, Value::boolean(r.read_bit() != 0));
            continue;
        }
        if (!r.read_bit()) continue;   // the default
        for (const RepCommand& c : cmds) {
            auto v = read_command(c, rpc.element_commands, r, opts);
            if (!v) return std::nullopt;
            params.values.emplace(c.handle, std::move(*v));
        }
    }
    if (overran(r) || r.pos() != end) return std::nullopt;
    pos = end;
    return params;
}

size_t write_rpc_field(const schema::ClassSchema& cls, const RpcSchema& rpc, const RpcParams& params, BunchWriter& out,
                       const CodecOptions& opts, bool* failed) {
    if (failed) *failed = false;
    auto fail = [&]() -> size_t {
        if (failed) *failed = true;
        return 0;
    };
    const schema::NetField* f = cls.field(rpc.field_index);
    if (!f || f->kind != schema::FieldKind::Function || f->rpc >= cls.rpcs.size() || &cls.rpcs[f->rpc] != &rpc)
        return fail();
    BunchWriter payload;
    bool bad = false;
    write_rpc_params(rpc, params, payload, opts, &bad);
    if (bad) return fail();
    return write_field(cls, rpc.field_index, payload, out);
}

std::optional<std::vector<RpcCall>> read_rpc_fields(const schema::ClassSchema& cls, const uint8_t* data, size_t end,
                                                    size_t& pos, const CodecOptions& opts) {
    std::vector<RpcCall> calls;
    size_t at = pos;
    while (at < end) {
        if (calls.size() >= kMaxClientRpcsPerBunch) return std::nullopt;
        const auto h = read_field_header(cls, data, end, at);
        if (!h) return std::nullopt;
        const schema::NetField* f = cls.field(h->field);
        if (!f || f->kind != schema::FieldKind::Function || f->rpc >= cls.rpcs.size()) return std::nullopt;
        RpcCall call;
        call.rpc = &cls.rpcs[f->rpc];
        call.param_pos = at;
        call.param_bits = h->payload_bits;
        size_t p = at;
        if (auto params = read_rpc_params(*call.rpc, data, at + h->payload_bits, p, opts)) {
            call.params = std::move(*params);
            call.decoded = true;
        }
        at += h->payload_bits;
        calls.push_back(std::move(call));
    }
    pos = at;
    return calls;
}

std::optional<ClientBunch> read_client_bunch(const schema::ClassSchema& actor_class, const ResolveSubobject& resolve,
                                             const uint8_t* data, size_t bits, const CodecOptions& opts) {
    ClientBunch out;
    size_t pos = 0;
    while (pos < bits) {
        const auto block = read_content_block(data, bits, pos, /*server_form=*/false);
        if (!block) return std::nullopt;
        const schema::ClassSchema* cls = block->header.is_actor ? &actor_class
                                                                : (resolve ? resolve(block->header.object) : nullptr);
        if (block->header.has_rep_layout || !cls) {
            ++out.skipped_blocks;
            continue;
        }
        size_t at = block->payload_pos;
        auto calls = read_rpc_fields(*cls, data, block->payload_pos + block->payload_bits, at, opts);
        if (!calls || out.rpcs.size() + calls->size() > kMaxClientRpcsPerBunch) return std::nullopt;
        for (RpcCall& c : *calls) {
            ClientRpc r;
            r.on_actor = block->header.is_actor;
            r.object = block->header.object;
            r.cls = cls;
            r.call = std::move(c);
            out.rpcs.push_back(std::move(r));
        }
    }
    return out;
}

}}}  // namespace aoc::protocol::repl

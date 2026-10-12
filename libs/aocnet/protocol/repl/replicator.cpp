// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/replicator.cpp
//
//  See replicator.h.  SerializeNewActor is UPackageMapClient::SerializeNewActor
//  as tools/decode_actor_open.py reads it (all 743 captured opens): the actor,
//  archetype and level GUIDs, then location, rotation, scale and velocity each
//  behind a presence bit (a vector: a bShouldQuantize bit and UE::Net::
//  WriteQuantizedVector; a rotation FRotator's compressed shorts).
// ============================================================================
#include "protocol/repl/replicator.h"

#include "protocol/emit/intrepid_netguid.h"

#include <map>
#include <set>

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;

Subobject* Actor::find(const FIntrepidNetworkGUID& g) {
    for (Subobject& s : subobjects) {
        if (s.guid == g) return &s;
    }
    return nullptr;
}

const Subobject* Actor::find(const FIntrepidNetworkGUID& g) const {
    for (const Subobject& s : subobjects) {
        if (s.guid == g) return &s;
    }
    return nullptr;
}
// (C.Y.B ~ C.O.A)

void ShadowUndo::rollback(ActorShadow& shadow) const {
    if (whole) {
        shadow = *whole;
        return;
    }
    for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
        if (it->is_actor) {
            it->rep.apply(shadow.actor.rep);
            it->fast.apply(shadow.actor.fast);
            shadow.actor.opened = it->was_opened;
            continue;
        }
        const auto sub = shadow.subobjects.find(it->id);
        if (sub == shadow.subobjects.end()) continue;
        if (it->created) {
            shadow.subobjects.erase(sub);
            continue;
        }
        it->rep.apply(sub->second.rep);
        it->fast.apply(sub->second.fast);
        sub->second.opened = it->was_opened;
    }
}

namespace {

/// The object's RepLayout block, then its fast-array fields: the payload of its content block.  The shadow's active
/// mask decides which properties this connection gets.  `rep_undo` / `fast_undo` record what the writes overwrote; on a
/// failure the write that failed left its own shadow as it was, but an earlier write of the object may have committed.
bool write_object_payload(const ObjectState& o, ObjectShadow& sh, BunchWriter& payload, bool& has_rep_layout,
                          RepShadowUndo* rep_undo, FastArrayShadowsUndo* fast_undo) {
    has_rep_layout = false;
    if (!o.schema) return true;
    bool failed = false;
    const size_t rep = write_rep_block(*o.schema, o.state, sh.rep, payload, o.opts, &failed, &sh.active, rep_undo);
    if (failed) return false;
    has_rep_layout = rep != 0;
    write_fast_array_fields(*o.schema, o.arrays, sh.fast, payload, o.opts, &failed, &sh.active, fast_undo);
    return !failed;
}

bool collect_blocks(const Actor& a, ActorShadow& shadow, std::vector<ContentBlock>& out, bool open, ShadowUndo* undo) {
    out.clear();
    ShadowUndo local;
    ShadowUndo& u = undo ? *undo : local;
    u = ShadowUndo{};
    // An open replaces the whole shadow (and is rare): keep it as it was.  An update records only what it touches.
    if (open) {
        u.whole = shadow;
        shadow.forget_sent();
    }
    auto fail = [&]() {
        u.rollback(shadow);
        out.clear();
        return false;
    };
    // An update records, per object, what its writes commit (the record is dropped again for an object that changed
    // nothing).  An open has `whole`.
    ShadowUndo::Object* rec = nullptr;
    auto begin_object = [&](const ObjectShadow& sh, bool is_actor, uint64_t id, bool created) {
        if (open) return;
        u.objects.emplace_back();
        rec = &u.objects.back();
        rec->id = id;
        rec->is_actor = is_actor;
        rec->created = created;
        rec->was_opened = sh.opened;
    };
    auto end_object = [&](const ObjectShadow& sh) {
        if (open) return;
        if (rec->rep.entries.empty() && rec->fast.entries.empty() && !rec->created && rec->was_opened == sh.opened)
            u.objects.pop_back();
        rec = nullptr;
    };
    auto write = [&](ObjectShadow& sh, const ObjectState& obj, ContentBlock& b) {
        return write_object_payload(obj, sh, b.payload, b.has_rep_layout, rec ? &rec->rep : nullptr, rec ? &rec->fast : nullptr);
    };

    {
        ContentBlock b;
        begin_object(shadow.actor, true, 0, false);
        if (!write(shadow.actor, a.obj, b)) return fail();
        end_object(shadow.actor);
        if (b.payload.bit_pos() != 0) out.push_back(std::move(b));
    }
    for (const Subobject& s : a.subobjects) {
        const auto [it, created] = shadow.subobjects.try_emplace(s.guid.ObjectId);
        ObjectShadow& sh = it->second;
        const bool is_new = !sh.opened;   // not seen by the client yet: a fresh entry, or one the caller made for its mask
        ContentBlock b;
        b.sub = &s;
        begin_object(sh, false, s.guid.ObjectId, created);
        if (!write(sh, s.obj, b)) return fail();
        // A subobject the client has not seen gets its block even when it has nothing to say: that is how the
        // client learns of it (pkt#78's OwnerInfo, Backpack, Equipment ...).
        if (is_new || b.payload.bit_pos() != 0) {
            sh.opened = true;
            out.push_back(std::move(b));
        }
        end_object(sh);
    }
    return true;
}
// [C.O.A ~ Cyb]

bool write_conditional_vector(BunchWriter& out, const std::optional<QuantVec>& v) {
    out.write_bit(v.has_value());
    if (!v) return true;
    out.write_bit(1);   // bShouldQuantize: the captured opens all quantize
    return write_quantized_vector(out, *v);
}

/// SerializeNewActor and, for a PlayerController, its OnSerializeNewActor fields.
bool write_new_actor(BunchWriter& out, const Actor& a) {
    const ActorOpen& o = a.open;
    // A static (odd) GUID is exported with its own path and has no archetype, level or transform: not supported.
    if (a.guid.ObjectId == 0 || (a.guid.ObjectId & 1) != 0 || o.archetype.empty() || o.level.empty()) return false;
    emit::write_intrepid_guid(out, a.guid);
    emit::write_intrepid_guid(out, o.archetype.front().guid);
    emit::write_intrepid_guid(out, o.level.front().guid);
    if (!write_conditional_vector(out, o.spawn.location)) return false;
    out.write_bit(o.spawn.rotation.has_value());
    if (o.spawn.rotation && !write_struct("FRotator", Value(*o.spawn.rotation), out)) return false;
    if (!write_conditional_vector(out, o.spawn.scale)) return false;
    if (!write_conditional_vector(out, o.spawn.velocity)) return false;
    if (o.player_controller) {
        // emit::write_player_controller_open_fields (actor_builder.h clashes with the generated PropType).
        out.write_uint8(o.player_controller->net_player_index);
        out.write_uint32(o.player_controller->client_handshake_id);
    }
    return true;
}

/// The chain that exports a subobject when a block names it; empty for a subobject that cannot be named.
ExportChain export_of(const Actor& a, const Subobject& s) {
    if (s.dynamic_class) return *s.dynamic_class;
    if (s.name.empty()) return {};
    return component_export(a.guid, s.guid, s.name, s.checksum);
}

std::optional<BlockHeader> header_of(const ContentBlock& b) {
    BlockHeader h;
    h.has_rep_layout = b.has_rep_layout;
    if (!b.sub) return h;
    h.is_actor = false;
    h.object = b.sub->guid;
    if (b.sub->dynamic_class) {
        if (b.sub->dynamic_class->empty()) return std::nullopt;
        h.stably_named = false;
        h.dynamic_class = b.sub->dynamic_class->front().guid;
        h.outer = b.sub->outer;
    }
    return h;
}

std::optional<Emission> assemble(const Actor& a, const std::vector<ContentBlock>& blocks, const ExportAcks& acks,
                                 bool open) {
    std::vector<ExportChain> chains;
    if (open) {
        chains.push_back(a.open.archetype);
        chains.push_back(a.open.level);
    }
    for (const ContentBlock& b : blocks) {
        if (b.sub) {
            ExportChain c = export_of(a, *b.sub);
            if (c.empty()) return std::nullopt;
            chains.push_back(std::move(c));
        }
        chains.insert(chains.end(), b.exports.begin(), b.exports.end());
    }

    PackedExports packed = pack_exports(chains, acks);
    if (!packed.ok) return std::nullopt;
    Emission e;
    e.exports = std::move(packed.bunches);
    e.exported = std::move(packed.exported);
    e.exported_by_bunch = std::move(packed.exported_by_bunch);
    if (open && !write_new_actor(e.payload, a)) return std::nullopt;
    for (const ContentBlock& b : blocks) {
        const auto h = header_of(b);
        if (!h || write_content_block(e.payload, *h, b.payload) == 0) return std::nullopt;
    }
    return e;
}
// C.O.A x C.Y.B

/// True when `target` is the actor (nullptr) or one of its subobjects: a pointer into Actor::subobjects.
bool belongs_to(const Actor& a, const Subobject* target) {
    if (!target) return true;
    for (const Subobject& s : a.subobjects) {
        if (&s == target) return true;
    }
    return false;
}

}  // namespace

bool collect_open_blocks(const Actor& actor, ActorShadow& shadow, std::vector<ContentBlock>& out, ShadowUndo* undo) {
    return collect_blocks(actor, shadow, out, true, undo);
}

bool collect_update_blocks(const Actor& actor, ActorShadow& shadow, std::vector<ContentBlock>& out, ShadowUndo* undo) {
    return collect_blocks(actor, shadow, out, false, undo);
}

std::optional<Emission> assemble_open(const Actor& actor, const std::vector<ContentBlock>& blocks,
                                      const ExportAcks& acks) {
    return assemble(actor, blocks, acks, true);
}

std::optional<Emission> assemble_blocks(const Actor& actor, const std::vector<ContentBlock>& blocks,
                                        const ExportAcks& acks) {
    if (blocks.empty()) return std::nullopt;
    return assemble(actor, blocks, acks, false);
}

std::optional<Emission> open_actor(const Actor& actor, ActorShadow& shadow, const ExportAcks& acks, bool* failed) {
    if (failed) *failed = false;
    ShadowUndo undo;   // holds the shadow as it was: collect_open_blocks resets it and the assembly may still fail
    std::vector<ContentBlock> blocks;
    if (!collect_open_blocks(actor, shadow, blocks, &undo)) {
        if (failed) *failed = true;
        return std::nullopt;
    }
    auto e = assemble_open(actor, blocks, acks);
    if (!e) {
        undo.rollback(shadow);
        if (failed) *failed = true;
    }
    return e;
}

SentRecord sent_record(const ShadowUndo& undo, const ActorShadow& after) {
    SentRecord out;
    for (const ShadowUndo::Object& u : undo.objects) {
        const ObjectShadow* now = nullptr;
        if (u.is_actor) {
            now = &after.actor;
        } else {
            const auto it = after.subobjects.find(u.id);
            if (it != after.subobjects.end()) now = &it->second;
        }
        if (!now) continue;
        SentObject o;
        o.id = u.is_actor ? 0 : u.id;
        o.is_actor = u.is_actor;
        o.introduced = !u.is_actor && !u.was_opened && now->opened;
        for (const auto& [handle, before] : u.rep.entries) o.handles.push_back(handle);
        for (const auto& [field, before] : u.fast.entries) {
            const auto cur = now->fast.by_field.find(field);
            if (cur == now->fast.by_field.end()) continue;
            std::map<int32_t, int32_t> was;
            if (before) {
                for (const auto& [id, key] : before->items) was[id] = key;
            }
            SentObject::Fast f;
            f.field = field;
            std::set<int32_t> still;
            for (const auto& [id, key] : cur->second.items) {
                still.insert(id);
                const auto w = was.find(id);
                if (w == was.end() || w->second != key) f.changed.push_back(id);
            }
            for (const auto& [id, key] : was) {
                if (still.count(id) == 0) f.removed.push_back(id);
            }
            if (!f.changed.empty() || !f.removed.empty()) o.fast.push_back(std::move(f));
        }
        if (!o.handles.empty() || !o.fast.empty() || o.introduced) out.objects.push_back(std::move(o));
    }
    return out;
}
// -- C | O | A x C.Y.B --

std::optional<Emission> update_actor(const Actor& actor, ActorShadow& shadow, const ExportAcks& acks, bool* failed, SentRecord* record) {
    if (failed) *failed = false;
    ShadowUndo undo;   // the entries the collect touches: nothing when nothing changed
    std::vector<ContentBlock> blocks;
    if (!collect_update_blocks(actor, shadow, blocks, &undo)) {
        if (failed) *failed = true;
        return std::nullopt;
    }
    if (blocks.empty()) return std::nullopt;
    auto e = assemble_blocks(actor, blocks, acks);
    if (!e) {
        undo.rollback(shadow);
        if (failed) *failed = true;
    } else if (record) {
        *record = sent_record(undo, shadow);
    }
    return e;
}

std::optional<Emission> call_rpcs(const Actor& actor, std::span<const RpcRequest> calls, const ExportAcks& acks) {
    std::vector<ContentBlock> blocks;
    bool reliable = false, unreliable = false;
    for (const RpcRequest& c : calls) {
        if (!belongs_to(actor, c.target)) return std::nullopt;
        const ObjectState& obj = c.target ? c.target->obj : actor.obj;
        if (!obj.schema) return std::nullopt;
        const schema::RpcSchema* rpc = obj.schema->rpc(c.rpc);
        if (!rpc) return std::nullopt;
        ((rpc->function_flags & schema::kFuncNetReliable) ? reliable : unreliable) = true;
        ContentBlock b;
        b.sub = c.target;
        b.exports = c.exports;
        bool bad = false;
        if (write_rpc_field(*obj.schema, *rpc, c.params, b.payload, obj.opts, &bad) == 0 || bad) return std::nullopt;
        blocks.push_back(std::move(b));
    }
    if (blocks.empty() || (reliable && unreliable)) return std::nullopt;
    auto e = assemble(actor, blocks, acks, false);
    if (e) e->reliable = reliable;
    return e;
}

std::optional<Emission> call_rpc(const Actor& actor, const Subobject* target, std::string_view rpc_name,
                                 const RpcParams& params, std::span<const ExportChain> extra_exports,
                                 const ExportAcks& acks) {
    RpcRequest r;
    r.target = target;
    r.rpc = rpc_name;
    r.params = params;
    r.exports.assign(extra_exports.begin(), extra_exports.end());
    return call_rpcs(actor, std::span<const RpcRequest>(&r, 1), acks);
}

}}}  // namespace aoc::protocol::repl

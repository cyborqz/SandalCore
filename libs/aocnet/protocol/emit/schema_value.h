// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/schema_value.h
//
//  Runtime value type for schema-described properties.  A SchemaValue holds
//  the current value of a PropertySchema field — e.g. `"Hatemost"` for
//  CharacterName, `17747` for PrimaryArchetype.
//
//  This is a sum-type (variant) keyed by PropType.  The ActorBuilder
//  inspects the schema + the runtime value and emits the correct wire
//  format.
//
//  LAYER:  Protocol / emit (runtime state containers)
//  SESSION: C
//
//  NOTE on correction 1:  SchemaValue is a PURE DATA CONTAINER.  It doesn't
//  know about simulation or replication.  Both the Simulation layer (the
//  "real" value) and Replication state (the "last-sent" value) use the same
//  SchemaValue type; they're compared to compute dirty deltas.
// ============================================================================
#pragma once

#include "protocol/schema/actor_schema.h"
#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>

namespace aoc { namespace protocol { namespace emit {

struct FVector3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
struct FRotator { int16_t pitch = 0, yaw = 0, roll = 0; }; // compressed short representation

/// Runtime value.  Exactly ONE variant is populated per instance; which one
/// must match the PropertySchema's `type` field when emitted.
struct SchemaValue {
    schema::PropType type = schema::PropType::UInt32;

    // Value storage — only one is meaningful per instance.
    bool          b    = false;
    uint64_t      u    = 0;       // covers all uint/int types (up to 64)
    int64_t       i    = 0;
    double        f    = 0.0;
    std::string   str;            // FString, FName
    FVector3      vec;
    FRotator      rot;
    uint64_t      netguid = 0;
    std::vector<uint8_t> bytes;   // ByteArray, CustomDelta raw payload

    // ── Convenient constructors (type-safe) ──
    static SchemaValue make_bool(bool x)       { SchemaValue v; v.type = schema::PropType::Bool;    v.b = x; return v; }
    static SchemaValue make_u8(uint8_t x)      { SchemaValue v; v.type = schema::PropType::UInt8;   v.u = x; return v; }
    static SchemaValue make_u16(uint16_t x)    { SchemaValue v; v.type = schema::PropType::UInt16;  v.u = x; return v; }
    static SchemaValue make_u32(uint32_t x)    { SchemaValue v; v.type = schema::PropType::UInt32;  v.u = x; return v; }
    static SchemaValue make_u64(uint64_t x)    { SchemaValue v; v.type = schema::PropType::UInt64;  v.u = x; return v; }
    static SchemaValue make_i32(int32_t x)     { SchemaValue v; v.type = schema::PropType::Int32;   v.i = x; return v; }
    static SchemaValue make_float(float x)     { SchemaValue v; v.type = schema::PropType::Float;   v.f = x; return v; }
    static SchemaValue make_fstring(std::string x) {
        SchemaValue v; v.type = schema::PropType::FString; v.str = std::move(x); return v;
    }
    static SchemaValue make_netguid(uint64_t x) {
        SchemaValue v; v.type = schema::PropType::NetGUID; v.netguid = x; return v;
    }
    static SchemaValue make_fvector(float x, float y, float z) {
        SchemaValue v; v.type = schema::PropType::FVector; v.vec = {x, y, z}; return v;
    }
    static SchemaValue make_frotator(int16_t p, int16_t y, int16_t r) {
        SchemaValue v; v.type = schema::PropType::FRotator; v.rot = {p, y, r}; return v;
    }
    static SchemaValue make_bytes(std::vector<uint8_t> x) {
        SchemaValue v; v.type = schema::PropType::ByteArray; v.bytes = std::move(x); return v;
    }
    static SchemaValue make_custom_delta(std::vector<uint8_t> x) {
        SchemaValue v; v.type = schema::PropType::CustomDelta; v.bytes = std::move(x); return v;
    }
};
// ~ C.Y.B - CoA ~

/// Runtime state of one actor instance.  Maps property handle → value.
/// Component properties are keyed by (component_index, handle) pair.
///
/// Per architectural correction 1, this struct is the SIMULATION state;
/// Replication state (last-sent values, dirty flags) lives separately in
/// the world/replication layer.
struct ActorRuntime {
    uint64_t netguid = 0;
    schema::ActorType type = schema::ActorType::StaticWorld;

    // Root-actor properties: handle → value
    std::unordered_map<uint32_t, SchemaValue> root_values;

    // Component properties: (component_index << 32 | handle) → value
    //   key = (uint64)component_index * (1ull<<32) + handle
    std::unordered_map<uint64_t, SchemaValue> component_values;

    // Spawn-time info (used by SerializeNewActor)
    // H.3d: each NetGUID is actually an FIntrepidNetworkGUID (ObjectId u64 +
    // ServerId u32 + Randomizer u32).  The legacy `_netguid` field aliases
    // ObjectId; the companion `_server_id` / `_randomizer` fields default to
    // 0 for pre-H.3d call sites.
    uint64_t actor_netguid = 0;         // usually == netguid   (ObjectId)
    uint32_t actor_server_id = 0;       // 0 for local-bootstrap
    uint32_t actor_randomizer = 0;
    uint64_t archetype_netguid = 0;     // shared static, from schema (ObjectId)
    uint32_t archetype_server_id = 0;
    uint32_t archetype_randomizer = 0;
    uint64_t level_netguid = 0;         // shared static, from schema (ObjectId)
    uint32_t level_server_id = 0;
    uint32_t level_randomizer = 0;
    FVector3 spawn_location;
    FRotator spawn_rotation;

    // Transform-flag overrides for SerializeNewActor.  When false (default),
    // the builder decides based on whether the struct contents are non-zero.
    // Setting these explicitly is required to match captures that emit a
    // transform header even for all-zero values (e.g. captured pkt#22).
    bool     serialize_location = false;
    bool     serialize_rotation = false;
    bool     serialize_scale    = false;
    bool     serialize_velocity = false;

    // Session H.3e: quantized-location support.
    // UE5's SerializeNewActor writes the location body via
    // `SerializePackedVector<ScaleFactor, MaxBitsPerComponent>`.  The
    // captured PC spawn uses the quantized path (bQuantizeLocation=1) and
    // encodes scaled integer components.  When `quantize_location=true`,
    // the builder writes the packed-vector form using the integer fields
    // below; otherwise it writes the non-quantized (3×double) form.
    bool     quantize_location         = false;
    int32_t  location_scaled_x         = 0;   // pre-scaled integer
    int32_t  location_scaled_y         = 0;
    int32_t  location_scaled_z         = 0;
    // Per-axis bit count used by the wire format.  Stock UE5 uses 24 for
    // actor-spawn location (matches captured BitsNeeded=24 in pkt#22).
    int32_t  location_max_bits         = 24;

    // (velocity / scale use the same packed-vector shape when quantized;
    // not wired yet because captured has bSerializeScale=bSerializeVelocity=0.)

    // ── Accessors ──
    void set_root(uint32_t handle, SchemaValue v) {
        root_values[handle] = std::move(v);
    }
    void set_component(int component_index, uint32_t handle, SchemaValue v) {
        uint64_t key = (static_cast<uint64_t>(component_index) << 32) | handle;
        component_values[key] = std::move(v);
    }
// -- CoA x C.Y.B --

    const SchemaValue* get_root(uint32_t handle) const {
        auto it = root_values.find(handle);
        return it == root_values.end() ? nullptr : &it->second;
    }
    const SchemaValue* get_component(int component_index, uint32_t handle) const {
        uint64_t key = (static_cast<uint64_t>(component_index) << 32) | handle;
        auto it = component_values.find(key);
        return it == component_values.end() ? nullptr : &it->second;
    }
};

}}} // namespace aoc::protocol::emit

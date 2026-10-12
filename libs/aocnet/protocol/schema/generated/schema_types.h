// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/schema/generated/schema_types.h
//
//  The types the generated class schemas fill in.  Every other file in this
//  directory (except index.cpp's lookup code) is written by
//  tools/ghidra/schemagen.py from the client exe; do not edit those by hand.
//
//  A class schema holds three tables, all flattened super-first so one class
//  header is self-contained:
//
//  RepLayout commands (FRepLayout::InitFromClass, UE 5.6 RepLayout.cpp):
//    UClass::ClassReps lists the CPF_Net properties, super classes first, a
//    native class's in declaration order (UClass::SetUpRuntimeReplicationData
//    sorts by offset only for non-native classes), one entry per static-array
//    element.  InitFromProperty_r gives every leaf a command whose 1-based
//    RelativeHandle is the wire handle: a struct with a native NetSerialize is
//    one command; any other struct is flattened into its non-RepSkip members
//    sorted by offset then name (InitFromStructProperty); a dynamic array is
//    one command and its element commands restart at handle 1, terminated by
//    a Return command.  `commands[handle - 1]` is the command for `handle`.
//
//  Net fields (FClassNetCacheMgr::GetClassNetCache, CoreNet.cpp): the class's
//    own ClassReps properties (a static array is one field), then its own net
//    functions (FUNC_Net without a super function) sorted by name, numbered
//    after the super class's fields.  The field loop after a RepLayout block
//    reads `SerializeInt(field, GetMaxIndex() + 1)` (DataChannel.cpp
//    ReadFieldHeaderAndPayload) where GetMaxIndex() is the total field count:
//    ClassSchema::field_max().
//
//  RPC parameters (FRepLayout::InitFromFunction / SendPropertiesForRPC): the
//    function's CPF_Parm properties in declaration order, each flattened like
//    a class property; every parameter that is not a bool is preceded by one
//    "is set" bit on the wire, a bool is written bare.
//
//  Wire widths (FProperty::NetSerializeItem): bool 1, integers their width,
//    float 32, double 64, an enum CeilLogTwo(GetMaxEnumValue() + 1) bits
//    (FByteProperty/FEnumProperty::GetMaxNetSerializeBits, AoC's net version
//    is past EnumSerializationCompat) where GetMaxEnumValue counts the
//    `<Prefix>_MAX` entry UEnum::SetEnums adds when the enumerators have
//    none (ENetRole's explicit ROLE_MAX = 5 gives 3 bits; ETabTargetMethod's
//    0..3 plus the implicit MAX = 4 gives 3 bits too, as the captured PC
//    settings block shows), an object reference AoC's 128-bit
//    FIntrepidNetworkGUID, FVector three doubles (net version past
//    SerializeDoubleVectorsAsDoubles).  0 means variable: names, strings,
//    arrays and NetSerialize structs with their own codec.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace aoc { namespace protocol { namespace schema {

/// Leaf property kinds as UECodeGen's EPropertyGenFlags name them.
enum class PropType : uint8_t {
    Bool,
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float, Double,
    Enum,                 // FByteProperty/FEnumProperty with a UEnum; see enum_name
    Name, Str, Text,
    Object,               // UObject*, TSubclassOf, interfaces, weak/lazy pointers: a NetGUID
    SoftObject,           // FSoftObjectPath as a string
    NetSerializeStruct,   // one command; codec by struct_name
    Struct,               // RpcParam only: a flattened struct spanning several commands
    DynamicArray,         // uint16 count, then element commands per element
    Unknown,              // a kind the generator does not model (maps, sets, delegates)
};
// C O A x C-Y-B

/// Flags of a top-level replicated property (FRepParentCmd flags).
enum RepFlags : uint8_t {
    kRepNone        = 0,
    kRepCustomDelta = 1,  // struct with NetDeltaSerialize: sent through the field loop, not the RepLayout block
    kRepFastArray   = 2,  // ... and derives from FFastArraySerializer
    kRepNotify      = 4,  // CPF_RepNotify
    kRepStaticArray = 8,  // ArrayDim > 1: one field, ArrayDim commands sets
};

struct RepCommand {
    uint16_t    handle;       // 1-based; class commands: index + 1; element commands: within one element
    uint16_t    parent;       // class commands: ClassReps index of the top-level property; RPC commands: parameter index
    const char* path;         // dotted from the property, e.g. "AttachmentReplication.AttachParent"; static arrays "Name[i]"
    const char* owner;        // class declaring the top-level property
    PropType    type;
    uint8_t     flags;        // RepFlags of the top-level property
    uint16_t    width_bits;   // fixed wire width; 0 = variable
    const char* struct_name;  // NetSerializeStruct: "FVector", "FRepMovement", ...; else nullptr
    const char* enum_name;    // Enum: "ENetRole", ...; else nullptr
    uint16_t    elem_begin;   // DynamicArray: element commands [elem_begin, elem_end) in the owner's element_commands
    uint16_t    elem_end;
};

enum class FieldKind : uint8_t { Property, Function };

constexpr uint16_t kNoRpc = 0xFFFF;

struct NetField {
    uint16_t    index;           // FClassNetCache index (FFieldNetCache::FieldNetIndex)
    const char* name;
    const char* owner;           // class declaring the field
    FieldKind   kind;
    uint8_t     flags;           // RepFlags (properties)
    uint32_t    function_flags;  // EFunctionFlags (functions); 0 for properties
    uint16_t    handle;          // properties: handle of the property's first command; functions: 0
    uint16_t    rpc;             // functions: index into ClassSchema::rpcs; properties: kNoRpc
};

struct RpcParam {
    const char* name;
    PropType    type;
    uint16_t    width_bits;      // fixed width of a single-command parameter; 0 otherwise
    const char* struct_name;     // NetSerializeStruct or flattened Struct name
    bool        send_bit;        // preceded by a 1-bit "is set" (every type but bool)
    uint16_t    cmd_begin;       // commands [cmd_begin, cmd_end) in RpcSchema::commands
    uint16_t    cmd_end;
};

struct RpcSchema {
    const char*                 name;
    const char*                 owner;
    uint32_t                    function_flags;
    uint16_t                    field_index;
    std::span<const RpcParam>   params;
    std::span<const RepCommand> commands;          // handles 1..n across all parameters
    std::span<const RepCommand> element_commands;  // arrays in the parameters
};

/// EFunctionFlags bits the RPC tables carry (UE 5.6 ObjectMacros.h).
constexpr uint32_t kFuncNet          = 0x00000040;
constexpr uint32_t kFuncNetReliable  = 0x00000080;
constexpr uint32_t kFuncNetMulticast = 0x00004000;
constexpr uint32_t kFuncNetServer    = 0x00200000;
constexpr uint32_t kFuncNetClient    = 0x01000000;

struct ClassSchema {
    const char* name;              // C++ name, e.g. "AAoCPlayerController"
    const char* super_name;        // e.g. "APlayerController"; "UObject" at the root
    bool        is_actor;          // derives from AActor (else a subobject/component class)
    uint16_t    first_own_handle;  // first handle declared by this class (commands.size() + 1 if none)
    uint16_t    first_own_field;   // first field declared by this class (fields.size() if none)

    std::span<const RepCommand> commands;          // index == handle - 1
    std::span<const RepCommand> element_commands;  // referenced by RepCommand::elem_begin/end
    std::span<const NetField>   fields;            // index == field index
    std::span<const RpcSchema>  rpcs;
    std::span<const uint16_t>   commands_by_path;  // handles sorted by path
    std::span<const uint16_t>   fields_by_name;    // field indices sorted by name

    /// FClassNetCache::GetMaxIndex(): the number of net fields.
    uint32_t num_fields() const { return static_cast<uint32_t>(fields.size()); }
    /// The max for `SerializeInt(field, field_max())` in the field loop.
    uint32_t field_max() const { return num_fields() + 1; }

    const RepCommand* command(uint32_t handle) const {
        return handle >= 1 && handle <= commands.size() ? &commands[handle - 1] : nullptr;
    }
    const RepCommand* command(std::string_view path) const {
        size_t lo = 0, hi = commands_by_path.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            const int c = std::string_view(commands[commands_by_path[mid] - 1].path).compare(path);
            if (c == 0) return &commands[commands_by_path[mid] - 1];
            if (c < 0) lo = mid + 1; else hi = mid;
        }
        return nullptr;
    }
    /// Element commands of a DynamicArray command.
    std::span<const RepCommand> elements(const RepCommand& array) const {
        if (array.type != PropType::DynamicArray || array.elem_end > element_commands.size()) return {};
        return element_commands.subspan(array.elem_begin, array.elem_end - array.elem_begin);
    }
    const NetField* field(uint32_t index) const {
        return index < fields.size() ? &fields[index] : nullptr;
    }
    const NetField* field(std::string_view name) const {
        size_t lo = 0, hi = fields_by_name.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            const int c = std::string_view(fields[fields_by_name[mid]].name).compare(name);
            if (c == 0) return &fields[fields_by_name[mid]];
            if (c < 0) lo = mid + 1; else hi = mid;
        }
        return nullptr;
    }
    const RpcSchema* rpc(std::string_view name) const {
        const NetField* f = field(name);
        return f && f->rpc != kNoRpc && f->rpc < rpcs.size() ? &rpcs[f->rpc] : nullptr;
    }
};

}}}  // namespace aoc::protocol::schema

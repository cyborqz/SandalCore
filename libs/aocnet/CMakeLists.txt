# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
# ============================================================================
#  libaocnet — AoC / IntrepidNet protocol library
#
#  Wire parsing, bunch/RPC encoders, class schemas and NetGUID allocation,
#  extracted from the legacy emulator.  Boundary: may include only the C++
#  standard library, spdlog, and headers under libs/aocnet.
# ============================================================================

add_library(aocnet STATIC
    # conn (connection protocol: sequences, acks, handshake, framing)
    protocol/conn/handshake.cpp
    protocol/conn/handshake.h
    protocol/conn/nmt.cpp
    protocol/conn/nmt.h
    protocol/conn/packet_notify.cpp
    protocol/conn/packet_notify.h
    protocol/conn/packet_framing.cpp
    protocol/conn/packet_framing.h
    protocol/conn/sequence.h
    # wire
    protocol/wire/packet_parser.cpp
    protocol/wire/bunch_reassembler.cpp
    protocol/wire/actor_rpc_parser.h
    protocol/wire/bunch_reassembler.h
    protocol/wire/bunch_types.h
    protocol/wire/hit_confirmation.h
    protocol/wire/level_visibility.h
    protocol/wire/packet_info.h
    protocol/wire/packet_parser.h
    protocol/wire/packet_reader.h
    protocol/wire/server_move_parser.h
    protocol/wire/ue5_primitives.h
    # schema
    protocol/schema/actor_schema.cpp
    protocol/schema/schema_registry.cpp
    protocol/schema/pc_schema.cpp
    protocol/schema/pawn_schema.cpp
    protocol/schema/player_state_schema.cpp
    protocol/schema/actor_rep_handles.h
    protocol/schema/actor_schema.h
    protocol/schema/schema_registry.h
    # schema/generated — class schemas from the client exe
    protocol/schema/generated/schema_types.h
    protocol/schema/generated/index.h
    protocol/schema/generated/index.cpp
    # repl — the replicator core: values, per-object state, NetSerialize codecs, RepLayout blocks, fast arrays, RPCs, exports and content blocks, the replicator
    protocol/repl/value.cpp
    protocol/repl/rep_state.cpp
    protocol/repl/codecs.cpp
    protocol/repl/struct_codec_registry.cpp
    protocol/repl/rep_block.cpp
    protocol/repl/fast_array.cpp
    protocol/repl/rpc.cpp
    protocol/repl/move_packed.cpp
    protocol/repl/ability_rpc.cpp
    protocol/repl/item_codec.cpp
    protocol/repl/item_rpc.cpp
    protocol/repl/hit_codec.cpp
    protocol/repl/hit_rpc.cpp
    protocol/repl/leveling.cpp
    protocol/repl/skill_rpc.cpp
    protocol/repl/storage_rpc.cpp
    protocol/repl/subobject.cpp
    protocol/repl/replicator.cpp
    protocol/repl/send_ledger.cpp
    protocol/repl/block_reader.cpp
    protocol/repl/vendor_codec.cpp
    protocol/repl/callout_codec.cpp
    protocol/repl/channeling_codec.cpp
    protocol/repl/vendor_rpc.cpp
    protocol/repl/dialogue_rpc.cpp
    protocol/repl/node_rpc.cpp
    protocol/repl/node_layout.cpp
    protocol/repl/value.h
    protocol/repl/rep_state.h
    protocol/repl/codecs.h
    protocol/repl/struct_codec_registry.h
    protocol/repl/rep_block.h
    protocol/repl/fast_array.h
    protocol/repl/rpc.h
    protocol/repl/move_packed.h
    protocol/repl/ability_rpc.h
    protocol/repl/item_codec.h
    protocol/repl/item_rpc.h
    protocol/repl/hit_codec.h
    protocol/repl/hit_rpc.h
    protocol/repl/leveling.h
    protocol/repl/skill_rpc.h
    protocol/repl/rpc_fields.h
    protocol/repl/storage_rpc.h
    protocol/repl/subobject.h
    protocol/repl/replicator.h
    protocol/repl/send_ledger.h
    protocol/repl/block_reader.h
    protocol/repl/vendor_codec.h
    protocol/repl/callout_codec.h
    protocol/repl/channeling_codec.h
    protocol/repl/vendor_rpc.h
    protocol/repl/dialogue_rpc.h
    protocol/repl/node_rpc.h
    protocol/repl/node_layout.h
    protocol/repl/warehouse_codec.h
    protocol/repl/warehouse_rpc.h
    # emit
    protocol/emit/actor_builder.cpp
    protocol/emit/package_map_exporter.cpp
    protocol/emit/name_update_bunch.cpp
    protocol/emit/actor_builder.h
    protocol/emit/actor_bunch.h
    protocol/emit/bunch_writer.h
    protocol/emit/intrepid_netguid.h
    protocol/emit/name_update_bunch.h
    protocol/emit/net_serializers.h
    protocol/emit/package_map_exporter.h
    protocol/emit/rep_layout_writer.h
    protocol/emit/schema_value.h
    # emit/replayout — RepLayout property codecs
    protocol/emit/replayout/catalog.cpp
    protocol/emit/replayout/encoder.cpp
    protocol/emit/replayout/decoder.cpp
    protocol/emit/replayout/encoders/fstring_codec.cpp
    protocol/emit/replayout/encoders/scalar_codec.cpp
    protocol/emit/replayout/encoders/fobject_codec.cpp
    protocol/emit/replayout/encoders/fstruct_codec.cpp
    protocol/emit/replayout/catalog.h
    protocol/emit/replayout/decoder.h
    protocol/emit/replayout/encoder.h
    protocol/emit/replayout/property_type.h
    protocol/emit/replayout/property_value.h
    protocol/emit/replayout/encoders/fstring_codec.h
    protocol/emit/replayout/encoders/scalar_codec.h
    protocol/emit/replayout/encoders/fobject_codec.h
    protocol/emit/replayout/encoders/fstruct_codec.h
    # NetGUIDs
    protocol/net_guid_allocator.h
    protocol/server_guid_allocator.h
)

# Self-registering feature struct codecs under protocol/repl/codecs/*_codec.{h,cpp}.
# Each <feature>_codec.cpp defines `void register_<feature>_codecs();` in
# `namespace aoc::protocol::repl` and is called directly from the generated
# `registered_feature_codecs.inc` so static-library linking never drops it.
file(GLOB AOCNET_FEATURE_CODEC_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/repl/codecs/*_codec.cpp"
)
file(GLOB AOCNET_FEATURE_CODEC_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/repl/codecs/*_codec.h"
)
if(AOCNET_FEATURE_CODEC_SOURCES)
    list(SORT AOCNET_FEATURE_CODEC_SOURCES)
endif()
if(AOCNET_FEATURE_CODEC_HEADERS)
    list(SORT AOCNET_FEATURE_CODEC_HEADERS)
endif()

set(_aocnet_codec_decls "")
set(_aocnet_codec_calls "")
foreach(_codec_src IN LISTS AOCNET_FEATURE_CODEC_SOURCES)
    get_filename_component(_codec_name "${_codec_src}" NAME_WLE)
    string(REGEX REPLACE "_codec$" "" _codec_feature "${_codec_name}")
    if(NOT _codec_feature MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
            "Invalid feature codec filename '${_codec_src}': stem '${_codec_feature}' "
            "must match ^[A-Za-z_][A-Za-z0-9_]*$")
    endif()
    string(APPEND _aocnet_codec_decls "void register_${_codec_feature}_codecs();\n")
    string(APPEND _aocnet_codec_calls "    register_${_codec_feature}_codecs();\n")
endforeach()
if(_aocnet_codec_decls)
    string(APPEND _aocnet_codec_decls "\n")
endif()

set(_aocnet_generated_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/protocol/repl/registered_feature_codecs.inc")
file(CONFIGURE
    OUTPUT "${_aocnet_generated_inc}"
    CONTENT
"// Generated by libs/aocnet/CMakeLists.txt - do not edit by hand.
namespace aoc { namespace protocol { namespace repl {

@_aocnet_codec_decls@inline void register_all_feature_codecs() {
@_aocnet_codec_calls@}

}}}  // namespace aoc::protocol::repl
"
    @ONLY
)

if(AOCNET_FEATURE_CODEC_SOURCES OR AOCNET_FEATURE_CODEC_HEADERS)
    target_sources(aocnet PRIVATE
        ${AOCNET_FEATURE_CODEC_SOURCES}
        ${AOCNET_FEATURE_CODEC_HEADERS}
    )
endif()

# PUBLIC so consumers keep writing #include "protocol/wire/..." unchanged.
target_include_directories(aocnet
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(aocnet PUBLIC spdlog::spdlog)
target_compile_features(aocnet PUBLIC cxx_std_20)
if(MSVC)
    target_compile_definitions(aocnet PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
# ue5_primitives.h calls std::min inline; keep <windows.h>'s min/max macros out of it.
target_compile_definitions(aocnet INTERFACE $<$<PLATFORM_ID:Windows>:NOMINMAX>)
set_target_properties(aocnet PROPERTIES FOLDER "Libs")

add_subdirectory(tests)

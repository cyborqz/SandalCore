# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
# libaocnet tests — plain executables that print [ok ]/[FAIL] and exit 0 on
# success.  Output lands in dist/<Config>/ like every other target.

function(aocnet_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE aocnet)
    # Fixtures live under src/protocol/tools; PROJECT_SOURCE_DIR is the repo
    # root (CMAKE_CURRENT_SOURCE_DIR would be this directory).
    target_compile_definitions(${name} PRIVATE AOC_REPO_ROOT="${PROJECT_SOURCE_DIR}")
    if(MSVC)
        target_compile_definitions(${name} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
    endif()
    set_target_properties(${name} PROPERTIES FOLDER "Tests/aocnet")
endfunction()

foreach(t
    test_cs_packet_info
    test_level_visibility
    test_pc_bootstrap
    test_possession
    test_rep_layout_writer
    test_sc_packet_info
    test_serialize_new_actor
    test_hit_confirmation
    test_schemas
    test_replayout_codecs
    test_pkt22_round_trip
    test_pkt104_round_trip
    test_name_update_bunch
    test_property_update_bunch_builder
    test_pawn_spawn_diff
    test_actor_builder
    test_nmt_welcome
    test_nmt_challenge
    test_nmt_netguid_assign
    test_pc_spawn_diff
    test_package_map_export
    test_packet_notify
    test_handshake
    test_packet_framing
    test_server_guid_allocator
    test_generated_schemas
    test_repl_codecs
    test_repl_rep_block
    test_repl_fast_array
    test_repl_aoc_codecs
    test_repl_rpc
    test_repl_move_packed
    test_repl_ability_rpc
    test_repl_items
    test_repl_item_rpc
    test_repl_hits
    test_repl_leveling
    test_repl_actor_open
    test_repl_npc
    test_repl_send_ledger
    test_repl_vendor
    test_repl_loot
    test_repl_mount
    test_repl_nodes
    test_repl_node_layout
)
    aocnet_add_test(${t})
endforeach()

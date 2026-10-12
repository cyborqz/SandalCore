# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
# ============================================================================
#  worldd: the C++ world server (plan A4: the connection layer; plan A5: the native bootstrap, bootstrap/, and the
#  player session after NMT_Join, session/; plan A6: the move checks and the saving of positions, session/)
#
#  worldd_core holds everything but main() so the tests can link it.
#  PROTOC and GRPC_CPP_PLUGIN come from the top-level CMakeLists.
#
#  content/content_db reads content.db (plan C1's SQLite file) read-only: the vcpkg sqlite3 port
#  (vcpkg.json) gives unofficial::sqlite3::sqlite3.
# ============================================================================

find_package(unofficial-sqlite3 CONFIG REQUIRED)

# ── gRPC stubs for the internal services worldd calls ───────────────────────
set(WORLDD_PROTO_DIR "${PROJECT_SOURCE_DIR}/proto/internal")
set(WORLDD_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${WORLDD_PROTO_GEN_DIR})
set(WORLDD_PROTOS
    aoc/character/v1/character.proto
    aoc/citynode/v1/citynode.proto
    aoc/gateway/v1/gateway.proto
    aoc/mesh/v1/mesh.proto
    aoc/worldadmin/v1/worldadmin.proto
)
set(WORLDD_PROTO_SRCS "")
foreach(rel ${WORLDD_PROTOS})
    string(REGEX REPLACE "[.]proto$" "" base ${rel})
    set(outs
        "${WORLDD_PROTO_GEN_DIR}/${base}.pb.cc"
        "${WORLDD_PROTO_GEN_DIR}/${base}.pb.h"
        "${WORLDD_PROTO_GEN_DIR}/${base}.grpc.pb.cc"
        "${WORLDD_PROTO_GEN_DIR}/${base}.grpc.pb.h")
    add_custom_command(
        OUTPUT ${outs}
        COMMAND ${PROTOC}
            "--proto_path=${WORLDD_PROTO_DIR}"
            "--cpp_out=${WORLDD_PROTO_GEN_DIR}"
            "--grpc_out=${WORLDD_PROTO_GEN_DIR}"
            "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
            "${WORLDD_PROTO_DIR}/${rel}"
        DEPENDS "${WORLDD_PROTO_DIR}/${rel}"
        COMMENT "Generating protobuf/gRPC stubs for ${rel}"
    )
    list(APPEND WORLDD_PROTO_SRCS ${outs})
endforeach()

add_library(worldd_protos STATIC ${WORLDD_PROTO_SRCS})
target_include_directories(worldd_protos PUBLIC "${WORLDD_PROTO_GEN_DIR}")
target_link_libraries(worldd_protos PUBLIC protobuf::libprotobuf gRPC::grpc++)
if(MSVC)
    target_compile_options(worldd_protos PRIVATE /W0)
else()
    target_compile_options(worldd_protos PRIVATE -w)
endif()
set_target_properties(worldd_protos PROPERTIES FOLDER "Servers/worldd")

# ── stats: the stat engine (plan B3), a library of its own (no other worldd or net code) ─────────────────
add_library(worldd_stats STATIC
    stats/character_stats.cpp
    stats/character_stats.h
    stats/damage.cpp
    stats/damage.h
    stats/gear.cpp
    stats/gear.h
    stats/stat_engine.cpp
    stats/stat_engine.h
    stats/stat_program.cpp
    stats/stat_program.h
)
target_include_directories(worldd_stats PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(worldd_stats PROPERTIES FOLDER "Servers/worldd")

# ── nav: the area's ground (plan E1, stage 2): Recast and Detour (third_party/recastnavigation, zlib licence) with the nav file, the offline
#    builder and the run-time queries.  aoc_navbuild is the tool tools/mapc runs; worldd and the tests link worldd_nav.
set(RECAST_DIR "${PROJECT_SOURCE_DIR}/third_party/recastnavigation")
add_library(recast_detour STATIC
    ${RECAST_DIR}/Recast/Source/Recast.cpp
    ${RECAST_DIR}/Recast/Source/RecastAlloc.cpp
    ${RECAST_DIR}/Recast/Source/RecastArea.cpp
    ${RECAST_DIR}/Recast/Source/RecastAssert.cpp
    ${RECAST_DIR}/Recast/Source/RecastContour.cpp
    ${RECAST_DIR}/Recast/Source/RecastFilter.cpp
    ${RECAST_DIR}/Recast/Source/RecastLayers.cpp
    ${RECAST_DIR}/Recast/Source/RecastMesh.cpp
    ${RECAST_DIR}/Recast/Source/RecastMeshDetail.cpp
    ${RECAST_DIR}/Recast/Source/RecastRasterization.cpp
    ${RECAST_DIR}/Recast/Source/RecastRegion.cpp
    ${RECAST_DIR}/Detour/Source/DetourAlloc.cpp
    ${RECAST_DIR}/Detour/Source/DetourAssert.cpp
    ${RECAST_DIR}/Detour/Source/DetourCommon.cpp
    ${RECAST_DIR}/Detour/Source/DetourNavMesh.cpp
    ${RECAST_DIR}/Detour/Source/DetourNavMeshBuilder.cpp
    ${RECAST_DIR}/Detour/Source/DetourNavMeshQuery.cpp
    ${RECAST_DIR}/Detour/Source/DetourNode.cpp
)
target_include_directories(recast_detour PUBLIC "${RECAST_DIR}/Recast/Include" "${RECAST_DIR}/Detour/Include")
if(MSVC)
    target_compile_options(recast_detour PRIVATE /W0)
else()
    target_compile_options(recast_detour PRIVATE -w)
endif()
set_target_properties(recast_detour PROPERTIES FOLDER "Servers/worldd")

add_library(worldd_nav STATIC
    nav/area_nav.cpp
    nav/area_nav.h
    nav/nav_build.cpp
    nav/nav_build.h
    nav/nav_file.cpp
    nav/nav_file.h
)
target_include_directories(worldd_nav PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(worldd_nav PUBLIC recast_detour)
set_target_properties(worldd_nav PROPERTIES FOLDER "Servers/worldd")

add_executable(aoc_navbuild nav/navbuild_main.cpp)
target_link_libraries(aoc_navbuild PRIVATE worldd_nav)
set_target_properties(aoc_navbuild PROPERTIES FOLDER "Servers")

# ── ability_content: the ability tables of content.db (plan D2), a connection of its own ───────────────────
add_library(worldd_ability_content STATIC
    content/ability_content.cpp
    content/ability_content.h
)
target_include_directories(worldd_ability_content PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(worldd_ability_content PUBLIC
    worldd_stats
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
)
set_target_properties(worldd_ability_content PROPERTIES FOLDER "Servers/worldd")

# ── worldd_core ─────────────────────────────────────────────────────────────
add_library(worldd_core STATIC
    bootstrap/ability_replication.h
    bootstrap/bootstrap.cpp
    bootstrap/bootstrap.h
    bootstrap/captured_abilities.h
    bootstrap/captured_bits.h
    bootstrap/captured_character.h
    bootstrap/captured_gamestate.h
    bootstrap/captured_level_list.h
    bootstrap/captured_replies.h
    bootstrap/captured_setup.cpp
    bootstrap/captured_setup.h
    bootstrap/captured_stats.h
    bootstrap/character_setup.cpp
    bootstrap/character_setup.h
    bootstrap/constants.cpp
    bootstrap/constants.h
    bootstrap/mount_ride.cpp
    bootstrap/player_setup.cpp
    bootstrap/player_setup.h
    combat/aftermath.cpp
    combat/aftermath.h
    combat/combat_data.cpp
    combat/combat_data.h
    combat/combat_world.cpp
    combat/combat_world.h
    combat/fighter.h
    combat/hit_context.cpp
    combat/hit_context.h
    combat/hit_resolver.cpp
    combat/hit_resolver.h
    combat/npc_kits.h
    combat/npc_replicator.cpp
    combat/npc_replicator.h
    combat/npc_stats.cpp
    combat/npc_stats.h
    combat/rewards.cpp
    combat/rewards.h
    content/content_db.cpp
    content/content_db.h
    items/inventory.cpp
    items/inventory.h
    login/login_service.cpp
    login/login_service.h
    login/login_worker.cpp
    login/login_worker.h
    net/connection.cpp
    net/connection.h
    net/control_channel.cpp
    net/control_channel.h
    net/cookie.cpp
    net/cookie.h
    net/endpoint.cpp
    net/endpoint.h
    net/udp_socket.cpp
    net/udp_socket.h
    session/abilities.cpp
    session/abilities.h
    session/actor_view.cpp
    session/actor_view.h
    session/ability_catalog.cpp
    session/ability_catalog.h
    session/ability_pipeline.cpp
    session/ability_pipeline.h
    session/action_bar.cpp
    session/action_bar.h
    session/gm_travel.cpp
    session/gm_travel.h
    session/item_use.cpp
    session/item_use.h
    session/leveling.cpp
    session/leveling.h
    session/mounts.cpp
    session/mounts.h
    session/move_checker.cpp
    session/move_checker.h
    session/player_session.cpp
    session/player_session.h
    session/player_world.cpp
    session/player_world.h
    session/position_saver.cpp
    session/position_saver.h
    session/riding.cpp
    session/riding.h
    session/talents.cpp
    session/talents.h
    svc/city_node_control.cpp
    svc/city_node_control.h
    svc/city_node_feed.cpp
    svc/city_node_feed.h
    svc/grpc_services.cpp
    svc/grpc_services.h
    svc/service_token.cpp
    svc/service_token.h
    svc/services.h
    svc/world_admin.cpp
    svc/world_admin.h
    spawns/area.h
    spawns/area_loader.cpp
    spawns/area_loader.h
    spawns/area_spawns.h
    spawns/captured_spawns.cpp
    spawns/captured_spawns.h
    spawns/corpse_exports.h
    spawns/generic_npc.cpp
    spawns/gm_garden.cpp
    spawns/gm_garden.h
    spawns/garden_defs.h
    spawns/generic_npc.h
    spawns/mount_donors.h
    spawns/mount_spawn.cpp
    spawns/mount_spawn.h
    spawns/mount_state.cpp
    spawns/mount_state.h
    spawns/node_population.cpp
    spawns/node_population.h
    spawns/node_populations.cpp
    spawns/node_populations.h
    spawns/population.cpp
    spawns/population.h
    spawns/region_data.h
    spawns/region_index.cpp
    spawns/region_index.h
    spawns/spawn_filter.cpp
    spawns/spawn_filter.h
    spawns/spawn_util.h
    vendor/gm_vendor.cpp
    vendor/gm_vendor.h
    vendor/node_tax.cpp
    vendor/node_tax.h
    vendor/node_tax_table.h
    world/city_nodes.cpp
    world/city_nodes.h
    world/node_control.h
    world/node_layout_data.cpp
    world/node_layout_data.h
    world/node_origins.h
    world/node_scenery.cpp
    world/node_scenery.h
    world/node_wire.cpp
    world/node_wire.h
    world/town_floor.cpp
    world/town_floor.h
    world/corpse.cpp
    world/corpse.h
    world/actor_registry.cpp
    world/actor_registry.h
    world/interaction_state.cpp
    world/interaction_state.h
    world/places.cpp
    world/places.h
    world/relevancy.h
    world/roads.cpp
    world/roads.h
    world/zoi_surface.cpp
    world/zoi_surface.h
    world/zoi_surface_table.h
    npc/ai/npc_ai.cpp
    npc/ai/npc_ai.h
    npc/ai/threat.cpp
    npc/ai/threat.h
    npc/area_npcs.cpp
    npc/area_npcs.h
    npc/motion_table.cpp
    npc/motion_table.h
    npc/mover.cpp
    npc/mover.h
    npc/npc_world.cpp
    npc/npc_world.h
)
target_include_directories(worldd_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(worldd_core PUBLIC
    aocnet
    worldd_nav
    worldd_protos
    worldd_stats
    worldd_ability_content
    spdlog::spdlog
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    $<$<PLATFORM_ID:Windows>:ws2_32>
)
if(MSVC)
    target_compile_definitions(worldd_core PUBLIC
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
endif()
set_target_properties(worldd_core PROPERTIES FOLDER "Servers/worldd")

# ── worldd ──────────────────────────────────────────────────────────────────
add_executable(worldd main.cpp)
target_link_libraries(worldd PRIVATE worldd_core CLI11::CLI11)
set_target_properties(worldd PROPERTIES FOLDER "Servers")

# ── tests: plain executables that print [ok ]/[FAIL], like libaocnet's ──────
function(worldd_add_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE worldd_core)
    # capture_packets.h lives with the libaocnet tests.
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/libs/aocnet/tests)
    # the shared stat corpus (plan B3): the tests that build a content.db fixture take the stat program from it
    target_compile_definitions(${name} PRIVATE
        STATS_CORPUS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/stats/testdata/corpus.txt"
        TALENT_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/talent_fixture.sql")
    set_target_properties(${name} PROPERTIES FOLDER "Tests/worldd")
endfunction()

foreach(t
    test_worldd_abilities
    test_worldd_ability_session
    test_worldd_actor_nak
    test_worldd_actor_view
    test_worldd_area_session
    test_worldd_bootstrap
    test_worldd_channels
    test_worldd_character_setup
    test_worldd_connection
    test_worldd_content_db
    test_worldd_control_channel
    test_worldd_cookie
    test_worldd_endpoint
    test_worldd_items
    test_worldd_items_session
    test_worldd_leveling
    test_worldd_login
    test_worldd_move_checker
    test_worldd_movement
    test_worldd_npc_ai
    test_worldd_nav
    test_worldd_nav_window
    test_worldd_npc_move
    test_worldd_repl_acks
    test_worldd_services
    test_worldd_session
    test_worldd_talents
    test_worldd_udp_socket
)
    worldd_add_test(${t})
endforeach()

# ============================================================================
#  the stat engine's test (plan B3): the interpreter, the gear rules and the character's stats on the corpus
#  services/internal/statprog writes (stats/testdata/corpus.txt)
# ============================================================================
add_executable(test_worldd_stats stats/tests/test_worldd_stats.cpp)
target_link_libraries(test_worldd_stats PRIVATE worldd_stats)
target_compile_definitions(test_worldd_stats PRIVATE
    STATS_CORPUS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/stats/testdata/corpus.txt")
set_target_properties(test_worldd_stats PROPERTIES FOLDER "Tests/worldd")

# the ability tables' reader (plan D2) on the content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB), with the stat engine
add_executable(test_worldd_ability_content tests/test_worldd_ability_content.cpp)
target_link_libraries(test_worldd_ability_content PRIVATE worldd_ability_content worldd_stats)
target_compile_definitions(test_worldd_ability_content PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
set_target_properties(test_worldd_ability_content PROPERTIES FOLDER "Tests/worldd")

# the living area (plan E1) on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB)
worldd_add_test(test_worldd_area)
target_compile_definitions(test_worldd_area PRIVATE
    AREA_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")

# the Riverlands as the area (G1 stage 1, Part B): the index, the population against an independent oracle, the region's load, the GM garden
worldd_add_test(test_worldd_region)
target_compile_definitions(test_worldd_region PRIVATE
    REGION_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    REGION_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

# the scale test (G1 stage 1, Part B): the full region and synthetic sessions crossing it; needs the chunked area_nav.bin (it skips without), run it alone
worldd_add_test(test_worldd_region_load)
target_compile_definitions(test_worldd_region_load PRIVATE
    LOAD_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    LOAD_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv"
    LOAD_NAV_PATH="${PROJECT_SOURCE_DIR}/dist/content/area_nav.bin")

# the mounts (the mounts milestone, stage 2): the mount actor, its controller and the session on the real content.db of the working tree
worldd_add_test(test_worldd_mounts)
target_compile_definitions(test_worldd_mounts PRIVATE
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv"
    MOUNT_RIDERS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_riders.tsv")
worldd_add_test(test_worldd_mounts_session)
target_compile_definitions(test_worldd_mounts_session PRIVATE
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_RIDERS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_riders.tsv")
worldd_add_test(test_worldd_mounts_fly)
target_compile_definitions(test_worldd_mounts_fly PRIVATE
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv")
worldd_add_test(test_worldd_mounts_ride)
target_compile_definitions(test_worldd_mounts_ride PRIVATE
    MOUNT_ROADS_PATH="${PROJECT_SOURCE_DIR}/dist/content/roads.tsv"
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv"
    MOUNT_RIDERS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_riders.tsv")

# the GM travel tools (teleport and run speed): the session, the client's GM RPCs and the admin API on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB)
worldd_add_test(test_worldd_gm_travel)
target_compile_definitions(test_worldd_gm_travel PRIVATE
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv")

# the city nodes (Nodes 1, stage 1): the registry and the whole-world ZOI lookup on the real content.db of the working tree, the node reads against the shared corpus's
# nodehost blocks, the seed fallback, and the citynode feed against an in-process fake of the service
worldd_add_test(test_worldd_city_nodes)
target_compile_definitions(test_worldd_city_nodes PRIVATE
    NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NODES_SEED_PATH="${PROJECT_SOURCE_DIR}/deploy/citynode_seed.json")

# the nodes the client is told of (Nodes 1, stage 2): the maps, summaries and sieges of the registry's levels against capture 1198's, the session's answers (their order, the
# live update, the GM console) and the GM control over gRPC against an in-process fake of the node service
worldd_add_test(test_worldd_node_state)
target_compile_definitions(test_worldd_node_state PRIVATE
    NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NODES_SEED_PATH="${PROJECT_SOURCE_DIR}/deploy/citynode_seed.json"
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv")

# the towns' scenery (Nodes 1, stage 3a): the items of each town's replicator built from the real content.db against the 18 retail payloads of the captures, the replicator actors
# (their place, a level change as a new full payload), the actor view opening and closing them by range, and the world following the node levels
worldd_add_test(test_worldd_node_layout)
target_compile_definitions(test_worldd_node_layout PRIVATE
    NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NODES_SEED_PATH="${PROJECT_SOURCE_DIR}/deploy/citynode_seed.json")

# the towns' people (Nodes 1, stage 3b): the selection of each node's NPCs at each level against the Python oracle's golden counts (tests/node_pop_golden.h), the live manager (a level change
# spawns and despawns the right sets, a respawn follows the level, the ground's pending rules), the any-of siege rule and the roads by node level, on the real content.db
worldd_add_test(test_worldd_node_pop)
target_compile_definitions(test_worldd_node_pop PRIVATE
    NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NODES_SEED_PATH="${PROJECT_SOURCE_DIR}/deploy/citynode_seed.json"
    NODES_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv"
    NODES_ROADS_LEVELS_PATH="${PROJECT_SOURCE_DIR}/dist/content/roads_levels.tsv")

# the spawn filter (plan E1, stage 3) on the same content.db and dist/content/npc_motion.tsv
worldd_add_test(test_worldd_spawn_filter)
target_compile_definitions(test_worldd_spawn_filter PRIVATE
    AREA_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")

# effect state, expression programs and their host (plan D2), on the same corpus (its fxcase blocks)
add_executable(test_worldd_effects stats/tests/test_worldd_effects.cpp)
target_link_libraries(test_worldd_effects PRIVATE worldd_stats)
target_compile_definitions(test_worldd_effects PRIVATE
    STATS_CORPUS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/stats/testdata/corpus.txt")
set_target_properties(test_worldd_effects PROPERTIES FOLDER "Tests/worldd")

# the combat core (plan E1, stage 3): NPC stats, damage and the hit rules on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB)
worldd_add_test(test_worldd_combat)
target_compile_definitions(test_worldd_combat PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

# what follows a death (plan E1, stage 3, part 2): the experience, the loot, the corpse and the respawn on the real content.db
worldd_add_test(test_worldd_aftermath)
target_compile_definitions(test_worldd_aftermath PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

# what a kill hands out (plan E1, stage 3, part 2): the reward tables of the real content.db and dist/content/npc_motion.tsv
worldd_add_test(test_worldd_rewards)
target_compile_definitions(test_worldd_rewards PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

# the generic ability runtime (plan D2) on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB)
worldd_add_test(test_worldd_ability_pipeline)
target_compile_definitions(test_worldd_ability_pipeline PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
worldd_add_test(test_worldd_ability_retail)
target_compile_definitions(test_worldd_ability_retail PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
worldd_add_test(test_worldd_ability_all)
target_compile_definitions(test_worldd_ability_all PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
# every ability of every class against the client's tooltip (plan E1, stage 3 fix, Part C): tests/tooltip_key.h is written by tools/capture/gen_tooltips.py
worldd_add_test(test_worldd_tooltips)
target_compile_definitions(test_worldd_tooltips PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
# the class abilities that need another actor against the retail captures, on a creature of the world (plan E1, stage 3, part 2)
worldd_add_test(test_worldd_ability_npc)
target_compile_definitions(test_worldd_ability_npc PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")
target_compile_definitions(test_worldd_ability_session PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

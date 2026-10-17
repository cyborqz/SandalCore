# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
cmake_minimum_required(VERSION 3.22)

project(AshesOfCreation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ── Output directory ────────────────────────────────────────────────────────
set(AOC_DIST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH
    "Canonical output root: binaries land in <AOC_DIST_ROOT>/<config>/")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${AOC_DIST_ROOT}/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${AOC_DIST_ROOT}/Release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${AOC_DIST_ROOT}/RelWithDebInfo")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${AOC_DIST_ROOT}/MinSizeRel")
message(STATUS "AshesOfCreation dist/ root: ${AOC_DIST_ROOT}")

# ── Game data the build includes ─────────────────────────────────────────
# scripts/extract_game_data.ps1 (extract_game_data.bat) makes these from your own Ashes of Creation install; they are git-ignored.
set(AOC_GENERATED_FILES
    server/worldd/world/zoi_surface_table.h
    server/worldd/world/node_constants.h
    server/worldd/spawns/region_data.h
    server/worldd/spawns/garden_defs.h
    server/worldd/stats/testdata/corpus.txt
    server/worldd/tests/talent_fixture.sql
    server/worldd/tests/gear_fixture.h
    server/worldd/tests/tooltip_key.h
)
set(AOC_MISSING_GENERATED "")
foreach(f ${AOC_GENERATED_FILES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
        list(APPEND AOC_MISSING_GENERATED ${f})
    endif()
endforeach()
if(AOC_MISSING_GENERATED)
    list(JOIN AOC_MISSING_GENERATED "\n    " AOC_MISSING_TEXT)
    message(FATAL_ERROR "These files are made from your own Ashes of Creation install and are missing:\n    ${AOC_MISSING_TEXT}\nRun extract_game_data.bat (it builds all the game data), then build again.")
endif()

# ── Dependencies ────────────────────────────────────────────────────────────
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "grpc_cpp_plugin not found (install via vcpkg grpc)")
endif()
find_program(PROTOC protoc)
if(NOT PROTOC)
    message(FATAL_ERROR "protoc not found (install via vcpkg protobuf)")
endif()

# ── Proto codegen ───────────────────────────────────────────────────────────
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_FILES
    "${PROTO_DIR}/ics_common.proto"
    "${PROTO_DIR}/ics_auth.proto"
    "${PROTO_DIR}/ics_launcher.proto"
    "${PROTO_DIR}/ics_patcher.proto"
    "${PROTO_DIR}/ics_patcher_v2.proto"
    "${PROTO_DIR}/ics_tether.proto"
    "${PROTO_DIR}/ics_xclient.proto"
)

set(PROTO_GENERATED_SRCS "")
set(PROTO_GENERATED_HDRS "")

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(PB_CC   "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc")
    set(PB_H    "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h")
    set(GRPC_CC "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_H  "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.h")
    add_custom_command(
        OUTPUT ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
        COMMAND ${PROTOC}
            "--proto_path=${PROTO_DIR}"
            "--cpp_out=${PROTO_GEN_DIR}"
            "--grpc_out=${PROTO_GEN_DIR}"
            "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
            "${PROTO_FILE}"
        DEPENDS "${PROTO_FILE}"
        COMMENT "Generating protobuf/gRPC stubs for ${PROTO_NAME}"
    )
    list(APPEND PROTO_GENERATED_SRCS ${PB_CC} ${GRPC_CC})
    list(APPEND PROTO_GENERATED_HDRS ${PB_H}  ${GRPC_H})
endforeach()

add_library(ics_protos STATIC ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS})
target_include_directories(ics_protos PUBLIC "${PROTO_GEN_DIR}")
target_link_libraries(ics_protos PUBLIC protobuf::libprotobuf gRPC::grpc++)
if(MSVC)
    target_compile_options(ics_protos PRIVATE /W0)
else()
    target_compile_options(ics_protos PRIVATE -w)
endif()

# ── libaocnet: protocol library (wire / emit / schema / NetGUIDs) ──────────
add_subdirectory(libs/aocnet)

# ── worldd: the C++ world server (plan A4) ──────────────────────────────────
add_subdirectory(server/worldd)

# ── Server executable ───────────────────────────────────────────────────────
set(SERVER_SOURCES
    src/main.cpp
    src/services/auth/auth_service.cpp
    src/services/launcher/launcher_service.cpp
    src/services/tether/tether_service.cpp
    src/services/xclient/xclient_service.cpp
    src/generators/noop_generator.cpp
    src/generators/recorded_generator.cpp
    src/protocol/bootstrap/bootstrap_sequence.cpp
    src/protocol/bootstrap/pc_spawn_parser.cpp
    src/protocol/actors/player_controller.cpp
    # Phase II — replay mutator (rewrites captured bunches with live state)
    src/protocol/emit/replay_mutator.cpp
    # Path B — authoritative-server connect orchestrator (M1.0 scaffolding)
    src/net/native_connect_sequencer.cpp
    # Path B — M1.1 bootstrap emitter (ch=0 control bunches before PC spawn)
    src/net/bootstrap_emitter.cpp
    # Path B — M1.2 PC emitter (PlayerController ActorOpen + props)
    src/net/pc_emitter.cpp
    # Path B — M1.4.b Pawn emitter (spliced captured pkt#78 bunch stream)
    src/net/pawn_emitter.cpp
    # PM39 (2026-04-30) — Path C Phase 2: native player Pawn ActorOpen
    src/net/player_pawn_emitter.cpp
    # PM106 (2026-05-04) — Phase D Step 2.2: pkt#78 verbatim splice for
    # 8-subobject PlayerPawn ActorOpen (alongside synthetic emit_open)
    src/net/player_pawn_splicer.cpp
    # Phase D Step 2 (2026-05-04) — Appearance property update emitter
    src/net/appearance_emitter.cpp
    # Phase D Step 2.1 (2026-05-05) — Customization JSON parser + UE5 wire serializer
    # Uses Dumper-7 SDK layout to produce the exact bytes the client's
    # OnRep_CharacterCustomization expects.
    src/net/appearance_data.cpp
    # Phase D Step 3 (2026-05-05) — PlayerState ActorOpen emitter
    # Carries character identity (PlayerName/Archetype) so mesh assembler
    # has the data it needs.  Probe-gated, default disabled.
    src/net/player_state_emitter.cpp
    # Road A — Phase B.0: WorldBootstrapEmitter — single driver of the
    # post-NMT bootstrap stream (replaces the hybrid+native parallel-thread
    # model with one ordered plan that mixes splice + native emission).
    src/net/world_bootstrap_emitter.cpp
    src/net/world_bootstrap_plan.cpp
    # Path B — M2 bootstrap framework (CharacterProfile + NetGUIDAllocator + PacketRecipe)
    src/net/bootstrap/packet_recipe.cpp
    # Path B — M2 live server state (WorldState + CharacterState + 10Hz tick)
    src/net/world_state/world_state.cpp
    src/world/events/world_events.cpp
    src/world/simulation/actor_registry.cpp
    src/world/replication/visibility_manager.cpp
    src/world/replication/broadcast_manager.cpp
    # Session E — session layer
    src/net/session_registry.cpp
    src/net/opcode_dispatcher.cpp
    # Session F — live-world integration shim
    src/net/udp_packet_emitter.cpp
    src/net/live_world.cpp
)

set(SERVER_HEADERS
    src/services/auth/auth_service.h
    src/services/launcher/launcher_service.h
    src/services/tether/tether_service.h
    src/services/xclient/xclient_service.h
    src/net/game_server.h
    src/net/ue5_replication.h
    src/net/bunch_generator.h
    src/generators/generators.h
    src/data/bootstrap_data.h
    src/util/proxy_logger.h
    src/protocol/bootstrap/bootstrap_sequence.h
    src/protocol/bootstrap/bootstrap_data.h
    src/protocol/actors/actor_base.h
    src/protocol/actors/actor_manifest.h
    src/protocol/actors/player_controller.h
    # Orphan actor metadata headers removed from CMake (audit Finding 3):
    #   characters.h, npcs.h, game_state.h, interactables.h
    # These were declared but never included by any .cpp.  They still exist
    # on disk as design references for the future actor builders (Session B+);
    # re-add them here once corresponding .cpp implementations land.
    src/protocol/character_profile.h
    src/protocol/bunch_builder.h
    src/protocol/emit/replay_mutator.h
    src/world/events/world_events.h
    src/world/simulation/world_clock.h
    src/world/simulation/simulation_actor.h
    src/world/simulation/actor_registry.h
    src/world/replication/packet_emitter.h
    src/world/replication/visibility_manager.h
    src/world/replication/broadcast_manager.h
    # Session E — session layer
    src/net/client_session.h
    src/net/session_registry.h
    src/net/opcode_dispatcher.h
    # Session F — live-world integration shim
    src/net/udp_packet_emitter.h
    src/net/live_world.h
)

add_executable(aoc_server ${SERVER_SOURCES} ${SERVER_HEADERS})

target_include_directories(aoc_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PROTO_GEN_DIR}"
)

target_link_libraries(aoc_server
    PRIVATE
        aocnet
        ics_protos
        gRPC::grpc++
        gRPC::grpc++_reflection
        protobuf::libprotobuf
        OpenSSL::SSL
        OpenSSL::Crypto
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        CLI11::CLI11
)

if(WIN32)
    target_link_libraries(aoc_server PRIVATE ws2_32 bcrypt)
endif()

target_compile_features(aoc_server PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(aoc_server PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
    target_compile_options(aoc_server PRIVATE
        /wd4244  # conversion uint64_t -> uint32_t in generated protobuf
        /wd4267  # size_t -> uint32_t in generated code
        /wd4146  # minus operator on unsigned (spdlog)
    )
endif()

# ── Tether server (standalone UDP ARQ executable) ───────────────────────────
add_executable(tether_server
    src/tether_server_main.cpp
    src/services/tether/tether_service.h
)

target_include_directories(tether_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(tether_server
    PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        ws2_32
)

target_compile_features(tether_server PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(tether_server PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
endif()

set_target_properties(tether_server PROPERTIES FOLDER "Servers")

# -- Launcher HTTP auth server ------------------------------------------------
# Minimal local HTTP backend used by launcher.exe for /api/v1/auth/login and
# /api/v1/auth/register. The real client-facing auth/xclient gRPC services
# still live in aoc_server.
add_executable(auth_server
    src/auth_server_main.cpp
)

target_include_directories(auth_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(auth_server
    PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        ws2_32
)

target_compile_features(auth_server PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(auth_server PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
endif()

set_target_properties(auth_server PROPERTIES FOLDER "Servers")

# ── Launcher GUI application ─────────────────────────────────────────────────
# Self-contained Win32 GUI.  Reads config\paths.json for the game exe path
# and config\server.json for auth_server_host/auth_server_port/tether_port.
# No gRPC or protobuf dependency — only WinInet (wininet.lib) + nlohmann_json.
if(WIN32)
    add_executable(launcher WIN32
        src/launcher_main.cpp
        src/services/tether/ics_token_acl.cpp
    )
    target_include_directories(launcher PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(launcher
        PRIVATE
            nlohmann_json::nlohmann_json
            wininet
            comdlg32
            advapi32
            shell32
    )
    target_compile_features(launcher PRIVATE cxx_std_20)
    target_compile_definitions(launcher PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
    set_target_properties(launcher PROPERTIES FOLDER "Tools")
endif()

# ── replay_rpcs: RPCs the original server sent on one actor channel ─────────
add_executable(replay_rpcs src/tools/replay_rpcs.cpp)
target_link_libraries(replay_rpcs PRIVATE aocnet)
set_target_properties(replay_rpcs PROPERTIES FOLDER "Tools")

# ── replay_bunches: the original server's raw bunches as JSON lines ──────────
add_executable(replay_bunches src/tools/replay_bunches.cpp)
target_link_libraries(replay_bunches PRIVATE aocnet)
set_target_properties(replay_bunches PROPERTIES FOLDER "Tools")

# ── native_bunch_dump: the native bootstrap's bunches, generated offline ─────
add_executable(native_bunch_dump
    src/tools/native_bunch_dump.cpp
    src/net/world_bootstrap_emitter.cpp
    src/net/world_bootstrap_plan.cpp
    src/net/bootstrap_emitter.cpp
    src/net/pc_emitter.cpp
    src/net/pawn_emitter.cpp
    src/net/player_pawn_emitter.cpp
    src/net/player_pawn_splicer.cpp
    src/net/appearance_emitter.cpp
    src/net/appearance_data.cpp
    src/net/player_state_emitter.cpp
)
target_include_directories(native_bunch_dump PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(native_bunch_dump PRIVATE aocnet)
if(WIN32)
    target_link_libraries(native_bunch_dump PRIVATE ws2_32)
endif()
set_target_properties(native_bunch_dump PROPERTIES FOLDER "Tools")

# ── test_player_controller_builder (Phase 3.6 smoke test) ───────────────────
# Standalone executable that calls the PlayerController builder with a
# default profile and prints diagnostics about the output.  Compiles the
# builder + its dependencies directly (no server runtime needed).
add_executable(test_player_controller_builder
    src/tools/test_player_controller_builder.cpp
    src/protocol/actors/player_controller.cpp
)
target_compile_features(test_player_controller_builder PRIVATE cxx_std_20)
target_include_directories(test_player_controller_builder PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(MSVC)
    target_compile_definitions(test_player_controller_builder PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
set_target_properties(test_player_controller_builder PROPERTIES FOLDER "Tools")

# ── test_packet_parser (Session A byte-identity test) ────────────────────────
# Standalone executable that parses known captured packets through the new
# C++ packet_parser and verifies structure matches our Python reference.
# Exit criterion for the live-server implementation plan's Session A.
add_executable(test_packet_parser
    src/tools/test_packet_parser.cpp
)
target_link_libraries(test_packet_parser PRIVATE aocnet)
target_compile_features(test_packet_parser PRIVATE cxx_std_20)
target_include_directories(test_packet_parser PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(MSVC)
    target_compile_definitions(test_packet_parser PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
set_target_properties(test_packet_parser PROPERTIES FOLDER "Tools")

# test_ics_token_file: the launcher/tether login-token hand-off (plan A3)
add_executable(test_ics_token_file
    src/tools/test_ics_token_file.cpp
    src/services/tether/ics_token_acl.cpp
)
target_include_directories(test_ics_token_file PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(test_ics_token_file PRIVATE cxx_std_20)
if(MSVC)
    target_compile_definitions(test_ics_token_file PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(test_ics_token_file PROPERTIES FOLDER "Tools")

# ── test_replay_mutator (Phase II ReplayMutator library tests) ────────────────
add_executable(test_replay_mutator
    src/tools/test_replay_mutator.cpp
    src/protocol/emit/replay_mutator.cpp
)
target_compile_features(test_replay_mutator PRIVATE cxx_std_20)
target_include_directories(test_replay_mutator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(test_replay_mutator PRIVATE aocnet)
if(MSVC)
    target_compile_definitions(test_replay_mutator PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
set_target_properties(test_replay_mutator PROPERTIES FOLDER "Tools")

# ── test_simulation (Session D exit-criterion test) ──────────────────────────
# Verifies the world/simulation layer operates in isolation: event bus,
# actor registry, mutations fire correct events with correct ordering.
# No replication layer instantiated — per architectural correction 1.
add_executable(test_simulation
    src/tools/test_simulation.cpp
    src/world/events/world_events.cpp
    src/world/simulation/actor_registry.cpp
)
target_compile_features(test_simulation PRIVATE cxx_std_20)
target_include_directories(test_simulation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(test_simulation PRIVATE aocnet)
if(MSVC)
    target_compile_definitions(test_simulation PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
set_target_properties(test_simulation PROPERTIES FOLDER "Tools")

# ── test_replication (Session D' exit-criterion test) ────────────────────────
# Wires simulation + replication end-to-end with a mock PacketEmitter to
# verify event-driven packet generation flows correctly.
add_executable(test_replication
    src/tools/test_replication.cpp
    src/world/events/world_events.cpp
    src/world/simulation/actor_registry.cpp
    src/world/replication/visibility_manager.cpp
    src/world/replication/broadcast_manager.cpp
)
target_compile_features(test_replication PRIVATE cxx_std_20)
target_include_directories(test_replication PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(test_replication PRIVATE aocnet)
if(MSVC)
    target_compile_definitions(test_replication PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
set_target_properties(test_replication PROPERTIES FOLDER "Tools")

# ── test_dispatcher (Session E exit-criterion test) ──────────────────────────
# Drives the OpcodeDispatcher with a sequence of simulated packets and
# verifies the ClientSession advances through its phases in the correct
# order.  Proves the Session-Layer state machine is wired correctly before
# real wire-format parsing gets plugged in at Session F.
add_executable(test_dispatcher
    src/tools/test_dispatcher.cpp
    src/net/session_registry.cpp
    src/net/opcode_dispatcher.cpp
    src/world/events/world_events.cpp
)
target_compile_features(test_dispatcher PRIVATE cxx_std_20)
target_include_directories(test_dispatcher PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(test_dispatcher PRIVATE aocnet)
if(MSVC)
    target_compile_definitions(test_dispatcher PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
set_target_properties(test_dispatcher PROPERTIES FOLDER "Tools")

# ── test_live_server (Session F exit-criterion test) ─────────────────────────
# Wires up LiveWorld end-to-end with SessionRegistry + OpcodeDispatcher +
# ActorRegistry + VisibilityManager + BroadcastManager + (RecordingEmitter or
# UdpPacketEmitter) and drives it through a realistic client lifecycle.  Proves
# that when Config::enable_live_world is set on GameServer, the integrated
# pipeline works end-to-end without the UDP stack — handshake → login → join →
# spawn → delta → destroy all flow through the correct components.
add_executable(test_live_server
    src/tools/test_live_server.cpp
    src/net/session_registry.cpp
    src/net/opcode_dispatcher.cpp
    src/net/udp_packet_emitter.cpp
    src/net/live_world.cpp
    src/world/events/world_events.cpp
    src/world/simulation/actor_registry.cpp
    src/world/replication/visibility_manager.cpp
    src/world/replication/broadcast_manager.cpp
)
target_compile_features(test_live_server PRIVATE cxx_std_20)
target_include_directories(test_live_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(test_live_server PRIVATE aocnet)
if(MSVC)
    target_compile_definitions(test_live_server PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
set_target_properties(test_live_server PROPERTIES FOLDER "Tools")

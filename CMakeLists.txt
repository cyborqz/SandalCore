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
# worldd generates the gRPC stubs of the internal services it calls (server/worldd/CMakeLists.txt, from proto/internal) with the PROTOC
# and GRPC_CPP_PLUGIN found above.  The Go services generate theirs from proto/ (services/buf.gen.yaml).

# ── libaocnet: protocol library (wire / emit / schema / NetGUIDs) ──────────
add_subdirectory(libs/aocnet)

# ── worldd: the C++ world server (plan A4) ──────────────────────────────────
add_subdirectory(server/worldd)

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

# test_launcher_saved_sign_in: "Keep me signed in" for launcher.exe (src/launcher_sign_in.h): the Windows Credential Manager
# round trip under a test-only target name, how the auth server's answers are read, and the automatic sign-in at start
# (fake clock, fake auth server)
if(WIN32)
    add_executable(test_launcher_saved_sign_in
        src/tools/test_launcher_saved_sign_in.cpp
    )
    target_include_directories(test_launcher_saved_sign_in PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_launcher_saved_sign_in PRIVATE nlohmann_json::nlohmann_json)
    target_compile_features(test_launcher_saved_sign_in PRIVATE cxx_std_20)
    target_compile_definitions(test_launcher_saved_sign_in PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    set_target_properties(test_launcher_saved_sign_in PROPERTIES FOLDER "Tools")
endif()

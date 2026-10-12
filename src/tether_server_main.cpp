// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
/// ============================================================================
/// tether_server — standalone UDP ARQ tether entry point
/// ============================================================================
///
/// Reads config/server.json and config/paths.json relative to exe directory,
/// then runs the UDP ARQ tether server on the configured port.
///
/// Usage:  tether_server.exe [port] [--login-token-file]
///   port                overrides the value in server.json (default 19021 if
///                       absent): the first argument that is not the flag.
///   --login-token-file  hand a game client on this machine the login token
///                       launcher.exe saved in data\ics_auth_token.txt (the Go
///                       stack, scripts\launch_go.bat). Without it the client
///                       gets the emulator token and the file is never read.
/// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>   // GetModuleFileNameA, SetConsoleTitleA

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <nlohmann/json.hpp>

#include "services/tether/tether_service.h"
#include "services/tether/ics_token_file.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// ─── Globals for signal handling ────────────────────────────────────────────

static TetherServiceImpl* g_tether = nullptr;

static void signal_handler(int) {
    spdlog::info("[Tether] Shutting down...");
    if (g_tether) g_tether->stop();
}

// ─── Config helpers ──────────────────────────────────────────────────────────

static std::string exe_dir() {
    char buf[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string d(buf);
    const auto slash = d.find_last_of("\\/");
    return (slash != std::string::npos) ? d.substr(0, slash + 1) : "";
}
// -- Cyb x C.O.A --

static nlohmann::json load_json(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    try {
        nlohmann::json j;
        f >> j;
        return j;
    } catch (...) { return {}; }
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // ── Logging ──────────────────────────────────────────────────────────────
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    fs::create_directories("logs");
    auto now_t  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now_t);
#else
    localtime_r(&now_t, &tm);
#endif
    char ts[32]; std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        std::string("logs/tether-") + ts + ".log", true);
    auto logger = std::make_shared<spdlog::logger>("tether",
        spdlog::sinks_init_list{console, file_sink});
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::flush_on(spdlog::level::info);

    ::SetConsoleTitleA("Tether Server  -  AshesOfCreation Emulator");

    // ── Config ───────────────────────────────────────────────────────────────
    const std::string dir      = exe_dir();
    auto server_cfg  = load_json(dir + "config\\server.json");
    auto paths_cfg   = load_json(dir + "config\\paths.json");

    uint16_t    tether_port = 19021;
    std::string ics_host    = "127.0.0.1";
    uint16_t    ics_port    = 443;

    if (!server_cfg.is_null()) {
        if (server_cfg.contains("tether_port") && server_cfg["tether_port"].is_number_integer())
            tether_port = static_cast<uint16_t>(server_cfg["tether_port"].get<int>());
        if (server_cfg.contains("ics_host") && server_cfg["ics_host"].is_string())
            ics_host = server_cfg["ics_host"].get<std::string>();
        if (server_cfg.contains("ics_port") && server_cfg["ics_port"].is_number_integer())
            ics_port = static_cast<uint16_t>(server_cfg["ics_port"].get<int>());
        spdlog::info("Loaded config/server.json");
    } else {
        spdlog::warn("config/server.json not found — using defaults");
    }

    // CLI: --login-token-file anywhere; the first other argument is the port.
    bool        login_token_file = false;
    const char* port_arg         = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--login-token-file")
            login_token_file = true;
        else if (!port_arg)
            port_arg = argv[i];
    }
    if (port_arg) {
        try {
            int p = std::stoi(port_arg);
            if (p > 0 && p <= 65535) tether_port = static_cast<uint16_t>(p);
        } catch (...) {
            spdlog::error("Invalid port argument '{}', ignoring", port_arg);
        }
    }
    const std::string token_file = login_token_file ? aoc::ics_token::path_in(dir) : std::string{};

    // ── Banner ───────────────────────────────────────────────────────────────
    spdlog::info("================================================================");
    spdlog::info("  Tether Server  -  AshesOfCreation Emulator");
    spdlog::info("================================================================");
    spdlog::info("  UDP port  : {}", tether_port);
    spdlog::info("  ICS target: {}:{}", ics_host, ics_port);
    if (login_token_file)
        spdlog::info("  Login token: saved tokens enabled, read from {} (clients on this machine only)", token_file);
    else
        spdlog::info("  Login token: saved tokens disabled, the emulator token (--login-token-file enables them)");
    spdlog::info("================================================================");
// [Cyb + C-O-A]

    // ── Start ────────────────────────────────────────────────────────────────
    TetherServiceImpl tether;
    g_tether = &tether;
    tether.set_ics_endpoint(ics_host, ics_port);
    tether.set_token_file(token_file);

    if (!tether.start(tether_port)) {
        spdlog::error("Failed to start tether server on UDP:{}", tether_port);
        return 1;
    }

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    spdlog::info("Press Ctrl+C to stop.");

    // Block main thread until stop() is called from signal handler
    while (tether.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    spdlog::info("Tether server stopped.");
    return 0;
}

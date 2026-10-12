// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
/// ============================================================================
/// AOC Server Emulator - Main Entry Point
/// ============================================================================
///
/// Emulates the Intrepid Studios backend services for Ashes of Creation.
/// Implements AuthService and LauncherService via gRPC, with optional TLS.
///
/// Usage:
///   aoc-server-emu [OPTIONS]
///
/// Options:
///   --port PORT              gRPC listen port (default: 443)
///   --game-ip IP             Game server IP to return to client (default: 127.0.0.1)
///   --game-port PORT         Game server port (default: 7229)
///   --tls-cert FILE          TLS certificate PEM file
///   --tls-key FILE           TLS private key PEM file
///   --no-tls                 Disable TLS (use insecure channel)
///   --branch-id ID           Game branch ID (default: main)
///   --log-level LEVEL        Log level: trace/debug/info/warn/error (default: info)
///

#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/support/server_interceptor.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <CLI/CLI.hpp>

#include <fstream>
#include <string>
#include <memory>
#include <vector>
#include <csignal>
#include <cstdlib>
#include <filesystem>

// ─── gRPC interceptor to log ALL incoming requests ──────────────────────────
// (C O A + Cyb)

class LoggingInterceptor : public grpc::experimental::Interceptor {
public:
    explicit LoggingInterceptor(grpc::experimental::ServerRpcInfo* info) {
        if (info) {
            spdlog::info("[gRPC] >>> Incoming RPC: {}", info->method());
        }
    }

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override {
        methods->Proceed();
    }
};

class LoggingInterceptorFactory
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override {
        return new LoggingInterceptor(info);
    }
};

#include "util/proxy_logger.h"
#include "services/auth/auth_service.h"
#include "services/launcher/launcher_service.h"
#include "services/xclient/xclient_service.h"
#include "net/game_server.h"

namespace fs = std::filesystem;

// ─── Globals for signal handling ────────────────────────────────────────────

static std::unique_ptr<grpc::Server> g_server;

static void signal_handler(int signum) {
    spdlog::info("Received signal {}, shutting down...", signum);
    if (g_server) {
        g_server->Shutdown();
    }
}

// ─── TLS helpers ────────────────────────────────────────────────────────────

static std::string read_file_contents(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

/// Generate a self-signed certificate using OpenSSL CLI (if available)
static bool generate_self_signed_cert(const std::string& cert_path,
                                       const std::string& key_path) {
    spdlog::info("Generating self-signed TLS certificate...");

    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout \"" + key_path
                    + "\" -out \"" + cert_path
                    + "\" -days 365 -nodes -subj \"/CN=localhost/O=AoC-Emu\" 2>&1";

    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        spdlog::error("Failed to generate self-signed certificate. "
                      "Make sure OpenSSL is installed and in PATH.");
        return false;
    }

    spdlog::info("Certificate generated: {} / {}", cert_path, key_path);
    return true;
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Setup logging — console + timestamped file in logs/
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    // Generate timestamp-based log filename
    auto now_t = std::chrono::system_clock::now();
    auto now_tt = std::chrono::system_clock::to_time_t(now_t);
    std::tm now_tm{};
#ifdef _WIN32
    localtime_s(&now_tm, &now_tt);
#else
    localtime_r(&now_tt, &now_tm);
#endif
    char ts_buf[64];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y%m%d-%H%M%S", &now_tm);
    std::string log_ts(ts_buf);

    fs::create_directories("logs");
    std::string log_filename = "logs/emu-" + log_ts + ".log";
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_filename, true);

    auto logger = std::make_shared<spdlog::logger>("multi",
        spdlog::sinks_init_list{console_sink, file_sink});
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::flush_on(spdlog::level::info);  // flush on every info+ message
    spdlog::info("Log file: {}", log_filename);
// ~ C:Y:B - CoA ~

    // ── CLI parsing ─────────────────────────────────────────────────────────
    CLI::App app{"AOC Server Emulator - Ashes of Creation backend emulation"};

    int         grpc_port      = 443;
    std::string game_ip        = "127.0.0.1";
    int         game_port      = 7777;
    std::string tls_cert_path  = "";
    std::string tls_key_path   = "";
    bool        no_tls         = false;
    std::string branch_id      = "main";
    std::string log_level      = "info";
    std::string game_exe       = "AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe";
    std::string game_path      = "";

    app.add_option("--port", grpc_port, "gRPC listen port")->default_val(443);
    app.add_option("--game-ip", game_ip, "Game server IP returned to client")->default_val("127.0.0.1");
    app.add_option("--game-port", game_port, "Game server port")->default_val(7777);
    app.add_option("--tls-cert", tls_cert_path, "TLS certificate PEM file");
    app.add_option("--tls-key", tls_key_path, "TLS private key PEM file");
    app.add_flag("--no-tls", no_tls, "Disable TLS (insecure)");
    app.add_option("--branch-id", branch_id, "Game branch ID")->default_val("main");
    app.add_option("--log-level", log_level, "Log level")->default_val("info");
    app.add_option("--game-exe", game_exe, "Game executable relative path")->default_val("AOC\\Binaries\\Win64\\AOCClient-Win64-Shipping.exe");
    app.add_option("--game-path", game_path, "Game installation path");

    std::string packet_log_path = "";
    app.add_option("--packet-log", packet_log_path,
        "Structured JSONL packet log file (auto-timestamped if empty)");

    bool use_ds = false;
    app.add_flag("--ds", use_ds,
        "External DS mode: skip UDP GameServer (real UE5 DS handles port 7777)");

    std::string replay_file = "";
    app.add_option("--replay", replay_file,
        "Replay mode: replay captured S>C packets from binary file (e.g. replay_data.bin)");

    uint32_t replay_max_packets = 0;
    app.add_option("--replay-max-packets", replay_max_packets,
        "Truncate replay after N packets (0 = unlimited). Use for bootstrap-only tests.");

    // Road A — Phase B.0 (2026-04-26): pure-native bootstrap.
    // With --native, replay is normally LOADED (so the WorldBootstrapEmitter
    // can splice captured bytes via send_captured_packet) AND the legacy
    // replay_loop thread also runs in parallel.  This flag turns OFF the
    // legacy thread so the NativeConnectSequencer is the SOLE driver of
    // the post-NMT bootstrap stream.
    bool no_replay_loop = false;
    app.add_flag("--no-replay-loop", no_replay_loop,
        "(--native only) Load --replay file for splice access but do NOT "
        "start the legacy replay_loop thread.  Pure-native = "
        "--native --replay <file> --no-replay-loop.");

    std::string custom_name = "";
    app.add_option("--custom-name", custom_name,
        "Character name for native mode (--native).  In replay mode this "
        "is ignored (replay sends the captured name).  Default = empty "
        "(native mode falls back to 'PlayerOne').");

    // ── Tier-1 property overrides (replay-mode byte patches) ─────────
    // All default to -1 (skip).  See docs/TIER-1-PROPERTY-PATCHER.md.
    int32_t custom_level       = -1;
    int32_t custom_hp_current  = -1;
    int32_t custom_hp_max      = -1;
    int32_t custom_mp_current  = -1;
    int32_t custom_mp_max      = -1;
    int32_t custom_stamina_max = -1;
    int32_t custom_gold        = -1;
    int32_t custom_xp_current  = -1;
    int32_t custom_str         = -1;
    int32_t custom_dex         = -1;
    int32_t custom_int         = -1;
    int32_t custom_vit         = -1;
    int32_t custom_class_id    = -1;
    int32_t custom_race_id     = -1;
    app.add_option("--custom-level",       custom_level,       "Character level override (-1=skip, 1-60)");
    app.add_option("--custom-hp",          custom_hp_current,  "Current HP override");
    app.add_option("--custom-hp-max",      custom_hp_max,      "Max HP override");
    app.add_option("--custom-mp",          custom_mp_current,  "Current MP override");
    app.add_option("--custom-mp-max",      custom_mp_max,      "Max MP override");
    app.add_option("--custom-stamina",     custom_stamina_max, "Max Stamina override");
    app.add_option("--custom-gold",        custom_gold,        "Gold override");
    app.add_option("--custom-xp",          custom_xp_current,  "Current XP override");
    app.add_option("--custom-str",         custom_str,         "STR stat override");
    app.add_option("--custom-dex",         custom_dex,         "DEX stat override");
    app.add_option("--custom-int",         custom_int,         "INT stat override");
    app.add_option("--custom-vit",         custom_vit,         "VIT stat override");
    app.add_option("--custom-class-id",    custom_class_id,    "Class ID override (17748=Cleric captured)");
    app.add_option("--custom-race-id",     custom_race_id,     "Race ID override (captured_race_id must be set first)");

    // Captured-value overrides (only needed if your replay file has
    // different starting state than the defaults).
    int32_t cap_level    = -1;
    int32_t cap_hp_max   = -1;
    int32_t cap_gold     = -1;
    int32_t cap_class_id = -1;
    int32_t cap_race_id  = -1;
    app.add_option("--captured-level",    cap_level,    "Override default captured level value (1)");
    app.add_option("--captured-hp-max",   cap_hp_max,   "Override default captured hp_max (100)");
    app.add_option("--captured-gold",     cap_gold,     "Override default captured gold (0)");
    app.add_option("--captured-class-id", cap_class_id, "Override default captured class (17748=Cleric)");
    app.add_option("--captured-race-id",  cap_race_id,  "Override default captured race (-1=disabled)");

    // ── Path X / V3 — synthetic property emit (2026-04-26) ────────────
    bool     v3_emit                  = false;
    uint32_t v3_target_channel        = 3;
    uint32_t v3_num_properties        = 256;
    bool     v3_reliable              = false;
    bool     v3_has_mbg               = true;     // empirical: capture sets this
    int32_t  v3_rep_paused            = -1;       // -1=auto, 0=force off, 1=force on
    bool     v3_use_modern_format     = true;     // true=modern (no NumBits), false=legacy
    std::string v3_test_mode          = "";        // "" / "spectator" / "score" / "pvpoptin"
    uint32_t v3_subobject_guid        = 0;       // 0 = target channel root, !=0 = subobject
    uint32_t v3_cmd_name              = 0x6A;   // observed in pkt#104
    uint32_t v3_cmd_level             = 28;
    uint32_t v3_cmd_health            = 0;
    uint32_t v3_cmd_max_health        = 0;
    uint32_t v3_cmd_gold              = 0;
    std::string v3_name_override = "";
    app.add_flag  ("--v3-emit",            v3_emit,            "Path X: enable synthetic property-update bunch after replay");
    app.add_option("--v3-name",            v3_name_override,   "V3: separate name for V3 emit (overrides --custom-name for V3 only)");
    app.add_option("--v3-channel",         v3_target_channel,  "V3: target channel (default 3 = PC)");
    app.add_option("--v3-subobject-guid",  v3_subobject_guid,  "V3: SIP NetGUID of subobject to update (0=target channel root). Empirical: ch=3 PC has subobj GUID 7193 (likely PlayerState).");
    app.add_option("--v3-num-properties",  v3_num_properties,  "V3: NumProperties for cmd_handle SerializeInt width (default 256)");
    app.add_flag  ("--v3-reliable",        v3_reliable,        "V3: send bunch reliable (false = fire-and-forget)");
    app.add_option("--v3-has-mbg",         v3_has_mbg,         "V3: bHasMustBeMappedGUIDs header bit (default 1; capture sets this on all ch=3 updates)");
    app.add_option("--v3-rep-paused",      v3_rep_paused,      "V3: bIsReplicationPaused override (-1=auto: 1 if subobj else 0; 0=always 0; 1=always 1). Empirical: ch=3 GUID-7193 always uses 1.");
    app.add_option("--v3-modern-format",   v3_use_modern_format,"V3: inner property format (1=modern: no NumBits; 0=legacy: SIP NumBits). Per RE: modern path doesn't read NumBits.");
    app.add_option("--v3-test-mode",       v3_test_mode,       "V3: test mode override. ''=normal Name update, 'spectator'=set bIsSpectator=true (visible: HUD vanishes), 'score'=set Score=99999 (may not be visible)");
    app.add_option("--v3-cmd-name",        v3_cmd_name,        "V3: cmd_handle for CharacterName (default 0x6A)");
    app.add_option("--v3-cmd-level",       v3_cmd_level,       "V3: cmd_handle for Level (default 28)");
    app.add_option("--v3-cmd-health",      v3_cmd_health,      "V3: cmd_handle for Health (default 0 = skip)");
    app.add_option("--v3-cmd-max-health",  v3_cmd_max_health,  "V3: cmd_handle for MaxHealth (default 0 = skip)");
    app.add_option("--v3-cmd-gold",        v3_cmd_gold,        "V3: cmd_handle for Gold (default 0 = skip)");
// C.O.A.C-Y-B

    bool native_mode = false;
    app.add_flag("--native", native_mode,
        "Path B: run in AUTHORITATIVE-SERVER mode.  Instead of replaying "
        "captured packets, the server emits all world/PC/property data from "
        "live state via NativeConnectSequencer.  M1.0 (scaffolding) currently "
        "completes NMT handshake then logs stubs; M1.1-M1.4 will progressively "
        "fill in bootstrap, PC open, properties, and input handling.");

    bool use_embedded_bootstrap = false;
    app.add_flag("--use-embedded-bootstrap", use_embedded_bootstrap,
        "Phase 1 (no-replay path): load the 400-packet bootstrap from compiled-in "
        "data (protocol/bootstrap/bootstrap_data.h) instead of replay_data.bin. "
        "The server runs with no external data file.");

    std::string generator_config = "";
    app.add_option("--generators", generator_config,
        "Phase D1: path to generated_channels.json (pull-tick bunch generators)");

    bool verbose_bunches = false;
    app.add_flag("--verbose-bunches", verbose_bunches,
        "PHASE 2c: log every outgoing bunch in human-readable form (capped at 200)");
    uint32_t verbose_bunch_limit = 200;
    app.add_option("--verbose-bunch-limit", verbose_bunch_limit,
        "PHASE 2c: cap for --verbose-bunches (0 = unlimited). Default 200.");
    std::string verbose_bunch_log = "";
    app.add_option("--verbose-bunch-log", verbose_bunch_log,
        "PHASE 2c: write [SEND] bunch lines to this file instead of the main emu log");
    uint32_t verbose_bunch_start = 0;
    app.add_option("--verbose-bunch-start", verbose_bunch_start,
        "PHASE 2c: skip the first N replay packets before logging starts (default 0)");

    // ── Session F: live-world pipeline (opt-in) ───────────────────────────
    // When enabled, GameServer stands up a LiveWorld alongside the legacy
    // handshake/replay path.  Every connected client gets registered in the
    // SessionRegistry; the replication-tick thread runs at the configured
    // frequency; an ActorBuilder-backed UDP emitter is ready to fan bunches
    // out.  For Session F the pipeline is an OBSERVER — it watches real
    // clients but doesn't yet drive their packets.  Session G will wire the
    // wire-format parser into the dispatcher and flip this to active.
    bool enable_live_world = false;
    app.add_flag("--enable-live-world", enable_live_world,
        "Session F+G: run the new Session/World/Emit pipeline alongside the "
        "legacy replay path.  With --session-g-send off (default), LiveWorld "
        "observes + produces bytes in dry-run mode.  With --session-g-send on, "
        "real bytes are sent to the client alongside replay traffic.");
    uint32_t live_world_hz = 20;
    app.add_option("--live-world-hz", live_world_hz,
        "Session F: replication-tick frequency when --enable-live-world is on. "
        "Independent of simulation_hz; default 20 Hz.");
    bool session_g_send = false;
    app.add_flag("--session-g-send", session_g_send,
        "Session G: actually send LiveWorld-produced bytes via sendto().  "
        "Default OFF (dry-run): bytes are logged but dropped, client unaffected.  "
        "Turn ON only when you want to test Session G output on-wire.");
    bool session_g_no_spawn = false;
    app.add_flag("--session-g-no-spawn", session_g_no_spawn,
        "Session G: skip spawning player actors in LiveWorld (keeps it purely "
        "observer mode even with --enable-live-world).  Useful for diffing "
        "Session F observer mode vs Session G active mode.");
    bool send_level_visibility_acks = false;
    app.add_flag("--send-level-visibility-acks", send_level_visibility_acks,
        "Answer each ServerUpdateLevelVisibility with ClientAckUpdateLevelVisibility "
        "(PC field 21).  Default OFF: captured runs show levels become visible "
        "without acks.  Acks are queued while the replay runs and sent after it.");
    bool send_hit_confirmations = false;
    app.add_flag("--send-hit-confirmations", send_hit_confirmations,
        "Answer RequestHitVerification_Server with ClientHitConfirmationsReliable "
        "(PC field 171).  Default OFF: GUIDs are placeholders and no targets are sent.");
    bool answer_possession_checks = false;
    app.add_flag("--answer-possession-checks", answer_possession_checks,
        "Answer ServerCheckClientPossession (PC fields 77/78) with ClientRestart(Pawn) "
        "(field 45), at most 3 times per session; replay mode only.  Success shows up as "
        "SERVER ACKNOWLEDGE POSSESSION in the log.");
    // Removed: --mutate-disable-pkt104 / --mutate-disable-pkt79 flags
    // (2026-04-23).  ReplayMutator is not wired into the live path
    // anymore — partial-bunch chains made byte-level mutation a dead end.
    // The library stays for Phase III reference; no CLI surface needed.
// C | O | A / C:Y:B

    // Removed: --allow-variable-name CLI flag (2026-04-23).  Post-hoc
    // FString patching of captured bunches was abandoned entirely in favour
    // of full RepLayout synthesis.  Names (and any other property) now come
    // from live server state written directly by the emitter.  See
    // src/protocol/emit/ for the synthesis path.
    //
    // Removed: --live-pc-spawn / --live-pawn-spawn CLI flags.  They were
    // trying to re-emit fragments of multi-packet logical bunches, which
    // is conceptually impossible.  Phase II will replace them with a
    // full-bunch synthesis path (spawn_player_controller_for_client).

    CLI11_PARSE(app, argc, argv);

    // Auto-detect game path from Steam if not provided
    // GAME_ROOT (the variable scripts/launch_all.bat reads) names the game folder; else the default Steam library is tried
    if (game_path.empty()) {
        if (const char* env_root = std::getenv("GAME_ROOT"); env_root && *env_root && fs::exists(env_root)) {
            game_path = env_root;
            spdlog::info("Game path from GAME_ROOT: {}", game_path);
        }
    }
    if (game_path.empty()) {
        // Common Steam library paths to check
        std::vector<std::string> steam_paths = {
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Ashes of Creation\\Game",
            "C:\\Program Files\\Steam\\steamapps\\common\\Ashes of Creation\\Game",
        };
        for (const auto& p : steam_paths) {
            if (fs::exists(p)) {
                game_path = p;
                spdlog::info("Auto-detected game path: {}", game_path);
                break;
            }
        }
    }

    // Set log level
    if (log_level == "trace")      spdlog::set_level(spdlog::level::trace);
    else if (log_level == "debug") spdlog::set_level(spdlog::level::debug);
    else if (log_level == "info")  spdlog::set_level(spdlog::level::info);
    else if (log_level == "warn")  spdlog::set_level(spdlog::level::warn);
    else if (log_level == "error") spdlog::set_level(spdlog::level::err);

    spdlog::info("================================================================");
    spdlog::info("  AOC Server Emulator v0.1.0");
    spdlog::info("  Ashes of Creation - Backend Emulation Server");
    spdlog::info("================================================================");

    // ── Setup TLS ───────────────────────────────────────────────────────────
    std::shared_ptr<grpc::ServerCredentials> credentials;

    if (no_tls) {
        spdlog::warn("TLS disabled! Using insecure channel.");
        credentials = grpc::InsecureServerCredentials();
    } else {
        // Auto-generate certs if not provided
        if (tls_cert_path.empty() || tls_key_path.empty()) {
            fs::path certs_dir = fs::current_path() / "certs";
            fs::create_directories(certs_dir);

            // Prefer server.pem (AshesEmulator convention); fall back to server.crt
            fs::path pem_path = certs_dir / "server.pem";
            fs::path crt_path = certs_dir / "server.crt";
            tls_key_path = (certs_dir / "server.key").string();

            if (fs::exists(pem_path)) {
                tls_cert_path = pem_path.string();
                spdlog::info("Loaded TLS certificates (server.pem)");
            } else if (fs::exists(crt_path)) {
                tls_cert_path = crt_path.string();
                spdlog::info("Loaded TLS certificates (server.crt)");
            } else {
                tls_cert_path = crt_path.string();
                if (!generate_self_signed_cert(tls_cert_path, tls_key_path)) {
                    spdlog::error("Cannot generate TLS certificates. Use --no-tls to run without TLS.");
                    return 1;
                }
            }

            if (!fs::exists(tls_key_path)) {
                spdlog::error("TLS key not found at {} — cannot continue.", tls_key_path);
                return 1;
            }
        }

        try {
            std::string cert_pem = read_file_contents(tls_cert_path);
            std::string key_pem  = read_file_contents(tls_key_path);
// Cyb with C O A

            grpc::SslServerCredentialsOptions ssl_opts;
            ssl_opts.pem_key_cert_pairs.push_back({key_pem, cert_pem});
            // Don't require client certificates
            ssl_opts.client_certificate_request =
                GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE;

            credentials = grpc::SslServerCredentials(ssl_opts);
            spdlog::info("TLS enabled");
        } catch (const std::exception& e) {
            spdlog::error("Failed to load TLS certificates: {}", e.what());
            return 1;
        }
    }

    // ── Create services ─────────────────────────────────────────────────────
    AuthServiceImpl auth_service;

    LauncherServiceImpl::Config launcher_config;
    launcher_config.game_server_ip   = game_ip;
    launcher_config.game_server_port = game_port;
    launcher_config.branch_id        = branch_id;
    launcher_config.executable       = game_exe;
    launcher_config.file_storage     = game_path;
    LauncherServiceImpl launcher_service(launcher_config);

    XClientServiceImpl xclient_service;
    xclient_service.set_game_address(game_ip, game_port);

    // ── Start UDP Game Server ───────────────────────────────────────────────
    std::unique_ptr<GameServer> game_server;
    if (!use_ds) {
        GameServer::Config gs_config;
        gs_config.bind_ip      = "0.0.0.0";
        gs_config.port         = game_port;
        gs_config.replay_file  = replay_file;        // empty = normal, else replay mode
        gs_config.replay_max_packets = replay_max_packets;
        gs_config.disable_replay_loop = no_replay_loop;  // Road A — Phase B.0
        gs_config.custom_name = custom_name;          // Path B: char name for native mode

        // ── Tier-1 property overrides wired from CLI ─────────────────
        gs_config.custom_level        = custom_level;
        gs_config.custom_hp_current   = custom_hp_current;
        gs_config.custom_hp_max       = custom_hp_max;
        gs_config.custom_mp_current   = custom_mp_current;
        gs_config.custom_mp_max       = custom_mp_max;
        gs_config.custom_stamina_max  = custom_stamina_max;
        gs_config.custom_gold         = custom_gold;
        gs_config.custom_xp_current   = custom_xp_current;
        gs_config.custom_str          = custom_str;
        gs_config.custom_dex          = custom_dex;
        gs_config.custom_int_stat     = custom_int;
        gs_config.custom_vit          = custom_vit;
        gs_config.custom_class_id     = custom_class_id;
        gs_config.custom_race_id      = custom_race_id;
        if (cap_level    >= 0) gs_config.captured_level    = cap_level;
        if (cap_hp_max   >= 0) gs_config.captured_hp_max   = cap_hp_max;
        if (cap_gold     >= 0) gs_config.captured_gold     = cap_gold;
        if (cap_class_id >= 0) gs_config.captured_class_id = cap_class_id;
        if (cap_race_id  >= 0) gs_config.captured_race_id  = cap_race_id;
// -- C.O.A . C-Y-B --

        // ── Path X / V3 wiring ──
        gs_config.v3_emit_enabled         = v3_emit;
        gs_config.v3_custom_name          = v3_name_override;
        gs_config.v3_target_channel       = v3_target_channel;
        gs_config.v3_subobject_guid       = v3_subobject_guid;
        gs_config.v3_num_properties       = v3_num_properties;
        gs_config.v3_reliable             = v3_reliable;
        gs_config.v3_has_mbg              = v3_has_mbg;
        gs_config.v3_rep_paused_override  = v3_rep_paused;
        gs_config.v3_use_modern_format    = v3_use_modern_format;
        gs_config.v3_test_mode            = v3_test_mode;
        gs_config.v3_cmd_handle_name      = v3_cmd_name;
        gs_config.v3_cmd_handle_level     = v3_cmd_level;
        gs_config.v3_cmd_handle_health    = v3_cmd_health;
        gs_config.v3_cmd_handle_max_health= v3_cmd_max_health;
        gs_config.v3_cmd_handle_gold      = v3_cmd_gold;
        gs_config.native_mode = native_mode;          // Path B: authoritative-server flow
        gs_config.use_embedded_bootstrap = use_embedded_bootstrap;
        gs_config.generator_config = generator_config; // Phase D1 allowlist
        gs_config.verbose_bunches     = verbose_bunches;
        gs_config.verbose_bunch_limit = verbose_bunch_limit;
        gs_config.verbose_bunch_log   = verbose_bunch_log;
        gs_config.verbose_bunch_start = verbose_bunch_start;
        // Session F: opt-in live-world pipeline (see --enable-live-world above).
        gs_config.enable_live_world         = enable_live_world;
        gs_config.live_world_replication_hz = live_world_hz;
        // Session G: byte production is always on when live-world is on;
        // actual sending is gated here.  Actor spawn is on by default,
        // toggleable off via --session-g-no-spawn.
        gs_config.session_g_send            = session_g_send;
        gs_config.session_g_spawn_actors    = !session_g_no_spawn;
        gs_config.send_level_visibility_acks = send_level_visibility_acks;
        gs_config.send_hit_confirmations    = send_hit_confirmations;
        gs_config.answer_possession_checks  = answer_possession_checks;
        // (allow_variable_name / live_pc_spawn / live_pawn_spawn fields
        //  were all removed — patching-based paths are gone.)
        game_server = std::make_unique<GameServer>(gs_config);
        // SMALL STEP 1: GameServer pulls the live character name from
        // XClient's store right before replay starts.  This lets the replay
        // be patched with the player's chosen name (from character creation).
        game_server->set_character_name_provider([&xclient_service]() {
            return xclient_service.last_character_name();
        });
        game_server->set_character_archetype_provider([&xclient_service]() {
            return xclient_service.last_character_archetype_id();
        });
        // PD2.3 (2026-05-05): pipe the customization JSON through to
        // AppearanceEmitter so the visible character mesh is built from
        // the player's actual lobby selection (skin/hair/eye/etc) rather
        // than zero defaults.
        game_server->set_customization_json_provider([&xclient_service]() {
            return xclient_service.last_character_customization_json();
        });
        if (!game_server->start()) {
            spdlog::warn("Failed to start UDP game server on port {} — "
                         "game client connections will time out", game_port);
        }
    } else {
        spdlog::info("DS mode: UDP GameServer disabled (external DS expected on {}:{})", game_ip, game_port);
    }

    // ── Structured logging ──────────────────────────────────────────────────
    // Always enable the structured packet log
    if (packet_log_path.empty()) {
        packet_log_path = "logs/capture-" + log_ts + ".jsonl";
    }
    ProxyLogger::instance().open(packet_log_path);
    spdlog::info("Structured packet log: {}", packet_log_path);

    // ── Build and start gRPC server (main: TLS on port 443) ───────────────
    std::string listen_addr = "0.0.0.0:" + std::to_string(grpc_port);

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_addr, credentials);
    builder.RegisterService(&auth_service);
    builder.RegisterService(&launcher_service);
    builder.RegisterService(&xclient_service);

    // Set max message sizes (some messages can be large)
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);  // 64 MB
    builder.SetMaxSendMessageSize(64 * 1024 * 1024);

    // Register logging interceptor to see ALL incoming requests
    std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>> interceptors;
    interceptors.push_back(std::make_unique<LoggingInterceptorFactory>());
    builder.experimental().SetInterceptorCreators(std::move(interceptors));

    g_server = builder.BuildAndStart();
    if (!g_server) {
        spdlog::error("Failed to start gRPC server on {}", listen_addr);
        return 1;
    }

    spdlog::info("gRPC server listening on {} (TLS: Auth, Launcher, XClient)", listen_addr);
// ~ C.O.A - C Y B ~

    spdlog::info("Game server target: {}:{}", game_ip, game_port);
    spdlog::info("");
    spdlog::info("Services registered:");
    spdlog::info("  - ics_auth.AuthService [EMULATION]");
    spdlog::info("  - ics_launcher.LauncherService [EMULATION]");
    spdlog::info("  - TetherService (UDP:19021) [standalone tether_server.exe]");
    spdlog::info("  - ics_xclient.XClientService [EMULATION]");
    if (game_server) {
        spdlog::info("  - GameServer (UDP:{}) [{}]{}",
                     game_port,
                     !replay_file.empty() ? "REPLAY" : "EMULATION",
                     !replay_file.empty() ? " \u2190 " + replay_file : "");
    } else {
        spdlog::info("  - GameServer: DISABLED (--ds mode, external DS on {}:{})", game_ip, game_port);
    }
    spdlog::info("");
    spdlog::info("Press Ctrl+C to stop the server.");

    // ── Signal handling ─────────────────────────────────────────────────────
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Wait until shutdown
    g_server->Wait();

    // Clean shutdown
    if (game_server) game_server->stop();

    spdlog::info("Server stopped.");
    return 0;
}
